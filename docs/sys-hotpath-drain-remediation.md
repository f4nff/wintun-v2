# SYS 热路径与排空修正

后续空接收路径及未启用合并类型的重复工作修正见 [空接收与合并开销](sys-empty-receive-coalesce-cost.md)。

本轮起始源码指纹为 `0112f76b2c1a13f3376afe946ad5b080fb03a8c1a669c3effd6d09ec09c47420`。修正前完整源码快照保存在 `out/sys-hotpath-baseline/0112f76b2c1a`。审查针对 x64 `PACKETBRIDGE_PROBE` 活动 SYS、相关共享核心和宿主回归；没有安装、签名或加载驱动。

## 1 正常 DRAIN 的工作集合没有固定边界

**P2，原始 ABI 生产者可不断延长排空，同时持续消耗 CPU。** 原来 ReceiveBatch 在 Draining 后仍读取共享 producer 的当前值，QUERY 也用同一个可变值判断排空。若生产者在每轮消费后继续提交，worker 会不断处理停止后的描述符；即使没有原生 NDIS 引用，也可能一直不返回 STOPPED。

保存的原函数复现中，初始三条合法描述符，生产者在每次 receive 后追加一条，并持续消费 PB_STOPPED completion 以排除完成环背压。八轮后已完成十条、未进入 idle wait、原生引用为零，QUERY 仍为 QUARANTINED。八轮是测试主动终止界限，实际旧循环没有该界限。空初始队列和 uint64 游标回绕、暂停状态也能复现。该复现使用原始 ABI，不表示正常 C API 在 BeginStop 后必然继续提交。

修正将停止接纳、有限排空和实际所有权归还分别约束：

1. DRAIN 在 TransitionLock 排他保护下，为每个队列保存经过核心计数校验的 RX producer 边界。RxLock 同时保护快照和 worker 的 arm/recheck，避免两个上下文并发修改私有 consumer view 的 PeerCursor。
2. 全部队列捕获成功后才发布 DrainSnapshotReady 和 Draining。部分捕获失败转入原有 Faulted 路径，必须等待各 worker 的私有收尾见证，不能使用半份快照确认排空。重复 DRAIN 不更新边界和期限。
3. ReceiveBatch 同时限制额度、每批描述符/字节预算和距边界的剩余量。正常运行期间仍使用原有实时 producer；停止后不会消费边界之外的提交。
4. worker 的停止后 ready 判断使用私有边界；边界已经耗尽时不因后来共享 producer 增长而自旋。若额度在一次 receive 返回与准备睡眠之间归还，仍在同一 RxLock 下重新检查有限工作与额度；真实归还和完成消费者的唤醒继续有效。
5. 正常 QUERY 根据全部快照已发布、私有边界已处理、没有 credit 以及原生引用为零判断 STOPPED，不再被后续共享 producer 写入延长。故障 QUERY 继续采用独立的 worker 见证。

修正后同一复现只完成初始三条，两个 receive 回合后进入一次 idle wait，并报告 STOPPED；空初始队列只执行一个回合。停止后的额外共享写入不撤销这个私有结论。与停止并发的晚到提交不再交给 NDIS，DLL 遗留的 Published 仍通过既有可信 STOPPED 记账路径清退。真实 NDIS 引用、未交付完成的 credit、最后回调尾部、线程 join 和 Device Close 屏障仍保留；没有通过期限到达或共享提示提前释放。

## 2 批量归还逐 NBL 重复执行同步与唤醒

此前 NDIS 归还一条链时，回调逐 NBL 执行 RxLock、完成刷新、统计、ReferenceLock、rundown 和 KernelEvent。当前按连续同队列窗口归还，单窗口最多处理 **32 个原始描述符**，同时不超过 32 个 NBL。两个各含 32 描述符的合并 NBL 会拆成两个窗口；切换队列或遇到非 V2 tag 时交回第一个未处理节点。

`session_return.h` 集中实现窗口归还；单对象的本地取消仍通过 limit=1 的同一实现。每个窗口在释放任何对象和引用前取得会话 rundown，逐个释放真实归还 NBL/MDL，再在一个有限 RxLock 区间内更新私有槽与 completion。记录和合并缓存释放后，统一更新统计、在 ReferenceLock 下递减该窗口的 NBL 引用数并唤醒所属 worker。rundown 始终覆盖最后通知与返回尾部。

