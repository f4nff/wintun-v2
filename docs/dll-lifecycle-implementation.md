# DLL 生命周期修正实施记录

日期：2026-10-29。阶段 A 至 E 与阶段 F 的实现及宿主验证已执行；真实驱动的运行、Verifier、PoolMon、ETW/WPA 和吞吐验收仍待执行。范围保持在当前仓库，实际 Rust VPN 应用未改动，驱动未安装、加载或签名。

## 1 所有权与关闭实现

`api/v2/session.c` 负责资源创建、最终释放和接口组合，内部实现拆入 `session_state.h`、`session_queue.h`、`session_control.h`、`session_status.h`、`session_lifecycle.h`。

公开句柄、原生运行时登记表和排队工作分别拥有 session core 引用。数据 API 在 Lifetime 上取得读入场保护；ring/pool 操作另使用短 DataGate。控制请求在每会话私有单线程池内串行执行。State 只保护状态和进度，禁止持有 State 再取得其他锁或执行阻塞 I/O。

每个 session 在创建时预分配线程池、工作对象、关闭上下文和两个模块引用。运行时登记表保留 core 到实际资源关闭，最终回调在释放登记表与工作引用之前，将预取得的尾部模块引用交给 FreeLibraryWhenCallbackReturns。普通策略回调结束时，登记表与 core 引用继续保护模块；私有池保证这些回调与最终关闭回调不会并行。Close/Drop 提交时不分配任务，也不调用 GetModuleHandleEx 等加载器接口。

关闭使用单向阶段 Running、StopRequested、Draining、Prepared、Releasing、Closed。调用线程只发布请求并用条件变量等待结果，入口时计算的 deadline 覆盖自身的全部等待。后台执行 Drain IOCTL、状态确认、Device 关闭屏障、映射释放和 Adapter 引用释放。同步 Device 关闭失败时不释放映射；错误及资源继续保留，可以在外部条件恢复后重试。

Close 的零预算调用可以返回 PB_QUARANTINED；如果后台已经完成，也可以返回 PB_OK。PB_BUSY 表示用户仍持有 lease。非成功结果保留公开句柄。ReleaseSession 消费公开句柄，把未完成清理移交原生运行时；已消费的句柄不能再次调用。PrepareClose 成功保持公开句柄可查询，最终 Close 成功才消费它。

每个 DLL 实例默认最多接纳 16 个 session、16 个 adapter，并保守计费至多 256 MiB 的 session 映射。session 在创建关闭事件、提交映射和分配 slot 数组前预占数量与字节额度，未发布的构造也占用额度；统一 Destroy 路径回滚每个获取失败。适配器入场检查早于包 staging 和 SwDeviceCreate；排队创建和正在关闭的适配器继续占用额度。隔离对象占用预算，恢复并完成回收后额度归还。多个 DLL 实例的额度分别计算，不能把实例额度理解为全进程硬内存限制。

## 2 回调与等待

旧版 `api/cleanup.h` 替换无退出协议的 QueueUserWorkItem：提交前取得模块引用，失败时回滚调度状态和引用，回调返回后释放最后一个任务引用。重复请求合并，安装互斥等待和枚举循环可观察 shutdown。WintunShutdown 应在普通线程中执行，可取消尚未返回的旧版创建、打开和删除驱动调用；成功退出和卸载前仍须 join 所有公开调用者并关闭适配器。不能从 DllMain 或 logger 回调中调用。一个入口 deadline 覆盖清理、控制任务、辅助进程和依赖租约的全部等待，超时可重试，新适配器与驱动控制调用永久拒绝。

旧版 DllMain 只保留必要的装卸工作；安全对象、环境探测和 namespace 初始化改为按需执行，设备清理移至受模块引用保护的工作。真实 Windows 线程池 fixture 执行 64 次加载、入队失败重试、关闭超时和卸载循环。

旧版延迟加载依赖由调用、适配器和后台任务持有统一租约。适配器租约覆盖 SwDeviceClose 和创建回调退出屏障，控制调用自己的租约覆盖全部失败回滚。最后一次释放先隔离新入场，再从 PE 的延迟导入描述符枚举依赖，使用 /DELAY:UNLOAD 恢复 IAT 并释放模块引用；完成后允许后续调用重新按需加载。卸载过程在普通上下文执行，不持有租约状态锁，不在 DllMain 中调用 FreeLibrary。未调用显式 shutdown 的旧客户端也使用同一释放路径。

`api/rundll32_process.h` 为跨架构辅助进程提供最多四个任务的原生所有者。每个任务拥有命令、输出缓冲、重叠管道 I/O、进程、临时包、完成/取消事件及模块和依赖租约。创建进程使用 STARTUPINFOEX 的三句柄白名单，立即关闭父进程写端。以自身进程实际退出作为完成边界，有限排出已写入字节后取消剩余读请求，不依赖其他进程是否仍持写端；不再创建两个同步读取线程。日志处理保留 UTF-16 分片状态及行边界，输出有每流 1 MiB 和有限排出轮数限制。命名管道仅授权当前 token 用户与 SYSTEM，拒绝远程连接；不复用安装目录的高完整性 SACL。

