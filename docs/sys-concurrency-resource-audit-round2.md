# SYS 第二轮并发、资源与 CPU 审查

后续补充见 [第三轮审查](sys-concurrency-resource-audit-round3.md)：确认并修正工作线程清事件与故障通知的竞态，补齐逐 NBL 分配/归还测试和 DLL 跨队列故障广播。本文记录第二轮当时的结果。

日期：2026-10-28。版本仍为 1.0.1，版权年份范围截至 2026。本轮针对实际构建的 x64 PacketBridgeV2 SYS：`driver/wintun.c` 的 `PACKETBRIDGE_PROBE` 路径、`driver/v2`、链接的 `common` 模块及必要的 C/Rust API 回归。没有安装或加载驱动，没有签名，也没有修改已冻结的 Rust VPN 项目。

结论：又发现了可以由源码时序或独立复现确认的问题，已修正并增加回归。资源审查暂未发现新的确定性内存、句柄或线程泄漏，但离线测试无法证明实际 NDIS/LWF 环境下无竞态、无泄漏。SYS 使用 C 和系统线程，没有 Rust/Tokio 协程；本轮不覆盖 VPN 的全部异步任务。

## 新问题与修正

| 编号 | 级别 | 原问题、触发与影响 | 修正及证据 |
| --- | --- | --- | --- |
| B1 | P1 | 第一轮让控制请求及进程退出清理跨注销等待持有全局 CtxGuard 共享锁。Halt 排队取得全局排他锁后，已有注销等待的回调再发起上下文查询，可能被排队写者阻塞，形成 `注销→回调→全局锁→注销` 等待环 | 新增每适配器 ControlRundown。全局锁只用于查找并取得 rundown 引用；Halt 在全局排他锁内置 Halting、撤销 Reserved，释放全局锁之后才等引用。进程通知也只在短锁区选择并引用对象，在锁外注销，最后释放引用。宿主测试执行实际 context_gate.h：十万次查找、撤销等待已有读者、撤销期间嵌套查找立即拒绝，全局锁可被独立取得 |
| B2 | P1 | QUERY 原来与 DRAIN/CLOSE 共用 RegistrationLock；注销持锁等待 indication/线程退出，而回调触发的状态查询也等该锁，产生控制路径等待环 | QUERY 改为 TransitionLock 共享快照，读取队列排空状态时只短暂持有对应 RxLock，不再拿 RegistrationLock。实际 session_query.h 的宿主测试覆盖 running/draining/quarantined/stopped、未回收 credit、待提交描述符，并验证并发 detach 必须等快照结束。DRAIN 同时在快照锁内发布 Deadline 与 Draining，避免查询先看到状态却看到旧截止时间 |
| B3 | P1 | 第一轮把网络上调移出锁后，工作线程可在 PASSIVE 调用 indication。若上层同步发起会等待旧 indication 的常规 OID 配置，可能等待尚未返回的当前回调 | 接收 indication、RESOURCES 控制回复及发送 completion 统一通过无 Transition/队列锁的 DISPATCH_LEVEL 包装，设置匹配的 NDIS flags 并恢复原 IRQL。常规 MiniportOidRequest 的 PASSIVE 契约排除在该网络上调栈内直接执行等待自身的配置。宿主替身覆盖从 0/1/2 进入的九种调用、RESOURCES 保留及 IRQL 恢复；这不是实际内核 IRQL 或 NDIS 调度测试 |
| B4 | P2 | PbPoolReserve 使用不断增加的 uint32 Next 再逐次取模。非 2 次幂槽位数在 UINT32 回绕处可能重复访问某槽并漏扫空闲槽，错误返回 PB_FULL。例如 3 槽、0/2 已占用、Next=UINT32_MAX-1 时漏扫空闲槽 1 | 保持私有游标在 [0,Count) 内、局部推进并在退出时保存；每次扫描都完整覆盖所有槽，热循环不再逐槽除法。独立 A/B 复现以修改前源文件编译得到 PB_FULL，以当前源文件编译得到 PB_OK/slot 1；另有十万次循环回归 |

B1/B2/B3 是等待环与生命周期问题，不应统一称作已复现的内存破坏。B4 的边界条件通过主动设置私有游标复现，不需要实际运行数十亿次分配。

## 资源所有权再次核对

