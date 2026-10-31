# DLL 损坏完成的空间通知与关闭归还

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮起始源码指纹为 `ba18cd0579535787b65e2767189b4119148a3004cfdcc04871d467a50c375c3a`。原队列/session/lifecycle/state 快照保存在 `out/dll-close-wake-baseline/ba18cd057953/api/v2`，旧队列 SHA256 为 `5c5c8bc032b5b52750182c80d98c21259c094ce0322a75599a00ed5fe44eb950`。

## 损坏完成释放空间后没有唤醒生产者

`PollInternal` 先成功 Pop 并发布 Consumer，再验证整个 token 批次。旧路径发现错误 token 后直接返回 PB_CORRUPT，没有设置 KernelEvent。虽然私有槽不能退休，完成环空间已经释放；若 SYS 正因满环停放 pending，且 DRAIN 已经确认，DLL 后续只查询状态而不会再次发送 DRAIN，原生 pending/credit 可以一直滞留。

组合回归使用实际 DLL 消费、SYS 接纳/NBL/归还/Flush/DRAIN/QUERY helper 和真实 Windows 事件。有效原始 NBL 仍持有时，原始 ABI 的八个非法 epoch 提交产生拒绝完成并填满环；确认 DRAIN 后实际归还原 NBL，留下 pending=1、credit=1、原生引用=0。旧 DLL 消费非法完成后生产者仍在等待，实际原生 QUERY 不能确认 STOPPED，PrepareClose 保持隔离；显式补发 KernelEvent 才能恢复。

当前在已成功 Pop、但整批 token 校验失败的分支也发送 KernelEvent。仍返回 PB_CORRUPT、count=0，整个私有池和 Published 保持不变。SYS 因空间通知再次 Flush，真正清空 pending/credit，实际 QUERY 才确认 STOPPED；DLL 随后依据该确认归并 bookkeeping。该组合场景将实际 SYS QUERY 接入 DLL StatusInternal，关闭 TestActive 的模拟确认捷径，避免把“引用为零”误当成“已排空”。这仍是宿主 DDI 模型，不是已加载的驱动。

## 仅内核故障、回收环满时的用户 loan 滞留

旧本地归还捷径仅依赖 DLL Fatal；仅内核报告终止、DLL 只停止接纳时，一个不再合作的满 TX_RECYCLE 环仍使 ReleaseBatch 返回 PB_FULL，用户 loan 无法清退，PrepareClose 持续 PB_BUSY。回归保留未损坏的合法 loan，只在共享回收环中填入原始 ABI 的垃圾数据，确认了这个故障边界。

当前仅在 PB_FULL 且 CloseRequested 已永久停止接纳时，复用已有本地退休路径。先完整校验用户 token，再清退 DLL 的 view/loan；不发布回收项，不释放或复用 SYS 私有槽，物理释放仍必须经过原生停止和设备关闭屏障。运行中的 PB_FULL 保留原有背压，正常有空间的停止后归还仍发布回收项及必要通知。

## 无所有权进展调用的重复通知

原 `FinishQueueCall` 在 CloseRequested 为真时，无论调用返回成功、空队列、已停止还是无效 token，都设置 ProgressEvent。关闭线程仍被真实原生引用阻塞时，这些没有改变所有权的请求也会唤醒线程，使其再次轮询完成、检查所有权并查询原生状态。

实际宿主 session/queue/lifecycle 复现：在原生 QUERY 边界暂停关闭线程，分别调用 100 次空 PollCompletions、已停止 ReceiveBatch/ReserveBatch/CommitBatch 和陈旧 CancelBatch/ReleaseBatch。600 次调用生成 600 次真实 ProgressEvent SetEvent，而 Loans/Published 均没有变化。事件合并后仍使实际 Windows wait 返回并多查询一次原生状态；这不是 600 次 QUERY，也不是内核 CPU 百分比。

## 修正

仅在批量队列调用返回 PB_OK 且已请求关闭时，由公共调用尾部通知生命周期。现有六个有限批量入口只有实际借出、归还、预留、取消、提交或完成消费才成功返回 PB_OK；部分接受也继续以 PB_OK 报告真实前缀。

| 通知来源 | 当前规则 |
| --- | --- |
| 空完成、停止接纳、陈旧/无效 token、无空间且没有接受前缀 | 不产生公共尾部进度通知；已停止且回收环满的本地归还因实际清退 loan，仍是成功进展 |
| 成功归还/取消、完成消费、关闭竞态中已成功接纳的操作 | 继续产生进度通知 |
| 首次 PB_CORRUPT | 原有 FaultSession 在 Fatal/CloseRequested 改变时通知；重复失败没有新所有权变化，不重复通知 |
| QueueEnter 首次观察到内核终止提示 | 保留原有停止及进度通知 |
| Wait 已进入等待后首次观察到内核终止提示 | 将首次 CloseRequested 转换的进度通知显式放在该分支，保留原先通过公共尾部发出的通知 |
| BeginStop/Close/PrepareClose 的显式控制请求 | 保留其原有通知，仍能立即请求失败重试 |
| 最终关闭线程等待外部 API 的 Lifetime pin | 原有 SRW shared release 直接解除 exclusive 等待，不依赖 ProgressEvent |
| 原生引用、完成及终止状态变化 | 原有内核进度事件和一秒诊断回退均保留 |

