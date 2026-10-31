# Wintun TCP UDP 数据路径升级执行方案

设计日期：2026-10-28，Asia/Singapore。状态：开发进行中，已有 v2 队列、缓冲池、批量 C API、LSOv2/USO 和 RSC/URO 参考路径及离线测试；实际内核验证尚未进行。实现进度见 [实施记录](offload-capability-probe.md)。

目标是在保留 Rust 用户态隧道和现有线上协议的前提下，建立支持批处理、多队列、TCP LSOv2/RSC、UDP USO/URO 的新版数据路径。新版 SYS、DLL、Rust 绑定使用独立 ABI；旧环境通过功能降级或切换完整旧版组件恢复服务。最高吞吐、CPU/GB 和尾延迟共同决定最终默认配置。

本文描述最终实施规格；只有实施记录明确列出的能力已经提供，其余接口、配置键和参数仍待实现。

## 1 范围与基线

| 项目 | 约定 |
| --- | --- |
| 源码基线 | master，ec0a6b98456fe1ba52567bb2add4bbf5f64315a1 |
| 首个目标平台 | Windows x64；v2 首版最低为 Windows 11 24H2，较老系统默认使用 legacy 包 |
| Windows 接口 | v2 要求 NDIS 6.89 及以上；按已支持的结构版本和运行时有效能力选择功能，不另做 v2 的 Win7/Win10 适配 |
| 初始工具链 | 现有 VS 2022/MSVC 14.44、SDK 10.0.26100.0、WDK NuGet 10.0.26100.6584；构建时固定版本 |
| 用户态 | 新 C ABI 和 Rust 安全绑定；冻结版应用 D:\rust\rust-quiche-vpn 已接入，系统运行另行验收 |
| 线上协议 | 保持每条隧道报文的格式、MTU、加密记录、认证和重放语义；首版不增加跨网络的大包重组协议 |
| 旧接口 | 新 ABI 不承诺逐函数兼容旧 API；旧客户端使用原版 DLL/SYS 组合 |
| 当前签名包 | Release/amd64/driver 仍为原版 master 的未签名构建，不包含本文优化 |

保留最新 master 的 Ring Head 竞态修复、NBL 链尾初始化，以及用户态和内核态两端的内存屏障。不得用旧实验分支整体替换该基线。

原版 SYS 的签名前 SHA256 为 AC262356A881FF22720B35C0ACC39203B77F9F7D442AC877DF64BD9442592BC1。官方 0.14.1 ZIP 的 SHA256 为 07c256185d6ee3652e09fa55c0b673e2624b565e02c4b9091c79ca7d2f24ef51。未签名的 master 构建只能作为测试基线；生产回退必须使用通过目标系统签名验证的包。

## 2 必须先通过的能力验证

原始上游驱动使用 NdisMediumIP，仅注册基础 miniport 属性。实施时根据微软任务卸载的以太网封装约束，已选择独立 Ethernet 前端、共享 ABI 保持 L3 的路线，并补齐 ARP/NDP 与卸载偏移处理；该选择不构成任何四项能力已在真实 Windows 数据路径生效的证据。

| 验证项 | 必须取得的证据 | 未通过时的处理 |
| --- | --- | --- |
| TCP LSOv2 | 实际收到含有效 LSO OOB/MSS 的 NBL；参考分段器生成正确报文；完成信息正确 | 保留普通 TCP 包批处理；记录介质或绑定限制 |
| UDP USO | IPv4/IPv6 实际 USO 请求、有效 MSS；分段、尾段和校验和测试通过 | 保留普通 UDP 包批处理；不宣告尚未实现的能力 |
| TCP RSC | NDIS 有效配置已启用；合法合并 NBL 被 Windows 正确消费 | 单独关闭 RSC，保留其他能力 |
| UDP URO | NDIS 6.89 有效配置；合法 URO 元数据进入协议栈 | 单独关闭 URO；不能以 socket 软件合并代替驱动 URO 生效证据 |

原型必须具有正确的最小数据处理路径或明确的失败完成路径，不能只宣告能力后把卸载大包写入旧 ring。启用与关闭时遵循各功能的 DDI 契约。

若 NdisMediumIP 不满足所需能力，形成介质验证结论，再选择“NDIS 侧呈现以太网、共享 ABI 仍向 Rust 提供三层包”的独立适配器路线。该路线必须补齐必要的 ARP/NDP、地址与路由行为测试；不能默认修改 MediaType 就完成迁移。

NetAdapterCx 可作为新驱动路线的候选，须比较介质支持、共享内存接入、排空机制和性能，不能仅凭框架更新认定更快。它提供 GSO 管理和符合其约定的软件回退：[微软说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/netcx/introduction-to-hardware-offloads)。

URO 从 Windows 11 24H2/NDIS 6.89 开始提供 miniport 接口：[URO 规范](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload)。RSC 在开启 forwarding 的接口上会被禁用，绑定的过滤组件也可能影响生效：[RSC 编程约束](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/programming-considerations-for-rsc-drivers)。因此功能选择使用有效能力，不能只比较 OS 版本。

## 3 数据方向和总体架构

统一使用 HostTx 表示 Windows 到 Rust，HostRx 表示 Rust 到 Windows，避免沿用旧源码中容易混淆的 Send/Receive 命名。

