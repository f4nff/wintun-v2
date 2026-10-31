# DLL 同步 Drain 等待失败与重复重试（2.0.0）

本轮从已验收指纹 `e6d177c61ca7426ab10cabc45b0c0a46257f36d1b39792a117cf6e981f3e73b6` 继续检查 SYS／DLL 槽位归还、回收、唤醒和开销。产品版本保持 **2.0.0**，PE 数值文件版本 **2.0.0.0**，ABI 不变。

## 确认的问题

公共 C 同步 `Drain` 先请求生命周期 worker 排空，然后在 State 锁保护下等待 Draining 或 worker 错误。原循环忽略 `SleepConditionVariableSRW` 的返回值；同一实现内的 PollPolicy 和 CloseInternal 已处理非超时等待失败。

等待原语返回错误、排空谓词仍未满足时，旧 Drain 立即重试。在测试所提供的连续立即失败条件下，调用者重复执行等待且持有 State；若失败持续存在，循环没有退出条件，还可能阻止 worker 取得该锁。单次失败随后恢复时，旧 Drain 也会丢弃等待错误，并可能最终报告成功。这是控制等待路径的问题，本轮没有确认新的报文槽位提前复用或 SYS 物理回收缺陷。

微软接口定义失败返回零并通过 GetLastError 提供错误；超时另行标识 ERROR_TIMEOUT；正常／虚假唤醒后须重新检查谓词。[Microsoft：SleepConditionVariableSRW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw)。测试注入受控 ERROR_OPERATION_ABORTED，不能证明正常机器上自然发生该失败，更不代表已测到真实低资源或内核 NDIS 故障。

## 修正与维护边界

生产修改仅在 `api/v2/session_control.h` 的 Drain 循环：检测到非超时等待失败后返回 PB_CORRUPT；退出时仍释放 State。即使实际 DRAIN 在等待释放锁期间已经完成，等待错误也不再被随后读取的 Draining 覆盖。正常谓词检查、虚假唤醒后的重试、原生 I/O 自身错误保持原行为；回归同时检查调用者原始等待错误码。

采用与已有两个等待循环相同的错误处理方式，没有新增等待抽象、状态、锁、事件、定时器或 ABI。只结束当前调用者的等待：BeginStop 已请求的 worker 继续持有会话；不取消进行中的 I/O，不清除用户 loan／原生引用，不触发 FaultSession，也不授权物理释放。104 个原生产文件中，其余 103 个逐项哈希保持一致，所有 SYS 生产文件未改。

收益是使罕见控制错误有明确出口，消除该错误输入下无界立即重试的可能，并统一现有错误处理约定。报文热路径没有变化；不宣称正常收发 CPU 降幅。

## 复现与回归

`tests/model/drain_wait_tests.c` 包含实际 session/control/lifecycle。它使用真实 Windows SRW 锁、条件变量、事件、调用线程和私有线程池；DRAIN I/O 与等待失败是宿主边界。旧／新实现都在等待返回时持有 State，测试不手工修改 Draining、Progress 或回收状态。

每进程 19 个场景：

- I/O 排队／正在执行 × 一次／连续八次立即失败 × 原生 I/O 成功／失败，共八个。
- 在等待释放 State 期间让实际 worker 完成 I/O，再返回一次等待失败；组合原生成功／失败，共两个。
- 普通等待／一次虚假成功唤醒 × 排队／执行中 × 原生成功／失败，共八个。虚假唤醒不能让未完成的 Drain 提前返回。
- 空会话参数拒绝一个。

为有界复现旧实现，旧版观察器在故障预算之后才显式打开真实 worker 的门闩，等待实际 I/O 结束。这只是旧代码测试的恢复措施。十个错误等待场景中，旧版共注入 38 次错误、执行 46 次外层等待，需要八次显式恢复；当前版在每例第一次错误后返回，共十次错误、十次等待，无需观察器恢复。完成交错中当前版也返回原 ERROR_OPERATION_ABORTED。数字是宿主调用计数，不是 CPU 时间或原生唤醒次数。

每例预先保留真实 DLL reserved loan 和一个宿主原生 active 引用。无论 Drain 返回成功或错误，停止后的 reserve 仍被拒绝、既有 loan 仍可 Cancel、PrepareClose 不能绕过两道所有权边界。测试解除 I/O 故障与门闩、保留 native active 时，PrepareClose 仍不能完成；最后才归还 native active，走实际 Close 并检查 runtime 会话／保留字节／adapter、守护分配以及自有事件／线程句柄全部归零。原生引用使用 PB_API_TESTS 模型，不是实际 NBL／SYS 引用。

句柄检查直接跟踪本 fixture 和实际 session 创建／关闭的事件及调用线程，避免把 Windows 私有线程池异步销毁内部句柄或进程级运行时缓存的瞬时变化误判为会话泄漏。

`verify-dll-drain-wait.ps1` 固定校验旧 control header 哈希，使用同一当前 session/lifecycle 生成旧／新版本。ISO／MS volatile、普通／守护内存共八个正向进程；四个旧版进程另要求立即错误退出，必须以退出码 1 命中精确断言，其他失败不能算复现。生成的 session 只调整 include 解析和所选 control header；记录可执行文件、生成文件哈希和日志，源码指纹不一致即拒绝结果。

新增回归接入标准 Tests、Safety 和完整离线生命周期审计。修正前复现日志保存在 `out/drain-wait-review/pre-fix-baseline.log` 与 `pre-fix-require-fix.log`，基线生产哈希保存在同目录 `baseline.json`。

没有安装、签名或加载 SYS。既有槽位换代、最后归还／rundown、暂停与满环唤醒、关闭重试和成本基准随完整审计重跑；真实内核 CPU、NDIS／IRQL、Verifier、低资源和长稳仍须实机验收。`kernel_runtime_tested=false`、`production_ready=false`。