辅助调用的两分钟 deadline 覆盖调用方等待。超时或 shutdown 只结束等待与读取，任务继续持有进程、临时包和引用，直到安装进程实际退出；不终止安装进程。超时还停止该 DLL 实例的新控制入场，防止后来操作与仍在运行的安装器交错。需要完成 shutdown 并卸载后再启用新实例。后台最终回调通过 FreeLibraryWhenCallbackReturns 保护返回尾部，调用方释放库引用不会提前卸载。

V2 Wait 在持有存活保护时释放 DataGate 再睡眠，策略请求与 Wake 不共用控制写锁。保留 ring 的 arm 后复查与原有发布屏障。只读状态改用 Windows SDK 的 acquire 读取，队列 owner 的首次认领仍使用 CAS，slot 转换、引用计数与 ring 同步仍保留原子操作。

共享数据 ABI 2.1 定义显式生命周期记录和独立 ProgressEvent。终止标志只用于停止和唤醒，不能授权释放内存。故障、排空、完成发布、最后一个接收引用归还和注销推进生命周期事件；清理不消费数据 owner 的自动复位 UserEvent。Wait 不再逐次查询驱动。后台清理监听进度并保留一秒的低频诊断复查，不再 Sleep(1) 轮询。

用户 lease 与已发布缓冲按队列、按批次计数，清理始终执行计数范围检查；测试和调试构建还全量核对 slot 状态。单轮完成处理受批次数量约束。资源关闭还必须经过真实驱动关闭屏障，不能由一次计数或共享状态快照替代。

## 3 版本与调用方迁移

| 项目 | 当前版本或布局 |
| --- | --- |
| PacketBridge PE 与驱动包 | 2.0.0，来源仍为 wintun.props；旧分段写出入口已退役 |
| V2 函数表 ABI | 2.1，保持 280 字节及原函数偏移；两个旧写出槽位返回 UNSUPPORTED |
| 生命周期扩展 ABI | 1.0，函数表 80 字节，支持已定义前缀协商 |
| 分段扩展 ABI | 1.1，函数表 32 字节，要求源长度、旧入口退役及不重叠输出三项能力 |
| 关闭进度结构 | 96 字节 |
| 运行时额度结构 | 40 字节 |
| 共享数据 ABI | 2.1；Layout 296 字节、REGISTER 248 字节 |
| Rust 两个 crate | 3.0.0，要求分段扩展 1.1，不接受旧入口继续运行的 DLL |

新 Rust 绑定要求 PbGetLifecycleInterface 和 PbGetPacketInterface，旧 DLL 配对旧绑定。SYS、DLL、probe、应用 vendor 绑定及清单应来自同一冻结构建；新旧共享数据布局不混用。构建清单从 C 头文件读取 ABI 主次版本，函数表与共享数据 ABI 分别定义。分段入口的校验及旧接口限制见 [共享分段 plan 校验审查](shared-segment-plan-audit.md)。

启用实验卸载前，C 调用者必须完成分段扩展能力协商，并同时设置 PB_OPEN_EXPERIMENTAL_OFFLOADS 与 PB_OPEN_CHECKED_SEGMENTS。旧调用者只有第一个标志时，在分配会话和访问设备之前返回 PB_UNSUPPORTED。Rust 配置在成功加载扩展后自动携带两个标志。产品与 Rust 主版本更新反映此次行为不兼容；共享数据布局仍为 2.1。

生命周期表可以在同一主版本下追加函数，已定义前缀方法的 96 字节进度和 40 字节运行时记录保持冻结。以后新增记录布局必须追加新方法，不能在 minor 更新中扩大旧方法的输出写入范围。

Rust 调用方需要处理以下变化：

1. Session Drop 调用 ReleaseSession，不再同步 Close 或向全局 Vec 分配隔离节点。原生运行时自动继续清理。
2. 删除手动回收入口 reap_quarantined。诊断使用 Library::runtime_status、Library::retained_sessions 和 Session::close_progress；枚举是并发快照，不能把一次空列表当作关闭确认。
3. request_stop 成功只确认本地停止接纳和提交。必须取得 prepare_close 的成功结果，才能向回退控制器传入 native_prepared=true。observe_drain 新增该参数；引用和 lease 计数为零但没有原生确认时继续排空或隔离。
4. request_policy 返回借用 Session 的 PolicyChange；wait 只在实际确认后报告成功。set_policy 默认等待上限为两秒，也可调用 set_policy_timeout。超时返回 PB_EMPTY。Drop ticket 只放弃结果，不撤销已进入内核的操作；槽位在真实完成后回收。停止后的晚到成功不会把生命周期或回退控制器恢复为 Running。
5. Close/PrepareClose 的 timeout=0 现在严格执行零等待检查，不能假定空会话一定同步返回成功。需要确认完成时传入等待预算，并受应用自己的总退出 deadline 约束。