```mermaid
flowchart LR
    H[Windows TCP IP] -->|LSOv2 USO 或普通包| K[新版 SYS]
    K -->|HostTx 批量描述符| Q[多队列共享缓冲区]
    Q --> R[Rust 队列工作线程]
    R -->|按协议分段 校验和 逐记录加密| S[UDP socket]
    S -->|可用时 USO| N[网络]
    N -->|可用时 URO| S
    S -->|恢复边界并认证解密| R
    R -->|HostRx 批量提交| Q
    Q --> K
    K -->|RSC URO 或独立 NBL 批量提交| H
```

Windows 到 Rust 的数据通常仍需复制到已注册的共享缓冲区，除非另有经过验证的映射设计。Rust 到 Windows 可通过共享缓冲区的 MDL 描述数据。首版不以双向全程零拷贝为承诺。

大包跨内核和用户态边界，线上仍发送符合路径 MTU 的独立报文。不得把 TCP/UDP 卸载对象直接当成一个正常 IP 报文发到对端。

## 4 共享内存 ABI v2

### 4.1 会话与能力握手

新增 QUERY_CAPS、REGISTER_SESSION_V2、SET_POLICY、GET_STATUS、DRAIN_SESSION、UNREGISTER_SESSION 控制操作；具体 IOCTL 编号在 ABI 定稿时分配，与旧 REGISTER_RINGS 区分。

握手内容包含 magic、ABI major/minor、结构大小、build ID、会话 epoch、支持的描述符类型、队列数上限、注册内存上限、大包上限、批次上限和 IP/传输协议能力。major 不匹配时拒绝注册；minor 扩展按结构大小和显式能力协商，不猜测布局。

分开报告 Supported、Requested、Enabled 和 DisableReason。有效能力由已实现的 SYS 路径、OS/NDIS、当前绑定与策略、DLL/Rust 支持共同确定。尚未完成握手或没有可用处理路径时，不向 Windows 开启需要用户态处理的卸载。

将 HOST_TX_TCP_LSO、HOST_TX_UDP_USO、HOST_RX_TCP_RSC、HOST_RX_UDP_URO、OUTER_UDP_SEGMENTATION、OUTER_UDP_COALESCING 作为独立能力；IPv4/IPv6 和可选头部支持分别表达。

### 4.2 描述符内容

| 字段组 | 必须表达的内容 |
| --- | --- |
| ABI 头 | 描述符版本、类型、长度、已定义 flags；保留位必须为零 |
| 对象类型 | PLAIN_IP、TCP_GSO、UDP_GSO；接收方向首版由 SYS 从普通包构造 RSC/URO |
| 数据位置 | 注册区域 ID、区域内偏移、数据长度；首版每个对象使用一个连续数据区域 |
| 协议布局 | IPv4/IPv6、L3/L4 偏移、头部长度、segment payload size |
| 校验和 | 待完成的校验和类型、起始与字段偏移；不以一个布尔值替代完整语义 |
| 生命周期 | queue ID、session epoch、buffer ID、generation、completion token |
| 完成结果 | 成功、未接纳、拒绝、取消、终止，以及具体原因；不得把接纳等同于远端收到 |

固定宽度整数、显式对齐和单位；不共享 C 指针、size_t、NDIS 私有结构或 Rust enum 的隐式布局。使用缓冲区 ID/偏移，不暴露内核地址。

数据池和描述符队列分离，首版不支持任意 scatter-gather 用户描述符。大对象大小使用协商上限；不再把旧版 65535 字节限制应用于所有卸载对象。普通 IP 包和接收合并包仍满足各自的协议/NDIS 长度限制。

### 4.3 每队列队列布局

每个队列设置 HostTx ready、HostTx recycle、HostRx submit、HostRx completion 四个有界队列。HostRx 空闲块由用户态分配器管理；HostTx 空闲块通过 recycle 队列回到内核。队列本身只传递描述符或 token。

生产者和消费者计数分离到不同缓存行。使用对齐的 64 位序列号和有界差值检查，明确回绕规则，不能信任用户提供的队列计数。容量、索引和地址计算全部检查溢出。

每个队列规定唯一发布 owner。NDIS 并发入口通过有界暂存/同步交给对应 owner，或使用单独验证的 MPSC 入口；不得把实际多生产者队列当 SPSC 实现。

发布顺序为写入数据和元数据后 release 发布；读取方 acquire 后消费。空队列等待采用 arm、完整内存屏障、重查、等待的协议，生产方发布后检查是否需要唤醒。新增故意放大的竞争测试，禁止以 x64 编译器屏障代替所需的 store-load 屏障。

### 4.4 缓冲区所有权

HostTx 状态：FREE → KERNEL_WRITING → READY_FOR_USER → USER_OWNED → RECYCLE → FREE。

HostRx 状态：FREE → USER_RESERVED → USER_WRITING → PUBLISHED → KERNEL_VALIDATED → NDIS_OWNED → COMPLETED → FREE。

USER_RESERVED/USER_WRITING 允许取消：归还独立块即可，不回退一条共享字节 ring 的 Tail。发布后不得取消、改写或提前归还；NDIS 返回所有相关引用后才能产生最终 completion。

首版仅在一个队列上发布同一缓冲区。每次复用更新 generation；重复归还、旧 epoch、错误队列和无效 token 由内核私有状态拒绝。共享状态不作为所有权的唯一依据。

