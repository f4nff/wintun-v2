# 2.0.0：失败状态不能变成成功或卸载确认

本轮以 `e1ae1acb4404ee90167872eae22940359bc42b24ea072d776b354fe2c450c603` 为冻结基线。产品继续为 2.0.0，DLL/SYS 文件版本为 2.0.0.0。新增修改在 DLL 元数据与查询/等待控制路径，SYS、公开接口、单 DLL/单 SYS、数据队列及系统能力门控不变。

## 适配器 GUID 失败被误报为成功

PopulateAdapterData 原来只检查 CLSIDFromString 的 HRESULT 是否失败，随后却把 LOG 返回的线程 GetLastError 保存为结果。COM 不承诺把该失败写入线程最后错误。若无关错误恰为零，RET_ERROR(TRUE,0) 就会返回成功；此时后面的 Luid、IfType 和 InterfaceFilename 尚未取得。

冻结函数的实际控制流配合真实 CLSIDFromString 已复现：`{not-a-guid}` 返回失败，但元数据函数报告 TRUE，后续字段未读取。旧负对照要求退出 89，标记 `ADAPTER_POPULATED_WITH_INVALID_GUID`；退出前释放真实堆字符串和键的事件句柄替身，不以进程退出代替回收。

现在先捕获 HRESULT：FACILITY_WIN32 且 code 非零时保留 code，其他 COM 失败或零 code 使用 ERROR_INVALID_DATA，然后再日志与清理。[CLSIDFromString 文档](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-clsidfromstring)说明错误通过 HRESULT 返回。真实 GUID/原 ProgID 解析行为不改，没有另写更严格的 GUID 解析器。

函数提取到 adapter_data.h，生产和测试包含同一实现，聚合 adapter.c 其余字节由审计核对。新增矩阵每种 volatile 模式覆盖 465 个场景：合法 GUID、大小写与 GUID_NULL，错误/Unicode/长 GUID，零及陈旧线程错误，WIN32/COM HRESULT 边界，日志回调改动错误，各个获取阶段失败，以及字段、键和字符串所有权。原生解析输入均以左花括号起始，测试不提交 ProgID，也不修改真实注册表值或设备；不声称 COM 内部没有读取系统信息。

## 未发现驱动必须有成功枚举的见证

EnsureWintunUnloaded 原来把 MaybeGetRunningDriverVersion(TRUE) 的零返回一律解释为已卸载。零还可能来自取消、分配失败或 NtQuerySystemInformation 枚举失败。即便只检查 ERROR_FILE_NOT_FOUND 也不足：失败的原生状态同样可能映射到该错误。

修复使用独立的内部缺失见证：仅成功取得模块列表、完整查找后未发现 packetbridge.sys 才确认缺失；查询失败保留错误并返回 FALSE。仍已加载时保留原 50 ms 间隔及 1500 次预算，耗尽明确报告 ERROR_TIMEOUT。公开 WintunGetRunningDriverVersion 的 DWORD/错误约定保持不变。

必须区分这一函数的用途：现有安装调用方在等待失败后记录警告，并按原策略继续处理。本轮修复状态与诊断准确性，没有把这个等待改成新的安装授权屏障，也没有调整驱动包移除策略。

查询/等待函数区提取到 driver_version.h，测试运行真实函数，用替身提供模块枚举和操作等待边界，不查询实际内核模块。模块缓冲区使用真实堆并核对释放。旧负对照要求退出 90，标记 `DRIVER_QUERY_FAILURE_ACCEPTED_AS_UNLOADED`；修复版完整矩阵与同一控制都必须成功。具体执行场景和错误组合记录在审计日志，不以硬编码的成功摘要替代测试结果。

## 排除的候选与回归范围

曾观察到直接给 VerQueryValueW 不完整 root 数据时仍可能返回 TRUE。但进一步生成自己的 VERSION 资源 DLL，只调用真实 GetFileVersionInfoSizeW → GetFileVersionInfoW → VerQueryValueW 读取版本信息后，当前 Windows 在第一个 API 就拒绝了全部小于 52 字节的固定版本结构，52 字节样本才成功。因此直接 VerQuery 的输入实验不能证明实际 VersionOfFile 路径存在越界；本轮没有修改它。该实验只读取元数据，没有将这些资源 DLL 作为代码加载或执行，也不代表已验证 Win7 的同一实现细节。

本轮只读复核还排除了：Pause 先取得 pin 后注销的反向等待、暂停空快照后重新注册产生新 NDIS 所有权、同步归还后旧尾部再次清槽、构造失败误归还另一队列所有权，以及零引用后重新激活普通已发布 core。没有为了这些未成立猜测改变同步顺序。

新增测试纳入 Tests/UnifiedTests 与 Safety。冻结旧源码哈希、精确函数提取、依赖头、负对照码、最终指纹和可执行文件哈希由以下流程核对：

```powershell
pwsh -NoProfile -File tools/audit/verify-failure-owner.ps1
pwsh -NoProfile -File tools/audit/verify-pause-unregister.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

未增加线程、锁、周期性唤醒或数据包路径工作。失败返回触发的外层适配器/操作清理继续遵循原所有权；宿主测试检查堆、键句柄配对，已有线程与回调回归继续检查 join 和卸载边界。实际导入、文件版本及静态诊断与上一轮逐项比较。

真实旧系统、新系统 NDIS、IRQL、签名安装、Driver Verifier、休眠/移除和长期内核泄漏验收仍需实机完成。记录保留 `kernel_runtime_tested=false`、`production_ready=false`，不得从离线通过推导为这些实机验收已完成。
