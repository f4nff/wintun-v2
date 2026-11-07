# 多队列排空与 PrepareClose 复核

版本固定为 **2.0.0**。本轮以 `56d92e483bf1`、686 个文件的完整原始字节为基线，继续检查 SYS/DLL 的队列归还、停止、通知和协议边界。未确认新的生产缺陷，新增内容是回归测试、审计与验证入口。

| 新增回归 | 关键边界 | 范围 |
| --- | --- | --- |
| [多队列正常排空 32 包](../tests/model/tun32_multiqueue_drain_tests.c) | q0 上调 16 包并归还 15，q1 的 16 包留在提交环；分别先归还最后 NBL、先排空另一队列，两个障碍都解除后才可 STOPPED | 实际 BeginDrain、Receive、Return、worker 与 QUERY；两个真实 SDK worker 在退出并汇合后释放原宿主存储，未执行完整 Control/Free |
| [跨队列 PrepareClose 32 包](../tests/model/cross_queue_prepare32_tests.c) | 一个 owner 的真实 Wait 已返回系统调用，但 DLL 尾部仍持 Lifetime；另一 owner 在停止后取消 32 个预留，再跨越独立 native 引用障碍 | 实际 DLL 队列、生命周期、SDK 等待及模块卸载；内核 I/O 和状态由原 fixture 模拟 |
| [分族队列分类 32 包](../tests/packet/test_flow_family32.py) | TCP/UDP 各 32 个合法分片，以及改变端口和非地址字段的 32 包；完整报文与 59 字节 Ethernet 前缀分类一致 | 通过公开 V2 表调用实际 SelectQueue，仅取函数表、不 Open 会话或设备；两地址族分进程 |

正常排空测试使用两个队列共 32 个唯一 token。最后引用先归还时，Active=0 仍不能覆盖另一队列未到排空边界；另一队列先到界时，Active=1 仍阻止 STOPPED。最终完成项精确为 16 个 OK 和 16 个 STOPPED，且确认 STOPPED 时这些完成项尚未被 peer 消费。取消完成项保留提交 token 的第一代身份；后续[接收终结代际修复](admission-policy-review.md)使 q1 即使没有创建 NBL，也在 STOPPED 完成时将槽位代数终结为 1，测试已同步更新。NDIS 6.20/6.89、TCP/UDP、计数回绕和两种先后顺序各自验证；原始载荷、旧版本不可用 OOB 字段及引用资源均有断言。

PrepareClose 是非最终屏障。测试必须看到它成功时 Region 仍在、ResourcesClosed=0，而且另一个 Wait 尾部持有的共享 Lifetime 仍使排他获取失败。所有公开队列调用者退出并汇合后才执行最终 Close。真实取消、重复 BeginStop 的合并提示、worker 消费提示与 nativeActive=0 的最终提示分别观察；NtQueryEvent 只在测试内无消费地检查同步事件状态。SDK 入口或返回尾部标记均不冒称内核线程已经休眠。

分类回归的固定地址、FNV 值与队列 1–8 的字面预期均独立于 C 实现。参考计算先核对 [RFC 9923](https://www.rfc-editor.org/rfc/rfc9923.html) 的公开向量；分片依据 [IPv4](https://www.rfc-editor.org/rfc/rfc791.html) 和 [IPv6](https://www.rfc-editor.org/rfc/rfc8200.html) 规则，逐片重组验证完整 TCP/UDP 校验和。14/18 字节链路头、IPv4 options、IPv6 扩展头和非首片都不要求读取传输端口；每次 C 调用后完整源缓冲和哨兵保持不变。这是当前地址分类策略的回归，不承诺跨版本负载分布固定。

发送批次还核对了已有 `tun32_sys_tests.c` 的精确计数：单队列 32 包一次发布，交替多队列 32 次发布，通知数为队列数，载荷总复制为 32×1500 字节。此行为已是现有测试的明确边界，并非本轮新发现的逐包回退。当前窗口只持一把 TxLock，使用受静态限制的调用栈存储；切换队列不重置工作预算，并及时发布连续前缀及传播同一 NBL 的早期失败。跨队列聚合需要另外设计可重入暂存、空间预留和失败回滚，不能只为减少 Push 数而扩大同步范围。原生混合流 CPU、吞吐与延迟需要实际测量后再决定是否改变该设计。

新 C 回归接入 UnifiedTests 和保护页 Safety；分类回归在主 Tests 的 API shim 生成后按 IPv4/IPv6 独立运行。专项入口是 [多队列排空审计](../tools/audit/verify-tun32-multiqueue-drain.ps1)和 [PrepareClose 审计](../tools/audit/verify-cross-queue-prepare32.ps1)，均默认从当前源码独立生成，只有显式基线参数才要求完整历史身份核验。本轮封存与验收记录在 `out/multiqueue-drain-review/`。

以上为 hosted 验证。保护页覆盖相应宿主对象，部分原始 fixture 存储仍使用 VirtualAlloc/calloc；资源归零和线程汇合只证明该路径。目标旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力与 CPU 性能仍需目标机器验证。