对收到的用户描述符先复制到内核局部结构，再检查已注册范围、长度、类型和能力。共享头部存在修改竞争，内核需使用有界头部快照，后续访问以验证后的范围为准；用户可修改自己的数据不应转化成越界内核访问。

## 5 C API 与 Rust 接入约定

下列为逻辑接口名，ABI 定稿时固定名称和函数表版本：

| 接口 | 行为 |
| --- | --- |
| GetInterfaceV2 / QueryCapabilities | 取得带大小和版本的函数表、能力与实际限制 |
| StartSessionV2 / OpenQueue | 建立会话并取得独立队列句柄 |
| ReceiveBatch | 一次取得 HostTx 描述符数组，返回实际数量和状态 |
| ReleaseBatch | 一次归还多个 HostTx token；不依赖归还顺序回收数据块 |
| ReserveBatch | 为 HostRx 返回独立可写 lease；内存不足时可返回输入前缀 |
| CommitBatch | 检查 lease 后批量发布；返回已接纳数量，未接纳项仍由调用者持有 |
| CancelBatch | 仅取消尚未发布的 lease，不操作 NDIS_OWNED 数据 |
| PollCompletions | 批量取得 HostRx 完成/拒绝结果并更新可用块 |
| GetStatus / SetPolicy | 读取 Enabled/DisableReason 和设置允许的功能上限 |
| Drain / StopSession | 停止接纳、排空引用、最后解除注册 |

所有批次先检查结构参数和 token。任一结构参数非法时返回错误且接纳数量为零；结构合法而队列容量不足时只接纳输入前缀。已接纳和未接纳所有权严格区分，不静默丢弃，不隐式重发。传输层/协议验证拒绝通过 completion 返回原因。

会话控制句柄可由多个线程受控访问；数据队列句柄由单个 owner 发布/消费，不承诺无条件并发调用。复制/加密任务可以拥有独立 lease，最终发布由队列 owner 统一排序。

Rust 侧新增原始 FFI crate 和安全 wrapper。用 lease 类型区分可写、已发布、只读和完成状态；Drop 只能自动归还未发布 lease 或触发已定义的归还路径，不能直接释放 NDIS 持有的块。异步任务不得在 lease 已归还后继续引用 ring 数据。

实际应用为 D:\rust\rust-quiche-vpn 的冻结版 vpn-client。新后端已接入原有设备初始化、队列、代次授权和清理流程。该应用先通过 netstack 再走 H3 TCP/UDP 代理，因此 GSO 必须在进入 netstack 前还原为独立 IP 包；不改 H3 协议、认证或加密记录。外层 QUIC 的 Windows IOCP/USO/URO 使用应用已有实现，保持 socket 级回退和不重放约定。详细当前边界见 offload-capability-probe.md。

## 6 卸载与批处理实现

### 6.1 HostTx 的 LSOv2 与 USO

NDIS 接收入口识别卸载 OOB，将头部模板、MSS、校验和状态和负载复制为 v2 对象。建立参考软件分段器作为正确性基准，覆盖长度、序号、TCP 标志、IP ID、选项和校验和。LSOv2 长度以 NET_BUFFER 为准，不能把 IPv4 模板的零 TotalLength 当异常普通包。

原始 NBL 的完成至少要保证源数据不再被读取；完整复制到已拥有的有界队列后，按相应 DDI 处理发送完成和卸载完成信息。不得把完成回调延迟到远端接收确认，也不得在尚有源数据引用时完成。统计区分卸载对象数、分段后包数和有效字节。

Rust 在协议要求的位置完成分段和校验和，随后逐条生成原协议加密记录。参考分段器与优化实现做差分测试，优化实现不能只复制切片而遗漏头部语义。

能力关闭完成后到达的非零 LSO/USO OOB 请求按对应 DDI 拒绝/完成，不能统一写成“所有请求都软件分段继续发送”。[LSO 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/offloading-the-segmentation-of-large-tcp-packets)和[USO 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-segmentation-offload-uso-)分别实现。USO 宣告还须满足 IPv4/IPv6 及必需的选项/校验和要求；未完成必需部分时不开启该能力。

### 6.2 HostRx 的 RSC 与 URO

先做独立 NBL 的批量 indication，再加入合并器。第一版只合并当前批次中相邻、同流且满足规则的包，不等待未来报文、不跨批次长期持包。TCP 和 UDP 分别验证所有规则；不满足时立即提交已有合并对象，再正常提交当前包。

有效校验和由 SYS 验证或通过经过定义且可信的路径确认。AEAD 认证通过不等于内层 IP/TCP/UDP 校验和正确，不能直接信任用户态 checksum_valid 标志。

尽量以自有头部加 payload MDL 链描述合并数据；以额外复制换取简化的版本保留为比较实现。一个合并 NBL 可以引用多个原始块，必须建立每个块的私有引用计数，全部归还后分别完成；不能只用最后一个 ring 偏移代表整批生命周期。

混合 IPv4/IPv6 批量 indication 时正确设置 SINGLE_ETHER_TYPE。维护 NDIS 标准链与驱动私有完成链，先登记整批引用，再向 NDIS 交付，并支持同步/快速归还和乱序归还。

RSC/URO 关闭时先处理待合并对象，再满足各自开关同步规则。RSC 必须处理 forwarding/过滤限制；URO 的关闭、绑定兼容与状态通知按[微软规范](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload)实现。

