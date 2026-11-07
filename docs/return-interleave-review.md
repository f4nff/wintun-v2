# 跨队列引用交接、跨代唤醒与 TCP 零校验和复核

版本保持 **2.0.0**。本轮保存并认证 `64be5e2b5802` 的 735 个源文件，继续检查并发归还、停机及通知。未确认新的生产缺陷；SYS、DLL、公共头文件、共享核心和 Rust 运行时保持原始字节，既有卸载能力门控不变。

[跨队列引用交接测试](../tests/model/reference_handoff32_tests.c)先让 q0 实际接收 32 包并留在 NDIS 所有权中，q1 另行构造 32 个接收对象，在实际引用留存前保留 Transition 共享锁。q0 的实际 Return32 把 Active 减为 0，测试在仍持 ReferenceLock、尚未执行 AllReturned Set 的宿主边界暂停它；q1 在实际引用函数的获取锁入口确实 TryAcquire 失败后，转发原获取锁函数。另一线程同时调用实际 Pause，也必须在 Transition 排他获取入口观察到冲突。

释放旧归还后，旧 Set 与新引用的 0→32 Clear 必须在同一 ReferenceLock 下串行。q1 的留存完成前，Pause 不能通过 Transition；其后 AllReturned 已复位，即使旧批已归零，Pause 仍需等 q1 的真实 Return32。两个队列的 64 个完成 token 分别通过独立 peer 池验证并归还，汇合四个实际调用线程后执行实际 Unregister/Free；Free 停止并汇合两个宿主空闲线程。NDIS 6.20/6.89、TCP/UDP、计数回绕、ISO/MS 内存模型及保护页分别验证。

这些门控制的是用户态宿主的等价调度次序，不表示原生内核 spinlock 内允许阻塞，也不是 DPC/IRQL 或内核调度器验证。完整既有 WP/kernel 宿主保留原文，有限引用、Receive、TunPause 词法观察只转发实际函数，原 NT_ASSERT 仍由宿主 CHECK 执行；未添加私有引用修补或生产分支。

[跨代重用与终止提示测试](../tests/model/reuse_terminal32_tests.c)使用真实 DLL 和单一合法队列 owner。第一代 32 个接收 loan 尚由调用方持有时，实际 Wait 经公有 Wake 唤醒，保留真实 ring Armed 状态。调用方把 Release 的输入 token 数组放在首个 loan 的 payload 内，输出 accepted 留在独立存储；该输入别名是公开合同允许的形式。合法调用方修改后重算包头校验和，不把修改后的输入假定为原字节不变。

实际 Release 发布后，peer 通过独立 core 池消费回收、归还并自然预留同一组槽位的第二代，才覆写已转让的数据并重新发布 32 个有效 TCP/UDP 包。旧 Release 必须凭私有 token 快照完成；owner 在重新 Receive 取得第二代之前不再访问旧指针。ready 已存在的 Wait 不消费 UserEvent。late 分支先由实际 SDK Wait 消费残留提示，验证返回 OK 而无新数据，再在下一次实际 SDK 等待入口后发布终止提示；early 分支在第二代接收成功后、消费残留提示之前发布终止，下一次 Wait 应直接 STOPPED。两个分支都验证停止不撤销仍持有的第二代 payload 和最后 Release 权。

共享终止提示不是物理回收授权。最后 32 个 loan 归还、owner 汇合后，独立 NativeActive=1 仍保留资源，可信 STOPPED/Active=0 回复才允许准备关闭；最终 Close、所有捕获的回调线程汇合及真实 DLL 卸载按原资源账本验证。Native I/O、peer 和终止提示是声明的宿主边界，不是执行真实 SYS。新组固定 NDIS 6.20 能力模型，其他现代能力及多队列由既有回归继续覆盖；没有伪造 FULL、跨代 ABA、最终 Close 与公开调用并发或私有计数修补。

[TCP 零校验和 32 段测试](../tests/packet/test_tcp_zero_family32.py)按 IPv4/IPv6 分进程执行。Python 在构造阶段独立调整每段不透明标记之外的 payload word，使所有 32 段的 TCP 计算结果恰为 0000；源是带非零 pseudo-header seed 的合法 LSOv2 模板，不当作普通 wire 包解析。真实 checked writer 每族只调用一次批量接口生成 32 段，逐字节验证校验和字段、完整 wire 和校验和残余，以及奇数 63 字节载荷、序号回绕、IPv4 ID 回绕、首段 CWR、全段 ECE、末段 FIN/PSH 和原始 TCP Timestamp。

TCP 计算结果 0000 不代表省略计算。UDP 的 computed-zero→FFFF 和零校验和禁算规则由既有分族 UDP 边界回归另测，不把两种协议的字段语义混用。源、plan、输出 metadata、计数和前后哨兵分别检查；两族共 64 个输出，Plan 两次、checked batch 两次。这里没有修改 native/Rust borrowed loan，也不测量内部 memcpy、Submit/Commit 或 CPU。

新 C 组接入分族 UnifiedTests 和保护页 Safety，Python 组接入主 Tests。专项入口为 [引用交接审计](../tools/audit/verify-reference-handoff32.ps1)和[跨代终止审计](../tools/audit/verify-reuse-terminal32.ps1)，完整入口为 [validate-unified.ps1](../tools/build/validate-unified.ps1)。默认专项独立保存当前来源，显式历史模式认证完整 735 文件 typed Ordinal 身份；所有生产依赖须逐字节相等。原始宿主、有限投影、编译参数、执行日志和元数据拒绝合同分别认证，记录位于 `out/return-interleave-review/`。

未发现新的发送时间混用、逐包提交或重复载荷复制缺陷。宿主资源归零、保护页、句柄平衡、线程汇合和 DLL 卸载仅证明声明的路径；旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及原生 CPU、吞吐和延迟仍需目标机器验证。
