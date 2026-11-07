# 外层归还屏障与驱动枚举失败复核

版本保持 2.0.0，DLL/SYS 文件版本保持 2.0.0.0。本轮基线为
`180d7cbe5ff4dc745bf955dd15a8db72c93b95e8eef4c3619af66e32542c90b9`。

## 两层回收屏障

会话的 `AllReturned`、`Returns` rundown 与设备上下文的 `ReturnRundown`
保护不同的寿命。最后一个 NBL 已归还、会话已经释放后，外层
`TunReturnNetBufferLists` 回调仍可能处于退出尾部；`TunHaltEx` 必须继续等待
外层 rundown，才能释放 NBL 池、注册锁和设备上下文。

此前测试直接进入会话归还 helper，没有执行这两个外层入口。本轮新增从实际
`driver/wintun.c` 逐字提取入口的构建步骤，让宿主测试执行生产函数本身，
并记录整源与提取文本的哈希。删除屏障的负对照必须失败，现有生产实现必须通过。
这些负对照验证测试能发现错误，不表示生产代码曾缺少该屏障。

所检查的 SYS 归还、暂停、注销和停机交错没有确认新的生产缺陷，生产内核实现
保持原样。DLL 的 facade worker 与 native core 分别持有自己的资源和模块引用；
旧字节 ring 的释放不构成撤销 native 后台回收保护的依据。

## 枚举失败不能等同完整扫描

[SetupDiEnumDriverInfoW 的契约](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdienumdriverinfow)
仅以 `ERROR_NO_MORE_ITEMS` 表示枚举结束。旧安装、删除路径忽略其他枚举错误后
继续扫描，可能把不完整结果当成没有已安装驱动，或在没有完成删除时返回成功。
本轮对真实函数构造失败后再返回列表结束的序列，验证旧行为，再保存原始错误并
走现有回收路径。已停用适配器的恢复和资源清理仍由原有所有权链负责。

删除路径读取驱动详情失败时也需要报告未完成，不能仅打印日志后返回成功。
但缓冲区容纳不了可变长硬件 ID 列表与普通读取失败不同：
[SetupDiGetDriverInfoDetailW](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdriverinfodetailw)
保证在有效成员和缓冲区下填充静态字段，即使返回 `ERROR_INSUFFICIENT_BUFFER`。
安装和删除可以使用其中的 `InfFileName`，不因无关的可变长 ID 列表跳过该驱动。

没有新增系统导入、锁、线程、常驻对象或数据路径工作。正常枚举和版本选择保留。
枚举未完成或删除详情读取失败时报告失败；安装器对旧包详情、移除失败的既有
best-effort 警告策略保持。删除中的查询和卸载错误保留首错，后续取消仍按原优先级
返回取消错误，测试分别验证这些出口。

## 验证边界

新增测试覆盖外层回调尾部与 Halt 汇合、并发归还，以及安装、删除的枚举和详情失败。
同时重放此前的归还、槽位复用、generation 耗尽、分配失败、暂停和注销、事件唤醒、
helper 所有权与旧系统接口查询测试。执行结果、源码指纹、编译模式、诊断比较和产物
哈希记录在 `out/outer-return-investigation/final-review.json`。

```powershell
pwsh -NoProfile -File tools/audit/verify-outer-return-halt.ps1
pwsh -NoProfile -File tools/audit/verify-driver-enumeration.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

测试使用宿主 DDI 和 SetupAPI 替身及真实 Windows 锁、事件、堆和已汇合线程；
不安装或加载驱动，不操作真实设备或注册表。Windows 7 SP1 x64 仍是最低工程目标。
真实旧系统安装、NDIS/IRQL、Driver Verifier、长时间的内核池、句柄、线程及 CPU
验收仍待完成，证据保持 `kernel_runtime_tested=false`、`production_ready=false`。
