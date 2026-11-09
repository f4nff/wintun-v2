# PacketBridge：兼容 Wintun API 的统一 v2 网络驱动

本项目基于 [Wintun 原版](https://git.zx2c4.com/wintun/)开发，发布一个兼容 DLL `wintun.dll` 和一套 `packetbridge.sys/inf/cat`。DLL 同时提供旧 `Wintun*` 接口、新批量接口及 Rust 绑定；内核统一使用 v2 数据路径。面向应用的共享数据仍是 IPv4/IPv6 三层报文。

Wintun 最初为 [WireGuard](https://www.wireguard.com/)开发，适合主要在用户态实现网络隧道的应用。驱动使用独立 PacketBridgeV2 设备身份；旧应用可以加载新的兼容 DLL，新应用使用同一 DLL 中的 `Pb*` 接口。

## 与 Wintun 原版的区别

对比日期：2026-10-31。基线为上游 `master` 的提交 [`ec0a6b98456fe1ba52567bb2add4bbf5f64315a1`](https://git.zx2c4.com/wintun/commit/?id=ec0a6b98456fe1ba52567bb2add4bbf5f64315a1)，也是当前仓库的 Git HEAD；本次核对时上游 `master` 仍指向该提交。以下对比包含当前工作区中已修改和新增的源码，不仅限于已经提交的内容，也不以官方 0.14.1 发布包代替 master 基线。

| 方面 | Wintun 原版 master | 当前项目 |
| --- | --- | --- |
| 项目结构 | C 驱动、`wintun.dll`、C 头文件及示例 | 统一 v2 内核、Wintun API 兼容层、共享安装器、Rust 绑定及构建验证工具 |
| 系统目标 | Windows 7、8、8.1、10、11 | 当前构建为 x64；最低工程目标 Windows 7 SP1，运行时使用 NDIS 6.20–6.89，按能力启用卸载；旧系统实机验收尚未完成 |
| 驱动前端 | `NdisMediumIP` 三层 TUN | v2 对 NDIS 呈现 Ethernet 802.3，处理必要的 ARP/NDP；DLL/Rust 边界仍交付三层 IP 包 |
| 部署身份 | `wintun.dll`、`wintun.sys` 和 Wintun 设备身份 | 单个 `wintun.dll`，一套 `packetbridge.sys/inf/cat`，独立 PacketBridgeV2 服务/设备身份 |
| 数据组织 | 一组收发字节 ring，逐包申请、发送、接收和释放 | 独立 TX/RX 数据池、描述符与 token；每个队列有 ready、recycle、submit、completion 四条有界队列 |
| 批处理与并行 | 公开 API 以单包操作为主 | 1–8 个数据队列，批量获取、归还、预留、取消、提交和完成；单批最多 64 个对象 |
| TCP/UDP 卸载 | 基线未提供本项目的 LSOv2/USO/RSC/URO 路径 | 新增 TCP LSOv2、UDP USO、校验和处理、参考分段器，以及受限规则下的 TCP RSC / UDP URO 合并路径 |
| ABI 与接入 | `Wintun*` 导出函数及旧 ring 注册协议 | v2 通过带版本的 C 函数表、独立 IOCTL 和共享 ABI 接入；新增 `packetbridge-sys` 与 `packetbridge` Rust crate |
| 缓冲区所有权 | 按旧 ring 协议申请和释放 | 以 session epoch、slot generation、queue ID 和 token 校验所有权；区分未提交 lease、已发布对象及 NDIS 引用 |
| 关闭与资源管理 | 原有会话结束和适配器关闭接口 | v2 增加停止接纳、排空、关闭进度、超时保留和后台回收；旧版 DLL 增加 `WintunShutdown`、有界控制任务及依赖租约管理 |
| 安装与诊断 | 原有驱动安装与日志 | v2 增加源码指纹/ABI 身份检查、内嵌驱动包验证、一次卸载重装恢复，以及结构化创建错误、收发拒绝和池压力诊断 |
| 开发验证 | 原有 Visual Studio 工程与示例 | 新增数据包 oracle、状态/并发模型、边界与分配失败测试、Rust 原生 FFI 测试、静态分析、宿主基准及签名封装脚本 |

上游基线中的 ring Head 竞态修复、NBL 链尾初始化和 Alertable 唤醒屏障是本项目继承的基础，不应计作本分支新增功能。

### 当前实现的边界

- v2 已有代码实现与离线验证入口；真实驱动的连通性、NDIS 卸载生效、Verifier、长稳和端到端性能仍需单独验收。存在实现或宿主基准，不代表已经证明吞吐提升。
- 默认使用普通 IP 包，卸载掩码为 `0`。实验卸载需显式启用，并同时满足应用策略、NDIS 配置、封装握手和能力依赖。
- 当前 LSOv2/USO 宣告的最大卸载大小为 32 KiB。RSC/URO 只处理符合严格规则的相邻报文；不满足条件时走普通包路径。
- v2 是路由型 IP 适配器。Ethernet 前端及单层内联 802.1Q 解析不等于提供通用二层桥接或 VLAN trunk。
- 多队列当前按 IP 源/目的地址散列，同一地址对的不同端口不会因此分散到多个队列；增加队列数不保证单流加速。
- 兼容 DLL 保留旧 API 的私有字节 ring、逐包指针、申请顺序、可读事件和错误码，通过单个队列 owner 工作线程接入 v2；SYS 不实现旧 ring 注册协议。官方旧 DLL 不能连接本驱动，已有官方 Wintun 设备也不会被自动接管。
- 本仓库提供本机数据路径和绑定；隧道协议、加密、路由配置及外层 UDP/QUIC socket 的处理由应用负责。

### 统一兼容方案

最低工程目标为 Windows 7 SP1 x64，尚未宣称完成旧系统签名安装和运行验收。
基础路径始终使用 v2；RSC 从 NDIS 6.30、USO 从 6.83、URO 从 6.89 开放。
Windows 11 24H2 保留完整的新卸载能力，仍要求显式启用实验卸载和分段扩展。
DLL/SYS 均消除了加载阶段对更新系统的必要依赖；结构大小及 NBL 扩展字段按运行时版本处理。
DLL 的最低系统目标不降低 Rust 应用自身运行时或其他依赖的最低要求。

`UnifiedTests` 覆盖实际 Wintun/v2 适配层的并发、顺序、最大报文、回绕、失败回滚、
满 ring 关闭、延迟 NDIS 引用和资源计数。`Tests` 自动包含此阶段。
Api 构建还检查发布 DLL 的全部导出、旧导出序号及重复加载/关闭/卸载。
实施与验收边界见 [统一方案](docs/unified-driver-plan.md)。
槽位重用竞态、兼容层批处理、唤醒与回收的复核及重放入口见 [槽位归还审计](docs/slot-reclamation-compat-audit.md)。
SYS 的 MDL 映射参数、Win7/NX 兼容性和回收预算补充验证见 [MDL 兼容性复核](docs/slot-reclamation-mdl-review.md)。
32 包合并预筛选的无效整包复制修复、IPv4/IPv6 独立测试及宿主 CPU 基准见 [合并预筛选成本复核](docs/coalesce-prefilter-cpu-review.md)。
完成发布后的跨队列故障、SDK 等待返回后的停机覆盖及最大源跨度最后 32 段见 [后置检查与故障结算复核](docs/postwait-fault-highcursor-review.md)。
接收构造失败时的 Pause 屏障、Poll32 的私有结算/Lifetime 交接及 Rust 提交失败重试见 [构造回滚与完成消费复核](docs/rollback-poll-ownership-review.md)。
可选合并资源失败降级、停止后的32项归还修正重试及Rust前缀归还跨代重用见 [降级与归还重试复核](docs/degraded-retirement-retry-review.md)。
满槽发送的合法晚归还、关闭等待失败后的恢复及Rust双向批次Fault析构见 [发送压力与关闭交接复核](docs/pressure-close-fault-drop-review.md)。
依赖卸载期间的停机唤醒、等待失败及八队列并发归还验证见 [并发停机复核](docs/concurrent-stop-wait-review.md)。
线程/进程终止等待失败后的所有权保持和回收验证见 [终止屏障复核](docs/termination-barrier-review.md)。
异步管道取消、完成屏障、事件复位和装卸资源计数验证见 [异步 I/O 回收复核](docs/overlapped-drain-review.md)。
停机等待失败时的退避、Win7 系统依赖加载及资源计数验证见 [等待与加载兼容性复核](docs/wait-loader-compat-review.md)。
批量输出计数、转交数据区重叠、停机归还和资源保持验证见 [批量输出所有权复核](docs/batch-output-ownership-review.md)。
拥有者身份、孤立设备误回收、停机取消及旧系统属性边界验证见 [孤立设备回收复核](docs/orphan-owner-reclamation-review.md)。
首次初始化的事件对发布、失败准入、错误保存与并发停机验证见 [冷启动事件所有权复核](docs/event-initialization-ownership-review.md)。
命名空间创建/打开竞争的有限重试、显式停止、日志锁边界和失败回收验证见 [命名空间初始化复核](docs/namespace-initialization-reclamation-review.md)。
普通异步读取的虚假唤醒、复位/完成竞态与双流进度验证见 [管道读取唤醒复核](docs/helper-pump-wake-reclamation-review.md)。
资源提取失败的安全日志、临时目录所有权和错误回滚验证见 [资源失败回收复核](docs/resource-failure-reclamation-review.md)。
注销与暂停的退休会话汇合、Win7 就绪判断及注册表字符串边界见 [暂停与兼容性回收复核](docs/pause-unregister-compat-reclamation-review.md)。
适配器元数据失败、驱动查询缺失见证与错误回滚验证见 [失败状态回收复核](docs/failure-state-reclamation-review.md)。
辅助模块低内存参数处理、Win7 硬件身份与原生辅助 DLL 验证见 [辅助模块兼容性回收复核](docs/helper-host-compat-reclamation-review.md)。
接口列表变化的有限重试及 MDL/NBL 守护测试见 [接口发现与回收对象复核](docs/interface-discovery-reclamation-review.md)。
外层 NBL 回调与 Halt 的寿命屏障及驱动枚举失败验证见 [外层归还与枚举复核](docs/outer-return-enumeration-review.md)。
实际注册回滚、策略回调尾部交接与名称属性验证见 [注册与回调所有权复核](docs/registration-callback-ownership-review.md)。
进程退出、旧文件关闭、非零卸载策略和改名失败验证见 [进程所有权与策略复核](docs/process-owner-native-policy-review.md)。
实际 SYS 终止广播到 DLL 等待的交错及 helper 响应容量边界见 [终止唤醒与响应契约复核](docs/terminal-wake-helper-response-review.md)。
故障 QUERY、DLL 生命周期与 SYS 回收屏障的组合及 Win7 创建失败资源归还见 [跨层故障回收复核](docs/native-reclaim-win7-create-review.md)。
实际诊断调用的最后引用交接及打开适配器时的枚举失败见 [诊断最后引用复核](docs/retained-owner-adapter-open-review.md)。
实际 IOCTL 注册回滚与 Halt、关闭期间状态查询的寿命保护见 [分派与查询寿命复核](docs/dispatch-register-status-lifetime-review.md)。
旧接口启动错误、兼容层停机交接及 indication 返回屏障见 [启动与暂停边界复核](docs/native-start-facade-pause-review.md)。
发送完成尾部、未发布会话和统一适配器的资源交接见 [所有权交接复核](docs/send-unpublished-adapter-handoff-review.md)。

实际卸载处理器、多个队列等待者的停机退役及运行时初始化重试见 [等待退役与初始化复核](docs/wait-retirement-runtime-init-review.md)。

在途策略、同步 Drain、停机后迟到发布与控制结果的资源交接见 [停机进度与所有权复核](docs/stop-progress-ownership-review.md)。

后台关闭后的策略等待者、同步控制响应和命名空间清理取消见 [回收与重试所有权复核](docs/retirement-retry-ownership-review.md)。

正常接收背压恢复、Miniport 初始化回滚及参数结构版本边界见 [停机回收与背压复核](docs/stop-reclaim-backpressure-review.md)。

实际 worker 启动回滚、completion 分轮继续通知及日志分配归还见 [归还与停机通知复核](docs/return-stop-notification-review.md)。
跨 MDL 发送失败的槽位归还、初始策略期间的私有会话及环境探测回退见 [构造与发送失败边界复核](docs/lifetime-edge-reclamation-review.md)。
Open 返回前的终止通知、控制任务分配期间的 shutdown 及失败回收见 [启动通知与任务预留复核](docs/startup-stop-ownership-review.md)。
辅助进程收尾错误保存、多队列排空与归还故障交错见 [排空故障与收尾错误复核](docs/drain-fault-error-reclamation-review.md)。
TCP/UDP 上下行的 32 包批量、载荷复制及 TCP 时间戳边界见 [TUN 32 包路径复核](docs/tun32-packet-path-review.md)。
32 包在槽位压力、字节预算、合并缓冲区及停止借用边界的验证见 [TUN 预算与停机复核](docs/tun32-budget-stop-review.md)。
IPv4 / IPv6 独立编译、进程及 TCP/UDP 32 包验收见 [地址族独立复核](docs/ip-family-packet-path-review.md)。

LSO 普通校验和配置解耦、32 包真实卸载请求与终止归还测试见 [LSO 与停机复核](docs/lso-checksum-terminal-review.md)。

继续复核的跨代槽位复用、自然部分预留、校验和地址族门控及协议边界见 [分族归还与唤醒复核](docs/return-wake-family-review.md)。

上下行同时背压、暂停恢复后跨代复用及 32 包合并边界见 [双向背压与暂停恢复复核](docs/duplex-pause-family-review.md)。

多队列排空双屏障、PrepareClose 保留生命周期锁及 32 包分片分类见 [多队列排空复核](docs/multiqueue-drain-review.md)。

终止后的 1/7/24 分批归还、Rust 16+16 分段与描述符重定位见 [终止归还与重定位复核](docs/terminal-retirement-review.md)。

普通 completion 的 Wait 重新武装竞态与最大报文头 32 段边界见 [完成通知与报文头边界复核](docs/completion-arm-review.md)。

双合并缓存再次借出、双向最大包预算与变长 TCP 合并见 [双缓存归还与双向预算复核](docs/coalesce-tail-review.md)。

控制回复混合批次、坏状态回复与 32 段末项别名见 [控制回复与状态拒绝复核](docs/control-status-review.md)。

失败完成后的同代际重复接纳修复、32+32 策略替换及尾段重试见 [接收终结代际与策略复核](docs/admission-policy-review.md)。

回收拒绝后实际 Send、两个 owner 归还通知合并及 32 包合并总长边界见 [回收与归还提示复核](docs/recycle-wait-review.md)。

跨队列零引用交接与 Pause、同槽跨代重用后的终止唤醒及 TCP 零校验和 32 段见 [引用交接与跨代唤醒复核](docs/return-interleave-review.md)。

### 当前版本

版本以源码和对应构建的 `manifest.json` 为准，各层版本分别协商。

| 项目 | 当前值 | 来源 |
| --- | --- | --- |
| PacketBridge 产品版本 | 2.0.0 | [`wintun.props`](wintun.props) |
| v2 C 函数表 ABI | 2.1 | [`api/v2/packetbridge.h`](api/v2/packetbridge.h) |
| 共享数据 ABI | 2.1 | [`include/packetbridge_v2_abi.h`](include/packetbridge_v2_abi.h) |
| 分段扩展 ABI | 1.1 | [`api/v2/packetbridge.h`](api/v2/packetbridge.h) |
| 生命周期扩展 ABI | 1.x，主版本 1 | [`api/v2/lifecycle.h`](api/v2/lifecycle.h) |
| Rust crate | 两个 crate 均为 2.0.0；Rust 最低 1.85，edition 2024 | [`rust/Cargo.toml`](rust/Cargo.toml) |

`docs/` 包含不同阶段的实施记录，部分版本号、客户端选择策略和产物路径属于历史快照。排查当前行为时，应以当前头文件、实现和同一构建的清单为准。

## 目录说明

| 路径 | 用途 |
| --- | --- |
| [`driver/wintun.c`](driver/wintun.c)、[`driver/v2/`](driver/v2/) | 驱动入口、统一 v2 路径及队列、收发、回收和卸载实现 |
| [`api/`](api/)、[`api/v2/`](api/v2/) | Wintun API 兼容层、统一安装器与 v2 API；`api/v2/adapter_unified.c` 为发布用适配器实现 |
| [`include/`](include/)、[`common/`](common/) | 共享 ABI、诊断结构、队列/池、分段、合并、校验和及以太网处理 |
| [`rust/packetbridge-sys/`](rust/packetbridge-sys/)、[`rust/packetbridge/`](rust/packetbridge/) | 原始 FFI 与带所有权约束的 Rust 封装 |
| [`tests/`](tests/)、[`tools/audit/`](tools/audit/) | 离线测试、静态分析、专项验证和宿主基准 |
| [`tools/build/`](tools/build/)、[`tools/package/`](tools/package/) | 分阶段构建、源码指纹、驱动包校验与 DLL 封装 |
| [`tools/probe/`](tools/probe/)、[`docs/`](docs/) | 原生诊断工具与设计、实施及审查记录 |
| [`example/example.c`](example/example.c) | 旧版 Wintun API 示例 |

## v2 构建与部署

### 环境要求

- Windows x64、PowerShell 7。
- Visual Studio C++ 工具链；当前开发流程使用 VS 2022。
- Windows SDK `10.0.26100.0`。
- WDK 对应头文件和工具，脚本预期位于 `Release/build-tools/wdk/c`；具体检查见 [`tools/build/common.ps1`](tools/build/common.ps1)。
- Python 3，以及 Rust 1.85 或更新版本；完整 Tests 阶段会运行 Python 和 Cargo 测试。

### 开发构建

在仓库根目录运行：

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\build\build.ps1 -Stage UnifiedTests
.\tools\build\build.ps1 -Stage Safety
.\tools\build\build.ps1 -Stage Driver
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
```

默认阶段是 `Tests`。产物写入 `out/packet-path-v2/<产品版本>/<源码指纹前12位>/`，驱动包位于其中的 `driver/`。Tests/Safety 生成各自的实际执行记录；测试结果应读取本次构建的 JSON 和日志。

`Driver` 生成未签名开发驱动。普通 `Api` 生成的 `wintun.dll` 不内嵌驱动包，需要目标系统已经安装匹配的私有驱动；构建本身不会安装或加载新驱动。

源码指纹包含 Git 跟踪及未忽略的新增文件，**README 和其他文档也参与计算**。修改后要为新的冻结源码重新构建整套组件；不能将修改前的签名输入或 DLL/SYS 与新指纹混用。

### 签名与内嵌驱动 DLL

完成开发检查并冻结源码后，创建签名输入包：

```powershell
.\tools\package\create-signing-bundle.ps1
```

收到该冻结包对应的最终签名驱动后，校验并构建内嵌 DLL。以下两个变量应指向实际签名目录和原签名输入清单：

```powershell
$driverDirectory = 'D:\signed\PacketBridgeV2'
$signingManifest = 'D:\signing-input\signing-input.json'
.\tools\package\verify-signed-driver.ps1 -Directory $driverDirectory -ExpectedManifest $signingManifest
.\tools\build\build.ps1 -Stage FinalApi -SignedDriverDirectory $driverDirectory -ExpectedManifest $signingManifest
```

`FinalApi` 校验签名、版本、指纹和驱动包一致性，只重新编译 DLL，并原样嵌入最终 SYS/INF/CAT，随后逐项核对嵌入资源的哈希。它不会重新编译 SYS，也不会将签名验证等同于运行验收。

另有 `UnsignedTestApi` 阶段，通过 `-UnsignedDriverDirectory` 和 `-ExpectedManifest` 接收经哈希校验的未签名开发包，仅用于专门的测试环境。它不具备 `FinalApi` 的内核签名验证。

内嵌 DLL 创建适配器时暂存驱动包，并检查实际绑定驱动的版本、ABI 和完整源码指纹。新旧 API 共用下兼容的安装器，只管理 PacketBridgeV2 设备。失败保留 Windows 错误及诊断；旧版按名称删除其他设备的恢复路线不再用于发布构建。接口协商和会话打开仍检查版本、ABI、完整源码指纹。

## v2 接入方式

### 数据方向与 C 接口

本项目统一使用以下方向，避免与应用视角的“收发”混淆：

- **HostTx**：Windows 协议栈 → 驱动 → 用户态，应用通过 `ReceiveBatch` 获取，通过 `ReleaseBatch` 归还。
- **HostRx**：用户态 → 驱动 → Windows 协议栈，应用通过 `ReserveBatch` 预留、`CommitBatch` 提交，再用 `PollCompletions` 获取完成；未发布的 lease 可以取消。

C 调用者加载 `wintun.dll`，取得 `PbGetInterfaceV2` 并协商函数表大小和主版本；同时按需协商 `PbGetLifecycleInterface` 和 `PbGetPacketInterface`。定义分别见 [`packetbridge.h`](api/v2/packetbridge.h) 与 [`lifecycle.h`](api/v2/lifecycle.h)。创建适配器的结构化诊断入口为 `PbCreateAdapterEx` / `PbCreateAdapterEx2`。

| 操作 | 函数表接口 |
| --- | --- |
| 会话与适配器 | `Open`、`CreateAdapter`、`CloseAdapter`、`StartAdapterSession`、`GetAdapterLuid`、`OpenQueue` |
| HostTx 批量读取/归还 | `ReceiveBatch`、`ReleaseBatch` |
| HostRx 批量预留/取消/提交/完成 | `ReserveBatch`、`CancelBatch`、`CommitBatch`、`PollCompletions` |
| 等待、状态和能力 | `Wait`、`Wake`、`GetStatus`、`GetCapabilities` |
| 策略与关闭 | `SetPolicy`、`Drain`、`PrepareClose`、`Close`；生命周期扩展提供异步停止、策略请求、进度及所有权移交 |
| 分段与队列选择 | `PlanSegments`、`SelectQueue`；实际分段写出使用分段扩展的 `WriteSegment` / `WriteSegmentBatch` |

主函数表中的两个旧分段写出槽位已退役并返回 `PB_PACKET_UNSUPPORTED`。启用实验卸载前必须成功协商分段扩展 1.1 的必需能力，并同时设置 `PB_OPEN_EXPERIMENTAL_OFFLOADS` 和 `PB_OPEN_CHECKED_SEGMENTS`。

每个数据队列只有一个线程 owner，控制调用另行协调。批量调用可能只接纳输入前缀，必须按返回数量区分所有权；已接纳不等于远端收到。已发布的数据不得提前修改或释放，NDIS 仍持有引用时即使关闭等待超时也要保留映射。最后关闭前，应停止并等待所有调用者退出、归还所有 lease。

### Rust 绑定

- `packetbridge-sys`：C ABI 的原始结构、常量和函数指针。
- `packetbridge`：动态加载、`Adapter` / `Session` / `Queue`、批量 lease、分段和关闭/策略封装；Windows 原生功能以 `cfg(windows)` 启用。

默认 Rust `Config` 与 C 的 `PB_CONFIG_INIT` 均为 4 个队列、每队列每方向 256 个 slot、ring 容量 512 个条目，收发调用批宽上限为 64，卸载掩码为 0。HostRx/HostTx 工作字节预算分别为 512/128 KiB；默认共享区约 128.52 MiB，共享区上限为 256 MiB。`Queue` 不能跨线程传递，lease 的借用约束用于阻止会话关闭后继续访问缓冲区。应用应按队列数分别创建工作线程，让各线程取得并使用自己的队列。分段写出扩展仍独立限制为每次最多 32 段，URO/RSC 单个合并对象仍最多包含 32 个原始包。

接入时使用 [`rust/Cargo.toml`](rust/Cargo.toml) 中的工作区，或通过应用的本地路径依赖引用 `rust/packetbridge`。如需将绑定复制到另一应用，可使用 [`sync-rust-bindings.ps1`](tools/package/sync-rust-bindings.ps1) 的 `-ApplicationRoot` 参数；该脚本会检查目标 vendor 文件的所有权和哈希。

`Library::load` 加载同一个 `wintun.dll`，要求调用者保证 DLL 来源可信。加载时检查主函数表、生命周期扩展和分段扩展，旧 DLL 应配对旧绑定。`create_adapter_with_diagnostics` 保留原始 Windows 错误及恢复信息。

`Session::request_stop` 只确认本地停止接纳；只有 `prepare_close` 成功才能视为原生排空已确认。`close(timeout_ms)` 失败时保留会话，`Session` 的 Drop 将剩余清理交给持有独立 DLL 引用的原生运行时。关闭、策略和回退状态机的详细约定见[生命周期实施记录](docs/dll-lifecycle-implementation.md)。

## 验证与诊断

[`tools/build/build.ps1`](tools/build/build.ps1) 的 `Tests` / `Safety` 是离线验证入口，覆盖分段/校验和、合并、以太网边界、队列顺序、token、引用与完成、故障和关闭、资源分配失败及 Rust FFI。它们不要求将测试驱动安装到系统中。

[`tools/audit/validate-dll-lifecycle.ps1`](tools/audit/validate-dll-lifecycle.ps1) 汇总生命周期相关构建、旧版编译、静态分析及宿主基准；其他专项验证脚本位于同目录。宿主微基准只能说明对应代码路径的成本，真实吞吐、CPU/GB 和尾延迟需在匹配驱动的目标系统上对照测试。

[`tools/audit/bench-exchange.ps1`](tools/audit/bench-exchange.ps1) 使用实际 DLL 队列函数和共享核心、真实 Windows 事件及并发线程，对照批宽、队列数、slot/ring 深度和字节预算。支持隔离编译 32/64/128/256 批宽及 64/128/256 MiB 共享区，以便重放旧配置和比较候选；当前生产代码上限为 64 项、256 MiB。`-TokenValidation Bitmap` 只生成隔离的线性去重实验核心，不改生产文件。输出原始 JSONL、源码/二进制哈希及单元测试记录，使用 [`analyze-exchange.py`](tools/audit/analyze-exchange.py) 汇总。该基准只测描述符与少量数据标记的交付，不代表完整载荷或真实驱动吞吐。

本机参数对照及重放配置见 [用户态／内核交付参数分析](docs/exchange-throughput.md)。已采用其中的 4 队列、256 slot、ring 512、批宽 64 组合；多队列结果以流量分布到多个队列为前提，不能直接外推为单流或真实链路最大吞吐。

默认切换的 [验证记录](docs/throughput-defaults-validation.json) 包含 C/Rust 64 项批次、内核接收预算与私有快照复用测试，以及完整 Tests/Safety、SYS/DLL 构建和静态分析结果。

切换后的 [新旧默认复测](docs/default-throughput-comparison.md) 使用生产默认直接编译并交替重放旧参数：9 轮、162 个样本；1500 字节描述符交付的 HostRx/HostTx 中位数分别达到旧默认的 19.53/33.82 倍。报告包含单队列加深对照、资源代价、原始样本及复测脚本；仍为宿主交付基准。

`Probe` 阶段生成的 `pbprobe.exe` 支持：

```text
pbprobe --list
pbprobe --self-test
pbprobe --inspect DEVICE
pbprobe --rx-returns DEVICE
pbprobe --tx-rejects DEVICE
pbprobe --tx-pressure DEVICE
```

`DEVICE` 为 `--list` 输出的实际设备接口路径。`--inspect` 可查看构建身份、NDIS 版本、能力掩码及卸载计数；另外三个查询分别显示接收归还、发送拒绝和发送池压力。`--policy DEVICE MASK` 会修改策略，`--hold-device` 会创建临时诊断设备。历史 ABI 1 的 `--relay` 已禁用。

进一步阅读：[数据路径设计](docs/packet-path-v2-execution-plan.md)、[早期实现记录与卸载验证](docs/offload-capability-probe.md)、[分段边界审查](docs/shared-segment-plan-audit.md)、[TCP 卸载验证记录](docs/tcp-offload-validation.md)。历史文档的状态应结合当前源码和本次构建证据判断。

## 旧版 Wintun 使用方式

以下 `Wintun*` 调用方式适用于本项目的统一兼容 DLL。官方 [wintun.net](https://www.wintun.net/) DLL 属于另一套完整驱动，不能用于连接本项目的 SYS，也不包含 `Pb*` 或 `WintunShutdown`。

将 [`api/wintun.h`](api/wintun.h) 加入应用，通过 `LoadLibraryEx` 加载库，再用 `GetProcAddress` 按头文件中的函数类型解析导出。完整装载与网络配置示例见 [`example/example.c`](example/example.c)。

先创建适配器，并由应用配置地址、路由及所需网络状态，然后启动会话：

```c
WINTUN_ADAPTER_HANDLE Adapter = WintunCreateAdapter(L"OfficeNet", L"Wintun", &SomeFixedGUID);
if (!Adapter)
    return; /* 使用 GetLastError() 获取错误 */

WINTUN_SESSION_HANDLE Session = WintunStartSession(Adapter, 0x400000);
if (!Session)
{
    WintunCloseAdapter(Adapter);
    return;
}
```

用户态向 Windows 注入一个 IP 包：

```c
BYTE *OutgoingPacket = WintunAllocateSendPacket(Session, PacketDataSize);
if (OutgoingPacket)
{
    memcpy(OutgoingPacket, PacketData, PacketDataSize);
    WintunSendPacket(Session, OutgoingPacket);
}
else if (GetLastError() != ERROR_BUFFER_OVERFLOW) /* 示例在 ring 满时丢包 */
    Log(L"写入数据包失败");
```

从 Windows 读取 IP 包，处理后归还：

```c
for (;;)
{
    DWORD IncomingPacketSize;
    BYTE *IncomingPacket = WintunReceivePacket(Session, &IncomingPacketSize);
    if (IncomingPacket)
    {
        DoSomethingWithPacket(IncomingPacket, IncomingPacketSize);
        WintunReleaseReceivePacket(Session, IncomingPacket);
    }
    else if (GetLastError() == ERROR_NO_MORE_ITEMS)
        WaitForSingleObject(WintunGetReadWaitEvent(Session), INFINITE);
    else
    {
        Log(L"读取数据包失败");
        break;
    }
}
```

以上片段省略了应用退出事件和错误处理。高负载下可以先短暂重试接收，再等待可读事件；正常退出应让收发线程响应停止信号并退出，然后调用 `WintunEndSession`、`WintunCloseAdapter`。使用本分支旧版 DLL 且需要显式卸载时，还要遵守下面的 `WintunShutdown` 约定。

### 旧版 API 参考

#### 常量与类型

| 名称 | 含义 |
| --- | --- |
| `WINTUN_MIN_RING_CAPACITY = 0x20000` | ring 最小容量 128 KiB |
| `WINTUN_MAX_RING_CAPACITY = 0x4000000` | ring 最大容量 64 MiB |
| `WINTUN_MAX_IP_PACKET_SIZE = 0xFFFF` | 最大 IP 包长度 65535 字节 |
| `WINTUN_ADAPTER_HANDLE` | 不透明适配器句柄，用 `WintunCloseAdapter` 释放 |
| `WINTUN_SESSION_HANDLE` | 不透明会话句柄，用 `WintunEndSession` 释放 |
| `WINTUN_LOGGER_LEVEL` | `WINTUN_LOG_INFO` 信息、`WINTUN_LOG_WARN` 警告、`WINTUN_LOG_ERR` 错误 |
| `WINTUN_LOGGER_CALLBACK` | `void (Level, Timestamp, Message)` 日志回调；时间戳单位为自 1601-01-01 UTC 起的 100 ns，消息为宽字符串 |

头文件已不包含原 README 中遗留的 `WINTUN_MAX_POOL`、`WINTUN_ENUM_CALLBACK` 或 `WintunEnumAdapters`，因此不将它们作为当前可用接口。

#### 适配器、驱动与日志

| 函数 | 参数、返回值与资源约定 |
| --- | --- |
| `WintunCreateAdapter(Name, TunnelType, RequestedGUID)` | 创建适配器。`Name` 和 `TunnelType` 为以零结尾、最多 `MAX_ADAPTER_NAME - 1` 字符的字符串；可选 GUID 影响 NLA 身份，传 `NULL` 则由系统随机选择。成功返回句柄，失败返回 `NULL`，用 `GetLastError` 取错；创建的适配器会在关闭时移除。RequestedGUID 使用未公开的系统机制，仍应处理创建失败。 |
| `WintunOpenAdapter(Name)` | 按名称打开现有适配器；成功返回需关闭的句柄，失败返回 `NULL`。 |
| `WintunCloseAdapter(Adapter)` | 释放创建或打开的适配器；由 `WintunCreateAdapter` 创建的适配器同时被移除。 |
| `WintunDeleteDriver()` | 没有适配器使用驱动时删除驱动；成功返回非零，失败返回零并设置最后错误。 |
| `WintunGetAdapterLUID(Adapter, Luid)` | 将适配器的 `NET_LUID` 写入输出指针；动态解析时使用导出名中的大写 `LUID`。 |
| `WintunGetRunningDriverVersion()` | 返回当前已加载驱动的版本，失败返回零；`ERROR_FILE_NOT_FOUND` 表示没有加载 Wintun。 |
| `WintunSetLogger(NewLogger)` | 设置全局日志回调，传 `NULL` 禁用。回调可能由不同线程并发执行，需要串行化时由应用处理。 |

#### 会话与数据包

| 函数 | 参数、返回值与资源约定 |
| --- | --- |
| `WintunStartSession(Adapter, Capacity)` | 建立会话；容量须在上述最小/最大值之间（含端点），且为 2 的幂。成功返回需结束的会话句柄，失败返回 `NULL`。 |
| `WintunEndSession(Session)` | 结束由 `WintunStartSession` 创建的会话。 |
| `WintunGetReadWaitEvent(Session)` | 返回会话管理的可读事件。接收返回 `ERROR_NO_MORE_ITEMS` 时可等待该事件后重试；调用者不能对它执行 `CloseHandle`。 |
| `WintunReceivePacket(Session, PacketSize)` | 读取一个三层 IPv4/IPv6 包，将长度写入 `PacketSize`。成功返回可修改的数据指针，处理后必须归还。失败返回 `NULL`：`ERROR_HANDLE_EOF` 表示适配器正在终止，`ERROR_NO_MORE_ITEMS` 表示暂无包，`ERROR_INVALID_DATA` 表示 ring 损坏。此函数线程安全。 |
| `WintunReleaseReceivePacket(Session, Packet)` | 归还通过 `WintunReceivePacket` 取得的包。此函数线程安全。 |
| `WintunAllocateSendPacket(Session, PacketSize)` | 为待注入的三层包申请准确长度的缓冲区，长度不超过 `WINTUN_MAX_IP_PACKET_SIZE`。失败返回 `NULL`：`ERROR_HANDLE_EOF` 表示正在终止，`ERROR_BUFFER_OVERFLOW` 表示 ring 已满。此函数线程安全，分配调用的先后决定发送顺序。 |
| `WintunSendPacket(Session, Packet)` | 提交通过 `WintunAllocateSendPacket` 取得并填充的包，释放内部缓冲区。此函数线程安全，但提交返回不保证该包已实际发送；顺序仍由分配顺序决定。 |

完整的函数类型、调用约定和参数标注以 [`api/wintun.h`](api/wintun.h) 为准。

#### 本分支新增的 WintunShutdown

```c
BOOL WintunShutdown(DWORD Timeout);
```

`Timeout` 单位为毫秒，一个总预算覆盖孤立设备清理、保留的控制任务、辅助进程、日志回调和依赖租约。调用会停止新的适配器/驱动操作，并取消尚在等待的创建、打开和删除请求。先停止发起新调用；成功关闭或卸载前仍须等待所有公开调用者退出并结束会话、关闭适配器。不得从日志回调或 `DllMain` 调用。

超时返回零并设置 `ERROR_TIMEOUT`，仍在执行的工作继续持有自身资源和 DLL 引用，可以重试 shutdown。进入 shutdown 的 DLL 实例不会重新开放控制操作，需要完成关闭并卸载后才能加载新实例。

创建、打开和删除驱动会复制输入到有界原生执行器，每个 DLL 实例最多四个任务。调用方总等待期限为两分钟，覆盖调度和嵌套等待；软件设备回调等待还受十五秒上限约束。未返回的系统调用或关闭屏障可以超过调用方期限，工作线程仍保留互斥量、上下文、依赖和模块引用，并关闭调用方已离开后才成功创建的适配器。总等待超时后，该实例停止接纳新的控制操作。同一工作线程中的日志回调重入这三类操作返回 `ERROR_BUSY`。

跨架构辅助调用同样有两分钟期限和每 DLL 最多四个保留任务。超时后直到进程实际退出，仍保留进程及临时包；新的控制调用返回 `ERROR_SHUTDOWN_IN_PROGRESS`，不会强制终止安装进程。未调用显式 shutdown 的旧客户端，也会在最后一个操作、适配器或后台任务归还租约后释放延迟加载依赖。

### 旧版工程构建与分发

发布构建统一由 `tools/build/build.ps1` 管理。`wintun.proj` 及 Visual Studio 的 API/Driver 构建入口转发到该流水线，避免生成另一套内核协议。目前发布目标为 x64；其他架构尚未完成移植和 ABI 验收。

如需在专用测试系统中加载测试签名驱动，可按该系统要求启用测试签名；原上游开发流程使用 `bcdedit /set testsigning on` 后重启。本仓库的离线构建和测试不需要执行该步骤。

兼容入口沿用 `wintun.dll` 文件名，以支持已有应用的加载方式；产品、驱动、硬件 ID 和安装作用域均使用 PacketBridgeV2，不能冒充官方发布包或覆盖其设备。

## 许可证

保留上游版权声明：Copyright © 2018-2026 WireGuard LLC. All Rights Reserved。

仓库源码的总体许可证为 [GPLv2](COPYING)；个别文件以自身 SPDX 声明为准，例如 [`api/wintun.h`](api/wintun.h) 标注为 `GPL-2.0 OR MIT`。Rust 工作区标注为 `GPL-2.0-only`。

从 [wintun.net](https://www.wintun.net/)下载的官方预编译 DLL 使用其发布 ZIP 内附的预编译二进制许可证，该许可证针对官方分发的特定二进制文件。自行构建的 PacketBridge 产物不能直接套用该预编译授权。
