# SYS 故障 QUERY 与尚未记账的发送准入

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮起始完整验收指纹为 `838f9da30fa307c5a39fe76e1d0e572af49cd9a9ab6d7e5a22d5c37b031e2d4d`。冻结 QUERY/send/reference 源码位于 `out/sys-fault-query-baseline/838f9da30fa3/driver/v2`；旧 QUERY 的 SHA256 为 `f65df836a159bfb078bdf3518336304f3caaff8dea236e32add997596aeae6de`。

## 已复现的过早 STOPPED

发送初次查找在 TransitionLock shared 内完成：读取 Running/Draining/Stopping，取得 Returns rundown，再保留 Active 引用。Fault 不取得 TransitionLock，而是发布 Draining/Faulted 并唤醒 worker。旧 QUERY 也只取得 shared，因此可以与这段尚未记账的发送查找并行。

实际路径的受控时序为：

1. 发送已经读到准入允许并取得 rundown，但还没有保留 Active；此时 Active=0。
2. 另一个线程执行实际 Fault 和 worker 私有 quiescence helper；队列没有 RX credit/pending，FaultQuiesced=1。
3. 旧 QUERY 看到所有队列见证及 Active=0，返回 STOPPED。
4. 放行发送查找后，它仍会保留之前决定的引用，Active 变为 1。第二次准入检查正确拒绝数据工作；引用继续覆盖整个完成上调。
5. 完成回调期间再次 QUERY，状态又变为 DRAINING、Active=1。最终引用释放后才稳定 STOPPED。

这违反了故障停止确认应覆盖所有已准入引用的要求。本轮没有复现物理提前释放：既有 unregister 的 TransitionLock exclusive、AllReturned 和 Returns rundown 屏障仍提供保护。修正针对逻辑停止确认过早，不将其描述为已经证实的内存安全故障。

## 修正

正常运行和正常 DRAIN 的 QUERY 继续使用共享快照。正常 DRAIN 原本就用 exclusive 转换捕获边界，已经覆盖发送查找与引用记账。

QUERY 在排空状态观察到 Faulted 时，先释放 shared，再取得同一个 TransitionLock exclusive，重新查找 ctx->V2 并重建快照。exclusive 等待已经进入的短发送查找完成记账；后续查找会看到 Draining，不能再作新的准入引用决定。原有每队列 RX 见证、credit/pending、末次 Active 读取及并发故障复查保持不变。

不能在持有 shared 时递归获取 exclusive，也不能在释放/reacquire 之间保留旧 session 指针。Microsoft 的两种 ExAcquireSpinLock 接口均禁止调用者已持有同一锁，且要求匹配释放接口及恢复原 IRQL。当前重试只升级一次，所有成功/无会话出口按实际持有方式释放。[Microsoft：exclusive](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exacquirespinlockexclusive)、[Microsoft：shared](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exacquirespinlockshared)。

没有改变 session 布局、引用计数写入、槽位状态机、共享协议或 ABI。没有增加发送热路径操作或常驻回收状态。故障 QUERY 多一次锁切换并可能等待短查找；本轮不宣称它降低 CPU。正常 QUERY 保留共享方式，避免把所有控制查询变成独占快照。

## 定向回归

`fault_query_tests.c` 执行实际 PbV2Send、rundown/reference、Fault、private quiescence 和 QUERY，使用真实 Windows SRW locks、线程、事件及保护页。NDIS 完成边界使用宿主替身；读取/描述报文、控制回复和发布的替身一旦被调用即失败，确保故障后的发送没有数据工作或槽复用。

旧实现必须复现先 STOPPED、后 Active=1。当前 QUERY 在发送查找仍被保留时不能完成，放行后必须看到 Active=1；完成回调还实际重入 QUERY，验证上调不持有 TransitionLock 且引用覆盖整个回调。只有最终引用释放后才确认 STOPPED。AllReturned/rundown 的状态也独立核对，保留逻辑确认与物理回收的区别。

另有两个升级窗口：释放 shared 后，先将 ctx->V2 设为 NULL 或另一个 epoch=84 的新会话，再把旧 session 设为 PAGE_NOACCESS。重新取得 exclusive 后必须安静返回 DEVICE_NOT_READY，或返回新会话的 RUNNING/epoch=84 快照，不能使用旧指针或升级前快照。这两个新增窗口仅在当前实现执行，旧实现没有升级路径。

普通 running 和正常 drain 用例核对两次共享获取、零次 exclusive 获取。原 session-query 用例继续对照运行，覆盖 credit、有限提交边界、私有 fault 见证、快照中引用变化、并发故障及 detach 等待。

新旧实现均与相同当前 send/reference/core 配对，在 ISO/MS volatile 模式执行两类测试，共八个实际进程。宿主 NBL fixture 只补齐编译实际 send 所需的统计字段；其余包含 QUERY 的宿主回归补齐真实 SRW exclusive 对应操作。没有使用缩短超时或替换原子同步来掩盖时序。

修正前日志为 `out/sys-fault-query-work/pre-fix.log`；加入 detach/替换保护页核对的定向证据之一为 `out/sys-fault-query/0b16f80cafb5/evidence.json`。完整验收重新绑定最终同一源码指纹。

```powershell
.\tools\audit\verify-sys-fault-query.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

新场景接入 Tests/Safety，完整验收增加 SysFaultQuery。已有回调/物理清理、槽位归还、满环恢复、唤醒及成本回归继续运行。未安装、签名或加载 SYS；真实内核 IRQL、锁争用、NDIS、Verifier、ETW/WPA、CPU 与长稳仍须实机验证，`kernel_runtime_tested`、`production_ready` 保持 false。

后续[锁模式与恢复级别复核](sys-query-lock-mode-review.md)在生产实现不变的基础上，将入口级别、回调重入及移除/替换出口纳入观察，并增加四个独立负向对照；定向脚本现记录 12 个进程，其中四个必须在精确断言处按预期失败。