错误统计仍按失败 NBL 计数，成功包数与字节数仍按原始描述符统计；混合成功/失败不会被合并为一个状态。每个队列的 completion 顺序和 token 保持不变。没有新增返回队列、后台工作、堆分配或预分配缓存，原有逐对象分配/释放成本仍存在。

确定性宿主 DDI 操作计数如下，不代表内核时间或吞吐：

| 一次归还的普通 NBL | 修正前锁获取 | 修正后锁获取 | 修正前会话 rundown 获取/worker 唤醒 | 修正后 |
| --- | ---: | ---: | ---: | ---: |
| 1，同队列 | 2 | 2 | 各 1 | 各 1 |
| 32，同队列 | 64 | 2 | 各 32 | 各 1 |
| 64，同队列 | 128 | 4 | 各 64 | 各 2 |

每个窗口仍检查真实 credit 所有权。满完成环时保留 pending/credit 并等待消费者；故障时只清退已完成的私有记账。最后窗口将 Active 减到零后，清理仍须等待 Returns rundown，不能因统计已归零而解锁父 MDL。

## 3 回归与维护边界

- `return_batch_tests.c` 执行生产分配、归还、完成与引用函数，覆盖单对象、32/64 普通对象、交错双队列、16/32 描述符合并对象、混合状态、外来 tag 在链首/链中、满完成环和故障归还。两条真实宿主线程先同时取得 rundown，再归还 64 个互不重复的 token，验证统计、唯一完成、最后引用及资源/句柄平衡。
- `receive_reference_tests.c` 执行二十万轮并发留存，分别混用逐个释放和批量释放，检查 Active 与 AllReturned 在引用锁内一致。
- `fault_drain_tests.c` 同时覆盖单对象和窗口归还的尾部：在 Active/credit 已归零但回调仍持有 rundown 时发起真实清理函数，确认父 MDL 仍锁页；回调退出后才释放。
- `drain_boundary_tests.c` 执行实际 BeginDrain、ReceiveBatch、worker 和 QUERY，覆盖持续追加、空队列、回绕、暂停、多队列、重复 DRAIN、部分快照损坏，以及额度在准备 idle 前按窗口归还。尚未归还的真实 NBL 引用继续阻止 STOPPED。快照和 RX/TX arm 的宿主替身检查对应队列锁已持有。
- Safety 对两组新路径以及故障退出使用保护页分配；既有所有获取前缀回滚、故障/事件时序、暂停完成进度、DLL 生命周期和 Rust FFI 回归继续执行。

为直接测试生产接收函数，将其从 session.h 提取到 session_receive.h；功能变化只在排空额度限定。BeginDrain、接收工作量与 arm 的停止规则集中于 session_drain.h。内核私有结构增加边界字段；共享布局、函数表、ABI 版本不变，注册仍校验相同源码指纹。

## 4 复现与验收

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\build\build.ps1 -Stage Safety
.\tools\audit\verify-sys-hotpath.ps1 -BaselineDirectory out\sys-hotpath-baseline\0112f76b2c1a
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 入口使用冻结的旧归还/接收/worker/QUERY 函数体和当前生产函数，配合同一宿主 DDI 与核心，分别验证操作次数和有限进展；不是对旧、新真实驱动的性能对比。日志、实际执行退出码、二进制和源码哈希写入 `out/sys-hotpath-validation/<当前指纹前12位>`。完整离线入口在同一冻结指纹运行构建、Tests、Safety、旧分支编译、SYS/DLL 静态分析、A/B 和 DLL 基准；失败或期间源码变化不能标记通过。SkipBenchmark 会跳过依赖保存基线的 A/B/基准工作。

本轮没有以宿主操作计数代替 ETW/WPA、实际 IRQL、NDIS/LWF 调度、Verifier、PoolMon、低资源和长稳验收。每包 record、两个 MDL、NBL 的分配释放，以及合并复制、校验和统计缓存行争用仍是需要实测的候选成本。外部 indication/completion 回调或真实 NDIS 归还永久停滞时，必要的退出屏障仍可能持续等待；没有用超时强行释放这些所有权。清单继续保留 `kernel_runtime_tested=false` 和 `production_ready=false`。
