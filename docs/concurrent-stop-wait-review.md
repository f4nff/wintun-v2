# 并发归还、停机与等待复核

产品版本固定 **2.0.0**，PE 数值版本 **2.0.0.0**。从验收源码 `e2cad0884b0caa982e49b43f493205f5a73b00f4b3c96293652d8a40d6fa1733` 继续检查。发布仍为一个 `wintun.dll`、一套 SYS，Wintun 导出、序号、v2 数据/函数表 ABI、NDIS 能力策略与上一轮 MDL/NX 兼容修复不变。

## 确认的两个 DLL 问题

最后一个依赖租约归还时，`MainRuntimeRelease` 在锁内将 `DependencyUnloading` 置位，再在锁外卸载 delay-import DLL。其他线程的 `MainRuntimeAcquire` 等待该过程结束，防止使用正在恢复的 IAT。这个所有权边界继续保留。

原 `MainRuntimeAcquire` 忽略 `SleepConditionVariableSRW` 的失败返回。若等待持续失败，会在依赖锁内重试，无法返回原始错误，还可能阻碍卸载方重新取得该锁。微软的 [API 契约](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw) 明确区分成功与失败，并要求重新检查条件以处理伪唤醒。修复后，失败立即释放锁、保留 Windows 错误并返回，不取得租约；成功唤醒仍检查原条件，不将伪唤醒当成卸载完成。此项通过故障注入确认，不声称已在旧系统实机遇到该 API 失败。

另一个问题不需要注入失败：`MainRuntimeBeginShutdown` 原先只设置 RuntimeStopping，没有唤醒正在等待依赖卸载的获取线程，获取方的等待条件也未观察停机。于是停止接纳后，这些线程仍等到整个物理卸载结束才能退出。

现在停机与等待使用同一 DependencyLock 保护条件：第一次停止接纳时通知等待者，获取方看到停机立即返回 `ERROR_SHUTDOWN_IN_PROGRESS`。后续重复停机不重复广播。只中止获取资格，不取消正在卸载的线程，不提前清除 DependencyUnloading；`MainRuntimeShutdown` 仍等待真实卸载完成，预算为零时继续返回超时。

生产修改仅在 `api/main.c`。不新增定时器、线程、协程、锁、分配或逐包工作；不修改 SYS 回收/唤醒算法，也不移除 DLL 归还快照或原生回收屏障。

## 实际 DLL 等待测试

`runtime_wait_fixture.c` 复用已有真实运行时 DLL fixture，执行实际 `api/main.c`、真实 delay-import 加载/卸载与 Windows SRW/条件变量。测试仅在最后依赖租约归还后的真实卸载入口设置 gate，并注入条件变量失败或伪唤醒；没有手工清除生产卸载状态来取得通过。

`runtime_wait_tests.c` 覆盖：

- 五类等待失败，只调用一次等待、原错误保持、没有新租约，放行真实卸载后仍可正常获取及释放。
- 八个实际等待线程，0～3 次伪唤醒。卸载未完成时不提前获取，不使用正在卸载的依赖。
- 停机先于真实卸载结束：所有获取线程先退出，但依赖仍加载、卸载标志仍在，shutdown(0) 仍超时；只有真实卸载返回后才允许 shutdown 成功。
- 释放线程原有 LastError 保持；40 次并发 DLL 装卸，所有线程 join 后关闭句柄，再卸载 fixture。检查私有堆、后台 job、受测拥有资源、延迟依赖及进程句柄增量。

测试在原实现上具有两个精确反例：失败被再次重试时退出 42，输出 `FAILED_WAIT_RETRIED`；停止没有发出唤醒时退出 43，输出 `BLOCKED_ACQUIRE_NOT_WOKEN`。反例通过事件及实际通知观察确认，不把超时、崩溃或不相关失败算作复现。新实现完成整个正向矩阵，宿主资源检查无残留。

构建 helper 在 `tools/build/runtime-wait-tests.ps1`，固定沿用 Win7 SP1 编译目标和原有 delay-unload 参数。该测试进入 `UnifiedTests` 与 `Safety`，`Tests` 自动包含 `UnifiedTests`。

## 八队列并发归还与停止

`ownership_counter_tests.c` 在既有 40000 次并行收发/取消/完成及进度观察之外，新增 **64 个停机交错**：八个队列 owner，批宽 1/2/3/7/8/31/63/64，TX 归还先于 RX 取消及相反顺序，每个队列分别作为最后 owner，原生引用保留/释放、普通/回绕游标。

七个 owner 并发退还，最后 owner 在写入零 loan 后、仍持有 DataGate/Lifetime 时暂停。此时实际进度读取可以看到零 loan/Published，但 PrepareClose 不能据此越过 DataGate。放行并 join 全部 owner 后，保留的原生引用仍独立阻止准备完成；最终原生确认到达后先 PrepareClose，再按契约 Close。

测试没有在 queue 用户尚未 join 时调用最终 Close 或 ReleaseSession。SRW 独占等待可能阻塞新共享 owner，因此 gate 调度先完成其他 owner，再暂停刻意安排的最后 owner，避免测试自身制造等待闭环。普通及守护分配都必须完全回收。

此轮未发现新的 SYS 并发归还、最后 Active/rundown 或事件物理回收缺陷。原有跨队列 NBL 回调、暂停、最后引用/最后 pin、QUERY/故障排空、池代际及清理屏障反例继续复验；不能以 Active=0、进度快照或超时代替真实回收条件。

## 重放与验收边界

```powershell
pwsh -NoProfile -File tools/audit/verify-concurrent-stop.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

并发停机审计默认使用本轮修改前冻结副本 `out/concurrent-stop-baseline/e2cad0884b0c`，校验原 main.c 哈希。ISO/MS volatile 两种模式分别复现两个原始错误并运行当前 DLL；所有权测试另在普通/守护内存执行。保留每次退出码、日志、源码和二进制哈希，缺失冻结副本时明确失败。

使用的条件变量和唤醒 API 均在原有最低系统范围内，未增加更新系统的必要导入。完整离线验收继续检查旧导出序号、Win7 导入、内嵌 DLL/SYS 指纹一致、旧 NDIS 结构边界及现代卸载能力、Rust FFI、失败回滚、资源计数和静态分析。

历史暂停唤醒 A/B 脚本的整文件范围校验原先只接受暂停函数提取，已经不能接受后续经独立审计的下兼容修改。本轮保留冻结快照哈希和暂停函数精确比较，并在整文件比较前明确列出 pool-zero、NBL 版本字段和 MDL/NX 的已知转换；除此之外的差异仍拒绝。没有通过删除范围校验来使旧脚本通过。

没有安装或加载 SYS，不将宿主锁/事件及 DDI fixture 等同旧系统或真实内核验证。Win7 等目标系统签名安装、Driver Verifier、真实 NDIS/LWF、休眠/恢复/拔除、低资源和长期内核池/句柄/线程计数仍待测试机验收；报告保持 `kernel_runtime_tested=false`、`production_ready=false`。