| 资源 | 复核结果 |
| --- | --- |
| Ctx、ControlRundown、设备列表 | 仅完全初始化后发布；Halt 先撤销入口，等已有引用，再注销/等归还/释放。rundown 是内嵌同步对象，无需 ZwClose；失败初始化尚未发布，不存在合法控制引用 |
| 会话、TX/RX slot、pending、scratch、合并缓存 | 部分初始化失败与正常注销都走 PbV2Free；可选 scratch/合并分配失败允许普通包路径；没有把可选内存失败当成必需资源成功 |
| 父 MDL、锁页、系统映射 | 继续使用明确 PagesLocked 状态；join 线程、等待 NDIS 引用及返回 rundown 后才解锁并释放 MDL；映射失败仍能解锁 |
| RX record、两个 MDL、NBL | 所有分配失败出口成对回滚；提交后只在真实归还时回收，活跃/重复 token 不发布第二个 completion |
| 事件对象引用 | 每次 ObReference 对应一次 ObDereference；别名拒绝时也按引用次数回滚；未把对象引用错误地当作内核句柄关闭 |
| 每队列系统线程 HANDLE | 注册失败和正常注销均唤醒 start/stop、等待退出、ZwClose；最多 8 个。已故障 worker 退出，通知仍为一次性，不因损坏 ring 自唤醒空转 |
| 上下文/会话/NDIS 返回保护 | 三种保护分别覆盖控制调用、会话返回尾部、适配器返回尾部；AllReturned 已置位后仍等待最后一个回调退出 rundown，再释放其访问的对象 |

实际清理辅助函数的宿主测试继续覆盖 76 个初始化获取前缀、事件别名双引用回滚、最后归还回调延迟、8 个线程退出并 join，追踪内存/MDL/锁页/对象引用归零，进程句柄差值为零。这些计数是宿主替身的观察，不是 PoolMon/Verifier 的内核结果。

仍有明确的终止性限制：若 NDIS/过滤驱动永久不归还 NBL 或回调永久不返回，安全清理必须等待，不能超时后强制解锁/释放。DLL 故障隔离也可能有意保留无法证明已安全回收的映射/句柄。这两类保留不能宣称已经消除，也不应冒险改成强制回收。

## CPU 修改与测量

已做的低范围改动：

1. RX 接纳阶段对最多 32 个描述符只获取一次 RxLock，替代每描述符一次；锁内只做有界的校验/所有权/失败 completion，内存分配、包复制、NDIS 上调均不放入该锁。
2. TX 每批在局部累加字节与包数，再更新三项 MIB 统计。满批接受 32 个对象时，三项统计的原子加法由 96 次减为 3 次；另一次 TxObjects 更新仍保留。GSO 仍按实际分段数计算包数与重建头部字节，部分接受只统计已接受项。
3. 槽位扫描改为局部有界游标，修复 B4 并去除逐槽取模；共享 ring 的计数发布与 Armed 交换仍保留原有强原子语义，没有用普通读写替代。
4. 发送队列发布返回损坏/无效结果时进入一次性故障路径；PB_FULL 仍是容量反馈。

性能基线保存在 `out/audit2-baseline/a32a2ad7160e`。其 common/v2_core.c SHA256 为 `c6572484f45a57b80032bf5f9059d85e9fde307753d4e6b1e22ce2b8c1db3835`。`tools/audit/bench-core.ps1` 使用同一测试源、MSVC x64 /O2 /MT、独立翻译单元且不启用 LTO，分别链接前后核心。每项 200 万次、五轮交替顺序，线程固定在进程允许的最低编号 CPU，记录 QPC 和线程周期中位数。机器为 Intel Core Ultra 9 285H；系统调度和频率噪声仍存在。

一次测量如下；完整逐次样本写入独立输出目录的 `benchmark-results.json`：

| 槽位/场景 | 修改前 ns/次 | 修改后 ns/次 | 观察 |
| --- | ---: | ---: | --- |
| 32 / 预留后释放 | 2.418 | 2.422 | 基本持平，没有稳定提升依据 |
| 31 / 预留后释放 | 2.468 | 2.605 | QPC 约慢 5.6%；线程周期为 9.088→9.052，不能据此认定快路径改善 |
| 32 / 满池扫描 | 39.475 | 21.718 | 本微基准耗时下降约 45.0% |
| 31 / 满池扫描 | 38.374 | 21.182 | 本微基准耗时下降约 44.8% |

