# SYS 接收尾部的重复完成刷新

版本保持 `2.0.0`，PE 数值版本为 `2.0.0.0`。起始源码指纹为 `16b66e4aa3155ce038b5256c04ad08be2753c6aba28b3e5b13e3d75b25ad9c5e`。冻结接收函数保存在 `out/sys-receive-tail-baseline/16b66e4aa315/driver/v2/session_receive.h`，SHA256 为 `7d32566986ca866cec00fd2c3179055a87f6b73e2dbafe2eedd3dc9f4d2ee6bb`。

## 已确认的重复同步

`PbV2ReceiveBatch` 原来在调用 NDIS 指示或本地归还之后，无条件再次取得 RxLock 并刷新 pending。成功构造的 NBL 已经登记 Active，由实际 `PbV2ReturnNbls` 回调负责生成和刷新完成。普通成功批次的末次刷新没有新建的本地完成项，却仍执行一次锁获取/释放；同步归还已经刷新完成时也会再次执行。

宿主模型对实际函数计数：32 个普通成功描述符的接收函数自身原来取得三次 RxLock，修正后为两次。真实线程还复现了原函数在尾部等待一个已持有 RxLock 的归还回调；修正后接收函数可以在该回调继续前结束，其原生所有权仍由回调持有。

## 修正依据

复用已有的两个事务内数量：count 是本批快照描述符数，objects 是成功构造 NBL 所覆盖的原始描述符总数，合并 NBL 也按原始数量计入。只有 `objects < count` 时，接纳或构造失败才可能留下本批直接生成的完成项，需要原来的尾部刷新。

成功构造的对象仍全部交给实际指示/归还路径；构造中停止或故障造成的本地归还，也使用相同回调生成和刷新完成。已知 DRAIN/Stopping 的早期撤销分支继续在原位置刷新并返回。没有读无锁的 PendingCount，没有新增 dirty 标志、缓存、函数接口、原子操作、资源类型或调度机制。

| 场景 | 接收函数自身原 RxLock 次数 | 当前 |
| --- | ---: | ---: |
| 成功构造普通批次 | 3 | 2 |
| 成功构造 RSC/URO/混合批次 | 3 | 2 |
| 指示中同步归还全部对象 | 3 | 2 |
| 构造中故障、全部已构造对象本地归还 | 3 | 2 |
| 一组分配失败、仍有本地完成需刷新 | 4 | 4 |
| 空提交 | 1 | 1 |

以上只统计接收函数自身的 RxLock，不包含引用锁及归还回调自己的锁。实际 worker 下一轮仍按既有顺序取得 TransitionLock 和 RxLock；线程测试证明的是尾部同步点的省略，不能据此推导锁竞争等待时间的净收益或驱动 CPU 百分比。

## 旧 pending 和唤醒

`objects == count` 只能说明本批没有未构造描述符，不能证明队列过去的 pending 已经清空。初始 Flush 可能遇到满环或耗尽四批预算。因此修正保留这些进度依据：

| 待推进工作 | 依据 |
| --- | --- |
| 本批接纳/分配失败完成 | `objects < count` 保留原来的尾部刷新 |
| 成功 NBL 的完成 | 实际 NDIS 归还或本地取消回调自行刷新 |
| 初始 Flush 的预算余项 | Flush 保留 continuation；非空接收返回 TRUE，worker 下一轮先再次 Flush |
| 指示期间消费者释放旧满环空间 | 原有消费者 KernelEvent 保留；即使通知合并或已清除，本次非空返回也使 worker 继续一轮 |
| 最终满环且暂时无可用空间 | 原有消费者通知继续负责恢复，worker 不因本次改动新增轮询 |
| 故障和注销 | 保留私有 fault witness、worker join、AllReturned 和 Returns rundown 屏障 |

实际生产调用者仍是队列 worker，`PbV2ReceiveBatch` 非空时直接继续循环。没有把一个孤立的共享游标、计数零或超时用作释放依据。DLL ReleaseBatch、PollCompletions、Armed 握手、原生 credit 归还、槽位代次及空闲索引均未改变。

## 回归

`drain_boundary_tests.c` 继续执行实际 ReceiveBatch、共享核心、NBL 分配/归还、worker 和 QUERY，并增加以下验证：

- 普通、TCP/UDP/混合及最大合并对象的 32 项批次，分配失败、同步归还和构造中故障；接收 RxLock 计数、精确完成内容、统计和资源收支均核对。
- 初始完成环已满，指示中消费者释放空间，有和没有消费者通知两种时序；原始提交游标跨 uint64 边界。新函数返回时保留一条旧 pending，随后实际 worker 在一次空闲等待前推进完毕；NDIS 仍持有的 Active、槽位和 credit 不会被清退。
- 预留 257 条 pending 后再接收一个正常对象：原函数返回时余一条，当前余 129 条，实际 worker 都按既有有界刷新完成交付，再进入一次空闲等待。每个 Flush 的四批预算保持不变。
- 两个真实线程运行接收和归还。在实际归还已释放 NBL/MDL、刚取得 RxLock 时暂停它。旧接收函数尚不能返回，当前接收函数已返回；此时 record、Active 槽、credit、原生引用及 Returns pin 仍全部保留。继续回调后只有一次正确完成，线程、资源和句柄恢复。

新增 `AfterQueueLock` 仅属于宿主 DDI fixture，用于准确暂停上述锁区间，不进入 SYS。原来的本地取消锁计数改为在实际 rundown 取得/释放边界之间测量，去掉“总锁数减去一次尾部刷新”的假设；新旧及历史函数仍核对各自真实返回窗口数量。历史接收函数继续运行其原有回归，新尾部行为由独立冻结 A/B 对照。

定向证据为 `out/sys-receive-tail/fe483ea9e00f/evidence.json`。`verify-sys-receive-tail.ps1` 固定旧接收函数哈希，使用相同当前 helper，在 ISO/MS volatile、普通/保护页模式执行新旧八个实际测试进程，保存退出码、日志和源码/二进制哈希。新回归也由既有 Tests/Safety 入口运行。

```powershell
.\tools\audit\verify-sys-receive-tail.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

完整离线验收固定最终源码指纹，重新构建 SYS/DLL/probe，检查静态分析及原有槽位归还、暂停回收、唤醒、故障和卸载回归。本轮没有安装、签名或加载 SYS；真实内核 CPU、IRQL、NDIS/LWF、Verifier、ETW/WPA、低资源和长稳仍须实机验证，清单保持 `kernel_runtime_tested=false`、`production_ready=false`。
