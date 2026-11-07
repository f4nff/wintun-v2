# 终止等待与物理回收屏障复核

产品版本固定 **2.0.0**，PE 数值版本 **2.0.0.0**。本轮从验收源码 `6ca33c59d88e217bf1040c332afc03d8ce2c2cd84d8a5f1e5993ed76fc00b60a` 继续检查；冻结副本在 `out/termination-wait-baseline/6ca33c59d88e`。一个 DLL、一套 SYS、旧 Wintun 导出/序号、v2 ABI 和按系统能力启用卸载的策略不变。

## 确认的问题

DLL 的三个最终等待位置忽略了等待返回值：兼容层 `WintunEndSession`、启动失败回滚时 join worker，以及辅助进程清理时等待 process。若等待失败，原实现继续关闭线程/进程句柄，释放 ring、锁、路径和模块引用，或删除仍可能使用中的临时包。

线程/进程的实际终止必须由对象进入 signaled 状态确认；等待失败不提供该证据，参见微软的 [WaitForSingleObject](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject) 和 [进程终止](https://learn.microsoft.com/en-us/windows/win32/procthread/terminating-a-process) 文档。本轮通过故障注入精确复现“等待失败但对象仍活着”的控制流，不声称已在 Win7 实机遇到这种等待失败。

`compat_session_tests.c` 让实际 worker 暂停，在最终 join 注入失败，并在真正释放映射之前检查独立的线程 witness。原实现以 `LIVE_WORKER_MAPPING_RELEASED`、退出码 44 被拦截，分别覆盖正常结束和启动回滚；测试没有先制造实际 use-after-free 再依赖崩溃判断。

`helper_termination_fixture.c` 运行实际 DLL 传输和真实子进程，取消调用后保持子进程存活，在最终 process wait 注入失败。原实现继续查询退出码并进入清理，由 `LIVE_PROCESS_RELEASE_AFTER_FAILED_WAIT`、退出码 45 拦截。反例显式放行并 join 测试子进程后退出，避免留下孤儿进程；未运行真实安装器或修改设备。

## 统一修复

新增内部 `api/termination_wait.h`，只用于独占持有的 thread/process handle。三个位置共用同一策略：只有 `WAIT_OBJECT_0` 才允许物理回收；其他返回保留所有权并请求 1000 ms 退避后重试。记录第一次错误，缺失错误码或不适用于该对象类型的状态按 `ERROR_GEN_FAILURE` 记录。正常路径沿用原来的无限等待，不增加逐包开销。

这不取消或强杀 worker/安装进程。辅助进程原有四个保留任务上限、模块/依赖引用和临时包继续存在，调用方的取消/超时不替代真实退出；旧会话仍先确认 worker 退出，再释放私有 ring 和同步对象。若底层等待永久不可用，宁可保留现有所有者、低频重试，也不将无法确认的终止当成成功。旧接口的同步结束约定不变。

启动回滚保留原始启动错误，重试过程中其他 API 改写 LastError 也不能覆盖它。辅助进程只在实际终止之后记录终止等待错误，已有取消等首个错误不被覆盖。

## 新增覆盖

`termination_wait_tests.c` 使用真实线程与独立的脚本结果，覆盖正常终止、零错误码、连续不同错误、异常 WAIT_TIMEOUT/WAIT_ABANDONED 返回，以及连续 64 次失败。检查每次失败都请求退避，线程仍存活时没有完成回收，后续成功不覆盖首个错误，句柄增量为零。

兼容层新增 **12 个实际 worker 交错**：正常结束/启动失败、三类异常等待返回、两次重试、worker 先退出或重试先恢复。两个线程 gate 独立控制活跃 worker 和回收方；每个退避点检查 ring、路径、模块 pin 和 runtime 租约仍持有，真实退出后检查资源归零和原启动错误。

辅助进程测试覆盖取消、迟到退出、两次等待失败、三个异常返回状态、调用者放弃普通 DLL 引用，以及子进程实际以 **259** 退出。终止只依据对象信号，不能用退出码数值猜测是否存活；微软也特别说明了 [STILL_ACTIVE 与应用退出码混淆](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getexitcodeprocess) 的风险。测试保持真实子进程、临时目录/文件、依赖、任务槽位及回调模块引用，直到真实退出和安全回收；全部子进程 join，资源和进程句柄增量归零。

退避测试用 gate 替代真实计时，验证生产路径请求 1000 ms 退避且保留资源，不将测试运行时间解释为 CPU/调度性能数据。未增加协程、轮询线程或每包任务。

## 复核与重放

`tools/audit/verify-termination-wait.ps1` 校验两个冻结源文件的哈希，在 ISO/MS volatile 两种模式分别执行旧实现的精确反例和当前完整测试，记录实际退出码、日志、源码/二进制哈希。原始错误必须匹配退出码 44/45 和对应观察点；其他断言、崩溃或超时不能充当复现。

新增 helper 进程测试和通用屏障测试进入 `UnifiedTests`、`Safety`，worker 交错加入已有实际兼容层测试；`Tests` 自动包含 `UnifiedTests`。可重放：

```powershell
pwsh -NoProfile -File tools/audit/verify-termination-wait.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

本轮没有确认新的 SYS 回收/唤醒缺陷，未修改其生产实现。最后 Active、AllReturned、Returns rundown、worker join、暂停 arm/recheck、跨队列归还和故障回收边界继续由既有实际函数模型及反例验证。没有因为状态快照、退出码或超时而削弱任何物理屏障。

修复只使用原有系统范围内的 WaitForSingleObject/Sleep，不增加新系统必要导入。完整离线构建继续验证 Win7 SP1 x64、旧导出序号、单个内嵌驱动包及源码指纹、旧 NDIS 字段边界、新系统卸载能力、Rust FFI、失败回滚和静态分析。

未安装或加载 SYS。旧系统签名安装、Driver Verifier、真实 NDIS/LWF、休眠/恢复/拔除、低资源和长期内核池/句柄/线程计数仍待实机验收；报告继续标记 `kernel_runtime_tested=false`、`production_ready=false`。
