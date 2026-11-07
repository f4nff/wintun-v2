# 双合并缓存归还尾部与双向字节预算复核

版本固定为 **2.0.0**。本轮保存并逐文件验证了上一轮 `9631f540027d` 的 703 个源文件，继续检查归还、停机、唤醒和包路径成本。目前未确认新的生产缺陷，运行时源码保持原实现。

完整验收发现并修复了既有 [TerminationWait 回归](../tests/model/compat_session_tests.c)的同步缺口：Start 返回的 Ready 早于 worker 进入 pump 循环；测试随即启动 End 时，worker 可以先看到 Stopping，跳过测试要等待的 RecordedReceive 门。结束场景现在先确认 worker 已进入受控门，再启动 End；启动回滚场景仍先创建启动调用线程。原来的超时、两次等待失败注入、退避及真实退出 witness 检查全部保留。首次失败的源码和日志保存在 `out/coalesce-tail-review/failed-validation-22dfa/`。

[双缓存归还测试](../tests/model/coalesce_dual_tail32_tests.c)让实际 SYS Receive 分两次处理各 32 个原始包：第一批形成两个 16 包合并对象，第二批使用另外 32 个合法槽位。第一批实际 Return 在释放两条 NBL、对应 MDL 及两个缓存 lease 后，等待真实宿主诊断锁；唯一接收线程此时再次借出两个缓存。分别检查旧归还先结束与新归还先结束，旧 record 的诊断仍来自原描述符快照，旧尾部不会清除新缓存的 Busy 或改写新载荷。

第一批的 NBL 失败状态属于明确的宿主故障注入。测试使用实际 Receive、外层 Return、Pause、Unregister 和 Free；接收调度、NDIS 与 IRQL 仍为 fixture。Pause 等待全部 Active，随后 Free 必须等待最后的 Returns pin，所有测试线程先汇合再核对资源释放。新合并场景遵循能力边界：TCP 使用 NDIS 6.30/6.89，UDP 使用 6.89；没有在 6.20 上启用合并。旧系统普通包与能力拒绝继续由完整回归覆盖。

[双向预算测试](../tests/model/duplex_budget32_tests.c)通过真实旧接口与 v2 原生队列，同时传递每方向 32 个 65535 字节 IP 包。每方向的 512 KiB 单轮预算应产生四个 8 包批次：发送 Reserve/Commit 为 8/8/8/8，接收只获取一次 32 个原生 loan，再按 8/8/8/8 复制和归还。每个方向、每个序号的载荷复制独立计数。

停止场景在第一或第三个双向轮次结束后触发。应用先实际读取并归还已复制的 8 或 24 个旧接口指针，再调用原生 BeginStop。尚未复制的接收 loan 经原 ReturnPending 退还，发送尾部停止提交；最后由原 worker 完成关闭。fixture 保留真实 DLL 装载、卸载及原资源账本，线程与 SDK 回调均须汇合。

[变长 TCP 合并测试](../tests/packet/test_coalesce_variable_family32.py)为 IPv4/IPv6 分别提供 32 个长度各异、奇偶交替且序号连续的数据段。多个中段 PSH、仅中段出现的最小 TTL/Hop Limit、序号回绕、非连续 IPv4 ID 和不透明应用标记由独立完整 wire/checksum oracle 验证；输出保留第一个 IP ID，PSH 取并集。源、状态和输出尾部哨兵逐步核对。依据为 [Microsoft RSC 规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/rules-for-coalescing-tcp-ip-packets)与[合并后的 IP 头更新规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/updating-the-ip-headers-for-coalesced-segments)。这组只测 TCP；UDP 的等长及可选短末段合同保留在既有分族回归中。

本轮继续区分普通包的所有权复制、可选合并的私有快照、边界预读及元数据复制。既有容量边界预读计数保持原约定。包内标记、TCP Timestamp、日志 UTC、单调等待时钟和 Epoch/Generation 各有各的用途；上述标记没有作为发送时间参与计算。函数调用或复制字节计数不作为 CPU、吞吐或延迟测量。

新增两组 C 回归接入 UnifiedTests 与保护页 Safety，Python 回归按族接入主 Tests。专项入口为[双缓存审计](../tools/audit/verify-coalesce-dual-tail32.ps1)和[双向预算审计](../tools/audit/verify-duplex-budget32.ps1)，完整入口为 [validate-unified.ps1](../tools/build/validate-unified.ps1)。当前源码可独立生成专项产物；显式历史基线另做完整认证。本轮封存与验收记录位于 `out/coalesce-tail-review/`。

以上验证限于声明的宿主路径和保护对象。旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及 CPU 性能仍需目标机器验证。
