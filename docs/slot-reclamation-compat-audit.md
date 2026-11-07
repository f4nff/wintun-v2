# SYS/DLL 槽位归还与兼容层审计

版本固定为 **2.0.0**，PE 文件版本 **2.0.0.0**。继续发布一个 `wintun.dll` 和一套 `packetbridge.sys/inf/cat`；旧 Wintun 导出、序号、指针生命周期及错误约定不变。v2 函数表/数据 ABI 仍为 2.1，packet 扩展为 1.1，lifecycle 为 1。

本文记录首轮槽位修复。后续复核另发现并修复 MDL 映射参数的旧系统兼容性遗漏，详见 [MDL 兼容性与回收预算复核](slot-reclamation-mdl-review.md)。

## 已确认并修复

`api/v2/session_queue.h` 的 `ReleaseBatch` 原先在发布 recycle Producer 后，再次读取调用方 token 数组来退还 DLL 私有槽位。调用方可以把该数组放在本次归还的 payload 内；Producer 发布后，对端立即重用并覆盖数据完全合法。旧实现会返回成功并扣除 loan，却未退还被覆盖 token 对应的私有槽位，下一代报文到达时误判损坏。

修复在完整输入长度检查后，将最多 64 个 token 复制到私有栈数组。验证、发布、前缀退还均使用该快照，不再读取已移交所有权的 payload。不更改共享结构、描述符格式、队列容量或发布屏障。代价为每次归还最多 1024 字节的有限用户态复制；正确性优先于省去这次复制。`CommitBatch` 已有私有 token/descriptor 快照，`CancelBatch` 没有向对端移交数据所有权，不需要同类修改。

旧兼容层 worker 每包调用一次 Reserve/Commit、Receive/Release，并逐包设置可读事件。现在通过 `api/compat_pump.h` 统一批处理，每方向每轮最多 64 个对象，复制预算 512 KiB。仍由同一个已 join 的线程独占 native queue，不新增线程、协程、队列或定时轮询。

发送仅发布已提交的 legacy 前缀，所有未提交 reservation 都显式取消；零长度包继续丢弃。接收记录 Count/Copied/Retired，只有复制到私有字节 ring 后才归还 native loan，部分归还不会重复复制。停机先停止 native 接纳，再有限次退还所有已持有 loan；不能假设一次成功调用接受整个数组。满 byte ring 时继续使用原有外部事件等待，收到连续前缀归还才恢复，不因尚有 native ready 数据而忙等。

`WintunReleaseReceivePacket` 只在连续 Head 前进时发布并唤醒 worker。倒序归还中间包仍记录 release bit，但不发送不能释放空间的通知。最后缺失前缀归还时一次公布完整空间，保留并发和乱序语义。可读事件仍是“检查是否可读”的条件提示，不是每包一个计数信号；调用方照常读取到 `ERROR_NO_MORE_ITEMS`。

## 保留的内核约束

首轮没有确认需要修改的 SYS 回收或唤醒缺陷，其所有权和同步约束继续保留；后续 SYS 映射兼容性修复见上方链接。

- `PbV2RecycleLocked` 在 TxLock 下校验 token 后退还槽位，只在真实退还成功后清除池满缓存。失败前的合法前缀已退还，后续损坏 token 触发故障，不能再接纳新工作。单次最多四批。
- 合法布局满足 ring capacity ≥ slot count。正常 TX recycle 占用、尚持有的 loan 和 ready 对象共享同一有限槽位总量，不能据此添加逐次“recycle 空间可用”通知。伪造游标、重复/过期 token 仍走故障路径。
- NBL/MDL 在释放合并缓冲区租约前销毁。完成项元数据来自私有 record；不能在发布后继续读取已允许重用的 payload。
- Active 归零不代表回调尾部结束。物理回收依次 join worker、等待 AllReturned、等待 Returns rundown，最后释放事件、映射、锁定页和 MDL。QUERY 的 STOPPED 不能代替这些屏障。
- 暂停停止接纳但不停止退还；worker 先清事件、检查故障、回收，再按状态 arm/recheck。完成环满时等待 DLL 消费释放空间，不定时自唤醒。
- `PollInternal` 每次实际消费完成项后仍通知内核。Pop 前看到“未满”不能排除并发 producer 随后填满并入睡的交错，不能据该快照省去通知。NDIS return 的 credit 满转可用通知仍受同一 RxLock 保护。

