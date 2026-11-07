# 2.0.0：注销与暂停汇合、旧系统就绪和字符串所有权

本轮以 `31293a87271192383b0510d9a26814d7520887a732a2a13950ef0a857695b409` 为冻结基线，修复一处 SYS 暂停/注销交错和三处 DLL 控制路径边界。产品仍为 2.0.0，DLL/SYS 数字版本为 2.0.0.0；公开 ABI、旧导出、单 DLL/单 SYS 和按系统能力启用的卸载特性保持原定义。

## 暂停不能把已脱链会话当成已回收

注销在持有 RegistrationLock 独占锁时，先把 ctx->V2 清空，再由 PbV2Free 加入 worker、等待 AllReturned 和 Returns rundown。若上层仍持有接收 NBL，旧暂停路径可能在这个窗口读到 NULL，只等待已经空闲的 indication bank 就返回成功；退休会话的 NBL 尚未归还。

这是已脱链与已回收两个状态混淆，不是已证明的内存释放后使用。[MiniportPause 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_pause)要求暂停完成前等待全部接收 NBL 归还。NDIS 对 PnP 暂停/重启的串行保证，不会替自定义关闭 IRP 或进程退出回调加上注册锁。

修复让暂停仅在读取 ctx->V2、取得 Returns pin 的短窗口持有现有 RegistrationLock 共享锁，顺序仍为 RegistrationLock → TransitionLock。取得快照后立刻释放共享锁与 critical region，再执行原来的 wake、indication bank 和 AllReturned 等待：

- 注销先持独占锁：暂停等注销完成，之后读到 NULL 才能作为没有待回收会话的依据。
- 暂停先持共享锁：先取得既有 Returns pin，然后让出注册锁；注销仍能脱链和继续回收，pin 保护暂停的解锁后快照。

没有新增锁、事件、计数、线程或退休会话字段。归还、引用释放、indication 结束和 worker 退出不取注册锁；原 worker join → AllReturned → Returns 顺序保持不变。PbV2Unregister 原样提取为独立头文件，供生产和测试共用；审计核对整个旧 session.h 哈希及逐字相等的提取体。

`pause_unregister_tests.c` 直接运行实际 Pause、Unregister、Free、接收 NBL 构造、归还和引用辅助代码。NDIS 持有状态由 fixture 建立，DDI 由真实 Windows 锁、事件和线程承载，不冒充运行过原生 NDIS。每个当前矩阵含 48 个场景：三轮 × 两种先后顺序 × 归还尾部 gate × 一/两队列 × 自动/手动事件。验证第二个队列未归还时仍阻塞、Active 清零后 rundown 尾部仍阻止物理回收，以及暂停取得 pin 后注销确实能取得独占注册锁。

ISO/MS × 普通/守护内存各重放旧源负对照 86：`PAUSE_COMPLETED_BEFORE_RETIRED_RECEIVE_RETURN`；报告前先归还 NBL、加入所有测试线程并释放资源。修复版完整矩阵和同一控制均须成功。守护覆盖 session、池对象及映射 Region；MDL/NBL 用既有精确分配/释放账本，未宣称它们也使用守护页。测试 worker 是等待 Stop 的宿主线程；真实生产 worker 的暂停唤醒由既有 pause/wake 专项另行覆盖。

## DLL 的三个控制边界

RegistryQueryString 先分配 512 字节，查询可能只写入不带终止符的 6 字节。原修复函数将堆块重分配为 8 字节，却假设 HEAP_ZERO_MEMORY 会补齐末尾。实际缩容保留该位置的非零旧内容，返回字符串仍未终止，之后传入 GUID 解析、日志或环境展开可能越界读取。奇数返回长度还可能只增加一个清零字节，不能保证完整宽字符为零。[HeapReAlloc 文档](https://learn.microsoft.com/en-us/windows/win32/api/heapapi/nf-heapapi-heaprealloc)明确仅在增长时清零新增区域；[RegQueryValueExW 文档](https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regqueryvalueexw)明确不保证字符串终止。

修复在成功重分配后显式写 BufZ[Len]=0，保留原指针移交、首字符串及完整 WCHAR 截断策略。1173 场景覆盖零长度、等容量/缩容/增长、0–2048 字节中的边界与奇数长度、REG_SZ/EXPAND_SZ/MULTI_SZ、真实环境展开、所有分配与展开失败、查询重试、错误保留和原指针所有权。堆操作真实执行，包装层只预填未初始化字节、注入失败和记录精确存活块。旧源控制 84 直接观察缺失终止符；85 在展开边界先有界检查，清理全部块和私有堆后退出，不执行故意越界读取。

WaitForInterfaceWin7 把查询 NetCfgInstanceId 的所有结果中除 ERROR_MORE_DATA 外的结果都当成就绪，包括 ERROR_FILE_NOT_FOUND。因此设备句柄和 devnode 已就绪、注册表值尚未出现时，会提前返回，后续读取随即失败。改为仅 ERROR_SUCCESS 才继续判断就绪；原等待间隔、次数、取消入口、资源回收和后续类型检查不变。实际函数提取到 adapter_wait_win7.h，新增 225 场景覆盖各资源延迟、暂时/持续查询失败、CM 状态、取消和耗尽。旧源控制 87 要求观察到过早就绪，返回前同样配对关闭宿主句柄和释放路径内存。注册表/设备调用是替身，不操作实际键或设备。

HelperStart 也遗漏 GetWindowsDirectoryW 的容量返回检查，可能把不足缓冲区的结果交给 PathCombine，继续扫描未定义的栈内容。现在在返回长度达到或超过缓冲区容量时报告 ERROR_INSUFFICIENT_BUFFER；零返回与正常路径保持原处理。[GetWindowsDirectoryW 文档](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getwindowsdirectoryw)区分成功字符串长度与所需缓冲区容量。63 个场景检查零/正常/临界/MAXUINT 返回及错误码组合。旧源控制 88 在消费无效路径前报告违规，不声称触发了实际 OS 崩溃；正常测试也在第一处路径拼接受控失败，隔离目录验证，不提取包、创建管道或进程。

## 回归和边界

新增测试纳入 Tests/UnifiedTests 与 Safety。审计绑定最终源码指纹、旧文件哈希、对照码、可执行文件哈希和执行日志：

```powershell
pwsh -NoProfile -File tools/audit/verify-pause-unregister.ps1
pwsh -NoProfile -File tools/audit/verify-control-boundaries.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

本轮 DLL 队列发布/私有记账、晚到 Commit、健康队列与损坏队列并存、策略 callback 尾部、旧 facade 部分接收与停止移交未确认新的合法调用缺陷。注销 offload 状态上报与在途合并包的先后关系尚不足单独确认为契约违规，本轮没有据此改变卸载状态同步。

改动都在暂停或控制路径；数据包处理、空闲等待和周期性唤醒不增加开销。旧系统仅复用原已使用的注册资源锁/critical-region 与既有 Windows API，不提高导入版本要求；实际 PE 导入仍由完整构建核对。没有原生 CPU 基准测试结论。

宿主测试的句柄/堆块、NBL/MDL、映射、worker 和调用线程都有对应归还检查，但不代表已经完成真实 Win7/新系统、NDIS、IRQL、签名安装、Driver Verifier、休眠/热移除或长期内核资源验收。最终记录保留 `kernel_runtime_tested=false`、`production_ready=false`，产物为未签名开发版本。
