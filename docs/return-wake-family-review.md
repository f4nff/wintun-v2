# 分族 32 包归还、复用与校验和控制复核（2.0.0）

## 确认并修复的发送兼容性问题

`driver/v2/session_describe.h` 原来先检查普通发送校验和任务位，再验证地址族。
NDIS 则定义：IsIPv4、IsIPv6 均未设置时，不执行校验和任务；任务位应在地址族
选择之后解释。该控制下的正常 IP 包曾因其余任务位非零被拒绝。现在仅在有
地址族选择时解释普通任务位，主动卸载的错误地址族、TCP 偏移和策略检查保留。
LSO/USO 分支、旧接口、统一 v2 内核及系统能力门控保持原有行为，版本为 2.0.0。

依据：[NDIS 校验和处理步骤](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/offloading-checksum-tasks)、
[OOB 结构定义](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/nblchecksum/ns-nblchecksum-ndis_tcp_ip_checksum_net_buffer_list_info)。
这里验证定义明确的“不请求校验和”控制，不声称 Windows TCP/IP 自然会发送残留
接收标志或随机 OOB 数据；测试不设置保留位。

`tun32_checksum_metadata_tests.c` 每个地址族独立编译。每个进程覆盖 TCP/UDP、
NDIS 6.20/6.89、策略开/关及五种控制，共 40 个预热和 40 个测量场景，每个场景
包含 32 个原始 NBL 请求。合法无任务包必须保留整个输入字节，并一次提交 32 个
描述符。主动卸载的策略拒绝和双地址族错误继续拒绝。读取分类前缀 1888 字节与
完整 L3 48000 字节分别计数，不将分类前缀算成第二次完整载荷复制。

原始完整 Describe 文件作为非发布负对照：每进程 32 个场景违反上述控制契约，
全部回收后退出 141；修正后违规数为零。两种实现均须完成 7680/7680 个源映射
归还、64 槽 FREE、真实回收、严格句柄相等和无引用残留。调用者 NBL/NB 在完成
回调中实际守护释放；只读源 MDL 另外保留到验证后再释放。普通元数据存在计数
继续按原有规则统计，不冒充实际校验和计算次数。

## SYS 并发归还与跨代复用

`tun32_return_reuse_tests.c` 使用实际用户队列和内核归还路径：32 个原始 NBL
拆成四条乱序 8 项链。前三条在释放真实 RxLock 后、释放活动引用前暂停，用户
实际 Poll24 并 Reserve24，得到第二代槽位。此时另一次 Reserve1 自然返回 FULL，
因为 8 个旧 Published 加 24 个新 Loan 已占满全部 32 槽；没有虚构满环。

再次 Commit/Receive24 时，旧 8 个第一代 NBL 仍由协议端持有。旧回调尾退出
不能破坏新一代槽位。最后混合的 32 项归还后，全部 56 个实际 token 都通过实际
Poll 退休。held8 的第二次元数据检查单独计数，因此每例是 56 个独立 NBL、
64 次元数据检查，不能说成 64 个独立 NBL。

覆盖 Drain、Fault 的 Active/Quiesce 两序、自动/手动事件、uint64 游标跨零、
NDIS 6.20/6.89。每个地址族单独执行 TCP/UDP。每进程 96 场景，包含一半经过
检查的预热；四个归还线程及一个 Free 线程全部真实汇合。查询 STOPPED 后，
物理 Free 仍须等待最后 Returns 尾部。该模型验证实际 DLL 队列和 SYS 路径，
不冒充完整 DLL 生命周期或真实 NDIS 调度。

## DLL 自然部分预留与停机取消

`rejected_reuse32_tests.c` 的两个实际队列各有 32 槽和 32 环容量。q0 一次受控
NBL 构造失败产生包状态 PB_FULL=2，另 31 个 NBL 仍持有；q1 的一个已拥有 NBL
注入非成功返回状态，验证包状态 PB_RX_REJECTED=4。这一返回状态是防御边界，
不声称真实 Windows 协议栈自然如此报告。两个包状态都不能误作整个 Poll API
失败；尤其包状态 4 与 API 的 PB_CORRUPT 数值相同，语义必须分离。