保留的同步边界包括 Open、CreateAdapter、显式 GetStatus/GetCapabilities、原始 V2 Drain/SetPolicy，以及没有 session 接管最后一个引用时的独立 CloseAdapter。异步应用应在合适的阻塞调用上下文使用这些入口；本次新增的停止、策略提交、诊断快照和 Session Drop 不等待驱动 I/O。独立 adapter 的异步所有权移交需另设扩展，不能把本次 session 改动解读为所有旧入口均已异步化。

## 4 诊断与失败处理

进度包含阶段、阻塞位置、原始错误、用户 lease、已发布缓冲、最近已知的内核引用、剩余映射字节、启动时刻、最近实际进度时刻及复查次数。重复调用 Close 或纯粹的诊断复查不重置启动时间；状态请求、引用或缓冲变化、内核进度序号变化和实际资源释放更新进度时刻。Rechecks 记录事件或诊断等待后的复查次数，不等于失败 I/O 数量。

PB_RUNTIME_STATUS 的 RetainedBytes 是保守的准入计费，全部关闭屏障完成才归还；单个进度记录的 RetainedBytes 表示尚未释放的映射字节。映射已释放而适配器关闭仍阻塞时，两者可以不同。

若设备永久移除后无法再取得安全确认，或客户端遗失 lease，运行时保留无法证明可回收的对象，并限制后续资源增长。不能通过计时到期、取消请求成功、终止线程或强制释放映射清空隔离。是否能进一步回收这类对象需要真实驱动退出协议的额外证据。

## 5 验证入口与证据

统一离线验证：

```powershell
.\tools\audit\validate-dll-lifecycle.ps1
```

该入口保留 Tests、Safety、Driver、Probe、Api、旧版编译、DLL/SYS 静态分析和宿主基准日志；开始和结束核对同一源码指纹。结果写在 out/dll-lifecycle-validation，不修改应用或安装驱动。基准要求本轮保存的 out/dll-remediation-baseline，缺少时可以显式使用 -SkipBenchmark。

已加入正式测试的核心场景：

| 范围 | 场景 |
| --- | --- |
| API 进度 | idle Wait 下策略进入 I/O；策略挂起时 Wake 前进；跨队列故障；重复终止 |
| 关闭隔离 | Drain、Query、DeviceClose、AdapterClose 四个边界分别挂起；零预算返回；跨会话进度与所有权移交 |
| 错误恢复 | Drain、Query、DeviceClose 失败保留资源和原始错误，恢复后完成一次回收 |
| 策略 | 排队请求停止后不执行；在途请求晚到确认；旧序号不能确认新请求；遗弃 ticket 不永久占用槽位 |
| 资源 | 42 个 DLL 获取失败前缀和成功路径；77 个 SYS 获取前缀；进度事件别名失败清理；会话与适配器额度恢复 |
| 创建准入 | 在首个映射边界挂起并发构造，分别耗尽 session 数量和映射字节额度；拒绝请求不申请映射，未发布构造已计费，结束后额度归还 |
| 旧版依赖与辅助进程 | 400 次并发依赖租约与重载；真实子进程、句柄白名单、外部写端、UTF-16 分片与精确容量；23 个获取失败点；超时移交、四任务上限、logger 退出、shutdown 重试与 DLL 返回尾部卸载 |
| 卸载 | 64 次旧版调度器 fixture 循环，32 次实际 V2 DLL 在挂起清理后移交、释放调用方库引用并最终卸载 |
| CPU 通知 | 1000 次空 Wait(0) 零 QUERY；独立进度事件合并、终止状态保持、最后引用与 completion 通知 |
| Rust | ABI 布局、队列非 Send、缓冲借用、阻塞 Drain 下 Drop 返回、ticket 遗弃、原生 prepare 确认与晚到策略确认 |

Tests 与 Safety 的摘要来自实际执行进程、退出码和输出，不再写死通过数量。失败会覆盖旧摘要。辅助进程 fixture 在每个失败前缀后核对任务、工作对象、堆分配和临时句柄归零，在实际 DLL detach 时再次核对全量释放。静态分析覆盖 V2 与旧版 main、adapter、driver、rundll32 共 11 个翻译单元，保留原始 SARIF，不用批量 suppress 代替核对。

