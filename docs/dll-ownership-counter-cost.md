# DLL 私有所有权计数的串行发布

版本保持 `2.0.0`，PE 数值版本为 `2.0.0.0`。起始源码指纹为 `41f98ae3db4e36a0bbfaaf75215ff04128443ff5fbe832468a52028703cbebf2`。原始 session、queue、lifecycle 和 state 保存在 `out/dll-ownership-store-baseline/41f98ae3db4e/api/v2`，具体哈希由 `baseline.json` 及校验脚本固定。

## 已确认的重复操作

DLL 每次借出、归还、预留、取消、提交和消费完成后，都会更新队列私有的 `Loans/Published`。原实现使用 InterlockedAdd，生命周期归并使用 InterlockedExchange，进度读取则以 InterlockedCompareExchange(0,0) 取得快照。

这些计数已经具有一个更早建立的串行写入规则：

| 访问者 | 已有保护 |
| --- | --- |
| 普通队列调用 | QueueEnter 先持有 Lifetime/shared DataGate，再取得唯一线程 Owner；其他线程不能修改同一队列的池、ring view 或计数 |
| 生命周期完成消费及故障后归并 | 持有 exclusive DataGate，排除所有普通队列调用；不依赖 Owner 线程仍存活 |
| 关闭/进度观察 | 只读计数，不能凭计数单独释放资源；生命周期最终核对必须先取得 exclusive DataGate |
| 构造和销毁 | 发布之前清零；销毁发生在引用和资源屏障之后 |

全部运行期写入点均在上述两个写入区间内。同一队列没有并发计数写入，因此每次再做完整原子读改写是重复的同步成本。进度读者也不需要通过原子比较交换争取计数所在缓存行的写权限。

## 修正和维护约束

队列计数更新改为 Windows `ReadAcquire/WriteRelease`，对应快照改为 `ReadAcquire`。保留相同计数位置和更新顺序；生命周期归并在原有 exclusive DataGate 内以 release store 发布零。state 中集中记录写入规则，新增路径必须继续遵守它。

这只适用于 `PB_USER_QUEUE.Loans/Published`。Owner 的竞争 CAS、跨线程 Session 引用计数、PublicReleased/停止标记、共享游标与 Armed 的同步规则保持原样。没有加入锁、缓存、后台任务、分配或轮询；私有结构的布局和大小保持一致。

计数范围仍由原有所有权操作维持：Loans 最多为 TX 借出加 RX 预留的槽数，Published 最多为 RX 槽数。整批 token 校验、accepted 前缀、代次耗尽退休、故障本地归还和空闲索引维护都继续执行。

## 关闭与唤醒边界

`GetCloseProgress` 的多个计数不是原子的联合快照，原实现也没有这样的承诺。例如 Commit 在 Loans 减至零后、Published 加上提交项数前，进度读者可能暂时读到两个零。此时 QueueEnter 仍持有 shared DataGate 和 Lifetime，生命周期不能越过 exclusive DataGate 检查，也不能据此释放映射。

本轮新增测试在这个确切位置暂停实际 Commit，调用实际 PrepareClose，确认它保持未准备状态。继续 Commit 后，模拟原生引用仍单独阻止准备；只有真实控制路径的 STOPPED 确认才允许完成归并。损坏完成同样不能直接清除 Published。

队列调用可能在检查 Owner 之前等待 DataGate；测试中的错误 owner 调用放在独立线程，避免阻塞负责释放暂停点的测试线程。返回 PB_BUSY 的所有权规则没有改变。

归还和完成消费原有 KernelEvent、关闭 ProgressEvent、内核满 credit 唤醒、completion continuation，以及 worker join → AllReturned → Returns rundown → MDL/映射释放顺序均未修改。计数零、共享终止提示和超时都不是额外的物理释放依据。

## 验证

`ownership_counter_tests.c` 直接包含实际 DLL session 实现。拦截器只观察目标计数，继续执行真实 Windows 操作，并核对每个写入者的 owner/生命周期身份、DataGate 区间及计数范围。

- 一次 TX receive/release、RX reserve/cancel 和 reserve/commit/poll 组合，共八次计数更新：旧实现为八次额外 RMW，新实现为八次 release store、零次额外 RMW。一次 GetCloseProgress 的两个计数快照由两次 RMW 变为零次 RMW。
- 四个真实线程分别持有独立队列，以 1/3/8/32 项批量完成 40000 组上述组合，同时真实进度线程读取快照。每个 owner 在各步骤核对精确计数和描述符，结束时所有 Loans/Published 为零。
- Commit 中途停止、错误 owner、用户映射保留、原生引用阻塞、损坏完成保留 Published，以及原生 STOPPED 后生命周期独占归并均执行实际 API 路径。
- 普通和保护页分配、ISO/MS volatile 模式、新旧冻结实现组成八组计数测试；另有实际 API/lifecycle 回归。Close 的测试 shell pin 只用于等待回调结束，资源仍经过实际 public close 和原生控制屏障；会话预算和保护页分配最终归零。既有分配前缀、完成退休和卸载测试继续检查其他资源及回调尾部。
- 原生六个队列函数中计数更新的 locked add/xadd 指令点总数从七个变为零。Reserve → Commit → Poll 每批减少四次计数 RMW；Receive → Release 每批减少两次，与批量项数无关。

`verify-dll-ownership-store.ps1` 固定四个旧文件的哈希，复用相同当前核心和宿主 peer，在两种 volatile 模式下进行 A/B。它记录实际进程退出码、日志、源码及可执行文件/汇编清单哈希。新测试接入 Tests 与 Safety；完整验收继续固定一个最终源码指纹。

## 测量范围

复用现有实际 DLL completion-cycle 和 TX return dll-cycle：六种批量、顺序/逆序完成，真实 Windows 事件；宿主提交读取保留实际 SYS 的 128 KiB 字节预算。每组预热 2000 次、计时 100000 次，七轮交替新旧顺序并固定允许 CPU。

首轮完整定向证据为 `out/dll-ownership-store/343b861a934a/evidence.json`，共 44 个成功执行的测试/基准进程。TX 归还组合的 1/8/32 项中位耗时分别为 107.18 → 101.07、232.13 → 205.39、592.50 → 550.34 ns/批。但完成组合的 32 项顺序数据为 1095.30 → 1166.14 ns，逆序数据为 1210.34 → 1068.10 ns，轮间也存在明显漂移。因此不能据此声称所有路径或批量都有稳定百分比加速；确定证据是已串行区间内无用原子操作的消除，最终完整验收会重新保留全部时间样本。

```powershell
.\tools\audit\verify-dll-ownership-store.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

本轮不安装、签名或加载 SYS。宿主同步、机器指令和 DLL 基准不能替代真实内核 CPU、IRQL、NDIS/LWF、Verifier、ETW/WPA、低资源及长稳验收；清单继续保留 `kernel_runtime_tested=false` 和 `production_ready=false`。静态分析的既有所有权移交诊断仍需人工核对。