两个真实 owner Wait/Poll 后分别自然得到 1/32 个可复用槽，再 Reserve32 得到
1/32 个第二代 Loan。BeginStop 后新 Commit 必须 STOPPED/accepted0，原 owner
正常 Cancel 并汇合。旧 31 个 NBL 的 NativeActive 继续阻止物理关闭，直到实际
归还。两族各 48 场景，覆盖 TCP/UDP、故障位置 0/6/30、事件模式及系统版本。

额外负控只改变 actual Reserve 的部分成功返回表达式，真实 count1 不被改写，
由相同 owner 正常 Cancel 后完成全部停机与线程汇合，再退出 142。它检验回归
测试的辨别力，不代表当前生产代码已有这个错误。公开调用者及回调 OS 线程的
实际捕获句柄逐一汇合；不假定每轮 SDK 必须创建固定数量的回调线程。

## 协议与构建证据边界

`test_segment_edge_family32.py` 每族 2 项测试、12 个场景：MSS63、末尾 1 字节、
TCP CWR/ECE/ACK/PSH/FIN 的分段规则、序号/IP ID 回绕及不透明 TCP Timestamp。
UDP 输入 checksum=0 的 NDIS 控制与计算结果为零须写 FFFF 分开验证；不足 MSS
末段只用于已宣告该能力的 6.84+ 控制。完整 32 段与容量31后只重试末段均用独立
逐字节和校验和 oracle；Python 的 checked 调用次数不作为所有 memcpy 或 CPU
计数。日志 UTC、单调等待时钟、会话 Epoch/Generation 和数据包时间戳各守原用途。

普通 OID 由 NDIS 串行化，Direct/Synchronous 回调在本项目返回 NOT_SUPPORTED；
因此不把任意并发调用两个普通 OID 处理函数称作自然竞态。
[NDIS 串行化规则](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/miniport-adapter-oid-request-serialization)

构建时另确认旧 PowerShell object[] 排序未得到预期 Ordinal 顺序，指纹函数现在
显式使用 string[]，并防止 Git 引用 Unicode 路径后漏读文件。八项隔离 Git 测试
使用手工声明的独立顺序，覆盖索引分区、Unicode/空格、忽略产物、字节变化/恢复
及缺失 tracked 文件。旧 663 文件的原始顺序身份仍为 4fd68e…，其相同字节的真正
Ordinal 内容身份为 a44c68…；基线保留两者和明确顺序，不能重标旧指纹。

用户清理 out 后，原始执行证据已不可读取。本轮从未变的 663 个源码文件精确
恢复基线，并在隔离 Git 目录重新构建旧源码产物供比较；新执行记录明确标为
重新生成，不冒用已删除的历史结果。新的生产例外账本只路由两处已审阅完整文件
变换（旧 LSO 修正和本轮 Describe 修正），原 Send/Allocate 等合同保持独立。

清理后还发现默认 current helper-drain 构建无条件读取旧 out 快照，导致当前
测试不能从源码独立生成。构建器现按显式请求选择来源：默认 current 使用当前
源码、固定完整文件哈希及有限投影；before 或显式 BaselineDirectory 继续要求
完整旧快照和原始 7f57… 哈希，缺失就失败，不自动退化为 current。独立构建合同
运行原有场景，验证当前来源及两种历史来源缺失的拒绝行为，不虚构旧快照。

所有场景均为 hosted 验证。槽位、映射、对象、引用、句柄及测试线程的归还只证明
对应边界；旧系统实机、真实内核/NDIS/IRQL、Driver Verifier、持续压力和 CPU/
吞吐/延迟仍需目标机器验证。操作次数和单次复制断言不能替代这些实测。
