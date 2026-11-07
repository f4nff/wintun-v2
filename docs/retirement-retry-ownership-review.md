# 回收与重试所有权复核

版本保持 2.0.0，DLL/SYS 数值版本为 2.0.0.0。本轮基线为
`03bf04fcdbb36f907b320c021cc4c2ae7ec034880fdebc01acb5c9951293de30`，
582 个文件的完整快照位于 `out/retirement-retry-baseline/03bf04fcdbb3`。

本轮没有确认新的生产缺陷。新增回归直接执行现有路径的组合，生产同步、
旧接口、系统能力门控与热路径保持不变。安全负控用于检验断言能否发现特定
遗漏，不能作为当前实现有缺陷的证据。

## 后台关闭完成后的策略等待者

实际非 PB_API_TESTS DLL 中，Close(0) 先返回非成功，公开引用继续有效，再启动
1、2、8 个 PollPolicy 调用者。实际条件变量在策略完成后唤醒；观察器在返回
生产循环前短暂释放 State，等待门闩后重新取得，模拟另一调用者先得到锁。
观察器不修改状态或增加引用，SDK 入口也不代表已观察到内核调度器的等待。

这些等待者不持 Lifetime。后台可在 NativeActive 归零后关闭设备、映射并移除
runtime 登记；公开引用仍必须保留核心及模块。使用尚由公开引用持有的 Work
执行真实 WaitForThreadpoolWorkCallbacks 后，确认 References=1、PublicReleased=0、
Pins=1、RuntimeCount=0、Regions=0，核心和私有 work/pool 仍在。Pins 是宿主跟踪
的所有权计数，不是直接枚举 Windows 加载器的引用数。

随后分别让首个等待者确认、其他 Poll 先消费或 Forget 已完成票据。每张票据
最多确认一次，旧等待者返回 STALE 并保留输出哨兵。覆盖策略实际迟到成功和
失败；停机后新票据被拒绝，不伪造替换票据。所有等待者汇合后才最终 Close，
再销毁池、汇合回调 OS 线程并真正卸载 DLL。

负控仅省略实际 PollPolicy 循环中的 PolicyOccupied 检查，保留 ticket 校验。
观察重复确认后执行相同收尾，完整清理后返回 126；不修正引用或计数。

## 同步控制响应的临时资源

实际 ARP/IPv6 邻居解析、Send、ControlReply 和 NDIS callout 处理当前源码。
临时 data、MDL、NBL 的三处取得失败分别回滚；成功时在 RESOURCES 同步上调中
保留这三个对象，不等待不存在的普通 Receive Return 回调。

上调尚未返回时，Pause 等待指示 bank，公共 FILE 注销和 Free 等待发送 Active。
上调返回后，ControlReply 释放临时对象；原始发送 NBL 在 completion 上调中
释放，发送 Active 继续覆盖 completion 返回，而 Returns 另行覆盖最后尾部。
所有发送、暂停、注销和宿主原生线程汇合后才检查资源归零。

覆盖 NDIS 6.20、6.30、6.83、6.89，ARP/邻居请求、三处失败/成功以及自动/手动
事件。PB_NBL_VERSION 明确绑定运行时版本，拒绝默认 HOST_CURRENT。信息访问
观察器拒绝旧版本不可用字段并检查毒值保留；宿主 NBL 仍是完整模拟布局，不
冒称旧内核真实分配的结构尺寸。NDIS、IRQL 和接收上调均为宿主边界。

负控只让 NBL 构造失败时遗漏临时 MDL 释放。在该临时 payload 真正释放前，
观察器确认没有 NBL/NDIS 持有者，控制方通过实际 IoFreeMdl 恰补偿一次，再放行
payload 释放及其余发送、暂停、注销和 Free。正常构建不补偿；全部资源和线程
收尾后才返回 127，不编辑引用或资源计数。

## 清理取消及缓存命名空间

完整 main.c、namespace.c、cleanup scheduler 与实际 CleanupRun、WintunShutdown
执行当前源码。SID、BoundaryDescriptor、锁、运行时租约和延迟依赖卸载使用
真实 SDK；私有命名空间及命名互斥量的外部边界用未命名 event/mutex 替代，
不创建真实项目命名空间、不操作设备或注册表。

Create 成功但尚未发布缓存时，实际 shutdown 广播停止，零预算调用返回超时，
在途 cleanup 继续保留租约和回调模块引用。迟到 SDK 调用返回后，实际取消检查
回滚 PendingNamespace/PendingBoundary，不进入设备清理扫描。另一成功用例确认
缓存对象在依赖租约归零和真实延迟依赖卸载后仍由模块持有，直到实际 DLL detach
执行 NamespaceDone 才释放。句柄关闭接口见 [ClosePrivateNamespace](https://learn.microsoft.com/en-us/windows/win32/api/namespaceapi/nf-namespaceapi-closeprivatenamespace)；
本测试替代该外部边界，不测真实命名空间对象的内核回收。

负控只省略失败出口的 PendingBoundary Delete，不改变缓存的 NamespaceDone。
在实际 CleanupRun 最后归还租约的调用边界停住，确认初始化锁已退出、缓存和
PendingNamespace 均为空、唯一实际边界描述符仍在。普通加载引用有效时，对
AddSID 更新后的最终指针调用实际 DeleteBoundaryDescriptor 恰补偿一次，再放行
真实 Release 和回调返回。正常构建不补偿，完整卸载和资源清理后才返回 128。

cleanup 使用系统共享线程池；SDK 的工作线程不属于项目，不打开其句柄假称
join。测试汇合自有调用者，以真实回调返回和 DLL 卸载为边界，并逐项检查
测试拥有的资源归还。

## 验证与剩余边界

```powershell
pwsh -NoProfile -File tools/audit/verify-policy-shell.ps1
pwsh -NoProfile -File tools/audit/verify-control-reply.ps1
pwsh -NoProfile -File tools/audit/verify-cleanup-namespace.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

ISO/MS volatile、普通/守护内存组合记录完整原始源码、有限宿主适配、唯一负例
变异和产物哈希。完整预热后继续严格比较进程句柄数，每个用例检查其拥有的
资源，不关闭或排除未知 SDK 句柄。既有 SDK 首次缓存调查保留独立证据。
最终记录位于 `out/retirement-retry-investigation/final-review.json`。

生产路径未新增锁、线程、定时器或周期唤醒；宿主事件和调度断言不是实际内核
CPU 测量。本轮不安装驱动或修改系统配置。真实旧系统、NDIS/IRQL、Verifier、
长期内核池、句柄、线程和 CPU 验收仍需专用系统完成，证据保持
`kernel_runtime_tested=false`、`production_ready=false`。
