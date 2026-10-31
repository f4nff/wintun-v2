# PacketBridgeV2 2.0.0 x64：LSOv2/RSC 验证

本轮优先验证内层 TCP，不用 USO/URO 的零计数推断 TCP 卸载状态。
产品、DLL、SYS 版本固定 2.0.0，API/Data ABI 2.1、packet ABI 1.1 不变。

## 已确认及修正

原客户端 auto mask `0x1580c` 只选择 UDP 及依赖，没有请求 LSOv2/RSC。
现在 TCP 策略包含 LSO4/6、RSC4/6 及各自必需的发送/接收校验和，完整 TCP mask 为 `0xb6b3`。
auto 在原 UDP 策略上增加 TCP 策略，mask 为 `0x1febf`；收发方向和 IP 家族仍独立按 effective mask 门控。
协商失败保留原确认清零屏障及普通批量降级，禁止重放结果不确定或已接受的数据。

原 RSC 合并器只接受 ACK 数据段，PSH 数据段会保持单包。现在允许 ACK/ACK+PSH 数据段，
并在所有接纳检查成功后 OR PSH；校验失败、乱序、ACK/窗口/流变化、容量不足不修改既有 SCU。
遵循 [Microsoft RSC 规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/rules-for-coalescing-tcp-ip-packets)
及[异常边界](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/exception-conditions-that-terminate-coalescing)。
仍不合并纯 ACK、TCP 选项、IP 选项/扩展头、控制标志及校验失败。TCP timestamp 合并需要额外 OOB，当前保持单包。

LSOv2 路径已经检查 OOB 类型、IP 家族、以太网后的 TCP 偏移、MSS、原始跨度及策略，
并由经过校验的分段接口在原始 IP 消费边界展开。既有 IPv4/IPv6、选项、边界、停机和坏元数据回归保留。

## 可观测证据

- `pbprobe --inspect`：effective mask、`lso_nbls`、`offload_rejected`、`rsc_events/packets/bytes/aborts`。
- `lso_nbls` 是收到 LSO 请求的次数，不能单独证明完成或业务送达。
- 客户端 `[PacketBridge] event=batch-stats`：每队列已接受提交和非空获取的包数分布、原始 IP 包数/字节数，
  以及实际展开的 `rx_tcp_gso_descriptors` 和 `rx_tcp_gso_original_packets`。
- 直方图区间：1、2、3–4、5–8、9–16、17–32、33+。TX 单位为每次正接受前缀的提交包数；
  RX 单位为每次非空获取的描述符数，GSO 描述符与展开后的原始包数量分开。
- 队列本地累加；由原有 INFO/full 诊断 tick 请求发布，每轮只观察请求代数，无逐包时钟/原子加，
  无新增线程、驱动查询或唤醒。多计数快照非事务性，允许滞后到工作线程自然下一轮。
  停止负载后保持 VPN 运行两秒以上再采集，不能把首个零快照当作没有批量。

## 同一目标机对照

使用同一构建、配置、负载和单 worker；每次启动测试前按用户要求清理旧 PacketBridge 包/设备，
需要时重启；未签名包按 F7 测试流程启动。只有启动成功且 matching_build=true 的轮次可比较。

| 轮次 | BACKEND | OFFLOAD_MASK | 目的 |
| --- | --- | --- | --- |
| 普通批量 | plain | 不设置 | 无卸载基线 |
| 仅 LSO | offloads | 0xb3 | 内层 TCP 上传 |
| 仅 RSC | offloads | 0xb600 | 内层 TCP 下载 |
| LSO+RSC | offloads | 0xb6b3 | 双向及组合 |

变量全名为 `QUIC_WINDOWS_TUN_BACKEND`、`QUIC_WINDOWS_TUN_OFFLOAD_MASK`。
plain 不接受显式 mask；切换到 plain 前应删除该进程环境变量。
不要在活跃会话中切换后端，也不要靠等待凑批提高计数。
持续 TCP 上/下载分别观察相应方向，采集客户端日志和 inspect；需要时同时采集 rx-returns。
验收应同时包括应用字节/完整性、拒收与错误、尾延迟、CPU、批量分布和卸载计数。
未在目标机验证前，不能凭代码或离线回归宣称 LSO/RSC 已命中或提高性能。
