# SYS 第三轮并发、资源与 CPU 审查

后续补充见 [第四轮审查](sys-concurrency-resource-audit-round4.md)：修正暂停状态的 completion 进度，给刷新设置预算并验证继续调度，优化单队列选择路径。本文保留第三轮结果。

日期：2026-10-28。当前私有 PacketBridgeV2 x64 路径，版本 1.0.1，版权范围截至 2026。复核 `driver/wintun.c`、`driver/v2`、链接的 `common`，并检查直接影响故障退出的 `api/v2/session.c`。实际 Rust VPN 项目未修改。SYS、DLL、CAT 均保持未签名，没有安装或加载驱动。

本轮发现一个 SYS 故障丢唤醒竞态、两个 DLL 故障退出缺口，以及一个 SYS 接收元数据错误，均已修正。四条新回归分别在对应修复前失败，修复后通过。没有发现新的确定性内存或句柄泄漏；发现的线程滞留有具体的事件时序，不等同于已证实的永久线程资源泄漏。不能据离线结果宣称真实内核环境已无竞态、无泄漏。

## 已确认的问题

| 编号 | 范围/级别 | 修改前时序与影响 | 修正与回归 |
| --- | --- | --- | --- |
| C1 | SYS / P1 | Worker 先读 Faulted=false，另一线程设 Faulted 并发出唯一一次 KernelEvent 通知，Worker 随后 KeClearEvent 清掉通知。如果后续队列为空或已损坏，最终会在未置位事件上等待；反复调用 PbV2Fault 也不会再次通知，因为它采用一次性 CAS | 先清事件，再读取 Faulted。故障在 clear 前发生由状态检查捕获，clear 后发生保留事件通知。把实际 Worker 函数提到 session_worker.h 供直接测试，六种确定性时序覆盖空闲、清除前/后、接收中、两个 arm 阶段。未修复版本失败于 LostFaultWaits；当前通过 |
| C2 | DLL / P1 | 首次 Wait 收到内核故障事件并查询到停止，但未保存终止状态；同一队列再次 Wait(INFINITE) 时，一次性 UserEvent 已被消耗，可能永远等待。Reserve/Commit 也仍可能向已停止 Worker 提交更多工作 | 识别到非 RUNNING 状态后设置原子的 CloseRequested，并置位全会话的手动复位 ClosingEvent。后续 Wait/Reserve/Commit 立即停止，Release/Cancel 等清理操作继续走既有路径。测试由同一队列线程连续调用八次无限等待，确认全部返回 STOPPED，随后 Reserve 也返回 STOPPED。修改前线程未在测试时限内结束 |
| C3 | DLL / P1 | 某队列的 QueueLeave 发现 PB_CORRUPT 只设置会话 Fatal。其他队列若已经进入无限等待，不会再次进入 QueueEnter 检查 Fatal，也收不到对应 UserEvent，可能长期滞留 | 同时设置 CloseRequested 和 ClosingEvent，广播给所有已进入的等待者。双队列测试让队列 0 先无限等待，再让队列 1 读取错误 epoch 描述符触发 Fatal；无需任何内核故障通知，队列 0 即退出。修改前同样超时失败 |
| C4 | SYS / P2 | RSC/URO 构造统一设置 IpChecksumSucceeded，包含没有 IP 头校验和的 IPv6。该标记表达的“验证 IP 头校验和成功”与数据不符 | 仅 IPv4 设置该位；IPv6 仍保留对应 TCP/UDP 成功位，IPv4 URO 的 IpChecksumValueInvalid 保持既有处理。直接执行实际 PbV2Receive 的新测试先复现 IPv6 位错误，再验证修复后值 |

C2/C3 属于 SYS 故障或数据损坏后的 DLL 衔接问题，不把它们算成额外的 SYS 内存竞态。C1 在最终注销时仍可被 StopEvent 唤醒，因此修改前是故障后的线程滞留风险，并非已经证明无法回收的线程泄漏。

修改前源码快照位于 `out/audit3-baseline/1c50c2cebdbf`。对应上一轮的完整指纹为 `1c50c2cebdbfeed519c4cfb40a11f58db2165595e4659d784691cecc7b7d7237`。`out/audit3-repro` 保留 worker-before、api-before、api-before-broadcast、nbl-before 测试程序及失败日志；这些都是宿主回归程序，没有执行真实内核代码。旧路径的 NBL 标记是在拆出函数供测试后、修正标记前复现的。

## 资源与退出路径补查

前两轮的 76 个获取前缀主要覆盖会话注册清理。本轮补上逐包的实际分配/归还函数，而不是把这 76 项当作已覆盖每次 RX 热路径。

为了直接执行生产逻辑，将原有连续函数提取到 `session_nbl.h`、`session_completion.h`、`session_worker.h`，由同一 SYS 翻译单元包含；没有引入第二份替代实现。宿主测试提供 DDI 替身，追踪实际函数取得和释放的资源。

新增 `receive_nbl_tests.c` 的 48 个场景：

- 普通共享 payload 与私有合并 payload；IPv4/IPv6；TCP/UDP 元数据。
- record、payload MDL、Ethernet 头 MDL、NBL 四个获取失败点，逐个验证已取得资源全部回滚。
- 成功与错误归还，检查原始描述符只完成一次、slot Active 清除、credit/pending 归零、合并 buffer Busy 清除。
- 普通 payload 的 partial MDL 范围、私有 payload 的非分页 MDL、旧 OOB 清除、协议 flags 与 checksum flags。
- 成功路径直接调用实际 PbV2ReturnOne 与实际 completion flush，检查 NBL、两个 MDL、record 全部释放，返回 rundown 归零、AllReturned 置位、统计与对象数匹配。

48 个场景结束后追踪资源及宿主进程句柄差值为零。保留的会话清理测试继续验证锁页、映射失败、事件别名引用、8 个工作线程 join，以及 AllReturned 已置位但最后返回回调仍未退出 rundown 的时序。

所有权复核仍成立：

| 对象 | 本轮复核 |
| --- | --- |
| Ctx 与控制请求 | 查找在短全局锁内取得 ControlRundown 引用；Halt 撤销入口后在锁外等引用，随后才能注销和释放 |
| 未发布 NBL 链 | 仅 Worker 持有；构造期间 TransitionLock 仍为共享持有。注销要先排他 detach 并 join Worker，Pause 要先排他设置 Running=false，不会在半构造状态提前释放 |
| 已发布 NBL | 在任何 indication 或本地取消前统一登记引用。归还后释放 record/MDL/NBL、发布唯一 completion，再递减 Active；最后尾部仍由 Returns rundown 保护 |
| 系统线程 HANDLE | 注册失败与注销均先唤醒 StopEvent/StartEvent、等待退出、ZwClose；C1 修复补齐故障时的自主退出，不新增线程或句柄 |
| 事件引用、父 MDL 与锁页 | 仍在工作线程和返回回调完成后统一释放；本轮不改变所有者 |

SYS 为 C 代码，没有 Rust/Tokio 协程。本轮核对系统线程和回调生命周期，没有对 VPN 全部异步任务做完整审计。

## 已实施的 CPU 改动

1. **批量登记接收引用。** 此前每构造一个 NBL，都会获取 ReferenceLock、增加 Active、清除 AllReturned 并更新 RxObjects。现在构造完整链后，在发布前统一登记。满批 32 个独立 NBL 时，ReferenceLock 获取、Active 原子加、AllReturned 清除、RxObjects 原子加分别由 32 次减为 1 次。合并 NBL 的引用数与原始对象数分别累计，不能混为一个计数。
2. **跳过归还入口的零统计。** 私有 v2 NBL 已在 PbV2ReturnOne 更新统计并 continue，外层历史路径仍对四项计数执行原子加零。现在只在存在实际历史路径包/错误时更新，纯 v2 返回回调省去这四次原子操作；逐 NBL 的有效统计仍保留。

`receive_reference_tests.c` 直接包含实际引用辅助函数：验证 32 个 NBL 一次登记、逐个归还直到最后才置位 AllReturned、一个合并 NBL 对应 32 个原始对象，以及两个线程共十万轮并发登记/归还。每次在引用锁内核对 Active 与事件状态，最后引用为零、事件置位、对象统计准确、句柄差值为零。

批量登记的时序要求已经逐路径核对：构造阶段没有向 NDIS 暴露链；登记发生在释放 TransitionLock 之前；即使随后转为本地取消，也必须先登记再走同一归还函数；零个成功 NBL 不登记。这里没有减少返回回调的 rundown，也没有把 AllReturned 的状态更新移出引用锁。

这些是代码路径上确定的调用次数减少，没有真实内核 CPU/pps/吞吐测试，**本轮不提供吞吐提升百分比**。第二轮的池分配器微基准只证明其自身场景，不作为本轮 NDIS 引用优化的性能证据。

仍需采样确认的候选：逐 NBL record/MDL/NBL 获取与释放、按队列归并返回后统一 flush/wake、RSC/URO 复制和 checksum、ReferenceLock/IndicationLock/统计缓存行竞争、共享 ring 的强原子操作。`PbV2FlushLocked` 目前受 PendingCount/credit 上限约束，但最坏可在一次 RxLock 持有期间推进较多 completion；如 ETW 证明持锁时间较长，再加小批预算并验证剩余 pending 的唤醒与进度。不能仅为降低调用数而提前重用 MDL 或省略 Armed 的同步握手。

## 验证与边界

新增六个 Worker 调度场景、十万轮并发引用测试、48 个逐包资源场景、重复内核停止等待和双队列故障广播均通过。原有分段 13 项、合并 8 项、Ethernet 6 项、共享核心/百万并发描述符/十万所有权循环、76 个注册清理前缀、上下文和 indication gate、查询并发 detach、C API 与 Rust 原生 FFI/布局/编译失败测试继续通过。

7 个内核翻译单元重新运行 MSVC /analyze:only。仍为 57 条诊断：49 条在 SDK/WDK，项目 8 条为 C28197，分别是会话、所属五类队列缓冲区、Ctx、NBL pool 的成功移交路径；已再次核对后续释放出口。没有新增其他项目诊断，也没有全局禁用告警。这不是静态分析零告警。

PowerShell 7 复现：

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\audit\analyze-sys.ps1
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
```

使用同一指纹的输出，保留旧输出。开发 DLL 不内嵌驱动；最终嵌入包继续等待以后签名完成后生成。本轮不执行签名。

宿主 DDI 替身不模拟真实 IRQL、NDIS/LWF 调度、系统 PTE、内核池实现。签名后仍需在可恢复测试机运行 Verifier/PoolMon/ETW/WPA，以及并发 register/drain/close/Halt、进程强退、多适配器、动态 OID、pause/restart、低资源、延迟/乱序归还和长稳测试。若 NDIS 永久不归还 NBL，安全清理仍必须等待；故障隔离也可能有意保留尚无法证明可回收的资源。不能强行释放以制造“计数归零”。

契约参考：[事件的自动/手动复位及 KeClearEvent](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/defining-and-using-an-event-object)、[NDIS checksum OOB 各标记的含义](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/nblchecksum/ns-nblchecksum-ndis_tcp_ip_checksum_net_buffer_list_info)。故障时序与项目结论来自本地源码和回归，不是微软对本项目的认证。
