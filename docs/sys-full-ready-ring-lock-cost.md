# SYS 满 TX_READY 环的空窗口锁开销

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮起始完整验收指纹为 `496e59eac84b3b09290b50a6acf442910e3f16cfba1226097c56852dc935e85d`。旧 `session_send.h` 保存在 `out/sys-ring-pressure-baseline/496e59eac84b/driver/v2`，SHA256 为 `e07403f4d34dce82c9438fb274e750df64f5c08eaee0c0111aae9e27a4b2adc1`。

## 已确认的问题

实际 `PbV2Send` 在每次尝试前检查当前批次是否用完已观察到的空间。旧条件直接比较 `w.Count == w.Space`。当 TX_READY 持续满环时，两者均为零，空窗口也进入发布/解锁/重新获取 TxLock 的路径；实际没有描述符需要发布。

宿主执行实际 send/recycle/reference/core 复现：同队列 4,096 次 64 字节失败尝试执行 4,096 对 TxLock 获取/释放。每次仍是同一个满环，槽没有复用，所有 NBL 都以原 RESOURCES 状态完成。这是额外的锁调用成本，不能据此计算真实内核 CPU 百分比。

修改前，新增对照用例先确认旧实现的精确操作计数和原有回归，再用当前未修正实现复现新计数断言失败。日志为 `out/sys-ring-pressure-work/pre-fix.log`。修正后的定向证据之一为 `out/sys-ring-pressure/9ba941c04c27/evidence.json`；最终完整验收会记录最终同一源码指纹下的证据。

## 修正及边界

仅当存在待发布项时，才用 `Count == Space` 判断是否要发布并结束当前队列窗口。空的满环窗口沿用现有有界 TxLock 窗口。

将回收和空间刷新从“刚获取该队列锁”移到“当前观察空间为零”的分支：新获取的窗口本来就是零空间；持续满环时，每次尝试继续执行原有回收和空间检查。独立用户消费者不取得 TxLock，可能在持锁期间释放 TX_READY 空间或发布归还。不能把零空间缓存到窗口结束，也不能由共享游标变化直接授权槽复用。

正常有空间、非空前缀发布、队列切换、控制回复、失败和引用释放的顺序保持原样。没有增加缓存、标志、队列、等待或重试，也没有修改 DLL、公共核心、共享布局、原子发布或 Armed 握手。

现有每窗口至多 32 次尝试、128 KiB 声明长度收费及每次回收的 4×32-token 预算保留。空窗口现在会持有 TxLock 至有界窗口结束；这里不声称每次锁持有时长与旧实现相同。真实内核锁争用、尾延迟和 DISPATCH_LEVEL 时长仍须实测。队列切换仍释放当前锁，控制上调和完成回调仍在解锁后执行。

## 对照结果

每组均为 4,096 次实际发送尝试。锁对数来自实际 send 对 DDI 替身的调用；回收轮询和空间读取分别计数。

| 场景 | 旧 TxLock 对数 | 当前 | 回收轮询 / 空间读取 |
| --- | ---: | ---: | ---: |
| 同队列、64 字节 | 4,096 | 128 | 各 4,096 |
| 八队列交替、64 字节 | 4,096 | 4,096 | 各 4,096 |
| 同队列、65,536 字节 | 4,096 | 2,048 | 各 4,096 |
| 同队列、UINT32_MAX 声明长度 | 4,096 | 2,048 | 各 4,096 |

大长度继续按 PB_BLOCK_SIZE 收费，128 KiB 预算使每窗口至多两次尝试。小包最多观察到每次 TxLock 持有期间额外 31 次读取；既有预算仍约束整个窗口。满环场景要求零 reserve、零 Describe、零新发布、零 UserEvent 通知，全部私有 token 的代次/状态不变，拒绝原因精确保持 QUEUE_FULL，不设置 pool-full 缓存。NBL/NB 输入数组紧邻保护页。

另有八种普通/回绕的迟到 peer 时序，在第二次尝试前分别释放 ready 空间并归还 token、仅释放 ready 空间且已有已验证的空闲槽、损坏 recycle Producer、损坏 ready Consumer。新旧实现都必须及时使用真实空间/归还，或者保留原错误、停止后续接纳；精确核对代次和发布数，不能误用迟到但未校验的 token。

再有两种普通/回绕时序，在 ready 仍满时逐次归还两个合法 token：每次回收仍发生，私有槽成为 FREE；ready 空间尚未释放，不能 reserve 或发布。这些是原始 ABI 的独立游标条件，不表示用户可以继续读取已归还的 view。

上述 14 组定向场景和现有完整 send 回归在 ISO/MS volatile 模式下对照冻结/当前实现，共八个实际测试进程。现有回归继续覆盖池满缓存、迟到归还、损坏 token 与私有快照、前缀发布、暂停/脱离/故障预算、控制回复、重入、完成后不可访问输入，以及 Active/rundown 尾部。实际 Windows 事件、回收/关闭及其他 SYS/DLL 路径继续由完整验收覆盖。

## 验收

```powershell
.\tools\audit\verify-sys-ring-pressure.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

新场景接入现有 Tests/Safety 的 send-window 入口，完整验收增加 SysRingPressure。沿用现有窗口和验证流程，修改范围保持在空满环的锁生命周期及其回归证据。

本轮未安装、签名或加载 SYS。结果是宿主操作计数和离线构建/同步验证；真实内核 CPU、锁争用、NDIS/IRQL、Verifier、ETW/WPA、低资源与长稳仍待实机验收，`kernel_runtime_tested`、`production_ready` 保持 false。