这些约束沿用 `return_reclaim_tests`、`pause_wake_tests`、`worker_poll_tests`、`completion_retire_tests`、故障/排空/池代际及生命周期回归。真实 DDI 由宿主替身提供，不能把通过宿主测试等同真实内核/IRQL 验收。

## 回归与测量

`Tests` 自动包含 `UnifiedTests`，`Safety` 也加入本轮两个实际路径回归：

- `return_alias_tests.c` 精确地在 Producer 发布后消费归还 token、覆盖原 payload 并重新发布下一代描述符，覆盖批宽 1～64。旧源码必须复现私有槽位未释放及下一代误报损坏；当前源码必须正确释放并继续接收，进程句柄增量为零。
- `compat_session_tests.c` / `compat_pump_cases.h` 直接执行生产兼容层：64 包双向 burst，前缀 0～8、Reserve/Commit/Release 部分成功、未提交取消及失败、零长度、非法长度/lease、字节预算、满环真实事件等待、逐包唤醒恢复、停止时持有 63 个 loan，以及停止后每次仅接受一个归还。并保留四发送者/四接收者、最大 IP 包、回绕、每个构造失败点、native fault、延迟引用、100 次重复启动关闭和私有资源归零检查。

确定性 64 包 burst 中，Reserve/Commit/Release 和可读事件分别从各 64 次变为各 1 次；倒序归还的 worker 通知从 64 次变为 1 次。这些是操作计数，CPU 结论另由实际测量提供。

```powershell
pwsh -NoProfile -File tools/audit/verify-reclamation.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

审计脚本默认使用本轮修改前完整冻结副本 `out/slot-reclamation-baseline/ba49741c551c`，校验三个实际基线文件哈希。在 ISO/MS volatile 两种模式执行旧/新回归，随后进行七轮交替对照并保存原始 JSON、源码/二进制哈希和操作日志。冻结副本不属于发布产物；缺失时脚本明确失败，不能用当前源码充当旧基线。

兼容层成本测试使用双方相同的当前 native 实现、未经包装的 API 表、真实 Windows 锁/事件、1500 字节载荷、64 包 burst，每轮每方向 640000 包，核对所有载荷。调用线程与 owner 线程固定到前两个允许的逻辑 CPU；只有一个可用 CPU 时同核运行。成本记录 owner 的线程周期与 CPU 时间；CPU 时间受 Windows 计时粒度影响，比较主要使用七轮周期中位数。DLL return 微基准单独比较增加快照的实际队列函数，批宽 1～64；正确性修复可能有小幅 CPU 代价，不对性能数值设脆弱的单元测试门槛。

完整离线验收检查 Win7 SP1 x64 工程目标/PE 导入、20 个导出及旧序号、内嵌包哈希、81 个版本边界的 NDIS 结构和能力 gating、新系统 RSC/USO/URO 路径、Rust FFI、故障注入、守护内存和静态分析。原有能力策略不变，旧系统不访问不存在的扩展字段，新系统能力不因本轮修改降级。

没有安装或加载驱动。所有报告保留 `kernel_runtime_tested=false`、`production_ready=false`。各目标系统签名安装、真实 NDIS 卸载协商、Driver Verifier、休眠/恢复/拔除、低资源和长时间内核池/句柄/线程计数，仍须在测试机完成；宿主 CPU 改善不代表真实内核吞吐或旧系统运行已经通过。
