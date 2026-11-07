# 停机进度与所有权复核

版本保持 2.0.0，DLL/SYS 数值版本为 2.0.0.0。本轮基线为
`124a7befbedbec955525a83c80825404b48b386da2613f478a47bd1faff8b386`，
568 个文件的完整快照位于 `out/stop-progress-baseline/124a7befbedb`。

本轮没有确认新的生产缺陷。补测现有路径交叠时的所有权和进度约束，生产同步、
旧接口、能力门控及热路径保持不变。安全负控检验断言能否识别特定遗漏，不是
当前实现存在缺陷的证据。

## 在途策略、同步 Drain 和队列 owner

实际非 PB_API_TESTS DLL 的非零策略 I/O 已进入并持有 Control 时，让同步
Drain 执行 BeginStop，同时唤醒 1、2、8 个队列 owner 的实际无限 Wait。
每个 owner 各持有一个 TX loan 和一个 RX reservation，由原线程归还和取消。
策略分别迟到成功或失败，真实 DRAIN I/O 必须在策略 I/O 结束后才能进入。

同步 Drain 在真实条件变量上等待；单独的防御用例在该边界注入一次
ERROR_OPERATION_ABORTED，保留失败后仍持 State 的契约。失败调用必须退出，
不能消费在途策略票据，也不能在持 State 时反复重试。这个输入不是自然观察到
的 Windows 故障，SDK 调用入口标记也不代表已观察到内核调度器的等待状态。

策略完成后，Drain 可在 loan 和 NativeActive 仍非零时返回 PB_OK；它只确认
DRAIN 已完成。owner 全部汇合后，独立的原生引用仍必须阻止物理回收。最终
Close 只在公开调用者全部退出后执行，随后汇合真实回调线程并观察 DLL 卸载。

省略 Drain 非超时等待失败分支 break 的变异会再次到达等待入口。观察后调用
真实 SleepConditionVariableSRW 释放并重新取得 State，再放行策略和实际 Drain。
所有调用者、回调、资源和 DLL 卸载完成后才返回 123，不编辑引用或 Draining
计数，也不在持 State 的门闩里要求其他线程取得该锁。

完整进程先预热再检查进程句柄数相等。单个 CLI 用例逐项检查拥有的资源、句柄
关闭、线程汇合和 DLL 销毁；首次使用可能建立的 Windows 共享缓存不作为该 CLI
的全进程句柄相等断言。两种检查的范围分别记录，不放宽私有资源账本。

## 停机快照后的迟到 Commit

使用当前实际 SYS drain、receive、query、unregister、free 和 DLL 的完整队列与
生命周期源码。REGISTER 由既有宿主构建 ready peer，原生线程只执行 start/stop
等待；这个组合不是实际驱动安装，也不执行真实 NDIS 或内核 worker 调度循环。

queue owner 已通过 Commit 准入检查、尚未发布 Producer 时停住。BeginStop 后，
实际 SYS DRAIN 捕获旧 Producer 快照；随后原 Commit 才发布并把 loan 转为
Published。最后一次写入结束但 DataGate/Lifetime shared 尚未释放时，生命周期
排他快照仍必须等待。原生 drain 不能消费快照之后的条目，QUERY 可以依据已结束
的原生所有权返回 STOPPED；DLL 仅在该可信确认后把 Published 退回 FREE。

覆盖 1、2、8 个队列，批宽 1、2、7、63、64，自动/手动事件及 uint64 游标回绕。
确认归还后没有增加 generation 或再次准入，所有公开 owner 先汇合，再最终关闭。
真实 SYS Free 在 DLL 映射释放前完成，原宿主的各类资源账本和线程句柄均收敛。

停止后即使 Producer 有迟到条目，实际 SubmissionRoom 仍为零，ArmSubmissions
也必须报告 ready=0。负控只改变抽取函数的停止门控，使其调用普通 PbQueueArm，
观察 ready=1 后继续安全退出，最终返回 124。它检验可能造成空转的就绪条件，
不运行无限循环，也不冒称实际内核 CPU 测量；原生游标、引用及所有回收屏障不变。

短样本曾在本机第二次实际 BCryptGenRandom 调用中观察到一个持久 SDK
WaitCompletionPacket 增量。两个不含项目代码的对照在预热后，后续 4096 次 CNG
调用、不同缓冲区、新线程和私有线程池均无继续增长；两个完整预热后重放原用例
也保持句柄相等。这与 SDK 分阶段首次缓存一致，未确认逐会话生产泄漏。没有关闭
未知 SDK 句柄或更换随机数来源。完整矩阵预热 60 次再测量 60 次，CLI 预热两个
完整用例后再测量一个；两者都保留原有进程句柄相等及私有资源归零检查。

## 取消后的迟到适配器结果

实际 WintunOpenAdapter、RunAdapterOperation、OpenAdapterInternal 和完整
main.c 执行当前源码。SDK 宿主在 Populate 已成功、Open 仍未返回的边界设置
门闩；这时实际安装互斥量、设备信息集合、适配器和三份运行时租约均仍有效。

通过实际 MainRuntimeBeginShutdown 和 OperationStop 取消公开等待，调用方
返回 ERROR_CANCELLED 并汇合。默认 120000 毫秒期限保持不变，没有缩短期限
来制造取消。此时零预算 shutdown 仍不能把在途原生操作当成已回收。

公开调用方汇合后，放行 SDK 让实际 Open 迟到成功；实际 DisposeAdapterOperation
调用 WintunCloseAdapter 关闭适配器，worker 停在最后操作租约的归还边界。
完成所有公开调用和 fixture 观察后再归还普通加载引用，实际回调模块引用独立
保留 DLL。随后仅用宿主 SDK 放行最后的实际 Release、操作回收和 DLL detach。
销毁检查原有执行器与 SDK 资源账本，宿主汇合实际回调 OS 线程。

省略迟到 Dispose 的变异在 worker 最后一次实际 MainRuntimeRelease 调用前
停住。设备信息集合与安装互斥量已经释放，但无人认领的适配器及 RuntimeOwned
租约仍在。普通加载引用有效时，用真实 WintunCloseAdapter 恰补偿一次，再放行
worker 的真实 Release。正常构建不做补偿；所有线程和卸载完成后才返回 125。
运行时引用计数不由测试手工修正，生产源码只在负例副本中省略唯一 Dispose。

## 验证及边界

```powershell
pwsh -NoProfile -File tools/audit/verify-drain-policy.ps1
pwsh -NoProfile -File tools/audit/verify-late-commit.ps1
pwsh -NoProfile -File tools/audit/verify-operation-adapter.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

ISO/MS volatile 与普通/守护内存组合记录完整生产片段、有限宿主适配、负例
变异和产物哈希。重用的宿主与资源账本保持原样并锁定哈希。最终验证比较同一
源码指纹下的旧导出及序号、导入集合、数值版本和既有静态诊断；记录位于
`out/stop-progress-investigation/final-review.json`。

SDK 设备发现、原生设备 I/O、NDIS、内核调度与 IRQL 仍是宿主边界。本轮不安装
驱动或修改系统配置。真实旧系统、Driver Verifier、持续内核池、句柄、线程及
CPU 验收仍需专用系统完成，证据保持 `kernel_runtime_tested=false`、
`production_ready=false`。生产路径没有新增锁、线程、定时器或周期唤醒。