这是用户态分配器微基准，不是驱动 CPU、NDIS 吞吐或 VPN 性能。驱动使用 /kernel 与 LTO，实际收益必须签名后实测。其余两项优化目前只有结构性开销减少，没有运行数据可宣称吞吐提高。

仍值得采样的 CPU 候选，按预计实施风险排列：

| 候选 | 可见开销与下一步 |
| --- | --- |
| NBL/MDL/record 分配 | 每个接收 NBL 仍分配 record、payload MDL、Ethernet MDL，并从 NDIS pool 取得 NBL。先用 ETW/WPA 确认占比，再设计有上限的缓存；必须在真正归还后才能复用 |
| 返回批处理与通知 | TunReturnNetBufferLists 可接收 NBL 链，但当前每个记录仍获取 RxLock、更新引用/统计、设置 KernelEvent。可按队列归并连续返回，统一 flush/wake；要保留乱序归还、唯一 completion 和 rundown 尾部保护，不能直接跨队列合并 |
| 合并复制与 checksum | RSC/URO 为避免可变用户内存被错误标记 checksum 成功，继续复制到私有缓冲区并校验。大包/高 pps 下可能占比高；不能为了去复制而信任用户可变数据 |
| 跨队列共享缓存行 | 全局 ReferenceLock、IndicationLock、统计计数及 TransitionLock 可能造成多核竞争。先测竞争和单核占用，再考虑批量引用/按队列分片；直接删锁会破坏 AllReturned 与关闭屏障 |
| ring 发布与唤醒 | 当前 Load64 使用 locked compare-exchange、Store64 使用 interlocked exchange，存在成本。进一步改成 acquire/release 前必须针对 MSVC /volatile:iso、x64/内核内存模型及睡眠握手证明正确，尤其不能随意省略 Armed 的 RMW，否则可能丢唤醒 |
| 固定 64 KiB slot | 槽数决定小包在途上限，扩大字节容量未必增加有效并发。容量、批次与尾延迟需一起测；不以扩大 ring 替代 CPU 采样 |

## 离线验证与剩余验收

本轮新增测试直接包含生产辅助头文件，未另写一份清理/查询实现代替被测代码。原有分段 13 项、合并 8 项、Ethernet 6 项、共享核心/百万并发描述符/十万所有权循环、C API、Rust 原生 FFI/布局/编译失败测试继续运行。新测试包含 context gate、query/并发 detach、NDIS callout flag/恢复及 pool 回绕。

对 7 个内核翻译单元重新执行 MSVC /analyze:only：共 57 条诊断，其中 49 条在 SDK/WDK，项目 8 条均为 C28197。仍是 s、TX/RX slots、pending、scratch、合并缓存、Ctx、NBL pool 的成功所有权移交，已逐条复核释放出口；未新增其他项目诊断，也未全局屏蔽这些告警。这不是静态分析零告警。原始 SARIF、日志与 analysis-summary.json 随输出保存。

复现（PowerShell 7，在仓库根目录）：

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\audit\analyze-sys.ps1
.\tools\audit\bench-core.ps1
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
```

每次源码/文档变化产生独立指纹目录，使用同一指纹的 SYS、DLL 与 probe。历史基线与输出不覆盖。开发 DLL 不内嵌驱动，仍不能在没有匹配已安装驱动时当作最终包使用。最终嵌入 DLL 的构建继续留待 SYS/CAT 签名后执行。

签名后待在可恢复测试机进行：Driver Verifier 的 Special Pool/Pool Tracking/Force IRQL Checking/Deadlock Detection/低资源模拟及 NDIS 规则；并发 register/drain/close、查询与 Halt、进程强退、多适配器、动态 OID、pause/restart、延迟/乱序归还、过滤驱动、surprise removal；ETW/WPA CPU 栈和调度、PoolMon、进程句柄/系统线程的长稳计数，以及双向小包/大包的 pps、CPU/GB、丢包与尾延迟。未运行这些，不能作无竞态/无泄漏或吞吐提升承诺。

NDIS 契约参考：[接收 indication 的 DISPATCH/RESOURCES flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismindicatereceivenetbufferlists)、[常规 MiniportOidRequest 的执行级别](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_oid_request)、[发送 completion 及 flags](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismsendnetbufferlistscomplete)。这里对等待环的判断来自本地源码时序分析，并非微软已确认本项目缺陷。