没有增加事件、标志、缓存、线程或超时，也没有改变共享布局、游标或 Armed 握手。KernelEvent 按实际完成环空间释放发送，包括 token 校验失败的已消费批次；空队列和游标校验失败、尚未消费的情况不新增通知。

保留整批 token 校验、私有池代次/空闲索引维护和真实 STOPPED/Active、设备关闭及内核 join/AllReturned/Returns rundown 屏障。本地归还在上文的停止后满环分支扩展；accepted 表示用户 view 已归还，与原 Fatal 捷径一样，不一定表示写入了共享回收环。正常共享发布仍只接受实际前缀。

## 对照回归

新增 `close_wake_tests.c` 直接包含实际 DLL session，用真实 Windows SetEvent 和 WaitForSingleObject，并在已知 DRAIN/QUERY/park 边界控制调度。启动通知在控制点被清除；检查自动复位事件状态后，仅为旧实现恢复它原本已有的合并通知，再执行真实等待。没有替换协议同步操作或缩短原有诊断等待。

| 一次受控场景 | 旧 ProgressEvent 调用 | 当前 |
| --- | ---: | ---: |
| 600 次无所有权进展请求 | 600 | 0 |
| 合法 RX cancel | 1 | 1 |
| 已 arm 回收消费者的合法 TX release | 1 | 1 |
| 合法完成消费，包状态 0 或 PB_RX_REJECTED=4 | 1 | 1 |
| 17 次损坏完成 | 18 | 1 |

表中统计 DLL 的 ProgressEvent。17 次损坏完成仍实际释放了空间，因此当前同时产生 17 次必要 KernelEvent；旧实现为零，正是上述停滞根因。这些通知分别代表本地所有权/停止变化和原生完成环空间，不能按整个 API 返回值一并省略。

无进展请求保留原生引用、映射及未准备状态；有进展操作确实使实际关闭线程继续，原生引用仍独立阻止准备。TX 回收通知和成功完成消费的 KernelEvent 计数一致。包拒绝状态 4 仍是一次成功消费的包结果，不能混同 API 级 PB_CORRUPT。

实际 Wait(INFINITE) 与终止发布线程验证首次本地停止转换仍产生进度通知。既有 API/lifecycle 测试还继续检查 Wait 取消后 Lifetime pin 释放、跨队列故障、阻塞 I/O、失败重试和物理关闭。

## 异步进度快照测试修正

完整 A/B 在原 `FailedBarrierRetainsResources` 断言处发现一次既有测试竞态。Close 返回 PB_CORRUPT 后，工作线程可能已经消费启动通知并进入下一次重试；紧接着的 GetCloseProgress 是新的观察，可能读到重试中的 PB_QUARANTINED/LastError=0，而不是前一次失败。

新增可控回归对旧、新队列配合同一个未修改的 lifecycle 实现，都确定性观察到“失败及原始错误 → 重试中隔离 → 失败及原始错误”的序列，期间映射和原生屏障始终保留。因此修正原测试：持续核对 RetainedBytes，等待有界时间内发布的 PB_CORRUPT 快照，然后严格核对原始错误及隔离标记。没有将当前快照假定为早先 Close 返回值的延续，也没有放宽真实失败错误码或资源保留要求。

测试的公共 Close 仍执行实际资源屏障；仅保留一个测试 shell pin 来 join 回调，随后检查会话预算和保护页分配归零，所有测试同步句柄显式关闭。

## 证据与范围

早期无进展通知证据为 `out/dll-close-wake/97d40c4bf38c/evidence.json`；加入实际 SYS 空间停滞及满回收环归还的定向证据为 `out/dll-close-wake/51fd88d79cb9/evidence.json`。`verify-dll-close-wake.ps1` 核对四个旧文件哈希，以相同当前 lifecycle/core 比较实际旧、新队列，在普通/保护页和 ISO/MS volatile 模式执行八个通知/故障测试进程，加上八个实际 API/lifecycle 回归进程。旧实现明确按预期停滞及显式恢复核对，新实现必须自动推进。新用例接入 Tests/Safety，完整验收增加 DllCloseWake。

```powershell
.\tools\audit\verify-dll-close-wake.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

这些结果确认真实系统调用和无进展唤醒的减少，没有将其换算为整体 CPU 或吞吐收益。本轮未安装、签名或加载 SYS；真实内核 CPU、IRQL、NDIS/LWF、Verifier、ETW/WPA、低资源及长稳仍须实机验收。版本、ABI 和记录布局不变，清单保持 `kernel_runtime_tested=false`、`production_ready=false`。

后续增加停止后 TX 归还的八种满环/部分发布/终止提示/游标回绕组合，核对整批授权、跨线程拒绝及 SYS 只回收实际发布前缀；同时记录未采用的 Consumer 发布微基准候选。该后续轮次没有修改生产实现，见 [停止后归还边界复核](stopped-release-boundary-review.md)。
