# SYS 第五轮：发送工作预算与故障排空

后续正常 DRAIN 的有限提交边界、批量归还热路径及 A/B 证据见 [SYS 热路径与排空修正](sys-hotpath-drain-remediation.md)。

本轮从 `651d1cf4011ab147ed1d7ebd1df34843b950abfd7ada590f123a9fb9e388156b` 开始。原文件快照保存在 `out/sys-fix-baseline/651d1cf4011a`；复核证据保存在 `out/sys-review-20261029`。改动继续使用既有 V2 私有 SYS 路径，不改变共享布局、函数表大小或 ABI 版本。SYS、DLL、probe 仍须来自相同源码指纹。

## 1 修正的问题

| 问题 | 原行为 | 修正后的边界 |
| --- | --- | --- |
| P2：失败发送绕过批量预算 | 以成功描述符数和成功字节数判断批次；池满或 Describe 拒绝时计数不前进，4096 项链可持续持有 TxLock 和 TransitionLock | 每个 NB 尝试都收费，空 NBL 也占一个尝试；每窗口最多 32 次或 128 KiB 输入工作量，随后释放两层锁并重新检查接纳状态 |
| P2：故障后完成记账永久滞留 | worker 见 Faulted 立即退出，完成环满时留下 pending/credit；即使最后 NDIS 引用已归还且消费者腾出空位，后续唤醒也无人排空 | 所属 worker 在事务边界发布私有 FaultQuiesced；故障时清退已完成对象的私有记账，保留活跃槽和真实引用，QUERY 依据私有见证与实际归还确认 STOPPED |

故障滞留的原复现使用原始 ABI：非法 epoch 的错误完成填满 ring，有效对象归还留下 pending，重复 token 导致故障。它不代表正常 C API 必然生成同样流量。旧复现日志记录了 worker 已退出、原生引用为零、完成环已有空位，但八次唤醒后仍有一个 pending/credit 的状态。

## 2 将发送寿命与锁窗口分开

`session_send.h` 将批量发送、发布和窗口状态集中起来，`session_recycle.h` 保留原有最多四批回收预算；测试直接包含这些生产函数。没有加入后台发送队列、额外分配或新线程。

每个被接纳的发送调用取得一个 Returns rundown pin，并在 ReferenceLock 下登记 Active、清除 AllReturned。引用一直保留到整个发送完成上调返回，再释放 Active 和 rundown。控制回复上调、锁窗口间隙及 NDIS 发送完成都受该寿命保护；所有上调均在 TransitionLock 和 TxLock 释放后执行。

窗口预算具有以下规则：

- 成功、池满、格式/描述拒绝、控制帧和空 NBL 都受尝试数约束。输入长度按 `min(length, PB_BLOCK_SIZE)` 收费，超大畸形长度不会溢出字节计数。
- 切换队列、发布失败、仅刷新 TxLock 都不重置外层 TransitionLock 的工作预算。窗口结束才重置。
- 重新取得 TransitionLock 后，核对 `ctx->V2` 身份、Running、Draining 和 Stopping；暂停、故障或注销后，不继续访问旧会话的数据路径。
- TxLock 内首次确认池满后，缓存该结果到当前队列锁释放。其他路径在这段时间不能修改私有池，重复扫描无法得到不同结果；下个窗口先回收再尝试，所以消费者回收仍能推进。
- 同一 NBL 的较早 NB 若在窗口刷新时发布失败，保留该 NBL 的错误状态，不以稍后观察到的暂停覆盖，也不继续处理其后续 NB。

预算限制的是临界区内的工作量，不是整条任意长度发送链的总运行时间，也不是以微秒计的 DPC 上限。回收、解析和核心分配器各有既有的有限工作上限；真实 IRQL、调度、公平性和吞吐仍需内核测试。

`ActiveReferences` 现在明确包含已 indication 的 NBL 和已接纳的发送调用，不等于单纯的 NBL 数量。统计 RxObjects/RxReturned 仍只统计接收对象。

## 3 故障收尾的所有权

故障不再要求损坏或敌对的共享 ring 合作，但没有跳过原生释放屏障。

