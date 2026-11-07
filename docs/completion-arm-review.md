# 普通 completion 的重新武装边界

版本固定为 **2.0.0**。本轮以 `e2f0ffac4ea8`、698 个文件的完整原始字节为基线，未确认需要修改的生产缺陷。新增回归专注普通数据完成通知与最大报文头边界，运行时源码保持原实现。

[CompletionArm32](../tests/model/completion_arm32_tests.c)使用唯一 DLL queue owner，真实 Reserve/Commit 32 包。实际 SYS Receive 创建 32 个普通 NBL，在归还前核对其 token、地址族、协议、partial MDL 和完整载荷，再由两个独立 OS 线程实际归还 16+16。owner 两次 Wait/Poll 各消费 16 项，累计完成 32 项；没有伪造 FULL、修改所有权计数或逐包重放。

第二批分别在第二次 RX_COMPLETE arm 前及 arm 返回 ready=0 后发布。前者必须由实际 ready 重查返回，第二次不进入 SDK 等待；后者必须通过真实 auto-reset UserEvent 唤醒。首轮 SDK 等待固定存在，因此总 SDK 调用次数分别为 1/2。观察门标记的是选定 API 入口，不能据此声称 OS 线程已经休眠。测试不 Reset 生产 UserEvent，也不要求每包独立通知。

IPv4/IPv6 独立构建，TCP/UDP、NDIS 6.20/6.89、游标回绕和两种 arm 顺序逐项验证。旧版本不可用的 NBL 信息槽保留哨兵，新版本可用槽正确清零。所有公开 owner、Return 线程先汇合，再通过原 Close/Free 账本释放；原宿主的内部 core 引用只保护关闭后的工作对象观察，捕获的 SDK 回调线程对象也逐一等待退出。

本组把实际 DLL 源码与 SYS helper 编入 EXE。注册、I/O dispatch、idle-only native worker、NDIS 与 IRQL 仍由原 fixture 模拟，模块 pin 账本归零不等于本组验证了真实 DLL 卸载。完整 worker loop、DRAIN 唤醒和真实 DLL 卸载由既有回归分别覆盖，不合并冒称本组的证据。

[最大报文头 32 段回归](../tests/packet/test_max_header_family32.py)每族一次 checked batch 输出 32 段：IPv4 使用 IHL60 和 TCP60，IPv6 使用恰好 128 字节合法扩展头及 TCP60。独立完整 wire/checksum oracle 核对长度、序号/IP ID 回绕、CWR/FIN/PSH、40 字节 TCP options 与不透明 Timestamp，源、plan、输出指针/容量和尾部哨兵保持。IPv6 128 字节边界对应 [Microsoft 卸载组合合同](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/combining-types-of-task-offloads)；LSOv2 模板规则见 [Microsoft LSO 说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/offloading-the-segmentation-of-large-tcp-packets)。函数调用次数不作为内部复制或 CPU 测量。

本轮也复核了 Wait 的 TX/RX 顺次 arm、Terminal 二次检查、消费者释放空间后的 KernelEvent 通知、关闭事件的 core 生命周期，以及 OffloadMutex/状态上调的 IRQL 契约；未用非法同步普通 OID 重入或手工填满健康槽位来构造竞态。日志 UTC、等待时钟、Epoch/Generation 和包内标记保持各自用途。

新 C 回归接入 UnifiedTests 和保护页 Safety；最大报文头回归按族接入主 Tests。[专项审计](../tools/audit/verify-completion-arm32.ps1)默认从当前源码独立生成，显式基线才完整认证历史身份；来源路径集合、完整字节、有限投影及编译/日志/元数据拒绝合同均独立核对。本轮记录位于 `out/completion-arm-review/`。

以上为 hosted 验证。保护页只覆盖声明的宿主对象，资源账本与线程汇合只证明对应路径。旧系统实机、签名安装、真实 NDIS/IRQL、Driver Verifier、持续内核资源压力及 CPU/吞吐/延迟仍需目标机器验证。
