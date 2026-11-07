# 控制回复、状态拒绝与 32 段别名复核

版本固定为 **2.0.0**。本轮以 `46866e17acd2` 的 711 个源文件为基线，逐文件保存原始字节并验证完整身份，继续检查并发归还、停机和唤醒。当前未确认新的生产缺陷，运行时实现和上一轮的停机测试同步修复保持原样。

[控制回复批量测试](../tests/model/control_batch32_tests.c)在 16 个普通 IP 包与另 16 个普通 IP 包之间放入一个实际 ARP 或 IPv6 邻居请求，明确是 **32 个数据包加 1 个控制帧**。实际 Send 必须先发布前缀、释放队列及转换锁，再指示控制回复；回调内用另一组独立数据包合法重入 Send，回复后外层后缀重新检查准入。另一线程开始 Pause 或 Drain 的场景另行验证后缀停止，前缀不重放。Drain 组合执行实际 BeginDrain 与指示 bank 等待，完整 IOCTL dispatch 由既有回归覆盖。

测试绑定实际 ControlReply 的分配、MDL、NBL、元数据初始化与释放。控制回复使用 RESOURCES 指示，按 [Microsoft 接收指示合同](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismindicatereceivenetbufferlists)，其所有权在指示返回时立即回到驱动。控制缓冲区的构造复制与普通数据载荷复制分别计量。接收上调仍由宿主承接；不会把同步回调内等待 Pause/注销完成作为合法网络栈行为。最终按真实引用、Pause、Unregister 与 Free 的依赖顺序收尾，并汇合测试线程。

[状态所有权测试](../tests/model/status_ownership32_tests.c)使用真实 DLL/v2 队列，同一合法 owner 实际提交 32 个包，并从另一方向导入 32 个接收 loan。Open 之后分别注入长度、结构身份、版本、指纹和 I/O 错误的能力回复，公有 GetCapabilities 必须拒绝，队列所有权、载荷和停机标志保持。失败的能力输出结构没有不变性承诺，不据此额外收紧 API。

BeginStop 后，未归还的 32 个 loan 先阻止回收；同一 owner 实际归还并汇合后，可信 Active=1 回复继续保留资源。Active=0 时的坏 QUERY 长度、Size、Epoch 或 I/O 失败也不得授权退休 Published32。只有恢复可信 STOPPED 回复，实际生命周期 worker 才能协调本地账本并进入 Prepared。恢复后的回调先真实完成，再调用准备关闭及最终关闭；所有捕获的回调线程按实际数量汇合，并验证真实 DLL 卸载及原资源账本。

坏回复是明确的防御性驱动/SDK 边界输入。NativeActive 是独立的宿主关闭屏障，不代表执行了真实 SYS 或 NDIS。NDIS 6.20/6.89 变量用于既有能力分支，不能替代旧系统实机；无新增卸载能力、公共接口或协议变更。

[32 段末项别名测试](../tests/packet/test_request_alias_family32.py)在 IPv4/IPv6 独立进程中分别测试 TCP/UDP。前 31 个输出正常，仅令第 32 项的一个可访问字节分别与源、plan、输出元数据或 written 重叠。几何无效请求必须整批拒绝，written、源、plan、全部元数据及输出哨兵不变。只恢复末项的地址和容量后，一次 checked batch 生成全部 32 段，由独立完整 wire/checksum oracle 验证；不会把逐段调用作为恢复。该组按 C helper 的调用方校验合同执行，不声称别名是合法请求。

新 C 回归按族接入 UnifiedTests 与保护页 Safety，Python 回归接入主 Tests。专项入口为[控制批量审计](../tools/audit/verify-control-batch32.ps1)和[状态所有权审计](../tools/audit/verify-status-ownership32.ps1)，完整入口为 [validate-unified.ps1](../tools/build/validate-unified.ps1)。当前源码可以独立生成专项产物，显式历史基线另做完整认证。原始宿主、有限 include/观察包装、生产来源及编译/日志/元数据拒绝合同分别核验，记录位于 `out/control-status-review/`。

包内不透明标记、TCP Timestamp、日志 UTC、单调等待时钟与 Epoch/Generation 继续各自使用。复制字节和函数调用计数不作为 CPU、吞吐或延迟测量。保护页和资源账本只覆盖声明的宿主对象；旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及 CPU 性能仍需目标机器验证。
