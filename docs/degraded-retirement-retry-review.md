# 可选资源降级与归还重试复核

版本保持 **2.0.0**。本轮以完整认证的 `a0b750b05183a7075317cc2c1dcd2458770edc937d7c780f2b0814785eb0dc19`、764 个源文件为基线，继续检查 32 包的降级、归还、停机和重用。未确认新的生产缺陷；新增测试不修改运行时代码、公开 ABI 或系统能力门控。

## 可选合并分配失败后仍执行完整接收路径

[可选资源降级测试](../tests/model/optional_coalesce32_tests.c)在原 ready factory 中调用原宿主分配器，通过其失败点自然返回 NULL，分别验证 Scratch 缺失、buffer0 缺失、buffer1 缺失和两个 buffer 都缺失。每次实际接收 32 个有效 TCP/UDP 包，两条流各 16 包。

Scratch 缺失或没有合并缓冲区时，应正常上调 32 个普通 NBL。只剩一个缓冲区时，仍应保留可用合并能力：一个 16 包 SCU 加 16 个普通 NBL，共 17 个 NBL；buffer0 缺失时实际使用 buffer1。测试检查仍由 MDL 持有的载荷、原 token、OOB、完成记录和统计，并验证原 Pause、外层 Return、Unregister/Free 和 worker 汇合。

成功分配的每个可选指针必须只释放一次，失败的 NULL 指针不得释放。旧 NDIS 6.20 普通路径单独对照；现代 TCP 6.30 RSC、UDP 6.89 URO 只在相应能力下启用，没有为了测试在旧能力上强开合并。

本窄组执行 ready factory 后的实际数据和清理函数，不冒充重新执行完整 Register。注册入口接受可选分配失败的行为由既有 RegisterOwner 测试另行覆盖，独立记录其来源。宿主 DDI/调度门不是原生内核 IRQL 或真实内存压力证明。

## 末项无效的归还整批拒绝后，在停机状态下修正

[归还重试测试](../tests/model/retire_retry32_tests.c)使用实际 DLL 的唯一合法队列 owner，分别持有 32 个已接收 loan 或 32 个尚未提交的 reservation。在独立复制的 token 数组中，只让最后一项 epoch 错误或重复首项，调用原 ReleaseBatch/CancelBatch。

实际 token 校验已返回 STALE/INVALID、尚未离开 DataGate 时暂停 owner，再发起 BeginStop。生命周期的独占获取必须先确实 TryAcquire 失败，再转发真实获取锁；不能在公开调用仍处理错误时越过私有账本。错误返回后，原 pool、free bits、slot 状态、32 个 loan/reservation 及 ring 内容保持，Fatal 不得因正常调用参数错误置位。Release 的 accepted 为 0；Cancel 没有 accepted 输出参数。

停机不撤销合法归还权。原 owner 只修正复制数组的最后一项，再实际 Release32 或 Cancel32；所有调用者退出后，继续经过独立 NativeActive 与可信 QUERY 屏障，最后关闭、汇合全部捕获回调并验证真实 DLL 卸载。未制造正常运行时不可达的 FULL，也未修改私有引用或生产计数。

## Rust 前缀归还与新旧代际并存

[Rust 分族测试](../rust/packetbridge/src/native_tests.rs)先实际 Receive32，使用独立复制的坏 token 数组通过 unsafe raw ABI 调用真实 C：错误 epoch 和重复槽位都必须整批拒绝、accepted=0。Rust 仍持有原 32 个借用，全部指针、metadata 和 wire 不变，回收环尚未发布。

随后合法 `release_prefix(31)` 让原尾项重定位到 index0。宿主 peer 实际消费这 31 个回收 token，再自然预留 generation2 并写入 31 个新包；只读观察回收环从 0/0 到 31/0、31/31。按槽位和唯一性核对新借用与旧地址，不要求未承诺的槽位返回顺序，原尾项保持完整。最终旧尾 1 项和新 31 项分别批量归还，回收环为 63/31、loan 为零，再关闭。

错误输入只存在于复制的 token 数组，不修改 Rust 借用元数据，也不读取已转让的 generation1 前缀。环观察是受控、无并发写者的 PB_API_TESTS shim 边界，不是生产 Rust API 或真实 SYS。TLS 计数器转发原函数；每协议两个 Receive、五个 Release 调用，其中两次失败、成功归还共 63 项。没有前台 Reserve/Commit/Cancel/分片 writer 调用，也未将 FFI 次数当作内部复制或 CPU 测量。

## 验证与范围

IPv4/IPv6 分别编译和运行；C 专项覆盖 ISO/MS、普通/守护内存。原始宿主、生产正文和完整依赖摘要保留，有限观察只转发原操作。默认专项独立保存当前来源；显式历史模式完整认证 764 文件的 typed Ordinal 身份并要求生产依赖逐字节一致。Rust 仅在原完整测试文件之后追加两项测试，由既有分族与完整 Cargo 验收自动收录。

入口为 [可选资源审计](../tools/audit/verify-optional-coalesce32.ps1)、[归还重试审计](../tools/audit/verify-retire-retry32.ps1)和完整 [validate-unified.ps1](../tools/build/validate-unified.ps1)。原始记录、源码快照和产物身份位于 `out/retirement-failure-review/`。

本轮未确认新的 CPU 热点、发送时间混用、普通同队列逐包提交或重复载荷复制问题。必要的 canonical 校验和既有跨队列锁、顺序、栈预算约束保留。宿主资源账本、保护页、句柄平衡、线程汇合与 DLL 卸载只证明已声明路径；旧系统实机、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及原生性能仍需相应系统验证。
