# 回收拒绝、归还提示与 32 包合并上限复核

版本保持 **2.0.0**。本轮以 `4c7c7832d59c` 的 727 个源文件为基线，保存原始字节并认证完整身份。未确认新的生产缺陷；上一轮接收终结代际修复保留，SYS、DLL、共享核心、公共头文件及 Rust 运行时均保持原样。

[实际发送与回收测试](../tests/model/recycle_send32_tests.c)先上调另一方向的 32 个 RX 包并保持引用，再实际 Send32 填满 TX 池。第二次 Send32 在真实槽位压力下建立 FULL 缓存，没有预填假的状态或伪造 FULL。随后在 32 个回收 token 的第 17 项设置错误代际，第三次实际 Send 必须在回收拒绝后触发 Fault：仅前 16 个槽位归还为 FREE，后 16 个仍 Published，FULL 缓存解除；本次不再预留、Describe 或复制载荷。重复 Fault 不增加该阶段的终止广播。

三次 Send 的普通载荷读取精确为 32 次、48000 字节；分类前缀读取共 65 次、3835 字节，第三次 Send 在发现回收错误前仍需分类首包。这两类读取分开计量，不能把前缀检查算作重复载荷复制。回收错误是声明的共享 ABI 防御输入，正常 DLL Release 不应产生坏 token。整个回收批已从共享环取入私有快照，错误后缀不能授权后续工作。Reject 仅由原宿主边界观察实际 Send 的诊断参数，不执行生产诊断 history 或锁；SDK completion 也由宿主承接，相关独立回归继续保留。

故障后，原有 RX32 保持在途所有权。实际私有 quiescence 见证及两个依次启动、汇合的 Return16 线程分别检查引用从 32 到 16 再到 0，然后释放会话；原有并发归还回归另行继续运行。此时 Fault 私有退休待完成项，RxReturned=32 与共享 Completions=0 是不同统计；不声称向 DLL 投递了 32 项成功完成。新组执行实际 Query、Return、Free 及宿主拥有的空闲线程收尾，不声称运行实际内核 worker 循环。NDIS 6.20/6.89、TCP/UDP、计数回绕和两种编译内存模型分别验证。

[双 owner 归还提示测试](../tests/model/retirement_hint32_tests.c)使用真实 DLL：q0 借出 32 个接收 loan，q1 预留 32 个发送 loan。两个合法 owner 先通过真实 Wait 等待，BeginStop 唤醒后分别执行实际 Release32、Cancel32。观察门只放在实际 Progress SetEvent 已完成、DataGate 已释放而 Lifetime 仍被该调用持有的位置；既有完整资源宿主不替换生产函数体。

测试先让 SDK 真正消费 BeginStop 的初始提示，再将两个成功归还提示合并为一个事件状态，并检查实际 SDK 只新增一次成功唤醒。两个有限调用的尾部同时存活时，DataGate 排他可取，Lifetime 排他不可取。独立 NativeActive=1 先阻止 Prepared；恢复为 0 后允许 Prepared，但映射仍在。两个 owner 放行并汇合后才最终 Close，随后汇合全部实际回调线程并验证真实 DLL 卸载及原始资源账本。NativeActive 和设备 I/O 是宿主边界模型，不是执行了真实 SYS；事件状态查询仅作测试内非消费观察。

[32 包合并上限测试](../tests/packet/test_coalesce_limit_family32.py)在 IPv4/IPv6 独立进程中分别测试 TCP/UDP。调用方提供 65536 字节容量，实际合并器保守钳制为总长 65535。前 31 包每包载荷 2048 字节，第 32 个合法候选多一个字节时应 NO_SPACE，完整状态、输出、源及哨兵不变；只换成少一个字节的合法末包后继续，最终恰为 65535 字节。四个场景共验证 128 个合法输入、4 个拒绝候选和 4 个最终合并单元，由独立完整字节及校验和 oracle 验证，未重放前缀。

TCP 输出重算校验和，UDP 合并输出则按已有 checksum-success/OOB 表示保留零校验和字段，二者分别验证。这里的总长上限属于当前实现，不重新定义 IPv6 协议允许的 payload 上限，也不把 65536 字节 GSO 模板当作普通输入包。TCP PSH、最小 TTL/Hop Limit、序号回绕、首包身份和 16 位长度字段一起核验。

新 C 回归接入分族 UnifiedTests 和保护页 Safety，Python 回归接入主 Tests。专项入口为 [回收发送审计](../tools/audit/verify-recycle-send32.ps1)和[归还提示审计](../tools/audit/verify-retirement-hint32.ps1)，完整入口为 [validate-unified.ps1](../tools/build/validate-unified.ps1)。专项默认从当前源码独立保存闭包，显式历史模式另认证完整 727 文件身份；所有生产依赖必须逐字节相等。完整宿主、有限来源投影、编译参数、生成物和日志哈希，以及编译、日志解析、元数据拒绝合同分别记录。基线、最小用例和最终验收记录见 `out/recycle-wait-review/`。

未发现新的发送时间混用、逐包提交或重复载荷复制缺陷。等待时钟、日志时间、TCP Timestamp 与载荷中的不透明标记继续分开使用。还按 [Microsoft 等待与 APC 合同](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/waits-and-apcs)复核了内核非 alertable 等待，不以注入不适用的 UserMode/APC 返回值判定生产缺陷。

资源归零、保护页、句柄平衡、线程汇合和 DLL 卸载只证明声明的宿主路径。最低工程目标仍为 Windows 7 SP1 x64，新系统能力门控保持；旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及原生 CPU、吞吐和延迟仍需目标机器验证。
