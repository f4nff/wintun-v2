# SYS 多队列归还、最后引用与最后尾部复核

从完整验收指纹 `625f01964adca6e35b48d86fcd4c22462ab68be42f31e497f57bea6c71396033` 继续检查，产品版本保持 **2.0.0**，PE 数值版本 **2.0.0.0**。本轮未确认新的生产缺陷，104 个生产源码及版本属性文件逐项保持一致。

## 核对结论

归还路径的队列事务由各自 RxLock 保护，全局 Active／AllReturned 由 ReferenceLock 保护，剩余通知与回调尾部由会话 Returns rundown 保护。两个队列的最后引用归还顺序与最后 rundown pin 归还顺序可以不同，不能据前者推断后者。

故障 QUERY 还必须检查每个队列的 FaultQuiesced、credit 和 pending。即使 Active 已为零，一个队列已完成收尾，也不能代替另一个队列的私有见证。当前实现保留全队列检查；未缩短物理回收屏障。

复核的锁顺序没有发现新的反向等待：发送压力历史查询先释放 SendDiagnosticsLock，再取得 TransitionLock／TxLock；归还异常诊断锁与 RxLock 分开；回调释放 RxLock 后才更新全局引用。新增调度门闩也只放在释放 RxLock 后和释放 rundown 前，不持有队列／引用锁进行测试等待。

同时核对了状态上报与 OID 的接口边界：普通 MiniportOidRequest 在 PASSIVE_LEVEL 执行，当前 direct／synchronous OID 入口均返回不支持；NdisMIndicateStatusEx 会调用上层状态回调，文档允许至 DISPATCH_LEVEL。此次未基于未经确认的重入假设修改 StatusLock 或上报顺序；真实 NDIS 行为仍需目标环境验证。[普通 OID](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_oid_request)、[状态上报](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismindicatestatusex)。

## 新增 32 个组合

扩展既有 `return_reclaim_tests.c`，保留原有 64 个清理、72 个同槽换代和 24 个末代 token 场景，增加：正常 drain／fault × 两种引用归还顺序 × 两种 pin 归还顺序 × 同步／手动复位 KernelEvent × 普通／回绕游标，共 32 个场景。每个当前进程合计执行 192 个场景。

测试通过实际 DLL Reserve／Commit，以及实际 SYS ReceiveBatch，分别在队列 0 构造一个普通 NBL，在队列 1 构造一个含四个原始包的 RSC NBL；因此是五个原始描述符、两个 NBL、两个全局引用。两个真正的 Windows 回调测试线程同时进入实际 Return，每个先停在各自 RxLock 释放后，再停在各自 rundown release 前。

确定性检查以下阶段：

1. 两个队列的完成事务都已结束，NBL／数据 MDL 已释放，两个引用和两个 pin 仍存在。两队列 credit／pending 均为零，但 QUERY 仍不能确认 STOPPED。
2. 放行第一个回调更新引用，Active=1；AllReturned 必须保持未置位。另一个回调仍独立持有引用。
3. 再放行另一个引用，Active=0，AllReturned 恰好置位一次；两个尾部 pin 仍存在。
4. 正常路径由实际 DLL Poll 分队列消费完成项，逐项检查 epoch／generation／slot／queue／status。故障路径没有交付完成项：先只执行队列 0 的实际 quiescence helper，QUERY 仍必须拒绝 STOPPED；队列 1 也发布见证后才允许 STOPPED。
5. detach 并启动实际 native Free。清理取得 AllReturned 后，在真实宿主 rundown 独占锁上被两个 shared pin 阻塞。任选一个 pin 放行，仍须保留映射、五个事件对象和父 MDL；最后一个 pin 归还后才允许完成物理释放。pin 释放顺序独立于引用释放顺序。

检查两次 rundown 获取／归还、单次 AllReturned 信号、每队列的完成与原始包统计，以及资源／进程句柄平衡。此布局队列 0 只有一个 credit，不触发 full-to-room 通知；队列 1 的四个 credit 用满容量，会产生一次该通知。故障广播另有两次队列通知。这些是调用计数，不是实际 worker 唤醒次数或 CPU 测量。

## 反例和 fixture 边界

原有 helper 扩展为可创建多个队列，原单队列入口仍通过原参数调用；DLL pool、queue index、ring 和收包入口均使用实际队列编号。原场景的断言保留，completion 检查还增加 queue 身份核对。

`verify-sys-return-reclaim.ps1` 保留原 20 个错误反例，另在 `out` 生成四个仅将 QUERY 队列循环限制到队列 0 的反例。它们只运行新多队列矩阵，必须在“队列 1 尚无 FaultQuiesced”处以退出码 1 命中精确断言；其他错误、崩溃或超时不算通过。生成 header 只改变该循环边界，生产 header 不变。

专项共 28 个进程：四个 ISO／MS volatile、普通／守护当前进程，各执行全部 192 场景；24 个预期拒绝。生成的 core／query／cleanup／return header、可执行文件和日志有独立哈希，源树指纹不一致即拒绝结果。标准 Tests、Safety 和完整审计继续沿用既有入口，没有增加标准测试进程数。

user session 是 fixture 组装的队列状态，不是完整 DLL open／close。故障分支在可信 native STOPPED 后销毁测试侧未交付的 Published bookkeeping，不把它伪装成实际 DLL close-worker reconciliation。新增场景执行真实 native Free、真实 Windows 锁／事件／线程和宿主 DDI；不启动 queue worker。完整 DLL 关闭与原生发送生命周期仍由已有专项分别验证。

维护收益是将“最后引用”“最后回调尾部”“每个队列的故障见证”分开验证，使未来缩减锁、事件或扫描时能够发现跨队列回归。本轮没有新增生产同步、轮询、缓存或热路径指令，也没有取消原有正确屏障，不宣称 CPU 降幅。

没有安装、签名或加载 SYS。真实内核 CPU、NDIS／IRQL、Verifier、低资源和长稳仍待实机验收；`kernel_runtime_tested=false`、`production_ready=false`。
