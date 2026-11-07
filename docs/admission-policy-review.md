# 接收终结代际与策略替换复核

版本保持 **2.0.0**。本轮保存并认证 `0f920bb470e3` 的 719 个源文件，以原始字节基线检查槽位归还、停止回收、等待者和 TCP/UDP 批量重试。生产修复限于共享核心的 RX 接纳及其内部合同注释，不改变导出、ABI、安装身份或卸载能力门槛。

确认的缺陷是失败完成后的同代际再次接纳：一个作用域正确、未在途的新 token 若因描述符畸形被拒绝，原实现返回失败完成权，却没有保存该 Generation。同一 token 的合法描述符随后进入同一接收窗口时仍可被接纳。旧实现的实际 32 项接收复现中，同一 token 先有 INVALID completion，NDIS 仍持有其后接纳的包，再有成功 completion；完整归还后资源账本为零。该场景是共享描述符边界的防御测试，正常 DLL Commit 和 Rust loan 不应重复发布 token。

修复让作用域正确、非零且更新的 Generation 在描述符校验或停止拒绝之前记录终结。一旦交出失败或停止完成权，该代际不能再次接纳，也不能再获得完成权。旧代际、重复代际及仍在途槽位的未来代际仍先拒绝；不会更新在途槽位。错误 epoch、队列、槽号、零代际及零会话 epoch 不得推进本地记录。下一合法代际可继续复用；UINT32_MAX 自然终结，不回绕到零。没有增加锁、分配、逐包提交或载荷复制。

[核心回归](../tests/model/core_tests.c)先在旧生产源码上执行 32 个独立拒绝：28 个畸形描述符和 4 个停止请求，已获得完成权却没有终结的记录为 32 个；修复后全部终结。随后检查重复拒绝、下一代复用、在途未来代际、最大代际和跨作用域保护。原先允许 STOPPED 后同代际再次接纳的核心测试相应改为 STALE，再使用下一代际接纳。清除核心模拟的 Active 仅代表该层的归还动作，实际 SYS 回收由下述组合覆盖。

首次完整验收还检出旧多队列 drain 测试要求停止拒绝后 Generation 保持 0 的过时断言。保存失败来源与日志后，仅将其更新为与完成 token 一致的 Generation=1；原有两个 worker 汇合、STOPPED16、完整 completion32、引用和资源检查保持。该用例继续分别覆盖 NDIS 6.20/6.89、TCP/UDP、回绕和最后归还的两种次序。

[SYS 32 项接纳回归](../tests/model/receive_admission32_tests.c)保留完整既有宿主，以有限的 include 投影和透明完成函数观察调用实际 Receive、Fault 和 Return。旧批 32 个包形成一个包含 16 个原始包的合并 NBL 和 16 个普通 NBL；后续批在旧对象仍在途时分别引用旧 token、活动槽位的未来代际，或先提交畸形 token 再重复同一 token。错误批触发实际 Fault，其余合法项在本地取消，旧对象保持唯一归还权。正序和逆序归还后检查 Active、Returns、槽位、credit、MDL/NBL 及宿主句柄。新组最后执行宿主 Destroy，不额外声称执行实际 worker、Query 或 FreeSession。

Fault 路径会私有退休待完成记录，并不承诺把失败 completion 投递到共享环。因此新组分别检查实际完成函数授予的唯一完成权与共享环的零投递，避免将两者混同。该组使用确定性调用次序；现有实际多线程归还、唤醒重新武装、Pause/Unregister 双屏障回归继续运行。TCP 合并使用 NDIS 6.30 能力模型，UDP 合并使用 6.89，未向 6.20 开放合并。

[DLL 32+32 策略回归](../tests/model/policy_abandon32_tests.c)由同一合法 owner 实际提交 Published32 并借出另一方向的 32 个 loan。真实同步 SetPolicy 在声明的条件变量等待失败边界自动 Forget；第一个策略 I/O 失败后，新的 ticket 占用控制槽。两个已等待的旧 ticket Poll 调用恢复时必须返回 STALE，旧 Forget 也不得清掉新 ticket。第二个真实 I/O 在 BeginStop 后分别成功或失败，并覆盖消费结果和放弃结果两种收尾。所有 public 调用者及 owner 先汇合，再最终关闭；实际 callback 数量、资源账本、句柄和真实 DLL 卸载分别验证。未确认 DLL 策略实现的新缺陷，不据此改动生产逻辑。

[尾部重验证回归](../tests/packet/test_retry_revalidate_family32.py)在 IPv4/IPv6 独立进程中分别测试 TCP/UDP。初次 checked batch 写出前 31 段，末项容量不足；恢复末项容量后，仅在两次已完成调用之间改变调用方自有源包的 TCP DataOffset 或 IP 协议字段。旧 plan 失配必须拒绝尾项，前缀、源、plan、元数据和输出哨兵保持。恢复源字节后只重试末一段，由独立完整 wire/checksum oracle 验证总共 32 段。plan 是派生几何记录，不是内容哈希；该测试不修改已提交 native/Rust loan，也不要求把 GSO 的 UDP Length 当作陈旧 plan 的判据。

新 C 组接入分族 UnifiedTests 和保护页 Safety，Python 组接入 Tests。专项入口为 [接纳审计](../tools/audit/verify-receive-admission32.ps1)和[策略审计](../tools/audit/verify-policy-abandon32.ps1)，完整入口为 [validate-unified.ps1](../tools/build/validate-unified.ps1)。专项默认独立保存当前来源；显式历史模式认证完整 719 文件身份，仅允许上述两个生产文件的精确旧/新哈希差异。完整宿主、生产来源、编译参数、生成投影、执行日志和拒绝合同均记录来源及哈希。原始红测、绿测、完整验收及最终复核保存在 `out/admission-policy-review/`。

包内 TCP Timestamp、不透明载荷标记、日志时间、单调等待时钟与 Epoch/Generation 仍各自使用，未发现新的发送时间混用、逐包提交或重复载荷复制缺陷。这里的操作计数与宿主资源账本不代表原生 CPU 性能或持续内核泄漏验收。最低工程目标仍为 Windows 7 SP1 x64，新系统按能力保留卸载功能；旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续资源压力及端到端 CPU/吞吐仍需目标机器验证。
