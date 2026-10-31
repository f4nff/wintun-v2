# SYS 接收批次统计与本地归还

后续事件状态转换和普通报文策略读取修正见 [SYS 事件转换与无 offload 报文策略读取](sys-event-policy-cost.md)。

本轮从 `1a1a72e2e21c10194c969b0f87f35c09db92c95c00b65d19952eddc4f8b2b7d9` 继续检查；冻结源码保存在 `out/sys-receive-batch-baseline/1a1a72e2e21c`。范围是实际 ReceiveBatch 的重复记录读取、统计原子更新和指示前撤销，没有安装、签名或加载 SYS。

## 指示统计重新遍历整条 NBL 链

原来 ReceiveBatch 在构造并登记整条链后，再次遍历每个 NBL，读取其私有 record 的 Count，决定是否增加 RSC/URO 统计。普通接收也经过这次记录读取；合并对象逐个执行三次 RSC 或两次 URO 原子更新。

现在在构造成功的合并对象旁，将同一 record/OOB 分类结果累计到本事务的 `PB_RX_INDICATION_STATS`，并且只有最终决定实际指示时才提交。普通对象不读取统计 record，不再为了统计遍历整条链。同一类 RSC/URO 计数每批各提交一次，数据类型和原有累加含义不变。仅保留数量和字节和，不留存额外 NBL 指针，没有缓存、堆分配或新的跨回调状态。两个内部 helper 强制内联，避免源码分离增加每对象调用。

统计仍基于成功构造对象的实际 record 与 NDIS OOB，未改成猜测报文协议。分配失败的对象不计入；构造中发生故障而未指示的链丢弃本地统计和，不更新指示探针。原来的统计字段也不是一次原子快照；此次改变批内增量的可见时点，完成整个批次后的累计值一致。统计提交后才调用 NDIS，上调同步归还整条链也不会使后续统计访问已释放对象。

目前每队列只有两个合并输出缓冲，同一尚未指示的批次最多持有两个合并 NBL。以下比较严格按该上限运行实际编排，不把模型中逐组立即释放输出的结果当作真实批次：

| 一个 32 描述符批次 | 原统计 record 读取 | 当前 | 原探针原子更新 | 当前 |
| --- | ---: | ---: | ---: | ---: |
| 全部普通对象 | 32 | 0 | 0 | 0 |
| 两个双包 TCP 合并对象，加 28 个普通对象 | 30 | 2 | 6 | 3 |
| 两个双包 UDP 合并对象，加 28 个普通对象 | 30 | 2 | 4 | 2 |
| 一个 TCP、一个 UDP 合并对象，加 28 个普通对象 | 30 | 2 | 5 | 5 |
| 32 包合为一个 TCP 对象 | 1 | 1 | 3 | 3 |

这些是实际宿主函数的操作计数，不是内核 CPU 时间。

## 构造期间的故障仍逐对象本地归还

已知停止发生在接纳后、构造前时，原有早期撤销路径已经避免创建 NBL。但其他线程可以在构造中标记故障。此时已经构造并登记引用的私有链不会上交 NDIS，原代码逐对象设置 PAUSED 并调用 ReturnOne，每个对象重复取得 RxLock、ReferenceLock、session rundown 并唤醒 worker。

当前先给该私有链设置 PAUSED，再使用已有 `PbV2ReturnNbls` 窗口归还。链中所有对象来自同一队列，本批原始描述符总数最多 PB_MAX_BATCH，所以整条链符合一个窗口的对象预算；调用仍采用该预算，异常外来 tag 不会造成无进展自旋。无需另建一套释放和记账函数。每个 MDL、NBL、record、槽位、credit 和合并输出的释放继续经过原有归还路径；Active 减为零之后，通知和最后尾部仍在 Returns rundown 保护中。

32 个普通对象构造期间发生故障的宿主结果：归还阶段的锁获取由 64 次降为 2 次，rundown 获取/释放对由 32 降为 1，归还引起的 worker 唤醒由 32 降为 1。日志中总 KernelEvent 设置为 33→2，额外一次是原始故障唤醒，未被删除。第二个 NBL 分配失败、只成功构造 31 个时，也按一个窗口归还。

worker 的故障见证、QUERY、AllReturned、线程 join、上调和 rundown 退出条件不变。没有其他原生引用时，本地链归还完成也不能替代 worker 的私有故障见证；已有 NDIS 接收对象时，那个对象继续持有槽位、credit、MDL、NBL 和 Active，真正归还后才允许 STOPPED。

## 回归和证据

扩展 `drain_boundary_tests.c`，执行实际接纳、coalescing、NBL 构造、ReceiveBatch、归还、worker 和 QUERY。新增覆盖：

- 普通、两组 TCP、两组 UDP、混合、单个最大描述符组；预置非零统计值后验证累加，直接统计 record 读取与五个探针字段的原子更新。
- 首组合并对象分配失败，验证其描述符返回 PB_FULL，而统计、引用和后续对象只包含成功构造的部分。
- 上调中同步归还全部 NBL，包括前缀分配失败情形；资源与槽位归零后，ReceiveBatch 继续完成最后刷新，不再访问已释放的对象。
- 首个 NBL 构造期间注入真实 Fault，验证普通链、第二个分配失败，以及混合 RSC/URO 链撤销；合并 busy 标记释放，未指示的统计和不提交。
- 另有一个先前已由 NDIS 持有的对象时，本批 32 个私有对象撤销后仍不能报告 STOPPED；该原生对象真正归还后才结束。

测试替身仅在分配边界注入故障，并统计同步操作；全程检查资源、引用、完成状态和句柄收支。普通回归与 Safety 都继续执行同一模型；scratch/合并输出始终使用保护页，Safety 另外将 record 池分配置于保护页。

```powershell
.\tools\audit\verify-sys-receive-batch.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 使用冻结旧 ReceiveBatch 函数体与当前函数，配合同一当前核心和宿主模型，分别运行普通及保护页版本；记录实际退出码、计数和源码/二进制哈希，写入 `out/sys-receive-batch/<当前指纹前12位>`。统一离线验证增加 SysReceiveBatch；依赖本地历史快照的六项 A/B 与 benchmark 继续由 SkipBenchmark 控制。

本轮没有确认新的无限等待。实际内核 IRQL、NDIS 并发、Verifier、PoolMon、ETW/WPA、低资源和长稳仍待验收；计数不能换算为驱动 CPU 百分比、吞吐或延迟，清单继续保留 `kernel_runtime_tested=false` 与 `production_ready=false`。
