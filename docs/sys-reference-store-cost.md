# SYS 引用计数的锁内发布

起始源码指纹为 `5028998bf71e8dc97f55d404b545a6e6a4650452836a25ad344fceb3c2f4be5a`，快照位于 `out/sys-reference-store-baseline/5028998bf71e`。

## 问题与修正

每次 SYS send 保留引用，以及 receive/return 批量保留和释放引用，都会修改私有 `Active`。既有 `ReferenceLock` 已经串行化全部运行期写入，同时保护 `AllReturned` 的零与非零转换。原实现仍在锁内调用 `InterlockedExchangeAdd` / `InterlockedAdd`，每次更新多执行一次带完整屏障的原子读改写。

保留现有锁，把计数更新统一改为锁内 `ReadAcquire` / `WriteRelease`。QUERY 继续通过 `ReadAcquire` 无锁观察计数。第一条引用仍先发布正计数，再在同一把锁内清除 `AllReturned`；最后一条引用仍先发布零，再设置事件并报告排空进展。新旧实现都在释放锁前完成事件转换。调试断言检查保留前的非负计数、加法范围及释放前的数量。

这不适用于任意引用计数：当前结构的构造阶段先清零、再公开；运行期只有这两个帮助函数写 `Active`。今后增加写入路径必须继续持有 `ReferenceLock` 并使用相同发布规则。不能重新引入绕过锁的 Interlocked 更新，与锁内读写混用。

## 等待和生命周期

引用覆盖已交付的 NBL 和已准入的 send，send 的无锁上调结束后才释放引用。Returns rundown 继续保护最后释放后的通知和回调尾部。worker 私有 quiescence、QUERY 的二次计数观察、TransitionLock、indication bank、`AllReturned` 和 rundown 的等待顺序均未修改。事件不是单独的释放依据；调用方仍需原有停止准入和尾部屏障。

没有通过超时绕过 NDIS 归还，也没有新增轮询、缓存、句柄或内存分配。

## 验证

`receive_reference_tests.c` 直接执行实际引用帮助函数，记录额外原子读改写及 release store，并断言写入、清除/设置事件都发生在 `ReferenceLock` 内，事件转换前计数已经发布。64 个交叠 owner 加一次部分释放后的重新获取共 130 次计数更新：旧实现执行 130 次额外原子读改写，当前实现为零；锁和事件调用规则保持一致。

真实线程和事件覆盖单条及批量释放、合并 NBL 计数、部分释放后的重新获取、最后一个 owner 唤醒 waiter，以及 30 万次并发保留周期。另有 10 万次实际 QUERY 与保留/释放并行运行：持有 sentinel 引用期间必须保持 DRAINING，最后释放且所有私有见证已到位后才可报告 STOPPED。所有线程退出并检查句柄数量恢复。普通测试和 Safety 都执行此测试。

`tools/audit/verify-sys-reference-store.ps1` 使用冻结/当前帮助函数，在两种 MSVC volatile 模式下对照上述测试，并对照实际 send、普通 receive 及保护页 receive 的构造、失败回滚、同步归还、故障排空和等待路径。完整离线验收还保留既有 callback/rundown 尾部回归。

以上是宿主操作计数、真实宿主同步和离线编译验证。SYS 未加载；真实内核 CPU、延迟、吞吐、IRQL、Verifier、ETW/WPA、低资源及长稳仍需内核验收。
