# 双向背压与暂停恢复的分族审查

版本固定为 **2.0.0**。本轮从已验收的 `dc6f9aef0663`（678 个源文件）另存完整字节基线，继续检查归还、复用、停机、等待和协议边界。尚未确认新的生产缺陷；已有旧接口、统一 v2 内核和系统能力门控保持原实现。

新增回归各自只选择 IPv4 或 IPv6，不以混合运行代替分族验证：

| 回归 | 要验证的交叉条件 | 证据边界 |
| --- | --- | --- |
| [worker 暂停与恢复](../tests/model/tun32_worker_pause_tests.c) | 首批 32 包归还后，旧回调尾部仍持有 rundown；第二代 32 包在暂停期间排队，只由实际 Restart 的 RunningEvent 恢复 | 实际 worker、Pause/Restart 和队列路径；hosted DDI，不是内核调度实测 |
| [旧接口双向背压](../tests/model/duplex_backpressure32_tests.c) | 私有接收环只容纳前缀时，另一方向的真实槽位也已占满；通知、恢复及停止不能重放已复制前缀或遗失未归还贷款 | 实际旧 worker 和 DLL 队列；内核 peer 由 fixture 模拟 |
| [32 包合并边界](../tests/packet/test_coalesce_edge_family32.py) | 奇数 63 字节 payload、TCP 序号/IP ID 回绕、最小 TTL、PSH 合并、UDP 末尾 1 字节，以及容量 31 和 hop 边界 | 独立 Python 全字节及校验和 oracle；只计直接函数调用，不冒充提交、内部复制或 CPU 计数 |

协议回归每族包含 TCP 两个、UDP 四个场景，每个场景有 32 个完整且校验和正确的输入。TCP 容量 31 场景中，只有被拒的第 32 包携带 PSH 和更低 TTL；拒绝后整个状态和输出必须保持原字节，随后仅处理被拒尾包。UDP 输入中合法的 FFFF 校验和与 URO 聚合输出的零校验和分别验证；后者是带成功 OOB 的 NDIS 表示，不能作为普通 IPv6 线上报文解释。

RSC 字段规则见 [Microsoft 的 TCP/IP 合并规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/rules-for-coalescing-tcp-ip-packets)及 [IP 头更新规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/updating-the-ip-headers-for-coalesced-segments)；URO 表示见 [Microsoft UDP RSC 说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload)。

日志 UTC、单调等待时钟、Epoch/Generation 与包内不透明数据各有独立用途，当前数据路径没有新增发送时间字段。测试使用序号和不透明标记追踪顺序，不把这些标记换算成日志或等待时间。普通包的所有权复制、合并时的私有快照以及元数据复制也分别计量，不因存在多个 memcpy 就认定重复复制。

新 C 回归接入 UnifiedTests 和保护页 Safety，两族分别构建；合并边界接入主 Tests。专项入口为 [worker 审计](../tools/audit/verify-tun32-worker-pause.ps1)和 [双向背压审计](../tools/audit/verify-duplex-backpressure32.ps1)，完整入口仍是 [validate-unified.ps1](../tools/build/validate-unified.ps1)。生成的本轮封存及验收记录位于 `out/return-wake-next/`。

worker 矩阵包含 6.20 普通 TCP/UDP、6.30 TCP RSC、6.89 UDP URO，结合事件类型与队列计数回绕；每个场景先后处理两代各 32 包，汇合六个实际 SDK 线程。双向背压使用真实占满的 64 个 native 槽与 128 KiB 私有环：接收目标先复制 6 包、持有 26 包；恢复时分别测试两种通知顺序，停止时只提交获得的 7 个槽，未复制的 26 个贷款直接归还。每序号的复制次数、非空 Reserve/Commit 批次数与最终资源状态均有断言。

专项审计默认只依赖当前源码，可以在清空 out 后重建。只有显式传入 `-BaselineDirectory` 才比较历史基线；该分支必须完整认证 678 个文件及其原始指纹，缺失或不匹配就失败。本轮最终验收显式使用已保存的基线，同时重新编译当前源码；先前的最小探针和综合验收记录分别保留。

审计还独立核对完整依赖路径集合与提取字节、原始哈希和 Restart 标记；重复依赖、篡改的片段哈希、错误来源及错误标记均有拒绝测试。此项加固只涉及证据校验，没有修改数据路径。

资源计数与线程汇合只证明相应 hosted 路径。目标旧系统的签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力以及 CPU、吞吐和延迟仍需目标机器验证。