| 所有权 | 收尾依据 |
| --- | --- |
| worker 尚在构造的私有 NBL 链 | 只有队列所属 worker 能发布 FaultQuiesced；它在 ReceiveBatch 返回后才发布，此前链已登记引用并 indication，或已本地归还 |
| 私有 pending completion | RxLock 下验证 PendingCount 不大于 Credits，Credits 不大于 RingCapacity；只减去这些已经完成的条目，不操作用户共享 consumer 游标 |
| NDIS 仍持有的槽 | 不清除 Active，不减去对应 credit；只能由实际 ReturnOne 清除槽，生成并清退其完成记账，随后释放实际引用 |
| 故障队列确认 | worker 在 RxLock 下扫描有限私有槽，确认剩余 credit 等于活跃槽数且无 pending，才发布私有见证；不变量不满足则保留隔离 |
| QUERY 的 STOPPED | 每个队列都有私有见证、没有 credit/pending，并在检查队列后重新读取 Active 为零；并发故障使正常排空快照失效，需下一次查询重新确认 |
| 映射、MDL、事件、队列内存 | 注销阻断接纳，停止并 join 所有 worker，等待 AllReturned，再等待 Returns rundown 完成，最后解锁父 MDL、释放所有资源 |
| DLL Published | 无用户 lease、真实 QUERY 返回 STOPPED 且无原生引用后才归并；兼容“只有内核发现故障，DLL 本地 Fatal 未置位”的情形，最终仍执行 Device Close 屏障 |

故障后的 QUERY 不再使用共享 RX_SUBMIT producer 决定是否排空，已停止的敌对生产者写入也不能撤销私有收尾。正常排空仍处理提交和完成；故障清退不会增加 Completions，因为它没有向用户实际交付完成。NDIS 错误/成功归还统计仍来自真实回调。

## 4 回归与证据

- `send_window_tests.c` 执行实际 send、recycle 和引用函数，以及实际共享核心分配器。覆盖 4096 项池满、4096 项拒绝、4096 个空 NBL、64 KiB 与 UINT32_MAX 输入、两队列切换、成功批量、多 NB 同 NBL 的窗口发布错误。还覆盖暂停/注销/故障/消费者回收发生在窗口释放时、控制回复实际重入另一发送，以及完成回调将 NBL 页改为 PAGE_NOACCESS 后无后续访问。
- 满池 4096 项的确定性操作计数由原先单锁 4096 次槽池扫描，变为最多 32 次尝试/窗口、总计 128 次槽池扫描，每个 TxLock 内仅一次失败扫描。正常 32 项批次仍批量发布。该结果不是内核 CPU 时间或吞吐测量。
- `fault_drain_tests.c` 使用真实故障 worker、NBL 分配/归还、completion、QUERY、Free 函数。覆盖满/损坏完成环、重复原始 token、故障后的敌对 producer、两个队列独立见证、延迟 NDIS 归还、Active 已为零但最后回调仍在 rundown 内、没有接收 NBL 的发送调用引用。真实宿主线程和事件验证两个释放屏障，结束时锁页、池、MDL、NBL、事件引用、线程句柄和进程句柄差值均归零。Safety 还以保护页分配执行相同故障收尾。
- `session_query_tests.c` 在首次引用快照后注入 worker 留存与见证发布，验证最终重新读取引用；还覆盖队列检查期间发生故障、共享 producer 不可信和并发注销等待快照。
- `lifecycle_tests.c` 提交一个 Published 并保留一个合法 lease，模拟仅内核故障且不交付完成。先确认 lease 与原生引用各自阻止收尾，再确认真实 STOPPED 后可以清退 Published，且 DLL Fatal 没有被依赖。
- 原有 100000 次并发引用、六种故障/事件 worker 时序、暂停完成进度、所有资源取得前缀回滚、DLL/Rust 故障归还与异步卸载继续纳入验证。

统一复现入口（PowerShell 7）为 `tools/audit/validate-dll-lifecycle.ps1`，沿用其名称。它冻结一个源码指纹，依次运行 Tests、Safety、Driver、Probe、Api、Legacy、AnalyzeDll、AnalyzeSys、Benchmark；任一失败或期间源文件变化都不记为通过。证据写入 `out/dll-lifecycle-validation/<指纹前12位>/validation.json`，测试数量和退出码来自实际执行记录。SYS 静态分析的所有权移交诊断仍须对照对应析构路径人工核对，不能只凭编译退出码宣称无问题。

本轮只做离线构建和宿主验证，未安装、签名或加载 SYS。真实 NDIS pause/restart/Halt、低资源、Verifier、PoolMon、ETW/WPA、过滤驱动组合与长稳验收仍未完成，清单继续标记 `kernel_runtime_tested=false` 和 `production_ready=false`。
