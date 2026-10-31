# PacketBridgeV2 2.0.0：发送拒绝与外层 UDP 降级

产品、DLL、SYS、SDK 保持 2.0.0/x64；API/Data 2.1、packet 1.1、lifecycle 1 不变。
旧 probe 232 字节、RX-return schema 1 / 4224 字节也不变。

## 现有证据与边界

目标机 bdb37d933e1f 快照的 `offload_rejected=14` 只有累计值，无法恢复历史原始状态。
它来自发送路径的拒绝分支，含队列/池资源、生命周期、解析与策略等失败，不等于
14 个唯一 NBL，也不能与 5 条 ICMPv6 的 RX-return 拒收合并归因。

新增独立只读 IOCTL function 0x803、schema 1：
`include/packetbridge_send_diagnostics.h` 定义 96 字节事件及 6528 字节快照。
`pbprobe --tx-rejects '<设备路径>'` 输出：

- 原始 `ndis_status_raw`、此前 NBL 状态、拒绝分支；未知状态不改写成通用错误。
- 原始 LSO/USO/checksum OOB 位值，在 LSO completion 改写和归还 NBL 所有权前复制。
- session epoch（未取得 session pin 时为 0）、队列（未知为 UINT32_MAX）、NB 长度及范围。
- 包解析/队列返回码、所需与实际有效 mask，以及发布失败的接受/尝试数。
- adapter 生命周期分支总数、最近 64 条记录与覆盖数量。

计数单位是拒绝分支事件，保持旧计数行为；同一个 NBL 可能跨多个失败点。
事件是分支当时的状态，不冒充所有最终发送完成的审计；无记录不代表 peer 送达。
发布事件中的 NB 长度属于该 NBL 的首个 NB；其他分支属于被拒绝的 NB，字段明确标记。
不记录 payload、MDL 内容、指针、对端地址，不额外读取 IP 字节。
成功热路径不新增诊断锁、分配、时钟或回调；异常路径用独立 spin lock 写有界历史。
查询只取得同一诊断锁，内核输出位于 METHOD_BUFFERED 非分页缓冲区，不把快照放内核栈。

发送分支还确认并修正一处状态覆盖：同一 NBL 的后续 NB 已失败时，先前 NB 延后发布
再失败，不得把此前的原始错误无条件覆盖成 RESOURCES。保留先出现的失败；不改变
token 释放、已发布前缀、窗口预算、完成回调或 pin 的归还顺序。

## 外层接收修正

真实 Windows 回环测试在同步 WSARecvMsg 与 IOCP 上均复现：一个合法 2800 字节普通
UDP 被旧的 1500 字节判断丢弃，并永久关闭该 socket 的接收合并。
1500 是普通消费者缓冲区约定，不是 UDP 协议边界。修正将该检查限制于原生捕获容量，
无 COALESCED_INFO 的完整消息按一个 UDP 报文交付，不从长度猜 stride。
小消费者只丢弃装不下的那个原始报文，继续公平处理，不因此关闭卸载。
截断、坏/重复控制项、非法 stride、计数不一致、关闭失败等既有检查继续保留。
IPv4/IPv6、2800/8192 字节、owned/copy 消费、后续普通报文与真实坏元数据回归覆盖该边界。

这修复已复现的客户端误降级，不证明历史 2800 字节的线上边界，也不证明物理 NIC URO。
依据 [Winsock UDP 契约](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-udp-socket-options)，
普通未合并消息仍可大于合并选项阈值；只有 COALESCED_INFO 授权拆分。

## 目标机复测

沿用用户确认的旧 PacketBridge 驱动/设备清理和 F7 未签名测试流程，一起更新 EXE、DLL、probe。
运行前后分别采集 inspect、rx-returns、tx-rejects，核对 matching_build 和源码指纹；
不同 adapter 生命周期计数不能相减。保留原始日志和 JSON，不覆盖旧包或用户签名输入。
旧 14 次原因不可追溯；新记录用于确定下一轮实际拒绝分支，确认前不放宽包验证或重放数据。
