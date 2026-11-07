# MDL 兼容性与回收预算复核

版本固定 **2.0.0**，文件数值版本 **2.0.0.0**。本次从首轮验收指纹 `7b618fdac6f55354246215762fe60d1e892b2ac02ca5dc42fea295615f789850` 继续复核。冻结源文件保存在 `out/reclamation-followup-baseline/7b618fdac6f5`。DLL 的槽位快照、兼容层批处理、旧导出/序号与 v2 ABI 不变。

## 确认的兼容性遗漏

`driver/v2/session.h` 注册共享区，以及 `session_read.h` 读取发送 MDL 时，都显式传入 `NormalPagePriority | MdlMappingNoExecute`。微软的 [MmGetSystemAddressForMdlSafe 文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-mmgetsystemaddressformdlsafe) 规定 `MdlMappingNoExecute` 从 Windows 8 起支持。直接传入该位绕过了旧系统的参数兼容决定；这是旧系统 DDI 契约遗漏，不能仅靠 PE 导入检查发现。没有据此声称已在 Win7 实机复现崩溃。

项目原本已在 DriverEntry 最开始调用 `ExInitializeDriverRuntime(DrvRtPoolNxOptIn)`，构建定义 `POOL_NX_OPTIN=1`。实际 WDK `wdm.h` 会根据运行系统设置 `ExDefaultMdlProtection`：Windows 8 之前为零，Windows 8 及以后为 NX 位；其 `MmMapLockedPagesSpecifyCache_NXOptIn` 内联再将该值加入映射参数。实际 `MmGetSystemAddressForMdlSafe` 正是通过这条内联调用路径创建映射。

新增 `driver/v2/platform_mdl.h` 集中两个映射位置，仅传普通优先级，由已有 WDK 运行时机制选择保护参数。两处调用统一使用该 helper，避免将来只修复一个位置。编译保护拒绝缺少 runtime opt-in、显式 opt-out 或 unconditional opt-in 的配置，防止后续构建修改绕过旧系统兼容和新系统 NX。该 helper 强制内联，不增加版本查询、锁、事件、线程或每包分配。共享映射的锁页/解锁、MDL 释放和返回回调 rundown 顺序未变。

## 实际 WDK 回归

`mdl_mapping_wdk.c` 与 Windows 宿主测试分开编译，直接包含发布使用的 WDK，执行真实 `ExInitializeDriverRuntime`、MDL 定义、`MmGetSystemAddressForMdlSafe` 和映射内联，不用一份手抄平台决策替代 WDK。只有内核导入的版本查询、例程发现和最终映射调用使用宿主替身。替身记录/校验最终映射参数，并支持逐次失败；这仍然不是实际内核执行。

`mdl_mapping_tests.c` 使用真实只读页和 no-access 页。每个 ISO/MS volatile 进程执行 **419330** 个场景：

- 旧系统、Win7、Win8、Win8.1、当前/未来版本、最大版本号，以及版本查询失败的 WDK 保守回退。
- 共享映射 helper 的成功/失败；每条新映射都检查 Win7 无 NX 位、新系统有 NX 位。
- 不连续和空 MDL 链、当前 MDL 的每个偏移、每个 skip/length 组合、零长度、无 MDL、非法偏移及长度大于实际链。
- 两个映射位置分别失败，返回失败前可能已复制的前缀保持原契约；已映射及 nonpaged MDL 复用已有地址，不再次调用映射器。
- 所有输入只读，输入及输出末尾紧贴守护页，检查精确内容和未使用输出字节，释放所有宿主映射。

`tools/audit/verify-mdl-compatibility.ps1` 另编译冻结的原 `session_read.h`，在 `--win7-only` 中要求实际退出码 1 且精确匹配 Win7 映射契约断言。崩溃、超时或不相关断言不能算反例通过。当前实现必须正常完成全部矩阵。三个错误构建配置必须由新 helper 的明确 C1189 错误拒绝，脚本保存编译日志及源码/WDK/二进制哈希。

## 槽位、唤醒和 CPU 复核

新增 `recycle_budget_tests.c`，执行真实 `PbV2RecycleLocked`、共享队列核心和 fault 通知。**5158** 个场景覆盖 1/63/64/65/255/256/257/511/512 个槽位、普通/回绕游标、正常/最终代际、四批预算边界，以及 512 个位置逐个出现 epoch/generation/queue/slot 错误或重复归还（首项无前项可重复）。

测试独立检查每次最多四次 Pop、尚有回收项时 arm/recheck 继续处理、仅合法前缀槽位变为 Free、空环不清除池满证明、故障只有一次 Kernel/User/Progress 通知，以及损坏游标不释放槽位。最终代际不能再次 Reserve，失败不改输出 token。合法回收不凭空发送事件，句柄数无增量。

既有暂停/恢复、满完成环、最后 NBL 归还、跨队列、Active/rundown 两阶段屏障、QUERY、故障排空和停机 pending loan 回归继续保留。此次没有确认新的槽位回收或唤醒算法缺陷，因此未改动这些生产路径。

另对归还快照的 `memcpy` 与逐 token 结构体赋值进行了七轮交替、固定 CPU 的宿主实验；编译器把两者都归约为 `memcpy`，各批宽未表现稳定收益。实验源码、原生列表和原始结果保存在 `out/reclamation-followup/experiment`，不将噪声提升为性能结论，不改动已经修复所有权交接的 DLL 快照。首轮约 39.4% 的兼容 owner 周期下降属于首轮宿主基准，本次不宣称额外 CPU 或真实内核吞吐收益。

## 验收

新增两个回归均进入 `UnifiedTests` 和 `Safety`；`Tests` 自动运行 `UnifiedTests`。可重放：

```powershell
pwsh -NoProfile -File tools/audit/verify-mdl-compatibility.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

完整离线验收继续检查 Win7 SP1 x64 导入、旧导出序号、内嵌包/源码指纹一致性、NDIS 版本边界及现代卸载能力、Rust FFI、构造失败回滚、并发、守护内存和静态分析。版本号、旧接口、卸载策略及新系统功能保持原约定。

未安装或加载 SYS；真实旧系统签名安装、Driver Verifier、休眠/恢复/拔除、真实 NDIS 卸载协商、低资源和内核长稳仍须测试机验收。所有报告维持 `kernel_runtime_tested=false`、`production_ready=false`，不以宿主 DDI 契约测试代替实际内核验证。
