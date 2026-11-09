# 构造回滚、完成消费与借用重试复核

版本保持 **2.0.0**。本轮完整保存并认证 `5f3f178fc376407e5e802821a813b70b499c9460a99591143a747c25a9f41441` 的 757 文件基线。未确认新的生产缺陷，继续保留已有修复；新增测试覆盖三个独立的所有权窗口，运行时代码、ABI 和系统能力门控保持不变。

## Active 为零时仍在构造或回滚

[接收回滚测试](../tests/model/receive_rollback32_tests.c)由实际 worker 接收 32 包，使用原宿主的第四个分配失败点，让真实 NBL 分配返回 NULL。在原失败回滚执行前，首个 record 和两个 MDL 仍被持有，Active 尚为零，AllReturned 仍是旧的 signaled 状态。合并场景还持有私有合并缓冲区。

此时实际 Pause 必须先与 TransitionLock 同步。测试确认其独占 TryAcquire 确实失败，再转发原始获取锁函数；不得仅凭 Active=0 或旧 AllReturned 信号提前完成。放行后，原始回滚归还失败对象，剩余普通包或第二个 SCU 继续正常构造、Retain 和上调。真实归还已经令 AllReturned 再次 signaled 时，仍在执行的 indication bank 继续阻止 Pause 提前返回。

最终退出上调、完成 Pause，再通过原 Unregister/Free 停止并汇合实际 worker。模型中的 MDL、池、对象引用、映射、页面锁定和句柄按原账本归零。NDIS 6.20 使用普通包回退；现代 TCP 6.30 RSC、UDP 6.89 URO 分别保留合法合并能力。测试没有在旧能力上强开卸载，没有修补 Busy 或私有引用计数。

这些门重放宿主调度次序，不表示原生内核 spinlock 内可以阻塞。原始宿主及生产正文完整保留，有限词法观察只转发实际操作；真实内核 DDI、IRQL 和内核调度仍需目标系统验证。

## 已消费完成环与尚未完成的公开调用

[Poll 交接测试](../tests/model/poll_handoff32_tests.c)由唯一合法 owner 实际 Reserve/Commit 32 包，宿主 peer 消费提交并发布 32 条完成。实际 Pop 已把 Consumer 前移，但 DLL 尚未验证 token、释放槽位和扣减 Published 时，测试暂停 Poll。

BeginStop 随后启动实际生命周期路径。后台独占 DataGate 的 TryAcquire 必须确实失败并转发原获取锁；此时 Published 仍为 32，不能凭已前移的共享 Consumer 越过私有结算。放行后 Poll 完成全部 32 项，在 DataGate 已释放、Lifetime 仍由公开调用持有的尾部再暂停。

独立 NativeActive=1 继续阻止 Prepare；可信停止/Active=0 回复满足条件后，可以 Prepare，但映射、设备及模块引用仍保持。公开 Poll 先真正返回 32 项，同一 owner 再确认下一次 Poll 为空并退出，汇合后才执行最终 Close、全部捕获回调的 OS join 和真实 DLL 卸载。

Native I/O 和 peer 是明确的宿主边界，不冒充实际 SYS。测试没有伪造正常 FULL、修改计数或在公开调用尚未退出时执行最终 Close。公开输出与稳定的内部观察点分开，不把调用中间值视为应用已获授权的结果。

## Rust 提交失败后保留借用并整批重试

[Rust 分族测试](../rust/packetbridge/src/native_tests.rs)向实际 C shim 预留 32 个普通 TCP/UDP 包，只漏掉最后一项的 `set_length`。实际 Commit 返回 INVALID、accepted=0；Rust 仍持有 32 项，原指针、descriptor、载荷和哨兵保持，native Loans=32/Published=0，peer 的 Take 返回 EMPTY。

仅补上最后一项长度后，一次 Commit32 成功。peer 取得原 token 与完整 wire，完成和 Poll32 后再次自然 Reserve32；按槽位匹配检查 generation2 和原数据地址，检查全部 32 个槽位唯一，不要求 Reserve 返回未承诺的顺序。Drop 随后只调用一次 Cancel32，Loans 和 Published 归零后关闭。

IPv4/IPv6 分进程执行，每族分别覆盖 TCP/UDP。TLS 计数器转发原始函数，没有强制返回值；每协议两次 Reserve32、两次 Commit32，其中只有一次成功发布，最后一次 Cancel32。测试没有调用分片 writer，也不把这些 FFI 次数当作内部 memcpy 或原生 CPU 的测量。报文中的不透明标记没有被解释为 API 等待时钟。

Rust 改动仅追加测试，原完整测试文件前缀保留；native/fallback、C 核心及公共结构保持原文。既有 `batch32_ipv4_`、`batch32_ipv6_` 和完整 Cargo 验收自动收录新测试。

## 验证与范围

新增 C 组接入分族 UnifiedTests 和守护内存 Safety。独立审计覆盖 IPv4/IPv6、ISO/MS、普通/守护内存，默认独立保存当前来源；显式历史模式认证全部 757 文件的 typed Ordinal 身份，生产依赖须逐字节相同。完整冻结宿主、有限投影、编译参数、执行日志及元数据拒绝合同分别认证。

入口为 [接收回滚审计](../tools/audit/verify-receive-rollback32.ps1)、[Poll 交接审计](../tools/audit/verify-poll-handoff32.ps1)及完整 [validate-unified.ps1](../tools/build/validate-unified.ps1)。本轮原始记录、源码摘要和产物身份保存在 `out/ownership-window-review/`。

本轮未确认新的 CPU 热点、普通同队列逐包提交、发送时间混用或重复载荷复制缺陷。检查未据正常环几何上不可达的 FULL 场景改写路径，也未移除必要的 canonical 校验。宿主资源归零、保护页、句柄平衡、线程汇合及 DLL 卸载仅证明声明的路径；旧系统实机、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力和原生 CPU/吞吐/延迟仍待相应系统测试。