### 6.3 完成与描述符复用

回收和发送完成均按有界批次处理。锁内摘取安全对象和更新共享进度，锁外做适合该 IRQL 的释放/完成；仍持有 TransitionLock 时当前 IRQL 可能仍为 DISPATCH_LEVEL。

达到一次回收预算后必须保证后续继续排空，不能等待一个可能永远不会再发生的回调。完成顺序、协议包顺序和 buffer 归还顺序分别建模，不将三者混为一谈。

保留 NDIS pool，按采样决定是否增加有上限的 partial MDL 缓存。复用时恢复 MDL、NB、NBL 的长度、映射、链、状态和 OOB 元数据；父映射解除前清理所有依赖。参考[partial MDL 规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-iobuildpartialmdl)。

### 6.4 多队列与外层 UDP

按稳定的流标识分配队列，同一流固定 owner。IPv4 分片和 IPv6 扩展头不能简单按不存在的端口字段散列；无法安全识别时进入确定性的保守队列。队列数变更只在会话重建时生效，首版不在线迁移活跃流。

外层 UDP 单独协商 socket 功能。发送合并相邻且符合长度规则的完整加密报文，使用 UDP_SEND_MSG_SIZE；接收使用 UDP_RECV_MAX_COALESCED_SIZE 和 UDP_COALESCED_INFO 恢复边界后逐记录认证。不同长度不强行补齐、不重排同流报文、不把认证标签一起错误切段。[Winsock 定义](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-udp-socket-options)。

按协议要求保留 nonce/序号唯一性、发送顺序和接收重放窗口；多线程批量加解密不能改变这些约束。socket 提交结果不明确时不盲目重发整批，以免重复发送已接纳记录。

## 7 回退机制

### 7.1 三层回退

| 层级 | 触发条件 | 行为 | 保留能力 |
| --- | --- | --- | --- |
| 单项功能降级 | OS/绑定不支持、策略关闭、特定包不符合已声明的组合 | 只关闭或绕过该项；使用普通包、批量 indication，或契约允许的参考分段器 | v2 ABI、其他卸载、批量、多队列 |
| 会话重建 | 队列故障、资源压力、配置改为较少队列、数据 owner 退出 | 停止接纳，排空/隔离旧会话，以新 epoch 建立 v2 普通包会话；必要时降为单队列 | 新版驱动和已验证的基础路径 |
| 版本回滚 | ABI/签名不匹配、新版初始化失败、重复崩溃、驱动版本被禁用 | 选择完整 legacy DLL/SYS 包，切换适配器及本应用网络配置 | 原版功能 |

“普通包模式”仍使用 v2 描述符，不改写成旧版 ABI。旧驱动只由旧版组件组合访问，不将新描述符送入旧 IOCTL。

### 7.2 会话状态机

```text
STOPPED -> NEGOTIATING -> RUNNING
RUNNING -> QUIESCING -> DRAINING -> STOPPED
DRAINING -> QUARANTINED       排空超时且仍有引用
STOPPED -> NEGOTIATING        新 epoch，可能使用更保守配置
NEGOTIATING -> LEGACY_PENDING 新版不适用，交由版本管理器回滚
```

功能 mask 的变化包含 policy epoch，并在批次边界处理。每个在途对象保留其原有类型和 epoch，直到消费/完成，不在关闭功能时把队列里的 GSO 描述符重新解释为普通包。

### 7.3 排空协议

1. 停止接纳新预留、NDIS 新工作和新优化对象；按控制契约使相关能力/链路进入停止状态。
2. 通知所有队列 owner，取消未发布 lease，结束或正常提交已开始的批次。
3. 待合并对象及时提交或在仍归驱动拥有时释放；已交给 NDIS 的对象只能等待归还。
4. 使回调、完成队列和本地清理继续运行，等待所有引用计数满足排空条件。
5. 没有引用后更新 epoch，停止 worker，最后解除映射和关闭句柄。

排空条件至少包括：未发布 lease、已发布描述符、待提交 NBL、NDIS 引用、本地锁外清理、活动回调/rundown 均已处理完成。单纯 ActiveNbls 队列为空不充分。

所有等待在允许的 PASSIVE_LEVEL 执行，不持有完成路径需要的锁。初始控制面排空观察超时为 2000 ms，用于报告和启动隔离，不是允许释放内存的期限。超时保持引用和映射，禁止强制解除仍被 Windows 使用的内存。

出现所有权/边界错误时立即停止该会话并隔离客户端，不能静默重试损坏的描述符。无法安全重建时交由版本回滚或设备恢复流程处理。

### 7.4 回退原因和防抖

原因使用稳定枚举：ABI_MISMATCH、OS_UNSUPPORTED、NDIS_UNSUPPORTED、FRAMING_UNSUPPORTED、FILTER_BLOCKED、FORWARDING_ENABLED、SOCKET_UNAVAILABLE、RESOURCE_PRESSURE、DRAIN_TIMEOUT、INVALID_DESCRIPTOR、DRIVER_INIT_FAILED、MANUAL_OVERRIDE、PERF_PROFILE_REJECTED。

每次变化记录 build ID、会话/队列、旧/新 mask、原因、受影响在途数、耗时及结果。生产自动回退依据明确的本地故障或已验证的配置禁用规则，不把一次网络降速当作驱动故障。

