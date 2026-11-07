# 排空故障与收尾错误复核

版本保持 2.0.0，DLL/SYS 数值版本保持 2.0.0.0。本轮基线为
`d1dc19684d050d0acece95c8d29204dc0c9350fca6c8af7f314c5d75331c461f`，
完整 623 个源文件快照位于 `out/retire-edge-baseline/d1dc19684d05`。

## 辅助进程退出后的错误传播

确认并修复 HelperFinishStream 的异常遗漏：进程退出后的 Peek、直接 Read
失败及新提交读取的异步终态失败，原来可以直接结束收尾而不记录错误。
若此前已收到可解析的成功响应，Job.Error 仍为 0，调用者就可能将传输失败
当作成功。真实命名管道复现保留完整有效 stdout，再在 stderr 读取边界注入
配额失败；旧实现完成所有资源清理后返回专用代码 137，直接证明该遗漏。

现在每次 SDK 调用后立即保存错误，统一交给普通读取已使用的
HelperReadResult 分类。正常 BROKEN_PIPE、OPERATION_ABORTED 和无数据结束
沿用原有语义；已有首错不会被另一条流或后续格式错误覆盖。异步读取仍须
等待真实终态后才能清除 Pending、关闭句柄或释放缓冲区，原取消屏障和
字节/轮数预算保持不变。

微软说明 ReadFile 可以因为缓冲区无法锁页而返回 ERROR_NOT_ENOUGH_QUOTA；
同一文档也要求 Windows 7 的字节计数参数不能为 NULL。本次保留原有
`&count`。参见 [ReadFile 文档](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)。
故障在指定 SDK 边界注入，不宣称实机触发了资源耗尽；异步错误替身只在
真实 native I/O 已达终态后改变观测结果，不伪造 I/O 已完成。

该源码位于保留的 MAYBE_WOW64 分支，当前 x64 发布流水线未启用该宏。
定向测试明确启用并执行此分支，不能据此宣称当前 x64 发布二进制已出现
该问题或已经通过旧系统实机运行验证。修复复用既有分类函数，没有增加
生产锁、分配、线程、轮询或数据路径操作。

## 多队列排空与归还故障相交

新增组合回归固定实际 BeginDrain 采完队列 0、尚未采队列 1 的窗口。
另一真实归还调用经完成队列校验触发 Fault，排空随后仍可完成整个快照。
QUERY 必须使用故障路径的私有静止见证，而不是仅凭 SnapshotReady 确认
STOPPED。两种次序分别保留最后一个 native NBL 或最后一个 worker 见证，
验证 Active 归零和全部 FaultQuiesced 都是必要条件。

测试覆盖 2/8 队列，执行实际 Return、Flush、Fault、Query 和 worker 的
故障早停路径。没有手工写入 Fault 或私有引用/credit 来制造结果。每个
drain、query、return、worker 测试线程都核对实际线程 ID、退出码并汇合。
全部 NBL、MDL、池、引用与 credit 归还后才执行原宿主清理。

原 drain_boundary/kernel_nbl 宿主保持不变；只在忽略输出目录中将四个
队列数组的容量从 2 扩到 PB_MAX_QUEUES，并规范包含路径。11 份完整内核
头文件快照用于证据，编译直接使用原生产路径。原 NT_ASSERT=CHECK 是更强
的宿主检查，不等于实机断言行为。调度门闩使用宿主锁模拟另一 CPU 交错，
并非让实际内核在持有自旋锁时睡眠。

## 历史重放与验收范围

共享 helper-drain-ledger 对完整旧、新头文件及原始编码作精确检查，只接受
本次已审修复。16 项文本/文件契约包括额外改动及 BOM、UTF-16、换行差异。
四项完整依赖检查与一项历史容量投影接入该规则，旧基线及原正负对照保留。

```powershell
pwsh -NoProfile -File tools/audit/verify-helper-drain.ps1
pwsh -NoProfile -File tools/audit/verify-drain-fault-capture.ps1
pwsh -NoProfile -File tools/audit/verify-helper-response.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

新增回归进入普通及守护内存验收，定向矩阵覆盖 ISO/MS volatile。守护范围
仅限测试实际拥有的对象，SDK 内部资源不伪称受守护。最终结果、源码指纹、
历史证据及准确资源范围见 `out/retire-edge-investigation/final-review.json`。

Windows 管道、事件、锁、DLL 装卸和指定测试线程实际执行；内核 DDI、NDIS、
IRQL 及辅助进程退出边界仍由宿主提供。旧系统实机、Driver Verifier、长期
内核资源与 CPU 测量仍待专用系统验收。证据保持 `kernel_runtime_tested=false`
和 `production_ready=false`。
