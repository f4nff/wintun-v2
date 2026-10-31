# SYS 暂停、停止唤醒与迟到归还复核

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮起始完整验收指纹为 `f871a753e42107bf462ac7bf3634070654b11a0358990828abd4db433a9c3e22`。本轮未确认新的生产缺陷，没有修改 SYS/DLL 生产实现；增加回归覆盖，防止后续维护错误合并不同的停止与回收条件。

## 再次核对的所有权和唤醒条件

DLL 归还 TX loan 的成功发布、SYS 实际验证 recycle token 并释放私有槽、NDIS 接收引用归零及物理关闭是不同的状态。只发布 token 不等于 SYS 已回收；Active=0 或 QUERY=STOPPED 也不能推断每个 TX 槽已经 FREE。

当前故障 worker 在确认 Faulted 后，只执行自己的 RX quiescence 并退出，不再消费可能被持续写入的共享 recycle 环。尚未被验证消费的 TX token 仍对应 PUBLISHED 槽。这不是归还泄漏的证明：停止接纳后不能复用这些槽，DLL 必须先退还用户借用；最终原生关闭继续承担物理回收屏障。不能为使计数看起来归零而在故障见证中伪造 TX 回收。

正常 DRAIN 则允许暂停中的 worker 完成既有工作：其自身 KernelEvent 唤醒后先回收 TX，再处理已捕获的有限 RX 边界。边界外的晚到提交不能延长排空；完成消费者、credit 归还、预算 continuation 和停止通知仍各自负责相应进展。DLL 的本地退休回退、公开 loan/Published 记账、AllReturned、Returns rundown、worker join 与 Device Close 顺序继续保留。

## 新增的交叉窗口

`tests/model/drain_boundary_tests.c` 在既有真实 Windows 线程/事件 fixture 内增加 16 个组合：

| 维度 | 两种取值 |
| --- | --- |
| 队列 KernelEvent | 同步事件、手动复位事件 |
| worker 的等待位置 | 实际等待之前的 gate、无 gate 的宿主等待路径 |
| RX/recycle 游标 | 普通值、UINT64_MAX 附近回绕 |
| 停止方式 | 正常 DRAIN、Fault |

每个组合预置四个已实际 reserve/publish 的私有 TX 槽及三条 RX 提交；确认空闲位图为零后，才设置满池证明，并让暂停中的实际 worker 进入等待路径。正常 DRAIN 捕获三条提交的边界；随后再追加三条。只将其中一个有效 TX 归还 token 写入实际 recycle 环，生产者返回 notify=1，但测试刻意停在其 SetEvent 之前。只发出一次实际 DRAIN 唤醒或 Fault 唤醒；没有另补归还通知，核对停止通知本身足以让 worker 重查必要状态。

gate 明确控制实际等待之前的窗口；无 gate 分支的 WaitEntered 标记进入宿主等待包装函数，之后何时真正阻塞由 Windows 调度决定，不宣称已控制精确的阻塞时刻。

正常 DRAIN 必须满足：

- 实际 recycle 消费前进一项，仅对应槽变为 FREE，generation 不变，空闲位图恰好只有这一位，满池证明失效；其他三个未归还槽仍为 PUBLISHED。
- 仅最初三条 RX 提交得到停止完成，边界外三条保留未消费；没有 NDIS indication、Active、pending 或 credit 遗留。
- 总计两次接收检查、两次进入等待点，其中第二次是排空后的空闲等待；在有限观察窗口内不再重复进入等待点。这里只有一次停止 KernelEvent 写入。

Fault 必须满足：

- worker 从等待中退出并发布 FaultQuiesced，零次接收检查；没有消费 RX 或 recycle 环。
- 四个 TX 槽仍为 PUBLISHED、空闲位图仍为零、满池证明保留；没有伪造 completion 或原生槽复用。
- QUERY 可以在私有 RX 见证和零原生引用的基础上返回 STOPPED，但这个结果不改变上述 TX 所有权。

两条路径都在之后将共享 producer 改成非法远端值，再核对 QUERY 的私有停止结论不被这些共享尾部撤销。正常 worker 最后仍须通过单独的 Stopping/StopEvent 退出并 join。fixture 核对所有 NBL、MDL、池对象及句柄收支；故障路径保留的私有 TX 状态不会被解释为已经完成的实际 recycle。

## 维护与验证范围

新增用例接入原有 Tests/Safety 和 `verify-sys-paused-recycle.ps1` 当前实现入口；该脚本的冻结旧 worker 继续独立复现原有暂停归还缺口，不把新增组合算作旧实现已经通过。ISO/MS volatile 与普通/保护页模式继续执行。定向脚本仍为八个进程，其中两个是原有旧 worker 的预期失败；每个当前 drain 进程执行上述 16 个组合。

没有增加生产状态、定时器、锁、唤醒或热路径工作，也没有改变槽位状态机、共享布局或 ABI。起始时保存的 103 个生产文件及版本属性文件通过 SHA256 逐项核对；记录位于 `out/paused-stop-review/baseline.json`。本轮的收益是把停止进展和实际槽位回收的边界固定在可重复回归中，不宣称新的 CPU 降幅。

```powershell
.\tools\audit\verify-sys-paused-recycle.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

完整离线验收在最终同一源码指纹执行 36 项检查，并比较原有 DLL/SYS 静态诊断及版本资源。宿主等待和操作计数不代表真实 IRQL、NDIS 调度、锁争用或内核 CPU。未安装、签名或加载 SYS；Verifier、ETW/WPA、低资源和长稳仍须实机验证，`kernel_runtime_tested`、`production_ready` 保持 false。