候选策略：新版在 10 分钟内两次启动/进程崩溃则下次启动使用 legacy；被故障关闭的功能本进程内保持关闭，直到显式恢复或重新验证。正常的单包不适用仅逐包回退，不因 IPv6 扩展头等合法流量永久关闭全部功能。

用户可选 auto、v2_plain、legacy 三种运行模式，并能逐项禁止卸载。auto 是默认；模式切换是否需要会话重建由状态响应明确报告。

资源不足时优先背压、缩小批次或返回明确资源错误，不通过把一个 GSO 对象立刻展开成大量小包来扩大内存压力。候选新会话仍不能建立时，进入已验证的 legacy；legacy 也不可用时停止隧道并保留已有访问控制策略，不自动改成明文直连。

## 8 版本包与网络配置回滚

新驱动使用独立产品身份、服务名、硬件 ID、设备接口 GUID 和 DLL 文件名。本文临时工程名为 PacketBridgeV2，发布名在实施时统一确定；不得复用官方 Wintun 身份来部署定制驱动。这样新旧驱动可以独立保留并支持回滚。

发布目录按 build ID 隔离，包含 SYS/INF/CAT、对应 DLL、Rust 客户端、PDB、文件哈希、ABI/capability 清单和签名信息。保留 active、last_known_good、legacy 三个选择记录；legacy 本身也要经过启动与网络测试。

由启动器/管理服务读取用户指定模式和故障记录，使用完整路径装载匹配 DLL，校验实际驱动 build/ABI 后再创建会话。不得在运行中覆盖已加载 DLL 或指望只替换 DLL 就回滚内核驱动。

网络切换使用持久化事务记录，步骤为：准备新适配器和控制会话 → 配置地址/MTU/必要的出口例外 → 确认目标连通 → 切换本应用拥有的路由/DNS → 标记提交。失败时按事务记录撤销本应用所加配置，并恢复上个可用适配器配置；不回写整张系统路由表或覆盖其他程序的新配置。

切换期间继承产品已有的流量保护规则。保护失效或没有可用目标时，报告切换失败并保持既有访问策略；正常退出时是否恢复直连由产品原有的显式策略决定，不能由性能回退擅自改变。

回滚到 legacy 时先准备其适配器，保护隧道服务器的物理出口路由，再切换地址、路由、DNS和本应用拥有的过滤/NAT 配置。不同适配器的 LUID/GUID 必须重新解析。默认不同时接管两套隧道路由。

版本切换可能造成短暂中断或连接重建，不承诺无损热切换。新驱动已崩溃导致蓝屏时，用户态无法即时回退；测试/部署手册必须包含下次启动、恢复环境或安全模式下禁用新设备/驱动并启用 legacy 的恢复步骤，且在测试机演练。

已有稳定包只有在新版本通过规定测试并被标为 last_known_good 后才更新；保留至少一个独立验证过的 legacy 包和安装恢复材料。

## 9 分阶段工作包与验收门槛

每个工作包独立提交、独立 build ID、独立功能开关。先具备 plain 与 legacy 回退能力，再接入优化；不得一次启用全部功能后仅做峰值吞吐测试。

| 阶段 | 代码与工作范围 | 输出及验收 | 失败时停留的路径 |
| --- | --- | --- | --- |
| M0 基线和构建隔离 | 固定源码、工具链和包清单；建立独立产品身份、测试项目、驱动/DLL 分阶段构建；加入最小统计 | 原版可重复构建；基线压测报告；legacy 包通过签名与恢复验证；记录当前 SYS 哈希 | 原版组合 |
| M1 卸载能力原型 | NdisMediumIP 的最小 LSO/USO/RSC/URO 实现与 OOB 观测；必要时比较 L2 或 NetAdapterCx 候选 | 每项有效状态、真实 OOB/合并证据和正确性报告；确定驱动前端路线 | 原版或仅已验证能力，尚不发布全功能版本 |
| M2 ABI v2 普通包 | 能力握手、独立缓冲池、四队列模型、单队列 owner、plain 数据路径、lease/token、排空和异常拒绝 | C/Rust 布局一致；双向普通包传输；部分成功/取消/回绕/停止测试；v2 plain 与 legacy 切换 | legacy |
| M3 基础批处理 | 批量 indication、回收和 completion；进度合并、通知合并、活动引用管理 | 独立开关 A/B 对照；内存与句柄回到基线；末次归还可完整排空；混合 IP 正确 | v2 plain 单队列 |
| M4 发送卸载 | 先 TCP LSOv2，再 UDP USO；参考分段器、元数据跨 ABI、Rust 分段和校验和、状态变化 | 分段差分测试通过；IPv4/IPv6；协议必需选项；关闭/在途请求契约通过；不破坏线上格式 | v2 批量普通包或契约允许的分段路径 |
| M5 接收合并 | 先 RSC，再 URO；相邻同流合并、私有块引用、检查和 OOB、flush 与统计 | 逻辑数据流/UDP 边界一致；例外及时停止合并；乱序归还、关闭、暂停无泄漏；过滤/forwarding 正确降级 | 独立 NBL 批量 indication |
| M6 并行和应用接入 | 多队列、按流分配、Rust 工作线程、外层 UDP 分段/合并、加密记录与背压 | 同流有序；多流扩展；socket 回退正确；真实 Rust 应用完成对照 | 单队列、普通外层 UDP；内层功能可保留 |
| M7 故障与性能定版 | 全部回退演练、Driver Verifier、压力测试、兼容矩阵、自动策略与安装事务 | 正确性全部通过；选出实测配置；签名包和回滚材料完整；发布开关只允许已验收组合 | last_known_good/legacy |

