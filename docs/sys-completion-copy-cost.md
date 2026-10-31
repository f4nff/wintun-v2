# SYS 完成发布的中转复制

起始源码指纹为 `2aa1358ac8daf7afa98640d8910bd3b3d3c9460501b07bda091b19396a92a392`，完整快照保存在 `out/sys-completion-copy-baseline/2aa1358ac8da`。

## 问题与修改

`PbV2FlushLocked` 原来在 RxLock 内将私有 pending 环逐条复制到 `PB_DESCRIPTOR items[32]`，再由 `PbQueuePush` 将同一批描述符复制到共享完成环。每个描述符为 64 字节，一满批增加 2 KiB 的中转写入及其后读取；即使完成环已满，原函数仍先复制这一批再得知无法发布。这是确定存在的复制与栈空间开销，尚无内核 CPU 时间测量。

共享核心增加 `PbQueuePushRing`，从调用者持锁的私有循环缓冲区直接复制至目的环。它检查源容量为不超过 4096 的二次幂、head 有效、请求不超过源容量及原有 32 条批次限制。源和目的可以独立回绕，源 head 不由核心修改。调用者负责源有效期、串行访问以及源和目的互不重叠。

连续数组入口 `PbQueuePush` 和循环源入口共用一个强制内联的发布实现：先验证对端 consumer，根据实际空间复制已接纳前缀，最后只发布一次 producer 并交换一次 Armed。连续数组的源掩码为全 1，编译器可以消除其额外索引运算。旧入口的参数、返回值及通知约定保持不变。

Flush 直接传入受 RxLock 保护的 Pending、PendingHead 和容量，只有成功发布后才按 accepted 更新 pending/head/credit。故障时的私有完成清退、真实 NDIS 引用和回调 rundown 不变。仍然每次最多四批、128 条完成；预算耗尽安排后续处理，满环等待消费者释放空间，暂停状态仍处理完成。

## 验证

- `queue_ring_tests.c` 使用生产共享核心。74,592 组保护页用例覆盖 1 至 4096 条源环、独立源/目的/uint64 回绕、部分接纳、满环及通知；源页设为只读，逐字节对比连续数组入口和预期描述符，并检查每次成功仅一次 producer 发布和 Armed 交换。另测非法边界、空参数、损坏 consumer，无发布副作用。
- 两个真实宿主线程分别通过容量 1 和 64 的共享环传递各 200,000 条描述符，以真实事件 arm/wait、生产者重用私有源和消费者立即重用共享槽验证顺序及全部字段。
- `completion_progress_tests.c` 执行实际 Flush/worker。普通和回绕各 4096 条完成，旧函数中转复制各为 262,144 字节，新函数为零；核对相同的 32 个处理回合和用户唤醒次数。覆盖源/目的独立回绕时部分接纳后继续处理、满环不自旋、损坏环故障收尾、暂停期间的唤醒和跨三个预算排空。pending 存储后接不可访问保护页。
- `verify-sys-completion-copy.ps1` 从冻结快照提取旧完成函数，配合相同宿主 DDI、worker 和当前核心执行 A/B；两组完成回归和共享核心回归均以 `/volatile:iso`、`/volatile:ms` 编译运行。它记录实际退出码、源码及二进制哈希，不将复制计数当作内核性能数据。
- Tests、Safety 已纳入这些用例；完整离线验收通过 `tools/audit/validate-dll-lifecycle.ps1` 执行，并冻结一个源码指纹。

没有新增轮询、后台任务、内存缓存或超时时间。未发现需要改变等待期限的新证据；现有事件和实际所有权释放屏障继续由回归覆盖。仍须在真实驱动环境验证 IRQL、Verifier、ETW/WPA 和长稳；没有安装、签名或加载 SYS。

```powershell
.\tools\audit\verify-sys-completion-copy.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```
