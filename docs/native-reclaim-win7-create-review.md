# 跨层故障回收与 Win7 创建失败复核

版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮基线指纹为
`019ed63327d911bed092f5586da47d204998714bda93d894e88707683777a31a`，
快照位于 `out/native-close-baseline/019ed63327d9`。

## 已确认的设备信息集遗漏

`CreateAdapterWin7` 先创建空设备信息集，随后在跨架构 helper 失败时直接跳至
最终返回标签，漏过 `SetupDiDestroyDeviceInfoList`。宿主复现记录创建一次、
销毁零次；观察器随后显式归还该资源，再报告旧代码的预期失败。

按照 [SetupDiCreateDeviceInfoListExW 的所有权契约](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdicreatedeviceinfolistexw)，
调用方必须销毁不再使用的信息集；
[SetupDiDestroyDeviceInfoList](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdidestroydeviceinfolist)
负责释放其关联内存。修复只将这个失败出口改至现有 `cleanupDevInfo` 标签，
保留错误码、安装策略和其余清理顺序。

该缺陷位于保留的 `MAYBE_WOW64` 分支；当前原生 x64 发布流水线未启用它。
本轮不据此声称发布产物已经出现过这类泄漏，也不改变已安装设备的回滚策略。
新测试执行完整 Win7 创建函数，在同一目标配置下分别选择本机与 helper 分支，
覆盖获取失败、取消、超时、属性/ID/等待失败、成功和清理失败的所有权边界。
SetupAPI、helper 和就绪等待为宿主边界；事件、堆、CRT 及输出守护页为实际资源。

## SYS 到 DLL 的故障回收链

此前已有实际 SYS 故障/归还/QUERY 测试，也有实际 DLL 生命周期测试，但故障
收尾组合中仍有由宿主直接提供 STOPPED 的路径。新回归把生产 DLL 队列和
生命周期接到生产 SYS 接收、归还、故障、队列静止见证、QUERY 及回收代码。

测试不使用 `PB_API_TESTS` 的状态替身。共享终止提示、用户借用、已发布槽位、
原生 Active 与 Returns 回调尾部是不同的条件：终止提示不能直接授权回收，
Active 降为零也不能替代回调尾部的 rundown 屏障。调用最终 Close 前先汇合
公开调用者；关闭设备的宿主边界再执行实际的会话注销和释放。

内核会话的就绪构造、DDI 和系统 worker 仍由宿主提供，不声称执行了真实注册、
NDIS、IRQL、I/O 管理器 Close 分派或驱动装卸。负对照须在不安全释放之前
固定观察点，显式放行并汇合归还线程，释放所有资源后才报告预期失败。

## 验收

```powershell
pwsh -NoProfile -File tools/audit/verify-win7-create.ps1
pwsh -NoProfile -File tools/audit/verify-native-reclaim.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

已有孤立设备审计仍保留冻结整文件哈希及整体比较，仅补入这一处已复现的
Win7 清理标签变更。最终源码、负对照、版本、导出/导入和产物哈希记录在
`out/native-close-investigation/final-review.json`。

真实旧系统安装、Driver Verifier、NDIS/IRQL、长期内核池、句柄、线程及 CPU
验收仍待专用系统完成；离线证据保持 `kernel_runtime_tested=false` 和
`production_ready=false`。