验收依赖为 M0 → M1 → M2 → M3 → M4 → M5 → M6 → M7。按用户最新指示，开发和单元测试持续推进，最后统一签名；开发构建可以包含后续阶段代码，但未通过 M1 内核验证的路线不进入生产发布，单元测试不能替代系统验收。

### 9.1 M0 执行清单

- 记录原版 HEAD、工具链版本、编译参数和签名前哈希；保留官方包及原 master 构建。
- 将新项目输出固定到独立的 out/packet-path-v2/<build-id>/，不覆盖当前 Release/amd64/driver。
- 为参考包和现代包分别生成 manifest，验证启动器可选择、加载并识别匹配版本。
- 准备支持目标系统的测试机/虚拟机和另一台网络端点；先保存可恢复的网络配置并验证 legacy 连通。
- 固定两方向的 pps、有效吞吐、CPU/GB、P50/P95/P99、丢包/重传、内存和系统调用基线。

### 9.2 M1 首个决定性产物

输出 docs/offload-capability-probe.md，逐项填写测试 OS/build、NDIS、介质、绑定组件、forwarding、Requested/Enabled、OOB 样本和包级结果。结果必须区分不支持、被策略关闭、原型实现缺失和实际可用。

LSOv2 原型首先验证有真实 MSS 的发送及正确完成；USO 原型满足两种 IP 协议的必需支持后再宣告。RSC/URO 只在合并和校验和路径正确时启用。采用普通复制和参考分段器可以作为原型实现，不要求此时已达到性能目标。

如需要改变前端，写明新增 L2 控制逻辑、安装身份和 NetAdapterCx 适配成本，再更新本方案的项目文件映射。未通过的介质路径不得进入全功能发布清单。

## 10 拟新增和修改的代码范围

下表是职责划分。抽取当前源码时保留能审查的改动，先建立测试，再移动共享逻辑，避免将功能修改和大规模重排混成一次提交。

| 路径或模块 | 职责 |
| --- | --- |
| include/packetbridge_v2_abi.h | 固定宽度共享 ABI、能力位、token、状态与大小约束 |
| api/v2/packetbridge.h、exports.def | 新版 C 函数表和公开声明 |
| api/v2/session.c、queue.c、lease.c | 注册与控制、批量队列、lease 管理及等待协议 |
| driver/v2/session.c、queue.c | 会话、注册内存、NDIS 并发入口和共享描述符验证 |
| driver/v2/offload.c | 能力、OID、状态通知、策略 epoch 和每项 enable/disable |
| driver/v2/tx.c、segment.c | HostTx 入口、复制、大包描述及参考分段器 |
| driver/v2/rx.c、coalesce.c | HostRx 验证、批量 indication、RSC/URO 合并与 flush |
| driver/v2/completion.c、pool.c | 引用、乱序完成、清理预算、MDL/NBL 生命周期和缓存 |
| driver/v2/adapter.c、私有 INF | 独立适配器身份、介质、初始化、暂停/重启/卸载 |
| rust/packetbridge-sys | C ABI 绑定、布局断言和原始函数表 |
| rust/packetbridge | 安全 Session/Queue/Lease、批量接口、状态与错误模型 |
| tests/model、tests/packet、tests/driver | 所有权/队列模型、协议差分、系统与故障测试 |
| tools/bench | Windows x64 压测端、配置/版本快照、结果导出与对照 |
| tools/build、tools/package | 驱动与 DLL 分阶段构建、签名交接、资源提取核对与 manifest |
| 应用侧启动器与版本管理 | 选择完整组件、故障记录、适配器/路由/DNS 事务和恢复 |

当前 api/session.c、driver/wintun.c、api/resources.rc、构建项目提供原版行为和可复用逻辑；不在原版导出或原版环形记录中暗中增加字段。新项目的源文件列表、版本和资源文件独立设置。

## 11 初始参数与配置

以下数值只作为测试起点，不代表已测最优。M2/M3 固定单队列；M6 对照 1/2/4/8 队列后才决定 auto 策略。

| 参数 | 首轮候选 | 约束 |
| --- | --- | --- |
| 每批对象数 | 16、32 对照 | 对象数与分段后包数分别统计 |
| 每批数据量 | 128 KiB | 达到任一预算即提交；单对象仍受协商上限限制 |
| 组批处理时间预算 | 50 微秒 | 限制持续处理，不主动等待凑批；间隔采样计时避免每包计时成本 |
| 队列容量 | 每队列每 ring 1024 描述符 | 容量为 2 的幂；与全局内存预算联动 |
| 首轮发送大对象上限 | 65536 字节，含 L3/L4 头和负载 | 此为 ABI 对象总长；NDIS MaxOffLoadSize 的负载上限另按规范设置并校验，后续独立评估更大档位 |
| 接收合并上限 | 对应 RSC/URO 合法上限 | 不将发送大对象上限直接套给接收 |
| 全会话数据池 | 64 MiB 候选 | 按队列分配；另统计描述符、MDL/NBL、元数据开销，注册前检查总预算 |
| 控制面排空观察超时 | 2000 ms | 超时报告/隔离，不强制释放引用 |
| 首轮长稳测试 | 24 小时 | 覆盖持续负载、空闲、连接切换和停止重建 |

