# 等待返回、故障结算与高分片索引复核

版本保持 **2.0.0**。本轮以 `9ddcb5506fe9d6a05d45e9a5ba6949b60116e5bca2dd4d5d0d9798c7f69afc94` 的 749 个源文件为基线，继续检查并发归还、停机、唤醒和 TCP/UDP 上下行。未确认新的生产缺陷；新增测试用于固定尚未覆盖的交接窗口。原有一个 DLL、一套 v2 SYS、Wintun 旧接口和卸载能力门控保持不变。

## 跨队列故障发生在完成发布与结算之间

[跨队列故障测试](../tests/model/cross_queue_fault32_tests.c)让两个队列各实际接收 16 个普通包。q0 的真实完成发布已提交 16 条记录，但 `PbV2FlushLocked` 尚未更新其私有 Pending/Credits 时，测试在转发原操作的宿主观察点暂停归还线程。q1 随后遇到明确注入的坏共享 Consumer，通过原核心校验触发实际 Fault。恢复 q0 后，已发布前缀必须只结算一次，不能因全局 Fault 又私有退休一次。

反序也单独验证：Fault 先于 q0 发布时，两个队列都应按故障路径退休私有账本，不能再发布完成。Query 必须经过真实锁竞争，并等待归还引用及每个 worker 的私有 quiescence 见证。测试覆盖地址族、TCP/UDP、NDIS 6.20/6.89 能力模型和游标回绕。

每例总共 32 包。坏游标是对不可信共享 ABI 的防御性注入；没有伪造正常环 FULL，没有调整私有计数。宿主门可以重放锁持有期间的交接次序，不表示原生内核 spinlock 内允许阻塞。原有完整 DDI 宿主保持原文，模型中的线程和所有 NBL/MDL、池、引用、credit 在原 Destroy 前汇合或归零；本窄组不声称执行实际 Unregister/Free/Halt。

## SDK 等待返回后的停机覆盖

[等待后置检查测试](../tests/model/wait_postcheck32_tests.c)由单一合法队列 owner 取得并持有 32 个实际接收 loan。队列为空后，实际 SDK 等待以 0 或 1 毫秒超时返回 `WAIT_TIMEOUT`；测试在 SDK 已经返回、DLL 尚未完成后置状态检查时暂停 owner，再发布 BeginStop 或共享终止提示。

恢复后，实际 Wait 必须返回 `PB_STOPPED`，而不是把已过期的 `PB_EMPTY` 交给调用方。停机提示也不能撤销既有 loan：owner 检查全部 32 个 payload，完成实际 Release，再退出并汇合。可信 native Active 仍非零时继续保留资源，可信停止回复满足条件后才准备和最终关闭；捕获的 I/O 回调全部汇合并验证真实 DLL 卸载。

该组重点覆盖旧系统能力模型。Native I/O 和终止通知是已声明的宿主边界，不冒充执行真实 SYS；不并发执行最终 Close 与公开调用，不通过撤销 loan 制造释放后访问。

## 最大公开源跨度的最后 32 段

[高索引分片测试](../tests/packet/test_high_cursor_family32.py)按 IPv4/IPv6 分进程执行 TCP 和 UDP。公开 checked packet API 允许 65536 字节源跨度；MSS 为 1 时，从六万多的全局分片索引直接写出最后 32 段。独立 literal wire 和校验和验证奇数一字节载荷、序号及 IPv4 ID 回绕、TCP Timestamp 原始字节、全段 ECE、仅最后 FIN/PSH，以及高索引处清除首段 CWR。

边界包括越过末段的非法索引、首输出容量为零后合法重试，以及请求 32 段而只剩 31 段时成功截短并保留未使用的第 32 个输出。源、plan、输出元数据、计数及各缓冲区哨兵分别检查。每协议一次 Plan、五次 checked 调用，两族合计 252 个 wire 输出；这不是内部 memcpy 或 Submit/Commit 的计数。

此边界属于公开报文引擎，不表示 SYS 宣告的 32768 字节 GSO payload 上限能够产生这些源包。实际 writer 直接按请求窗口迭代，没有遍历此前六万多段；未为消除必要的 canonical 校验增加缓存或重复维护的解析器。

## 验证与范围

新增 C 组接入分族 UnifiedTests 和守护内存 Safety；独立审计分别运行 ISO/MS 与普通/守护内存配置，默认从当前源码保存完整依赖，不依赖历史 out。显式历史模式须完整认证 749 文件的 typed Ordinal 身份，生产依赖仍须逐字节一致。编译入口、完整冻结宿主、有限观察器、原始日志和元数据合同独立核对。

专项入口为 [跨队列故障审计](../tools/audit/verify-cross-queue-fault32.ps1)和[等待后置审计](../tools/audit/verify-wait-postcheck32.ps1)，完整入口为 [validate-unified.ps1](../tools/build/validate-unified.ps1)。本轮源码、验证记录和产物摘要保存在 `out/stop-wake-budget-review/`。

本轮未发现新的发送时间混用、普通同队列逐包提交或重复载荷复制缺陷。前轮合并预筛选修复保留；直接 helper 的零掩码微基准不能外推真实接收默认路径，详见[合并预筛选复核](coalesce-prefilter-cpu-review.md)。宿主资源账本、保护页、句柄平衡、线程汇合和 DLL 卸载只证明声明的路径。旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及原生 CPU/吞吐/延迟仍需相应系统验证。
