# 满槽恢复、关闭等待失败与双向批次析构

版本保持 **2.0.0**。本轮从已验收的 `114acbbb18b0b63fcb4b738d6eb1525547661c633ed56ada614ae175572147c8`、771 文件完整基线继续排查。未确认新的生产缺陷；运行时代码、ABI 和按系统能力启用功能的规则保持。新增场景复用现有实际路径宿主和构建入口，避免维护重复 fixture。

## 同一次发送中的合法晚归还

[Send/回收测试](../tests/model/recycle_send32_tests.c)的 `--late-pressure32` 使用 16 个槽位和 32 项环，处理一条包含 32 个有效 TCP/UDP 原始 NBL 的链。前 16 项自然占满池，第 17 项实际遇到资源不足；原发送路径发布已准备的 16 项并释放锁。独立 peer 在这个交接窗口实际消费和归还，后续发送继续经过原 Read、Describe、Reserve 和 publication。

合法恢复应成功 31 项，只拒绝已失败的第 17 项，不等待、重试或重放该原始 NBL；后 15 项自然使用 generation2。Pause 对照在同一已解锁窗口改变运行状态，后 15 项不得再进入 Describe 或复制载荷。测试分别检查批次向量、分类前缀与完整载荷读取、原始包状态、槽位代际和终止清理。共享环和私有账本不被预填成假的 FULL，也不通过修改计数恢复资源。

正常分支应只有两个 publication，批次为 16+15，载荷读取 31 次、46500 字节，分类前缀 32 次、1888 字节。Pause 分支只发布一次 16 项，载荷读取 16 次、24000 字节，分类前缀 17 次、1003 字节。Pause 仅在真实 Running0/AllReturned 等待点放行 sender，避免测试门等待整个 Pause 而阻塞仍持有 Active 引用的发送者。所有 sender、peer、pauser 退出后，剩余真实回收 token 通过原 TxLock 和实际 recycler 归还，不修补池状态。

本组 worker 只等待终止事件并由原 Free 汇合，不执行真实 worker loop；接收/worker 的独立旧回归继续保留。交接门仅建模宿主调度，不表示可以在原生 spinlock 或相应 IRQL 下阻塞。

## 关闭等待失败后保留已提交所有权

[跨队列关闭测试](../tests/model/cross_queue_prepare32_tests.c)的 `<fixture.dll> --close-wait-failure32` 用原 owner 实际 Reserve/Commit32，形成 32 项 Published 和独立 NativeActive 引用。仅在指定非最终 PrepareClose 调用者的 condition-variable 等待处，注入一次非超时错误。

调用必须立即返回 PB_CORRUPT，保留原错误、公有引用、32 项 Published、工作和映射；失败不能触发忙重试或授权提前释放。随后恢复原等待，peer 实际完成，原 owner Poll32 并退出，NativeActive 归零后可信 QUERY 才允许 Prepared；所有调用者汇合后执行唯一最终 Close，并验证回调线程汇合、资源归零和真实 DLL 卸载。

此错误是明确声明的防御性 SDK 边界输入，不表示发现正常 Windows 会自然产生该错误。默认旧矩阵继续保留，专项默认审计和 Safety/Unified 同时执行新增 CLI，避免测试存在却没有被日常验收调用。

## Fault 下两个仍有借用的 Rust 批次

[Rust 分族测试](../rust/packetbridge/src/native_tests.rs)先实际 Receive32、Reserve32，合法归还接收前缀 1 项并取消发送 1 项，留下两个各 31 项的批次。通过既有 PB_API_TESTS completion 导出输入错误 epoch，实际公开 Poll 校验失败后由原 FaultSession 进入终止状态；不直接改 Fatal 或修复任何私有计数。

PrepareClose0 在 62 项借用仍存活时必须 BUSY。分别先析构接收批或发送批；首批 Drop 后，另一批的指针、token、数据和长度仍有效，剩余借用为 31，再次准备关闭仍 BUSY。第二批 Drop 后借用为零，但 NativeActive1 仍使 PrepareClose0 返回 QUARANTINED 且保留映射。退出全部借用作用域并恢复原接口表后，NativeActive0 才允许准备和最终关闭。

IPv4/IPv6 各独立测试 TCP/UDP 和两种析构顺序，共 8 个协议场景。每场景一个 Receive32、一个 Reserve32；前缀操作和 Drop 合计两个 Release、两个 Cancel，各归还 32 项，没有 Commit 或分片 writer。记录的 FFI 次数不等于内部复制或 CPU 时间，测试不读取已转让前缀的指针。原完整 Rust 测试文件字节前缀保持不变，仅追加两项分族测试。

## 证据与验证范围

专项入口为 [发送回收审计](../tools/audit/verify-recycle-send32.ps1)、[关闭准备审计](../tools/audit/verify-cross-queue-prepare32.ps1)和完整 [离线验收](../tools/build/validate-unified.ps1)。IPv4/IPv6、ISO/MS、普通/守护内存分别执行；当前源码快照与显式完整历史基线分别认证。原矩阵、失败拒绝合同和原始生产正文绑定继续保留。本轮源码、原始记录和产物位于 `out/reclaim-progress-review/`。

未确认新的发送时间混用、普通同队列逐包提交或重复载荷复制缺陷。既有 opaque64 包标记、协议 TCP Timestamp、日志时间和本地等待时钟保持独立。宿主中的读取次数、批次次数、保护页、句柄平衡和线程汇合只证明已声明路径；本轮没有原生 CPU 性能测量。旧系统实机、真实 NDIS/IRQL、Driver Verifier 和持续内核资源压力仍需在相应系统验证。
