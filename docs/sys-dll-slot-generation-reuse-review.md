# SYS / DLL 同槽换代与旧归还尾部复核

版本保持 **2.0.0**，PE 数值版本保持 **2.0.0.0**。从已验收指纹 `f17fdf97667fbc6ea9cd8675108b90c0b128a6ade7d149b1eb03651e97a9ca62` 继续检查，本轮未确认新的生产缺陷；104 个生产源码及版本属性文件逐项保持一致。变更限于测试、构建接线、审计脚本和本说明。

## 核对结论

RX 完成项交付后，DLL 可以立即归还私有池中的旧 token，并以更高 Generation 再次提交同一槽位。此时旧 NDIS return 回调可能仍未退出，甚至尚未归还自己的 Active 引用。这是允许的交错，不应通过等待整个旧回调退出才能复用槽位来串行化收发。

当前 `PbV2ReturnNbls` 在 RxLock 内清除旧槽位 Active、提交并发布完成项。随后不再修改该槽位。旧 NBL 和两个 MDL 已经释放；合并缓冲区的 Busy 也已在发布前释放，尾部只使用私有 record 的元数据。新一代槽位和同一个合并缓冲区因而可以在旧回调尾部仍存在时被安全使用。

会话 Active 统计 NBL／已接纳发送调用，不按 RX 槽位代次计数。新一代保留引用与旧一代释放引用都经 ReferenceLock 串行化；AllReturned 的置位／清除也在同一锁下处理。旧回调自己的 Returns rundown pin 则保护余下通知和尾部。这里的槽位归还、Active 归还和物理回收是不同边界，不能相互替代。

## 实际函数组合回归

扩展 `tests/model/return_reclaim_tests.c`，保留原有 64 个清理交错，并新增 72 个同槽复用场景。每个场景直接执行生产 DLL `ReserveBatch`、`CommitBatch`、`PollCompletions`，以及 SYS `PbV2ReceiveBatch`、`PbV2ReturnNbls`、引用、完成发布、BeginDrain、QUERY 和最终 Free。共享区域、两侧私有池、真实 Windows 锁／事件／线程由 fixture 组装；NDIS 指示和 DDI 使用宿主替身。测试中的 StatusInternal 仅桥接实际 SYS QUERY，不运行完整 DLL 生命周期或设备 I/O。

72 个组合为三种数据路径 × 三个旧回调暂停点 × 两种归还顺序 × 两种 KernelEvent × 两种游标起点：

- 普通 IPv4 TCP 单包、四个原始包的 IPv4 TCP RSC、四个原始包的 IPv6 UDP URO。合并走实际解析、校验和与 NBL 构造路径。
- 在旧回调释放 RxLock 后、释放 ReferenceLock 后、释放 rundown 前暂停；暂停点均不持有这些锁。它们分别覆盖旧 Active 尚存，以及旧 Active 已为零但尾部仍受 rundown 保护的阶段。
- DLL 实际消费第一代完成项，并立即将同一组槽位全部以第二代提交；先让旧回调退出，或先归还第二代 NBL。
- 同步／手动复位 KernelEvent，正常／跨 `UINT64_MAX` 回绕的提交与完成游标。

断言包括两代完成项的 epoch／generation／slot／status、DLL Loans／Published／私有池状态、SYS Active／AllReturned／credit／pending、旧 token 不得再授权完成、新一代槽位 Active、持有期间的数据字节和合并缓冲区租约。每场景两次实际 return rundown 获取／归还、两次满 credit 到可用的原生 KernelEvent 通知，以及两次已 arm 的 UserEvent 通知均精确计数。

开始 drain 后，第二代仍持有 NBL 时 QUERY 必须保持 DRAINING。第二代先归还、旧回调已释放 Active 时，QUERY 可以返回 STOPPED，同时旧 rundown pin 仍存在。两个回调结束后才执行本组场景的最终 Free，检查全部跟踪资源和进程句柄归零。真正与清理线程并发的 AllReturned／rundown 屏障仍由保留的 64 个场景验证，见 [归还、QUERY 与回收复核](sys-return-query-reclamation-review.md)。本轮不启动真实或宿主 queue worker。

## 反例、构建与维护收益

`tools/audit/verify-sys-return-reclaim.ps1` 在 ISO／MS volatile、普通／守护内存下重编译四个当前进程，每进程执行 64 + 72 个场景。保留八个删除单个清理屏障的反例，新增八个仅存在于 `out` 的错误回调：分别在 RxLock 释放后的尾部再次清除槽位 Active 或合并缓冲区 Busy。新增反例仅运行同槽复用组合，必须以退出码 1 命中对应所有权断言；崩溃、超时或其他断言不算通过。总共记录 20 个进程，包含 16 个预期拒绝。

所有反例由当前 header 生成，生产 header 不变；脚本保存生成文件、可执行文件哈希和各进程输出，并校验源树指纹。标准 Tests、Safety 和完整生命周期审计继续通过既有入口接入，不再新增独立验证入口。由于测试现在执行实际 ReceiveBatch，构建仅增加已有 coalesce／packet 源文件链接。

清理 WSL 空间和编译缓存后，原生 Windows MSVC／SDK／WDK 环境仍可初始化。实际构建和检查通过仓库脚本重新生成所需产物，结果按新源码指纹保存；不依赖旧可执行文件的通过记录。

本轮没有增加生产锁、轮询、定时器、状态或热路径指令，也没有删除现有同步屏障。新覆盖使后续降低归还成本时能够自动发现跨代清位和提前释放缓冲区租约的回归；没有可靠收益证据时保留当前实现。通知计数只验证该调度下的调用次数，不代表 worker 唤醒次数或真实 CPU 降幅。

没有安装、签名或加载 SYS。宿主回归不能替代实际 NDIS、IRQL、rundown、Verifier、低资源、长稳和内核 CPU 测量；`kernel_runtime_tested=false`、`production_ready=false`。
