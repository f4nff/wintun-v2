# DLL 策略等待者与替换请求槽位

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮从完整验收指纹 `3c1e122a4b0027ccd7a983147d8419dc1e87dbfd938aed52e503ae6e3d1f49ac` 继续检查槽位归还、回收、唤醒和 CPU 路径。确认并修正原生 C 生命周期扩展的策略请求槽位竞态；这与报文 TX/RX 槽位是不同资源，没有新确认的报文提前复用或内核提前释放。

## 已复现的旧等待者越权确认

PollPolicy 原先只在进入时验证 `PolicyOccupied` 和 `PolicyRequested==ticket`，随后等待 `PolicyCompleted==ticket`。SleepConditionVariableSRW 会释放 State，其他调用者可以消费或放弃旧结果，并提交新请求；旧等待者恢复时仍可能看到旧完成编号，却已经不再拥有当前槽位。

实际函数的受控复现如下：

1. ticket 1 已进入策略 I/O，等待线程在真实条件变量上睡眠。
2. I/O 完成，等待线程已被唤醒，但尚未恢复到生产代码读取状态。
3. 另一个调用者 PollPolicy 消费 ticket 1；它随后提交 ticket 2，并将 ticket 2 的 I/O 保持未完成。也覆盖以 ForgetPolicy 放弃旧结果的方式。
4. 旧等待者恢复时，PolicyCompleted 仍是 1。旧循环直接退出，第二次对 ticket 1 返回 PB_OK，并将 ticket 2 的 PolicyOccupied 清为 FALSE。
5. ticket 2 仍在执行时，ticket 3 被错误接纳，ticket 2 的调用者已因当前请求编号被覆盖而失去正常确认资格。

日志位于 `out/policy-wait-review/pre-fix.log`。若替换请求已经完成，旧等待者反而会继续等待一个不再能出现的旧完成编号。回归用有限 200 ms 等待复现 PB_EMPTY；INFINITE 不再能满足该谓词是基于编号单调前进的推论，没有另运行永久挂起的进程。

条件变量允许虚假唤醒和被其他线程抢先取得锁的唤醒，恢复后需要重新检查完整谓词。[Microsoft：SleepConditionVariableSRW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw)。仅重新检查完成编号不足以证明结果消费权仍属于原请求。

## 局部修正

PollPolicy 改为在每次循环取得 State 后，先重新检查槽位占用及当前 ticket，再检查是否完成，最后才决定继续等待或超时。占用已释放或编号已改变时返回 PB_STALE，不修改 operation，也不清除新请求的占用状态。成功复制结果与释放槽位继续位于同一个 State 独占区间。

没有新增状态、请求容器、锁、事件或定时轮询，没有修改 BeginSetPolicy、ForgetPolicy、实际策略 I/O 或停止转换。仍保持每会话一个未确认策略请求的背压；放弃操作仍保留真实 I/O 到完成之后，才允许重用槽位。输入校验、零/有限/无限等待、真正睡眠失败的 PB_CORRUPT 以及正常完成结果继续沿用原规则。

本轮生产改动仅在 `api/v2/session_lifecycle.h`。SYS、报文槽位、Active/AllReturned/Returns rundown、Device Close、ABI、函数表和版本均未改动。此次修正不增加包处理热路径工作；避免等待已失效 ticket 是控制进展修正，不宣称实机 CPU 降幅。

## 可重复回归及范围

`policy_wait_tests.c` 直接包含当前 session，运行实际策略/lifecycle 函数、真实 Windows SRW locks、条件变量、线程和私有单线程池。策略 I/O 使用原有 PB_API_TESTS 边界；未加载 SYS。

宿主等待包装器在真实 Sleep 返回后、生产代码尚未读取状态时，短暂释放 State 并等待 gate，之后恢复独占锁再返回。这只安排等价的“其他线程抢先取得锁”观察窗口，不修改任何 session 字段。请求消费、放弃和替换全部调用真实 C 接口。不能把这个受控时序说成已在真实 NDIS 调度中复现；也没有证明安全 Rust PolicyChange 的可变借用会允许同一 ticket 并发 wait/drop。

每个进程执行十组场景：

- 同伴 Poll 或 Forget 消费/放弃旧 ticket，各组合没有替换、替换正在执行、替换已经完成，共六组。新实现必须返回 STALE、保持输出哨兵、只经历原有一次睡眠；新请求仍占用槽位，第三次请求仍返回 BUSY。旧实现保留双确认、错误接纳第三次请求及旧 ticket 超时的证据。
- 未就绪的条件变量通知，以及真实 BeginStop 通知，各一组。等待者必须继续等待真实策略完成，再且仅再消费一次结果；允许 Windows 额外的合法虚假唤醒。停止场景保留用户 loan 和宿主 NativeActive 见证，归还及解除见证后才关闭。
- 有限超时和注入的条件变量失败，各一组。它们不能释放仍在执行的请求，也不能修改结果输出；真实操作完成后仍可正常确认。

所有场景在等待者退出后才关闭公共 session。额外 shell 引用覆盖线程池退出后的资源检查；关闭仍须完成原有资源流程，运行时 session/bytes/adapter 预算及保护页分配计数归零。

`verify-dll-policy-wait.ps1` 对相同当前 session 仅替换生命周期头，新旧各运行 ISO/MS volatile、普通/保护页模式，共八个进程。冻结头位于 `out/dll-policy-wait-baseline/3c1e122a4b00/api/v2/session_lifecycle.h`，SHA256 为 `a3f965c276124ba99d5ecd64de8ea51d81967ac56d6f8c70f30259849740029c`。代码、执行码、程序及源码哈希均保留。

```powershell
.\tools\audit\verify-dll-policy-wait.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

新场景接入 Tests/Safety，进程数分别为 45、37。完整验收新增 DllPolicyWait，在最终同一源码指纹执行 39 项检查，并比较既有静态诊断及版本资源。原有报文归还、满环、故障、回调尾部、物理关闭和成本回归继续运行。

未安装、签名或加载 SYS；真实 IRQL、NDIS、Verifier、ETW/WPA、CPU、低资源和长稳仍待实机验证，`kernel_runtime_tested`、`production_ready` 保持 false。
