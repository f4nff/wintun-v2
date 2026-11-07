# IOCTL 分派、注册回滚与查询寿命复核

版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮基线指纹为
`7b8cd9b2cae592f6e896b42eb2a8dc40edf9b32f62bc0ed7e9d36222775490a7`，
快照位于 `out/dispatch-register-baseline/7b8cd9b2cae5`。

本轮没有确认新的生产缺陷。新增回归固定此前缺少直接覆盖的两种合法交错，
保留现有寿命屏障；不根据测试变异推断生产代码曾存在相同缺陷。

## 实际分派到注册失败回滚

测试执行当前 `TunDispatchDeviceControl`、上下文引用与撤销、v2 控制分派、
注册、注销及 Halt 代码。注册资源、MDL、事件、工作线程和回滚记账复用已有
register 宿主；构建器只在输出目录有限适配上下文/IRP 类型、上下文存储及
include 路径，记录原文、提取区间、适配内容和哈希。

初始条件为 Pause 已完成且没有待归还的 NBL。分派取得 Control pin 并释放
全局查找锁后，Halt 撤销设备上下文并等待 Control 屏障；随后让注册继续，
覆盖关键资源申请失败和成功后的回收。不能在持全局查找锁时停住引用操作，
也不能把尚未完成 Pause 的 Halt 交错当成合法驱动生命周期。

测试还核对已识别 IOCTL 的访问拒绝、上下文不存在、旧注册请求、未知请求
转发，以及 IRP 状态、Information 和一次完成。Control pin 释放后，Halt
可以先释放上下文，分派随后完成 IRP；这段尾部不再访问上下文。

缺少 Control wait 的负对照在首次不安全释放之前停住，先让真实注册及分派
退出并汇合，再完成释放。负对照用提前回收的见证验证测试敏感性，不执行
已释放上下文访问。

## Close(0) 返回后的状态与能力查询

使用实际非 `PB_API_TESTS` DLL 会话及生命周期，复用 native-policy 宿主
提供的 I/O 和资源记账。首次 `Close(0)` 返回非成功，公开句柄仍有效；
随后状态或能力查询持有 Lifetime shared pin 并停在宿主 I/O 内。释放
native-active 条件后，后台回收必须等待查询退出才能取得 Lifetime exclusive
并物理关闭设备、映射和队列。

实际取得 exclusive 时核对查询的 `PB_BUSY` 行为；回收后核对保留公开句柄
的合成 STOPPED 状态及停止结果。最终成功关闭前先汇合其他公开调用者，
普通 LoadLibrary 引用保持到回调线程和公开调用线程均退出，最后实际卸载
DLL 并核对完整资源收敛。没有用测试额外引用替代被测 Lifetime 屏障。

独占锁观察器先执行真实 TryAcquire；当前查询持有 shared 时必须立即失败，
随后进入真实阻塞 Acquire，不以一段时间内未完成作为主要见证。配对移除
GetStatus pin 的负对照能在读者仍停留于 I/O 时取得 exclusive，观察器此时
将工作线程停在 ReleaseResources 之前。先放行并汇合读者，再让工作线程
正常关闭、最终关闭公开句柄并卸载 DLL；完整清理后才返回预期失败码 113。
每个当前完整进程预热 32 次、测量 32 次，查询成功与失败均覆盖。

## 验收入口与边界

```powershell
pwsh -NoProfile -File tools/audit/verify-dispatch-register.ps1
pwsh -NoProfile -File tools/audit/verify-status-lifetime.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

回归使用 Win7 目标编译、ISO/MS volatile 及普通/守护内存组合，核对原有
生产依赖不变、旧导出及导入兼容性、数值版本和产物哈希。最终执行记录位于
`out/dispatch-register-investigation/final-review.json`。

宿主不改变真实设备、注册表或驱动。生产数据路径、锁、工作线程、计时器和
唤醒机制均未修改，因此没有新增运行时 CPU 开销；宿主耗时不代表内核 CPU
测量。真实旧系统、NDIS/IRQL、Driver Verifier，以及长期内核池、句柄、线程
和 CPU 验收仍需专用系统完成。证据保持 `kernel_runtime_tested=false`、
`production_ready=false`。
