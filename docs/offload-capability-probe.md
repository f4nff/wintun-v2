# PacketBridgeV2 1.0.1 开发与接入状态

日期：2026-10-28。按用户要求，开发、单元测试和未签名构建先完成，签名统一放到最后。目前没有安装或加载新 SYS，没有内核运行、Verifier 或性能收益证据。本目录旧的 M1 签名包属于历史快照，不应继续提交签名。

## 当前实现

| 部分 | 已实现的代码与离线验证 | 运行限制 |
| --- | --- | --- |
| 驱动前端 | 独立 Ethernet 802.3 miniport，6 字节本地 MAC，ARP/NDP 终止，DAD 不响应；解析单层内联 802.1Q；共享边界仍为 L3 | 属于路由型 IP 适配器，不提供 VLAN trunk；尚未做 Windows 连通性验证 |
| ABI v2 | 固定布局、独立 TX/RX 数据池、四条 SPSC 队列、epoch/generation/token、1–8 队列、每队列独立事件/线程 | 必须使用同一源码指纹的 DLL/SYS，禁止原始 Wintun ring 注册 |
| 批处理 | 真正批量获取/归还/预留/取消/提交/完成；每轮最多 32 对象及 128 KiB；批量 indication 和源 NBL completion | 不等待凑满一批，不承诺吞吐提升倍数 |
| HostTx | TCP LSOv2、UDP USO、IPv4/IPv6 TX checksum；OOB 偏移包含以太网头，跨 ABI 后转换为 L3 偏移 | 32 KiB 卸载负载上限；USO IPv6 扩展头不宣告支持 |
| HostRx | 相邻、严格可验证子集的 TCP RSC/UDP URO；私有副本验校验和；最多两份合并缓存/队列；OOB 与 RSC 统计 | 不合并 TCP 选项、扩展头、控制包、零 UDP checksum 等；普通独立包回退 |
| 生命周期 | 真正 NDIS 归还后再完成；pause/restart；停止时关闭接纳并排空；超时保留映射与 DLL，不强制回收 | 未进行真实 NDIS 乱序归还、pause、卸载及 Verifier 测试 |
| Rust 绑定 | 两个 1.0.1 crate；固定 C 函数表 280 字节；Queue 不可跨线程；lease Drop 按所有权处理；真实 C 测试 DLL 跨 FFI 验证 | 不把原生测试 shim 当作内核运行 |
| 实际客户端 | D:\rust\rust-quiche-vpn；自动选择、批量 I/O、按地址分队列、TCP 代次授权跨提交保存、GSO 在 netstack 前分段、受控停止与回收 | 现有客户端版本 1.2.35 保留；新组件统一 1.0.1。多队列按 IP 地址对分配，同地址对不同端口不会横向分散 |

默认卸载掩码为 0。实际有效掩码同时受应用上限、NDIS 配置、封装握手、能力依赖及会话状态限制。未收到封装开启请求前不启用 RX 卸载。LSOv2 依赖相应 TX checksum，RSC/URO 依赖相应 RX checksum。

前端选择依据：[任务卸载组合约束](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/combining-types-of-task-offloads)、[封装握手](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/oid-offload-encapsulation)、[RSC indication](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/indicating-coalesced-segments)、[USO](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-segmentation-offload-uso-)。原版 master 与官方 0.14.1 组合独立保留。

## 客户端选择与回退

| 环境变量 | 含义 |
| --- | --- |
| `QUIC_WINDOWS_TUN_BACKEND=auto` | 默认：存在匹配的新 DLL/驱动时使用 v2 plain；缺失、初始化失败或该构建故障记录命中时，在安装隧道路由前选择原版组合 |
| `...=legacy` | 强制使用内嵌的官方 Wintun DLL/SYS；忽略新版队列、DLL 和卸载配置 |
| `...=plain` | 强制新版普通包；失败明确返回，不悄悄改变指定 profile |
| `...=offloads` | 显式实验卸载配置；不得视为已验收默认值 |
| `QUIC_WINDOWS_TUN_DLL` | 新 DLL 绝对路径；默认客户端 EXE 同目录的 packetbridge.dll |
| `QUIC_WINDOWS_TUN_QUEUES` | 1–8，默认 1；需要多地址对负载验证实际扩展 |
| `QUIC_WINDOWS_TUN_OFFLOAD_MASK` | 仅 offloads profile 接受；默认 0x1ffff；未知位拒绝 |

