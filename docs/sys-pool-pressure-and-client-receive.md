# PacketBridgeV2 2.0.0：完整外层报文与发送槽归还

产品版本固定为 2.0.0/x64。API/Data 2.1、packet 1.1、lifecycle 1、旧 probe、
RX-return schema 1 与 TX-rejection schema 1 均不变。

## 实机证据与归因边界

2026-10-30 07:49:16–07:53:51 的 d0ae13c8094c / client-3eaf16169b881dc1 日志：

- 原生外层接收 28,496 次，路由层接收 27,137 次；1,359 次消费者容量拒绝。
  拒绝日志明确显示输出容量 1,500。末尾累计字节差 4,530,313；计数为本地观察，
  不是物理链路丢包或唯一业务字节。
- 接收合并未关闭；未观察到 COALESCED_INFO。大报文的真实线上边界仍需对端和
  物理网卡边界捕获；不得由 2,800 等长度猜出分段大小。
- 1,139 次 TX 拒绝分支全部 pool-full；最近 64 条 raw NDIS 0xc000009a，
  core PB_FULL(2)，NB 长度 1,270，LSO/USO/checksum OOB 均为 0。
  它证明预分配 TX 池压力，不证明系统物理内存耗尽、永久槽泄漏或唯一 NBL 数量。
- 2,958 次 QUIC 载荷解密拒绝仍未归因。修复完整报文交付不等于修复缺失的
  原生分段元数据，更不等于通过物理网卡硬件卸载验证。

## 客户端完整报文交付

DatagramSocketRecv 新增默认的 poll_recv_from_vec；旧实现仍走原来的普通容量，
Box/&mut 转发保持接收者的实现。路由层改为接收 Vec 所有权，保留原来的目的地址、
接收时间、IOCP 完成观察和诊断计数。

Windows 客户端在实际选中的 current/candidate socket 上使用 native 接收者的
poll_recv_vec，按完整原始报文长度复制一次，不从应用侧 1,500 字节上限截断或丢弃。
迁移选择、旧 owner 退出、关闭失败、时间和绑定地址仍走同一选择函数。
同步普通降级复用 socket owner 的一个有界捕获缓冲区；路由中的小报文只持有自身长度，
不会各自保留 64 KiB。Pending/错误清空本次输出并清除上次交付元数据。
坏/重复 CMSG、原生截断、非法 stride、计数不一致和公平预算继续按既有检查处理。
外层 QUIC 参数与物理 MTU 不因捕获容量扩大而改变，不授权按长度拆 UDP。

本机真实 Windows UDP 回归覆盖同步/IOCP × 合并开/关、IPv4/IPv6、空报文、
1/1,400/2,800/8,192/60,000 字节及大报文后的单包。通过 trait 对象接收并校验
逐字节内容、来源、绑定地址、Pending 清空、native lease 归还和小报文容量。
路由层另测自定义 owned 接收，不得回到固定 ReadBuf 或从长度推断 GRO。
既有 QUIC/H3、迁移提交/撤销、关闭、双栈和独立收发后端回归继续运行。

## 已完成描述符的前缀归还

原客户端只在整个 ReceivedBatch 全部展开后 Drop。若同批多个 GSO 描述符跨越
多个 32 包生成轮次，已完成的槽会随未完成尾部继续被占用；下游压力会延长这种占用。

Rust SDK 的 ReceivedBatch::release_prefix 只归还已完成前缀，接受多少就从本地
loan 列表移除多少。客户端每个有界生成轮次后归还完成项，按实际接受数量重定位
描述符游标；未完成 GSO 的段游标和 immutable loan 保持有效。生成输出已复制为
BytesMut，可独立交给下游。常规整批完成也显式检查归还结果；退出 Drop 保留兜底。
不改默认 32 槽/64 ring，不增加待发送 NBL 队列、等待凑批或无界缓存。

现有 batch-stats 增加 rx_retire_calls/rx_retired_descriptors，表示用户已归还 token，
不是 SYS 已实际回收。它们与 rx_descriptors 单位一致，和展开后的 IP 包数不同。
计数仍由队列 owner 局部累计、原有周期观察器请求发布，不增加每包原子操作/时钟/唤醒。
停止过程中 Drop 兜底归还不在该常规路径计数中。

## SYS 锁窗口中的迟到归还

确定性执行真实 PbV2Send/PbV2RecycleLocked 复现：入口已回收后，用户独立生产者
在 TxLock 持有期间又归还有效 token。原发送代码第一次遇池满时不重新读取 recycle，
因此返回 RESOURCES，即使可复用 token 已在 recycle ring 中。

首次池满现在每个队列锁窗口最多再做一次原有 128-token 有界回收；仅当私有 recycle
游标前进才重新 reserve。无新归还不做额外池扫描；后续满池缓存仍到窗口结束才重置。
坏 token 立即 fault/停止本次入队，保留原始诊断；不在失败之后继续使用其余 token。
不等待用户态、不重放 NBL、不提前释放 borrowed 槽，不改变首个错误、回调或 pin 顺序。
成功且未遇池满的路径不增加回收。单窗口最多入口 128 + 压力刷新 128 次 token 退休；
原 32-attempt/128-KiB admission 工作预算和队列锁切换仍有效。

这确认了可导致 pool-full 的机制，不证明历史 1,139 次全部属于此机制；下游持续
慢于输入时，真实的有界池压力仍应返回原始 RESOURCES。

## 归还与唤醒核对

ReleaseBatch 在 recycle 发布之后按 Armed 通知 KernelEvent。worker 清事件后先
回收再 arm/recheck：发布在 arm 之前会被 recheck 看见，之后会产生事件。
生产代码的新增确定性回归覆盖初始回收前、回收与 arm 之间、arm 与 wait 之间的
三种 TX token 归还，验证槽回到 FREE、无丢唤醒、无空转和句柄泄漏。
既有百万 ring 并发、七种 fault/event 时序、空闲归还/满 completion credit、
pause/drain/close 和 guarded ownership 测试继续运行。未据此改写已正确的唤醒协议。

## 实机复测

一起更新 EXE、嵌有 SYS/INF/CAT 的 DLL、pbprobe；沿用已确认的旧 PacketBridge
驱动清理、重启/F7 测试流程。产品仍为 2.0.0；以完整 source fingerprint 和
client_revision 区分构建，不能只比较版本字符串。

在同一适配器生命周期和同一负载前后采集 inspect、rx-returns、tx-rejects 和客户端日志。
重点检查外层 policy_reject_output-capacity、路由接收/原生接收差额、
rx_retired_descriptors 与 acquire 数量、pool-full 分支增量和最后的 native 借用状态。
不同生命周期不可相减。若仍出现大消息无 COALESCED_INFO 或解密拒绝，保留短时
物理 NIC pktmon 与 Linux 发送侧捕获，继续定位真实边界；不能宣告丢包或硬件验证完成。

依据：[Winsock UDP 契约](https://learn.microsoft.com/en-us/windows/win32/winsock/ipproto-udp-socket-options)、
[NDIS 发送完成与所有权](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismsendnetbufferlistscomplete)。
