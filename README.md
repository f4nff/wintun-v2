# PacketBridge v2：基于 Wintun 的 Windows 三层网络适配器

本项目基于 [Wintun 原版](https://git.zx2c4.com/wintun/)开发，在保留旧版 Wintun 源码与接口的同时，新增独立的 PacketBridge v2 驱动、批量多队列数据路径、TCP/UDP 卸载处理、C 接口和 Rust 绑定。面向应用的共享数据仍是 IPv4/IPv6 三层报文。

Wintun 最初为 [WireGuard](https://www.wireguard.com/)开发，适合主要在用户态实现网络隧道的应用。本项目的 v2 组件使用独立名称 `packetbridge.dll` / `packetbridge.sys`，需要应用接入新接口。

## 与 Wintun 原版的区别

对比日期：2026-10-31。基线为上游 `master` 的提交 [`ec0a6b98456fe1ba52567bb2add4bbf5f64315a1`](https://git.zx2c4.com/wintun/commit/?id=ec0a6b98456fe1ba52567bb2add4bbf5f64315a1)，也是当前仓库的 Git HEAD；本次核对时上游 `master` 仍指向该提交。以下对比包含当前工作区中已修改和新增的源码，不仅限于已经提交的内容，也不以官方 0.14.1 发布包代替 master 基线。

| 方面 | Wintun 原版 master | 当前项目 |
| --- | --- | --- |
| 项目结构 | C 驱动、`wintun.dll`、C 头文件及示例 | 保留原有代码，新增 `driver/v2`、`api/v2`、`common`、`include`、Rust 工作区及构建、验证工具 |
| 系统目标 | Windows 7、8、8.1、10、11 | 旧版代码保留原有目标；v2 当前仅面向 Windows x64，最低 Windows 11 24H2 / build 26100，使用 NDIS 6.89 |
| 驱动前端 | `NdisMediumIP` 三层 TUN | v2 对 NDIS 呈现 Ethernet 802.3，处理必要的 ARP/NDP；DLL/Rust 边界仍交付三层 IP 包 |
| 部署身份 | `wintun.dll`、`wintun.sys` 和 Wintun 设备身份 | v2 使用 `packetbridge.dll`、`packetbridge.sys`、`packetbridge.inf`、`packetbridge.cat` 及独立服务/设备身份 |
| 数据组织 | 一组收发字节 ring，逐包申请、发送、接收和释放 | 独立 TX/RX 数据池、描述符与 token；每个队列有 ready、recycle、submit、completion 四条有界队列 |
| 批处理与并行 | 公开 API 以单包操作为主 | 1–8 个数据队列，批量获取、归还、预留、取消、提交和完成；单批最多 32 个对象 |
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
- v2 不兼容原版的 ring 注册协议，也不能通过重命名 DLL 替换旧接口。回退到原版时需使用完整匹配的 legacy DLL/SYS 组合，由应用负责切换。
- 本仓库提供本机数据路径和绑定；隧道协议、加密、路由配置及外层 UDP/QUIC socket 的处理由应用负责。

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
| [`driver/wintun.c`](driver/wintun.c)、[`driver/v2/`](driver/v2/) | 驱动入口、按构建条件选择的旧版/v2 路径及 v2 队列、收发、回收和卸载实现 |
| [`api/`](api/)、[`api/v2/`](api/v2/) | 旧版 Wintun DLL 与新增 PacketBridge DLL |
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
.\tools\build\build.ps1 -Stage Safety
.\tools\build\build.ps1 -Stage Driver
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
```

默认阶段是 `Tests`。产物写入 `out/packet-path-v2/<产品版本>/<源码指纹前12位>/`，驱动包位于其中的 `driver/`。Tests/Safety 生成各自的实际执行记录；测试结果应读取本次构建的 JSON 和日志。

`Driver` 生成未签名开发驱动。普通 `Api` 生成的 `packetbridge.dll` 不内嵌驱动包，需要目标系统已经安装匹配的私有驱动；构建本身不会安装或加载新驱动。

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

内嵌 DLL 创建适配器时暂存驱动包，并检查实际绑定驱动的版本、ABI 和完整源码指纹。特定初始化失败会按配置的适配器名称定位设备及其实际绑定的 OEM INF，执行一次卸载重装后重试；同名设备可能包含旧 Wintun 适配器，名称应由应用明确管理。失败后返回原始诊断，不在 DLL 内自动切换到旧版 Wintun。恢复范围和诊断字段见[安装恢复说明](docs/native-install-recovery.md)。

## v2 接入方式

### 数据方向与 C 接口

本项目统一使用以下方向，避免与应用视角的“收发”混淆：

- **HostTx**：Windows 协议栈 → 驱动 → 用户态，应用通过 `ReceiveBatch` 获取，通过 `ReleaseBatch` 归还。
- **HostRx**：用户态 → 驱动 → Windows 协议栈，应用通过 `ReserveBatch` 预留、`CommitBatch` 提交，再用 `PollCompletions` 获取完成；未发布的 lease 可以取消。

C 调用者加载 `packetbridge.dll`，取得 `PbGetInterfaceV2` 并协商函数表大小和主版本；同时按需协商 `PbGetLifecycleInterface` 和 `PbGetPacketInterface`。定义分别见 [`packetbridge.h`](api/v2/packetbridge.h) 与 [`lifecycle.h`](api/v2/lifecycle.h)。创建适配器的结构化诊断入口为 `PbCreateAdapterEx` / `PbCreateAdapterEx2`。

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

默认 `Config` 为 1 个队列、32 个 slot、ring 容量 64 个条目，卸载掩码为 0。`Queue` 不能跨线程传递，lease 的借用约束用于阻止会话关闭后继续访问缓冲区。应用创建队列工作线程时，应让各线程分别取得并使用自己的队列。

接入时使用 [`rust/Cargo.toml`](rust/Cargo.toml) 中的工作区，或通过应用的本地路径依赖引用 `rust/packetbridge`。如需将绑定复制到另一应用，可使用 [`sync-rust-bindings.ps1`](tools/package/sync-rust-bindings.ps1) 的 `-ApplicationRoot` 参数；该脚本会检查目标 vendor 文件的所有权和哈希。

`Library::load` 要求调用者保证 DLL 来源可信。加载时检查主函数表、生命周期扩展和分段扩展，旧 DLL 应配对旧绑定。`create_adapter_with_diagnostics` 保留原始 Windows 错误及恢复信息。

`Session::request_stop` 只确认本地停止接纳；只有 `prepare_close` 成功才能视为原生排空已确认。`close(timeout_ms)` 失败时保留会话，`Session` 的 Drop 将剩余清理交给持有独立 DLL 引用的原生运行时。关闭、策略和回退状态机的详细约定见[生命周期实施记录](docs/dll-lifecycle-implementation.md)。

## 验证与诊断

[`tools/build/build.ps1`](tools/build/build.ps1) 的 `Tests` / `Safety` 是离线验证入口，覆盖分段/校验和、合并、以太网边界、队列顺序、token、引用与完成、故障和关闭、资源分配失败及 Rust FFI。它们不要求将测试驱动安装到系统中。

[`tools/audit/validate-dll-lifecycle.ps1`](tools/audit/validate-dll-lifecycle.ps1) 汇总生命周期相关构建、旧版编译、静态分析及宿主基准；其他专项验证脚本位于同目录。宿主微基准只能说明对应代码路径的成本，真实吞吐、CPU/GB 和尾延迟需在匹配驱动的目标系统上对照测试。

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

以下内容只适用于 `wintun.dll` 和 `Wintun*` API。原版部署可从 [wintun.net](https://www.wintun.net/)获取平台匹配的官方 DLL，与应用放在同一目录。官方 DLL 不包含本项目的 v2 接口，也不包含本分支新增的 `WintunShutdown`。

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

`wintun.sln` 保留上游 Visual Studio 开发入口；上游工具链要求为 Visual Studio 2019、Windows SDK 和 WDK。v2 应使用前面的独立 PowerShell 构建流程，不能将两套产物混用。

如需在专用测试系统中加载测试签名驱动，可按该系统要求启用测试签名；原上游开发流程使用 `bcdedit /set testsigning on` 后重启。本仓库的离线构建和测试不需要执行该步骤。

沿用上游的分发要求：不要分发自行构建但仍以“Wintun”命名的驱动或文件，以免与官方部署冲突。使用原版功能时分发官方 DLL；本项目新增路线使用独立 PacketBridge 名称。

## 许可证

保留上游版权声明：Copyright © 2018-2026 WireGuard LLC. All Rights Reserved。

仓库源码的总体许可证为 [GPLv2](COPYING)；个别文件以自身 SPDX 声明为准，例如 [`api/wintun.h`](api/wintun.h) 标注为 `GPL-2.0 OR MIT`。Rust 工作区标注为 `GPL-2.0-only`。

从 [wintun.net](https://www.wintun.net/)下载的官方预编译 DLL 使用其发布 ZIP 内附的预编译二进制许可证，该许可证针对官方分发的特定二进制文件。自行构建的 PacketBridge 产物不能直接套用该预编译授权。
