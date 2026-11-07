# 2.0.0：冷启动事件所有权与失败回收复核

本轮确认并修复 DLL 清理调度器和辅助任务的首次初始化错误。产品固定为 2.0.0，单 DLL/单 SYS、公开 Wintun/v2 接口、SYS 和数据面同步顺序保持不变。

## 旧实现的四组负对照

成对创建 idle/stop 事件时，第一个 CreateEventW 失败后仍会调用第二个。后续成功调用可覆盖线程最后错误；HelperReserve 又用这个错误值决定是否增加任务计数，因而可能在没有全局停止/空闲事件时准入任务。CleanupInitializeLocked 虽返回 FALSE，CleanupRequest 仍可能因丢失的零错误码返回 TRUE。单个辅助任务的 Done/Cancel 事件和字符串也连续获取，后续成功调用会覆盖第一个事件的真实失败原因。

冻结旧实现后，故障注入分别要求退出 61/62/63/64，并匹配指定标记：缺失停机事件仍准入、清理初始化失败仍报成功、首次事件错误被覆盖、任务首次事件错误丢失。删除、安装或内核加载没有参与验证；这些用例也没有提交子进程。

这不是假设错误码在所有系统上都保持不变：微软明确要求在失败后立即读取错误，因为成功调用也可能清零或覆盖它。见 [GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror) 和 [CreateEventW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-createeventw)。

## 修复后的资源边界

`api/event_pair.h` 提供一个私有事件对构造函数。第一个获取失败就停止；第二个失败则关闭第一个，恢复已经保存的原始错误。只有两个都成功才交出句柄；失败输出均为空。失败却返回零错误的防御场景统一报告 ERROR_GEN_FAILURE，不能变成成功。

调度器和辅助任务继续在原状态锁下发布完整 idle/stop 对，再准入任务或重置 idle。每个任务的 Done/Cancel 初始均未触发；全局 idle 初始触发、stop 初始未触发。任务后续资源改为逐项短路获取，遇到第一个失败就进入原有回收路径，不创建或提交后续工作。

没有新增同步锁、线程池、常驻线程、延迟任务或轮询。正常数据路径不调用这个构造函数，成功路径保留原有两次事件创建；失败路径减少不必要的资源获取。没有增加旧系统 API 要求，新系统卸载能力门控未改变。

## 实际生产路径测试

`tests/safety/event_init_fixture.c` 包含生产清理调度器及完整辅助任务代码，复用现有真实 DLL 运行时夹具。测试钩子只控制获取失败和成功时的最后错误；句柄来自真实 CreateEventW，回收使用真实 CloseHandle。辅助任务工作项和堆分配沿用归属计数。队列数据面不参与这些冷启动用例。

每种 volatile 模式执行 342 次检查：

- 清理请求、清理停机、清理初始化和辅助任务准入，分别覆盖第一个/第二个事件失败，零/内存不足/访问拒绝/句柄不足，以及后续成功覆盖为零或其他错误。
- 失败后重试和完整发布；检查事件初始信号，已初始化调用不重复创建事件。
- 每个辅助任务从冷启动的九个资源获取位置逐一失败，核对错误、调用次数、未提交工作和全部私有资源回收。
- 五组真实八线程首次争用，涵盖初始化成功、第一/第二个事件失败和零错误。仅四个有效租约进入；不完整初始化不会占用配额。停机立即拒绝新租约，已有租约归还前不能宣告 idle，释放后才完成停机。
- 每个场景归属计数清零，四十个测试线程逐一等待退出并关闭。首轮完整矩阵记录进程句柄初始化差异，之后两轮检查句柄不增长；预热轮同样逐项检查所有应用获取的资源，未放宽归属检查。追加十六轮全新 DLL 加载、初始化失败、回滚和卸载，核实跨模块生命周期的句柄同样不增长。

`legacy_runtime_fixture.c` 只增加默认无操作的事件结果观察钩子，原传输测试不改变行为。新用例纳入 Tests/UnifiedTests 和 Safety。冻结旧源哈希、四组负对照、ISO/MS 运行结果和二进制哈希由审计脚本记录。

```powershell
pwsh -NoProfile -File tools/audit/verify-event-init.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

本轮还复核了 SYS 的引用、归还回调尾部、故障停机、暂停/重启与 worker 事件顺序，未据未证实的怀疑改动内核同步。离线宿主验证不等于旧系统或内核运行验收：真实 Win7/新 Windows、签名安装、NDIS、Driver Verifier、热移除、休眠恢复与长期内核资源监测仍待实机完成。记录保留 `kernel_runtime_tested=false`、`production_ready=false`，产物是未签名开发版本。
