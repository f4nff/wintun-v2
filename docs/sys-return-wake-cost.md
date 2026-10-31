# SYS 归还路径的 worker 唤醒

起始源码指纹为 `b260b7c381cb7b36ede826181d12c7c0da6a3a8b8ef5628da4488f8f4b580a4b`，完整快照保存在 `out/sys-return-wake-baseline/b260b7c381cb`。

## 问题

`PbV2ReturnNbls` 原来在每个归还窗口结束时无条件设置队列 KernelEvent。即使队列一直有可用 credit、RX_SUBMIT 为空、所有完成已经发布，也会让等待中的 worker 再执行一次回收、空接收和 arm/recheck，然后重新睡眠。暂停期间也会触发一次无实际新工作的完成扫描。

使用实际 worker 和返回函数、真实宿主线程及事件可复现：一个容量为 8、仅持有 1 个 NDIS NBL 的队列，归还前已经空闲。旧函数的归还产生一次 KernelEvent 写入，使空接收扫描由 1 次增加到 2 次、进入 idle wait 由 1 次增加到 2 次。这些计数不代表内核调度时间或 CPU 利用率。

## 修正和等待依据

归还函数在已有 RxLock 区间内记录归还前 credit 是否已满，执行既有完成刷新后，检查是否由“已满”变为“有空间”。只有这个转换才安排原有的返回尾部 KernelEvent 通知。worker 也在同一 RxLock 下检查 credit room 后决定等待。

| 等待原因 | 继续工作的通知来源 |
| --- | --- |
| RX_SUBMIT 为空 | 提交生产者的既有 arm/recheck 通知 |
| RX 已有工作，但 credit 全满 | 本次在 RxLock 下确认的 full → usable 转换通知 |
| 完成环满，pending 无法发布 | 完成消费者实际释放空间后的既有 KernelEvent 通知 |
| completion 四批预算耗尽 | Flush 自己安排的 continuation |
| NDIS 仍持有引用 | 真实归还更新 Active，最后一个引用发布 AllReturned 和排空进度 |
| 停止或故障 | StopEvent 或 Fault 的既有终止通知 |

如果归还生成的完成仍因满环留在 pending，credit 没有释放，这次归还不会伪装成可用空间。消费者随后释放空间并唤醒 worker，Flush 再发布 pending、释放 credit 并继续接收。若多个归还并发，RxLock 保证只有真正观察到 full → usable 的窗口负责任何必要通知；它的 Returns rundown pin 仍覆盖通知和最后回调尾部。

没有增加状态缓存、后台任务、定时轮询或等待期限，没有修改任何退出屏障。槽位清退、完成 token/顺序、统计、合并缓存释放、Active 递减及 rundown 次序保持原有规则。

## 回归

- `drain_boundary_tests.c` 新增真实线程等待：正常和暂停空闲归还不再触发多余扫描；在 arm 后、真正进入 wait 前归还，也验证事件不会丢失。满 credit 且生产者已 ready、Armed 已清除的 worker，必须只靠真实归还唤醒，继续处理 3 条既有提交后再次进入 idle。普通运行和正常 DRAIN 均覆盖。
- 完成环与 credit 同时全满时，真实归还保留 pending/credit，worker 保持等待；测试只在消费完成并发出消费者通知后允许继续，验证 pending 和原有提交随后推进。剩余 NDIS NBL 持有的实际引用继续阻止 STOPPED。
- `return_batch_tests.c` 继续覆盖普通/合并对象、失败、混合队列、外来 tag、满完成环和故障。两个真实返回线程并发归还 64 个唯一 token：容量 128 时两次无条件唤醒降为零，容量 64 时恰有一次 full → usable 唤醒。所有资源、句柄、统计和引用核对不变。
- 既有故障清理测试仍在 Active 已归零、回调尚未释放 Returns rundown 的时刻启动清理，确认父 MDL 不会提前解锁。
- `verify-sys-return-wake.ps1` 使用冻结的旧归还函数与当前归还函数，配合同一 NBL 构造、完成/引用/worker/QUERY/共享核心，在普通和保护页模式执行 A/B，记录实际退出码、日志及源码/二进制哈希。新增回归已由 Tests/Safety 的既有测试入口执行。

槽池稀疏空闲时的线性扫描仍是待评估成本；空闲索引需要同时覆盖内核分配、用户态镜像导入和故障记账，当前没有添加未经这一完整生命周期验证的缓存。

```powershell
.\tools\audit\verify-sys-return-wake.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

离线完整验收固定一个源码指纹。没有安装、签名或加载 SYS；真实 IRQL、NDIS/LWF 调度、Verifier、ETW/WPA、低资源及长稳仍需内核验收。
