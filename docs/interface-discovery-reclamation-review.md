# 接口发现竞争与回收对象守护复核

产品版本保持 2.0.0，文件版本为 2.0.0.0。本轮基线为源码指纹
`b208f76ac8e3f829cecebc2c11a83f351c845b91a668755589b9ef8f44713f98`。

## 确认的问题

`AdapterGetDeviceObjectFileName` 原来先查询接口列表长度，再分配缓冲区读取列表。
两次查询之间接口可能出现或重新绑定，使第二次查询返回 `CR_BUFFER_SMALL`。
旧实现直接释放缓冲区并失败；Win7 就绪循环会再次查询，但创建和打开适配器的
其他调用路径会直接失败。

[Microsoft 的函数契约](https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_device_interface_listw)
明确要求遇到该状态重新获取长度和列表。这是合法的系统状态变化，不依赖无效的
SDK 成功输出。回归测试编译冻结的原函数，以相同的合法查询序列验证旧失败和新成功。

修复仅对 `CR_BUFFER_SMALL` 重试：先释放本轮缓冲区，再重新查询长度。
重试服从所属操作的取消和总期限，同时有独立的 15 秒预算；退避从 1 毫秒增加到
50 毫秒上限，并受剩余时间约束。稳定列表仍一次完成，其他错误保持原有失败语义。
没有增加常驻线程、句柄、周期性定时器或数据路径工作。

15 秒限制的是每次发现调用中的重试。Win7 外层就绪循环可以再次调用发现函数，
但不会更新所属操作的总期限（默认 120 秒）；测试保留同一父操作起点，验证多次
发现调用仍在父期限到达时退出。同步系统调用本身不能中途抢占，因此这些预算
不构成对单次系统调用耗时的保证，也不宣称整个 Win7 就绪阶段最多 15 秒。

## 并发与资源复核

本轮继续检查 SYS 的最后一个 NBL 归还、`AllReturned` 和 `Returns` rundown 的顺序，
Pause 与 Unregister 的退休会话汇合、外层回调 rundown，以及暂停时的回收和事件唤醒。
DLL 复核包括归还与关闭、晚到适配器结果的唯一领取或释放、清理回调的模块尾部引用。
所检查的合法交错未确认新的生产缺陷，因此这些同步实现保持原样。

此前守护测试中的 hosted MDL 和 NBL 对象使用普通堆，只有池对象和共享区使用守护页。
本轮为这两类对象增加守护分配，并验证末端越界和释放后访问能被检测。
原有分配失败位次、计数器和回调钩子次序保留；普通模式继续使用原分配方式。
这扩大了真实生产 helper 在宿主测试中的内存错误检测范围，不代表执行了内核 DDI。

## 重放与证据

```powershell
pwsh -NoProfile -File tools/audit/verify-adapter-discovery.ps1
pwsh -NoProfile -File tools/audit/verify-kernel-object-guards.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-pause-unregister.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

接口测试覆盖连续变化、成功和空列表、查询与分配失败、取消、所属操作期限先到、
独立超时及退避边界，并核对每次缓冲区释放。SYS/DLL 回归矩阵继续覆盖并发归还、
槽位复用、generation 耗尽、分配失败、暂停和注销交错、手动及自动复位事件、
计数器回绕、回调尾部屏障和全部已创建线程的汇合。

最终源码指纹、实际执行数量、诊断比较、导出和导入检查、产物哈希与版本见
`out/interface-guard-investigation/final-review.json`。全量离线结果见
`out/unified-validation/summary.json`。

测试没有安装或加载驱动，没有修改真实设备、注册表或启动策略。Windows 7 SP1
x64 仍是最低工程目标；真实旧系统安装、NDIS/IRQL、Driver Verifier、长时间的内核池、
句柄、线程及 CPU 验收仍待完成。证据保持 `kernel_runtime_tested=false` 和
`production_ready=false`，不据宿主计数结果宣称全系统无泄漏。
