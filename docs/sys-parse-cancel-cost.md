# SYS 校验解析复用与故障接收撤销

后续共享游标读取与 arm/recheck 验证见 [SYS 共享游标读取与等待协议](sys-cursor-acquire-cost.md)。

本轮从源码指纹 `ad2b583e2e3204b9df8721207e5a4a2f04ec130b5c8a924d505e984625807e5c` 继续检查 SYS 热路径和等待。原源码保存在 `out/sys-parse-cancel-baseline/ad2b583e2e32`。没有安装、签名或加载驱动。

## TCP 校验和后的重复解析

普通发送的 TCP checksum offload 原先先调用 `PbFinalizeChecksums`，由它完整解析 IP/TCP 头并计算校验和，再调用一次 `PbPlanSegments`，仅为检查 NDIS 元数据中的 TCP 头偏移。同一份内核私有报文因此解析两次；IPv4 选项和 IPv6 扩展头也重复经过解析器。

共享核心增加 `PbFinalizeChecksumsWithOffset`，可在成功时返回本次解析验证得到的传输层偏移。输入仍是实际报文及其显式长度，不接受调用方提供的预验证计划。失败不写偏移输出；仅计算 IPv4 头校验和的路径不解析 L4，成功输出零。原 `PbFinalizeChecksums` 保留原签名，委托相同实现，既有导出表和 ABI 槽位不变。

SYS 的 TCP checksum 路径直接比较这次返回的偏移，不再重新解析。UDP、IPv4 头校验、GSO 仍走原有决策；offload 策略仍在校验完成后读取。错误头偏移和禁用策略的拒绝顺序、校验和写入行为维持原样。`Describe` 拆到单独内部头文件，宿主测试直接包含生产函数。

## 已知故障后的未指示接收

一个批次中出现重复的活动 token 会使会话故障，但同批其他合法描述符可能已经由私有接纳事务取得槽位。原路径仍为这些槽位分配接收记录、两个 MDL 和 NBL，再登记原生引用，最后因 Draining 而逐个本地归还。上层 NDIS 从未见到这些对象。

现在在接纳事务结束并传播故障后检查 Draining/Stopping。若已停止接纳，持 RxLock 仅撤销本批 `valid[]` 标记的私有槽位，按 PB_STOPPED 完成并按原预算刷新，再释放 TransitionLock 返回。拒绝的描述符不属于这组私有所有权，特别是重复 token 对应的已有 NDIS 槽位，绝不在这里清除。计数器 RxObjects/RxReturned 和 ifInErrors 不再统计这些未构造的 NBL；会话 fault 和已接纳描述符的完成记账仍然保留。

正常 DRAIN 的描述符本来就由接纳函数按 PB_STOPPED 拒绝，仍经过同一完成刷新。故障刷新继续使用私有 ledger 的退休规则。检查之后才到来的异步故障仍由已有的构造失败/指示前撤销路径处理。本轮未改动原生引用、AllReturned、rundown、线程 join 或 worker 故障退出见证，亦未给外部归还等待加不安全的超时释放。

## 可复现验证

确定性的宿主操作计数如下，不是内核 CPU 百分比：

| 场景 | 修正前 | 修正后 |
| --- | ---: | ---: |
| 每个 TCP checksum 请求在校验和之后的额外头解析 | 1 | 0 |
| 故障批次两个尚未指示接收项的资源分配尝试 | 8 | 0 |
| 上述两个接收项的 session rundown 获取/释放对 | 2 | 0 |

`describe_checksum_tests.c` 使用保护页报文和输出缓冲，执行实际 Describe 与共享报文核心，覆盖 IPv4/IPv6 TCP、IPv4 选项、IPv6 扩展头、VLAN/填充、64/1500/65000 字节、禁用策略、错误偏移/家族、TCP+UDP 冲突、UDP 和 IPv4 头校验。单独校验偏移输出契约、失败输出保持及长度边界，原有报文正确性测试继续验证校验和。guard fuzz 对十万次边界/变异输入验证新偏移输出。

`drain_boundary_tests.c` 直接执行 ReceiveBatch、worker、query 和原生归还，分别测试同批重复 token，以及已有 NDIS 引用时的重复 token。断言私有槽位/credit 清零、资源收支和句柄平衡；即使没有原生引用，worker 尚无退出见证时也不能报告 STOPPED。已有 NDIS 槽位必须继续保持 Active，真实归还后才能报告 STOPPED。原有持续追加、回绕、满完成环停车与消费者唤醒、预算续跑、多队列和归还时序回归继续执行。

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\build\build.ps1 -Stage Safety
.\tools\audit\verify-sys-parse-cancel.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 入口从冻结旧源码抽取实际 Describe/ReceiveBatch 函数体，与当前函数使用相同宿主 DDI 和当前共享核心，记录实际退出码、操作计数及源码/二进制哈希。该比较隔离驱动层重复调用与资源构造变化，不声称测量共享核心包装函数的时间。完整离线验证增加 SysParseCancel；需要本地历史快照的三项 A/B 与 benchmark 均可通过现有 SkipBenchmark 跳过。

x64 Release 的 PDB/反汇编另行检查源码拆分及包装函数的调用开销：Describe 仍为单独生产函数，未产生额外拆分层；原校验和入口的包装层经 LTCG 消除，没有独立包装函数或额外一次函数调用。该检查只读取构建文件。

本轮没有确认新的内部死锁。故障和正常排空的已有等待仍由私有边界、worker 见证及真实原生引用共同约束。实际 IRQL、NDIS 并发重入、Verifier、PoolMon、ETW/WPA、低资源及长稳仍需内核环境验收；结果继续保留 `kernel_runtime_tested=false` 与 `production_ready=false`。
