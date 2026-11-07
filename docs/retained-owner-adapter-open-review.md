# 诊断最后引用与适配器枚举失败复核

版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮基线指纹为
`5fd7eb8e235386abc8d2db192e1dc8e2731029968eceef45482fdcd738963823`，
快照位于 `out/diagnostic-owner-baseline/5fd7eb8e2353`。

## 已确认的枚举错误丢失

`OpenAdapterInternal` 原先忽略 `SetupDiEnumDeviceInfo` 的非终点错误并继续递增
索引。宿主执行原完整函数，先返回 `ERROR_NOT_ENOUGH_MEMORY`，随后正常结束，
最终却得到 `ERROR_NOT_FOUND`，掩盖了实际失败。
[SetupDiEnumDeviceInfo 的契约](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdienumdeviceinfo)
使用 `ERROR_NO_MORE_ITEMS` 标记枚举结束；其他失败不能据此证明适配器不存在。

修复立即保存枚举错误并进入现有清理路径，保持日志与清理后的错误码。
非终点失败但错误码为零的防御性注入映射为 `ERROR_GEN_FAILURE`，避免返回未完成
初始化的适配器；这不是另一个已观察到的自然 SDK 缺陷。单条设备属性读取失败、
类型不匹配后继续查找，以及首个匹配后停止扫描的行为保持。

回归执行完整原函数，覆盖首、中、末索引错误、无匹配、提前匹配、属性失败、
申请与部分填充失败、取消、日志和清理覆盖错误的边界。持续枚举失败仅为可取消
且有界的故障模型，不声称原生 SDK 会在任意索引无限返回同一错误。

## 实际诊断调用的最后所有者

已有回归覆盖后台资源回收与模块尾部。本轮固定实际 `GetRetained` 的独立窗口：
它在 RuntimeLock 内取得会话引用，脱锁后尚未读取状态；公开句柄随后移交，
后台完成物理关闭、移除登记并返回。此时诊断调用必须仍能读取 Closed shell，
并在退出时由诊断线程执行最后的 Destroy、线程池关闭和核心模块引用归还。

测试不额外手工 Retain 来延长这个窗口，不启用 `PB_API_TESTS`。
普通 LoadLibrary 引用保持到诊断调用线程汇合；随后才卸载 DLL。队列、映射、
核心事件、工作对象、线程池、模块引用和实际线程句柄分别记账，不把移出登记表
或 RuntimeCount/Bytes 为零等同于核心已经销毁。

缺失诊断 pin 的负对照在其他所有者仍有效时观察到引用不足，先结束并汇合诊断
调用，再完成公开句柄和后台清理。负对照不读取已释放对象，也不把测试变异说成
已确认的历史生产缺陷。

## 验收范围

```powershell
pwsh -NoProfile -File tools/audit/verify-adapter-open.ps1
pwsh -NoProfile -File tools/audit/verify-retained-owner.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

四份历史 aggregate 审计继续保留原文件哈希、整体比较和原有运行负对照；
共享的有限变换只允许 Open 函数中的这一处枚举错误分支，并由新增 Open 审计
独立核对完整生产差异。最终执行记录、版本、导出/导入及产物哈希保存在
`out/diagnostic-owner-investigation/final-review.json`。

实际设备 I/O、SetupAPI 枚举和安装边界由宿主提供，不改变真实设备或注册表，
不安装或加载驱动。真实旧系统、NDIS/IRQL、Driver Verifier、长期内核池、句柄、
线程和 CPU 验收仍待专用系统完成。证据保持 `kernel_runtime_tested=false`、
`production_ready=false`；本轮未修改 SYS 或 DLL 数据面的同步机制。
