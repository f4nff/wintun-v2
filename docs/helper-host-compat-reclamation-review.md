# 2.0.0：辅助入口失败处理与 Win7 硬件身份一致性

本轮从 `1befd7473d8627f43432b6d2a95cf7f5444651f94b06738ae6adc0ee0b914f18` 冻结基线继续。版本固定 2.0.0，DLL/SYS 文件版本固定 2.0.0.0。修复辅助模块参数失败处理及 Win7 创建辅助模块的硬件身份；SYS 与 DLL 数据队列、公开导出、单 DLL/单 SYS 发布构成和系统能力门控不变。

## 参数失败不能报告成功

setupapihost 的三个实例操作入口在 CommandLineToArgvW 返回 NULL 后仍检查未初始化的 Argc，并未先排除空参数数组；参数不足分支还保留 ERROR_SUCCESS，向父进程返回 HEX 字符串“0”，尽管没有执行请求。[CommandLineToArgvW 契约](https://learn.microsoft.com/en-us/windows/win32/api/shellapi/nf-shellapi-commandlinetoargvw)要求失败时检查 NULL，成功取得的整个参数数组由调用方用一次 LocalFree 释放。

冻结整份 host.c 的两组负对照已复现三个入口都返回“0”：

- 92 / HELPER_PARSE_FAILURE_REPORTED_SUCCESS：边界注入解析失败、返回 NULL 并把 count 写成零，验证假成功。不耗尽实际内存，也不把它描述为已复现的 OS 崩溃。
- 93 / HELPER_MISSING_ARGUMENT_REPORTED_SUCCESS：真实原生解析缺少实例参数的命令行，验证假成功。

修复共用 GetInstanceArguments：先初始化 count，检查返回数组，再检查参数数量。解析失败保留非零错误，零错误防御场景映射为 ERROR_GEN_FAILURE；参数不足使用 ERROR_INVALID_PARAMETER。三个原入口仍统一拥有并释放数组，原 HEX 响应格式、有效参数与后续设备操作策略不变。

helper_host_tests.c 包含整份实际 host.c，所有设备调用均使用替身，原生命令行解析、FormatMessageW、LocalFree 和匿名管道 I/O 真实执行。ISO/MS 每种模式执行 612 个场景，涵盖失败时 count 为负/零/极大值或未被写入，零/原错误，缺参数、引号、Unicode、长参数、额外参数，以及各设备调用阶段失败。每例核对数组/格式化块释放与管道、设备句柄替身归还。原生 Shell32/格式化首次初始化可能保留少量进程资源，先记录预热变化，再要求两轮完整测量与预热后的句柄基线相等；预热用例同样核对精确所有权，没有删除持续增长检查。

## Win7 辅助创建必须匹配统一 INF

API 的 adapter.h 与 driver.h 使用 PacketBridgeV2，统一 INF 的硬件模型也为 PacketBridgeV2；但 host_win7.h 仍使用旧 Wintun。辅助创建因此会提交另一种设备名和硬件 ID，无法按统一 INF 的身份完成驱动匹配，违背一个 DLL/一套 v2 SYS 的设计。

实际 CreateInstanceWin7 的旧源对照 94 / HELPER_WIN7_USED_LEGACY_HARDWARE_ID 观察到旧名称和精确的旧双 NUL 数据。匹配替身读取仓库真实 INF，模拟按该模型查找后无匹配，不将此测试冒充实机安装。

现在 include/packetbridge_identity.h 是 C 代码的硬件身份定义，三个 WINTUN_HWID 旧宏名均引用 PB_HARDWARE_ID。API 两处的展开值不变；Win7 helper 改为同一 PacketBridgeV2。测试读取实际 INF 核对值、长度及双 NUL 终止，防止独立常量再次漂移；没有改变设备安装、卸载或恢复策略。

helper_identity_tests.c 运行实际 Win7 创建函数，全部 SetupAPI 设备调用均为替身。ISO/MS 每种模式各 549 个场景：14 个获取/安装阶段、多种错误、清理失败组合、driver-info 列表与父集合所有权、Unicode 和最长实例 ID。测试明确区分列表显式释放与随父集合释放；[SetupDiDestroyDeviceInfoList](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdidestroydeviceinfolist)负责关联内存。每个模式中的 135 个清理失败模型残留由 fixture 显式收尾，没有把失败的清理替身假称为原生释放成功。没有访问或修改实际设备。

## 验证范围

新增用例纳入 Tests/UnifiedTests 与 Safety。专项还分别构建 ISO/MS 的旧/新原生辅助 DLL，核对实际版本、导入和四个原有导出，并对新辅助模块做静态分析；这些原生 DLL 只编译与读取 PE 元数据，不作为代码加载或执行。产品的单 DLL/单 SYS 产物仍由原完整流程生成，辅助模块审计产物不会自动加入发布包。

```powershell
pwsh -NoProfile -File tools/audit/verify-helper-host.ps1
pwsh -NoProfile -File tools/audit/verify-pause-unregister.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

历史失败状态审计对 adapter.h/driver.h 的依赖检查仅增加可核对的等价展开：移除共享身份 include、把别名展开回原 PacketBridgeV2 后，其余内容必须完全相同；共享定义本身也固定哈希，不能用放宽依赖检查掩盖无关变化。

本轮 SYS/DLL 只读复核未确认新的合法调用并发缺陷：旧 Send pin 阻止替代 session 的地址复用；最后一次 session Return 后的外层 callback 仍由 Ctx ReturnRundown 与 Halt 汇合；局部取消由 worker join 保护；未提交 facade 尾部仍计入 Loans，策略票据和工作/模块尾部引用都有归还者。SubmitThreadpoolWork 没有可套用 TrySubmit 的失败返回，Win7 临界区也不能套用旧 XP 的低内存异常模型，因此没有按这些不成立前提改动同步。

未新增线程、锁或周期性唤醒，也未向数据包处理增加工作。旧系统目标仍为 x64 Win7 SP1，宿主测试覆盖旧系统分支不等于在旧系统运行过。真实 Win7/WOW64、新系统 NDIS、签名安装、Driver Verifier、休眠/移除、长期内核资源与原生 CPU 验收仍待实机完成。最终记录保留 `kernel_runtime_tested=false`、`production_ready=false`。