回退分三种边界：NDIS/策略关闭功能时保留普通包能力；启动期新版失败时使用完整 legacy；运行期原生错误时沿用客户端原有故障停止流程，关闭隧道，不重放已接受的包，记录构建故障后让**下一次 auto 启动**选择 legacy。当前没有实现不中断连接的运行中整包热切换，也没有宣称能从内核蓝屏中热恢复。

故障记录位于 `%LOCALAPPDATA%/vpn-client/packetbridge-fault.json`，按源码指纹隔离。明确选择 plain/offloads 可重试该构建。原版缓存加载前逐字节核对内嵌 DLL，不能仅凭长度或 marker 选择错误内容。

## 构建和离线验证

使用 PowerShell 7、VS 2022、SDK 10.0.26100.0、仓库 WDK。版本来自 wintun.props。源码、文档或脚本有变化后重建所有阶段：

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\build\build.ps1 -Stage Driver
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
.\tools\package\sync-rust-bindings.ps1 -ApplicationRoot D:\rust\rust-quiche-vpn
```

输出 `out/packet-path-v2/1.0.1/<源码指纹前12位>/`。开发 DLL 没有嵌入驱动，需要已安装匹配私有驱动；绝不能替换原版 wintun.dll。Tests 包括分段/校验和 oracle、RSC/URO、以太网边界、随机输入、十万次所有权转换、百万并发描述符、C API 和 Rust 原生 FFI，以及 Queue 不可跨线程的编译失败测试。具体计数与结果以该构建 test-results.json 为准。

客户端使用显式 `--target x86_64-pc-windows-gnu --offline` 运行 cargo check/test；项目默认 target 是 Linux，不能省略。当前环境 BoringSSL bindgen 使用本地 libclang 18.1.1 及 MinGW 16.1.0 include，工具位于 Wintun 的 Release/build-tools，下载文件 SHA256 在 libclang/source.json。客户端全量单元测试：786 通过、33 原有忽略、0 失败；这不包含真实新驱动运行。

## 最后签名与 DLL 封装

源码定版并完成所有开发检查后，create-signing-bundle.ps1 固定当前 SYS/INF/CAT/PDB、测试和哈希，不覆盖同名包。签名只允许改签名数据；INF 与映像主体必须一致。接收最终 SYS/INF/CAT 后：

```powershell
.\tools\package\verify-signed-driver.ps1 -Directory D:\signed\PacketBridgeV2 -ExpectedManifest <原包signing-input.json>
.\tools\build\build.ps1 -Stage FinalApi -SignedDriverDirectory D:\signed\PacketBridgeV2 -ExpectedManifest <原包signing-input.json>
```

FinalApi 首先验证签名、版本、源码指纹、catalog/SIP 映像和 INF；随后仅编译 DLL，将最终三文件原样嵌入，并提取资源逐项核对 SHA256。最终 DLL 创建适配器前由 SetupAPI 暂存其签名包；开发 DLL 不执行暂存。输出独立 final-api 目录，已有结果拒绝覆盖；`production_ready=false` 直到实际运行验收完成。

签名后的专用测试机步骤包括：干净安装、普通双向 IP、真实 LSO/USO 请求、RSC/URO 计数和抓包校验、各项动态关闭、停止/乱序归还/卸载、Verifier、长稳和性能对照。pbprobe 支持 list/inspect/policy/hold-device；历史 ABI 1 的未加密 relay 已禁用。最终签名后再执行这些步骤，目前不改启动设置或系统网络。