产品配置概念示例：

```json
{
  "mode": "auto",
  "offloads": {
    "host_tx_tcp_lso": true,
    "host_tx_udp_uso": true,
    "host_rx_tcp_rsc": true,
    "host_rx_udp_uro": true,
    "outer_udp_segmentation": true,
    "outer_udp_coalescing": true
  },
  "queue_count": "auto",
  "batch_max_objects": 32,
  "batch_max_bytes": 131072,
  "assembly_budget_us": 50,
  "data_pool_budget_mib": 64,
  "fallback": {
    "allow_legacy": true,
    "drain_observation_timeout_ms": 2000,
    "modern_start_failure_limit": 2,
    "modern_start_failure_window_seconds": 600,
    "reenable_after_fault": "explicit_or_revalidated_restart",
    "on_all_paths_failed": "stop_preserve_access_policy"
  }
}
```

配置 true 表示允许申请，不能覆盖 NDIS/绑定禁用。状态页/日志必须给出实际 Enabled 和原因。mode=legacy 强制旧版；mode=v2_plain 保留新 ABI、关闭卸载；auto 选择已验收的可用配置。

## 12 回退故障注入与预期结果

| 注入场景 | 必须观察的结果 |
| --- | --- |
| OS/NDIS 不符合 v2 最低版本 | 启动器选择 legacy；不尝试让新驱动加载到不支持的环境 |
| DLL 导出、ABI、build ID 或 manifest 不匹配 | 在接管隧道路由前失败，选择匹配包；日志保留明确原因 |
| 签名/内核加载策略验证失败 | 不把包视为可用；保留已验证的 legacy，不修改完整性策略绕过 |
| NDIS 关闭 LSO/USO/RSC/URO | 对应 mask 和 epoch 更新；按规范处理在途对象；其余功能继续 |
| forwarding 或过滤组件禁用合并 | RSC/URO 按实际条件分别降级，维持独立包批量提交 |
| 保留 lease 时进程退出或 worker panic | 未发布块回收，已发布块按所有权完成；不会重复使用在途内存 |
| NDIS 乱序归还、极晚归还、最后一次归还超过回收预算 | 每个块恰好完成一次；有保证的继续回收；停止可以正确结束 |
| 描述符越界、长度溢出、伪造或重复 token、错误 epoch | 拒绝/隔离会话，内核无越界和重复释放；不静默重试 |
| 队列满、数据池耗尽、MDL/NBL 分配失败 | 明确背压或资源错误；只处理已接纳前缀；错误路径无悬空 pending |
| 外层 socket 不支持分段/合并 | 只关闭该 socket 的对应功能，不关闭内层全部卸载 |
| 外层发送结果无法确定 | 不重放整批已可能发送的加密记录；按 socket 契约报告 |
| 排空超过观察超时 | QUARANTINED，保留仍有引用的映射；不强制释放或装作无损切换 |
| 新版反复启动/应用崩溃 | 故障计数触发下次启动 legacy；不发生无限重试和功能振荡 |
| legacy 也不可用或切换期间流量保护不能维持 | 停止隧道并报告失败，保留原有访问策略，不自动改成明文直连 |
| 路由/DNS 切换中途管理进程退出 | 下次启动读取事务记录，恢复本应用拥有的配置，不改其他应用配置 |
| 新内核驱动蓝屏/导致启动失败 | 在测试机完成恢复环境/安全模式恢复演练；不宣称运行中热回退 |

## 13 正确性和系统验收矩阵

### 13.1 队列与所有权

用小容量模型强制覆盖回绕、空/满转换、发布与等待竞争、乱序完成、重复归还、取消、epoch 变化和 owner 崩溃。检查每个块仅有一个合法所有者，不会在 NDIS 引用结束前复用；加入通知漏唤醒的竞争放大用例。

C/Rust 对结构大小、对齐和字段偏移做双向断言。对用户描述符执行有边界的模糊测试，包括最大长度、加法溢出、非法 header offset、循环/越界引用以及校验后共享字段被改写的情况。

### 13.2 协议数据

TCP/UDP × IPv4/IPv6 × 两个方向，覆盖小包、MTU 边界、最后短段、大对象以及 1/多流。普通包包括分片、IPv4 选项、IPv6 扩展头；卸载只处理能力声明允许的组合，对其他合法输入执行规定回退。

TCP 覆盖序号回绕、选项、PSH/FIN/CWR、校验和错误、重复 ACK、重传/乱序和合并终止条件。分段器与参考实现比较输出；RSC 比较交付字节流、顺序和规范要求的头部/元数据，不要求合并后的 TCP 包保留所有原始分段边界。

UDP 覆盖同流等长/变长、最后短段、零载荷、错误校验和、IPv4 零 UDP 校验和的合法语义，以及 IPv6 校验和要求；验证任何合并都能按合法边界交付。外层另测不同密文长度、认证失败和重放拒绝。

### 13.3 系统与生命周期

