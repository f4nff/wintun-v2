# 2.0.0：暂停期间的 TX 槽位回收

产品版本保持 2.0.0，PE 文件版本保持 2.0.0.0；共享布局、函数表、Data/API 2.1、packet 1.1 和 lifecycle 1 不变。

## 确认的问题

旧 `session_worker.h` 的 SHA-256 为 `87f7d9af49ea9e21da23b0392a18323b71f5cd8d65016c571d6904da95f476d9`，保存在 `out/paused-recycle-baseline/driver/v2/session_worker.h`。

当 Running 为零且尚未 DRAIN 时，worker 清除 KernelEvent 后只刷新 RX 完成，随后等待 StopEvent、KernelEvent 或 RunningEvent。这个分支既不消费 TX_RECYCLE，也不对它 arm/recheck。

DLL 的 ReleaseBatch 成功只表示 token 已进入回收环、本地 loan 已归还；SYS 私有槽仍是 PUBLISHED。若暂停持续，归还可以一直停在环内：已经产生的通知会被清除；未 arm 的后续归还不会产生通知；即使有通知，旧暂停分支也不会回收。这是暂停期间的回收进度缺陷，不是已经证明的永久内存泄漏；restart、DRAIN 或最终注销仍有既有处理路径。

新增回归在修正前以退出码 1 复现未回收状态；同一回归在修正后通过。

## 修正及等待依据

worker 在选择运行或暂停路径前，统一在 TxLock 下执行原有有界回收；两条路径在睡眠前共用同一 TX_RECYCLE arm/recheck。暂停分支仍只刷新 RX 完成，不调用 ReceiveBatch，也不 arm/消费 RX_SUBMIT。回收失败仍立刻结束本轮事务，由下一轮发布私有故障见证。

| 归还发生的时间 | 保证进度的依据 |
| --- | --- |
| 清除 KernelEvent 前后 | 随后的实际回收读取 producer，事件合并不丢工作 |
| 回收读空之后、arm 之前 | arm 后的 producer recheck 发现工作，直接继续 |
| arm 之后、进入 wait 之前或之后 | 生产者清除 Armed 并设置 KernelEvent，真实事件保留通知 |
| 超过一次 128-token 回收预算 | arm/recheck 看到剩余 token，下一轮继续；每次 TxLock 持有仍受四批预算限制 |
| 暂停中重新启动 | RunningEvent 唤醒三对象等待；重新观察状态后恢复既有接收路径 |
| 暂停中 DRAIN、故障或停止 | 既有 KernelEvent/StopEvent 通知及私有状态检查仍适用 |

槽位仍必须通过 epoch、queue、slot、generation 和 PUBLISHED 状态验证后才能变为 FREE。未归还的用户槽、活跃 RX 槽、credits、Active 引用和 Returns rundown 不因暂停或事件状态提前释放。没有增加共享标志、缓存、线程、定时器、轮询期限或新的资源类型。

## CPU 开销与保留的通知

暂停时每次真正唤醒会增加一次有界回收及一次 arm/recheck；这是使已归还所有权推进所需的工作。排空后仍无限等待事件，不通过周期扫描维持进度。运行路径保持回收 → 接收 → arm/recheck 的原有顺序。

160 个唯一 token 在两轮预算内完成回收，随后只进入一次空闲等待；暂停期间零接收扫描，已 ready 的 RX 提交仍未消费。真实宿主 worker 连续接收 16 次归还时恰有 16 次通知及 16 次重新等待；无新工作时没有重复进入空闲循环。restart 只靠 RunningEvent 恢复预先排队的提交。它们是操作计数和宿主事件证据，不是内核 CPU 百分比或吞吐测量。

DLL PollInternal 每次成功消费完成项后设置 KernelEvent 的行为保留，并补充原因注释。不能只根据消费前 Available 的“未满”快照去掉通知：SYS 可在该快照之后、Consumer 发布之前填满完成环，并因 PB_FULL 留下 pending。DLL 随后释放空间时若省略通知，SYS 可以一直等待。Armed 当前服务于数据可读通知，不是生产者等待空间的独立握手。要减少这项调用，需另行完整证明空间通知协议及 ABI 兼容收益；本轮不增加这样的协议。

## 验证

- `worker_poll_tests.c`：执行实际 worker、recycle 和共享核心，覆盖正常/暂停 × 清除前后、回收读空后、arm 前后的十种归还时序，以及既有七种故障时序。
- `drain_boundary_tests.c`：执行实际 worker、回收、完成、NBL 分配/归还及 QUERY；增加暂停归还、160-token 跨预算及 uint64 环绕、真实等待前后的连续归还、restart，以及暂停期间损坏回收游标的故障收尾。故障测试保留真实 NDIS 模型对象，验证槽、credit、资源及引用必须等实际归还，不能由 worker 见证单独放行。
- `completion_progress_tests.c` 的暂停 fixture 连接完整的回收 view，继续验证满完成环不自旋、预算续行和完成消费者通知后的进度。
- Tests/Safety 及既有 DLL、Rust、有限 DRAIN、并发归还、故障和卸载回归继续执行。历史 send-fault A/B 的旧 worker 只运行原有正常运行范围；新暂停回归由当前 worker 和独立复现入口执行。
- 修复 Describe 历史 A/B fixture 的签名兼容：旧函数不带后来加入的诊断输出，测试适配器只省略旧版本不存在的诊断参数/断言；旧函数体不变，原有返回码、逐字节输出、策略读取和解析计数仍核对，当前实现继续检查诊断原因。
- 历史 Send 的 Describe 调用也使用旧签名适配器；旧版只运行当时已有的预算、回收故障、回调寿命及 Ethernet/prefix 合约，满池扫描按旧版每窗口一次核对。后来新增的诊断、跨窗口满池缓存和同窗口晚归还恢复只对当前版本执行，避免把后来的需求强加给冻结基线。
- 历史 token-copy 回收基线同样早于满池缓存，限定比较直接回收预算、复制字节、token/游标验证及故障后发送接纳；当前版本继续执行完整发送和恢复回归。证据 JSON 明确记录历史范围，旧版没有的验证不记为已通过。

```powershell
.\tools\audit\verify-sys-paused-recycle.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

复现入口核对旧 worker 哈希，将预期的旧版回归失败单独记录，当前 worker 在两种 volatile 模式及普通/保护页 fixture 中必须通过。完整验收固定同一源码指纹，输出实际退出码和日志。所有验证均为离线 WDK 构建、静态分析及宿主测试；未安装、签名或加载 SYS。真实 IRQL、NDIS 调度、Verifier、ETW/WPA、低资源和长稳仍须内核验收。