宿主空 Wait 基准固定可用 CPU，使用 MSVC /O2 /MT，七轮交替顺序，每轮 20 万次，记录 QPC 和线程周期。冻结基线与当前 DLL 都使用 hosted 测试状态，基线不会执行真实 QUERY IOCTL；它衡量 DLL 路径开销，不能换算成实际驱动 CPU/GB 或 VPN 吞吐。完整数值和来源文件哈希保存在 out/dll-bench。

DLL 的既有分析项需结合代码前置条件核对：layout 的最小区域大于头部、调用者按 requested 提供 lease 数组、WriteValidated 的内部调用者提供非空 written。SYS 的所有权移交诊断需对应到会话、队列缓冲和 NBL pool 的实际析构路径。最终数量以当前指纹的分析摘要为准。

## 6 尚待执行的系统验收

真实 NDIS pause/restart/Halt、适配器禁用和移除、低资源与进程退出、过滤驱动组合、Driver Verifier、PoolMon、ETW/WPA、长稳及吞吐没有由宿主模型替代。开发 DLL 未内嵌签名驱动，签名和 FinalApi 重建仍单独进行。在这些验收完成前，构建清单继续标记 production_ready=false。

## 7 故障归还和旧版控制任务修正

2026-10-29 的复核发现，Fatal 同时阻断数据接纳与缓冲归还，会把仍由合法调用者持有的 lease 永久留在隔离中。本轮将队列入口分成 Admission 与 Retirement。Receive、Reserve、Commit、Wait 继续在故障后拒绝；Release、Cancel、PollCompletions 仍须取得 Lifetime/DataGate、通过线程 owner 和 token 校验。重复、错误代次、错误队列和错误线程不能借故障状态跳过检查。

在 Fatal 已锁定停止接纳时，接收视图可以只归还本地所有权，不向损坏的回收 ring 发布。首次在 Release 内发现 ring 损坏时也由当前调用完成合法 token 的归还，避免 Rust Drop 丢失尚未归还的所有权。缓冲不会重新发给用户，映射继续保留。损坏 completion ring 的已提交缓冲，只能在真实 QUERY 确认 STOPPED 且没有原生引用后归并本地记账；共享 Terminal 提示不授权该转换，最终释放仍经过 Device Close 屏障。正常 Release 只在 ring 的 Armed 通知要求下 SetEvent，减少批量归还的系统调用。

旧版 CreateAdapter、OpenAdapter、DeleteDriver 通过 `operation.h` 和 `operation_runtime.h` 的统一执行器运行。调用者与工作回调各自拥有 job 引用；输入在派发前复制，命名、GUID 和调用栈不会被后台借用。每个 DLL 使用一个私有线程池，最多四个排队、构造或执行中的控制任务。首次初始化和每项资源取得失败都回滚额度；不使用进程默认池保留额外的长期工作线程。

Pending、Ready、Claimed、Abandoned 的结果交接在 job 锁下完成。成功结果只能交付一次；调用者超时、取消或未及时认领时，原执行线程关闭晚到的适配器。即使 SwDeviceClose 或外部系统调用仍阻塞，事件、栈上下文、安装互斥、依赖和模块引用也由原工作持有。FreeLibraryWhenCallbackReturns 覆盖回调返回尾部，DllMain 不 join 工作。

默认两分钟的总调用期限从派发前计算，覆盖排队和嵌套安装互斥、事件等待。软件设备回调还有十五秒的阶段上限；超时进入关闭屏障的仍是工作线程。Shutdown 的全局停止事件唤醒这些等待，轮询间隔和枚举也观察停止。总调用超时会停止本实例的新控制接纳；恢复需完成 shutdown 并重新加载。由同一个控制工作/logger 回调重入这三个入口返回 ERROR_BUSY，避免跨线程安装互斥自锁。单独 CloseAdapter 和 V2 原有同步入口仍按原合同使用。

回归覆盖同队列和跨队列故障、两类 lease 归还、回收与 completion ring 损坏、错误 token/线程、原生引用阻塞、Rust 批次 Drop；旧版真实 DLL fixture 覆盖九个资源取得失败点、32 次卸载循环、1024 次结果交接、输入复制、重入拒绝、取消等待、累计 deadline、四任务上限、晚到结果和挂起关闭屏障，逐项核对资源与句柄归零。证据由现有统一离线验证入口按最终源码指纹生成。

SYS 故障排空现在允许所属 worker 停止接纳后清退内核私有 pending completion；实际 NDIS 持有的槽和引用仍须正常归还。DLL 在真实 QUERY 确认 STOPPED、无原生引用和用户 lease 后，统一清退遗留 Published，不再要求 DLL 本地 Fatal 已置位。终止共享提示、超时或单独的引用零值均不能授权此操作。对应的队列私有收尾见证、QUERY 快照顺序、发送调用引用及回归见 [SYS 第五轮修正](sys-concurrency-resource-audit-round5.md)。