覆盖支持的 Win11 x64 基线和更新版本、目标绑定组件、forwarding 开/关、睡眠恢复、适配器禁用/启用、NDIS pause/restart、进程强退、重复启停和驱动卸载。

在可恢复的测试环境运行 Driver Verifier 的内存、IRQL、I/O、死锁及适用的 NDIS 检查。每轮记录环境和启用的检查项；当前设计任务不在用户主机自动安装测试驱动或修改启动/签名设置。

长稳测试结束后，队列、NDIS 引用、lease、句柄和映射计数归零；缓存可保留量必须等于配置定义，不能把持续增长解释为池化。

## 14 性能验收与配置选择

固定端点、链路、MTU、隧道协议、加密算法、CPU/电源策略和负载。每个组合先预热，再采集稳定区间；首轮建议预热 30 秒、采集 60 秒、至少 5 次，报告中位数和离散程度，低负载另测。

逐一对照：原版 → v2 plain → 批处理 → TCP LSO/UDP USO 单项 → RSC/URO 单项 → 多队列 → 外层 socket 优化 → 候选组合。不得只以“全开”与原版比较而无法解释回归。

必要指标：两方向有效吞吐、pps、总 CPU 和最忙核、CPU 秒/GiB、P50/P95/P99、丢包/重传、实际平均批量、每批字节、indication/completion 次数、唤醒次数、队列积压、块持有时间和池/非分页内存。

有效字节按应用交付口径固定，CPU 统计覆盖用户态和相关内核处理；不能只比较 Rust 进程 CPU 而遗漏转移到驱动的计算。保持内外层 MTU 不变，不把改变 MTU 的收益记为卸载收益。

首轮发布门槛的候选值如下，M0 固定测试方法后冻结，不能看到结果后随意放宽：

- 正确性、所有权、排空和回退测试全部通过，不以性能收益抵消错误。
- 普通包回退路径吞吐相对相同测试基线下降不超过 5%。
- 目标高负载场景至少获得 CPU/GiB 下降 15% 或有效吞吐/pps 上升 15% 中的一项；这是验收目标，不是收益预测。
- 对应场景 P99 不劣于基线加 max(基线的 10%, 100 微秒)；另记录网络噪声，不能用一次远端抖动判定。
- 多队列证明多流扩展；不要求一条固定到同一队列的流获得线性多核扩展。
- 低负载无主动凑批等待；连续和突发负载均不出现新漏唤醒、无限积压或内存增长。

auto 只启用已通过该环境/配置验收的功能集合。生产监控的网络吞吐波动仅作为诊断线索；不自动更换算法或反复升降级。所有 profile 必须能人工固定和完整复现。

## 15 构建签名和发布交接

1. 在独立输出目录编译 SYS，生成匹配的 INF/CAT；完成静态检查、测试构建和协议/生命周期验证。
2. 按用户最新指示，先完成开发、单元测试和未签名开发 DLL。源码定版后固定编译参数和包清单，输出 SYS/INF/CAT/PDB、签名前哈希和说明，再统一交给用户签名。
3. 接收签名服务返回的最终驱动包。核对产品身份、版本、INF 以及 SYS/CAT 内容关联，按目标系统的内核签名策略验证；文件显示普通 Authenticode Valid 不替代内核加载策略验证。
4. 最终 SYS/INF/CAT 放入只读的包输入目录。不得重新编译 SYS、修改 INF 或替换 CAT，否则重新走签名交接。
5. 仅重建对应 DLL，将最终驱动资源嵌入。再从 DLL 中提取 SYS/INF/CAT，逐字节或哈希核对，确保嵌入的正是返回的包。
6. 生成与该 build/ABI 对应的 Rust 客户端、完整 manifest、签名验证记录、功能状态样例和回滚材料。原版/legacy 包独立保留。
7. 在测试机验证干净安装、已有 legacy 并存、正常升级、回退及重启恢复。新包通过后才更新 last_known_good。

原型开发阶段可以生成独立的测试 DLL，但不得将包含未签名测试 SYS 的 DLL 标为生产候选。现有 Release/build-x64.ps1 已区分 Driver/Dll；实施新方案时新增独立脚本和输出，不用该脚本覆盖当前待签名基线。

## 16 执行前完成定义

本设计落地必须具备以下可审查结果，均通过后才称为完成：

- [ ] M1 的四项能力证据和确定的 NDIS/介质前端路线。
- [ ] ABI v2、C 函数表、Rust 布局和所有权说明定稿。
- [ ] v2 plain 与 legacy 两条回退路径在加入优化前已跑通。
- [ ] 四项卸载独立开关、实际状态、原因码和动态关闭测试通过。
- [ ] 批量 API、indication、回收、completion 和唤醒协议测试通过。
- [ ] 单/多队列和外层 UDP 优化经过对照，实际 Rust 应用接入通过。
- [ ] 故障注入、Verifier、24 小时长稳、资源归零和恢复演练通过。
- [ ] 原版、候选、last_known_good 和 legacy 包能清楚区分并验证签名。
- [ ] 用户签名后的 SYS/INF/CAT 被原样嵌入最终 DLL。
- [ ] 测试报告给出达到的收益、未启用能力及其回退原因，无未实测的全功能承诺。

当前继续开发和单元测试，不在中间阶段要求用户签名。M1 内核门槛尚未通过；生产发布仍需最终签名后的真实运行、故障和性能证据。原版基线 SYS 独立保留。
