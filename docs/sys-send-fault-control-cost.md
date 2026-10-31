# SYS recycle 故障传播与发送控制解析

后续接收统计与构造期间故障撤销见 [SYS 接收批次统计与本地归还](sys-receive-batch-sync-cost.md)。

本轮从 `a5bef19f1073ff0e67c01515f4ca988821ac79eb1bcb461bafa8f99488facfd3` 继续检查 SYS 的 CPU 与等待路径，冻结源码位于 `out/sys-send-fault-baseline/a5bef19f1073`。没有安装、签名或加载 SYS。

## recycle 故障没有结束调用方事务

`PbV2RecycleLocked` 原来返回 void。队列计数损坏时，它标记 Faulted 后跳出内部循环；token 损坏时，它甚至继续验证余下 token 和后续批次。发送调用方接着查询 TX 空间、取得新槽位、构造描述符并发布。worker 调用方则继续执行一次 RX，再 arm 和等待，下一轮才观察已经发生的故障。

用冻结的实际发送函数复现：recycle 环中有 128 个旧 epoch token，函数调用 Fault 128 次后，仍构造并发布当前报文；其 NBL 被按成功完成。下一个 NBL 才因 Draining 被拒绝。recycle producer 计数越界时，也能复现已知故障后的单个成功发布。当前引用屏障已避免提前回收，但这些额外工作和成功状态不符合故障后停止接纳的语义。

现在 helper 返回 `PB_RESULT`，并标注 `_Must_inspect_result_`，让静态分析能发现未来调用方遗漏返回值的问题：空环及四批预算正常结束返回 PB_OK，首个环错误或 token 转移错误立即标记故障并返回具体失败。两个调用方都处理失败：

- Send 终止当前窗口，当前及后续 NBL 按 PAUSED 完成，不再查询空间、取得新槽位或构造描述符。窗口结束仍释放 TxLock/TransitionLock；发送引用一直保持到完成上调返回，之后才释放 Active 和 rundown。
- worker 释放 TxLock 后回到循环顶部，执行既有故障收尾并发布私有 FaultQuiesced 见证，不再尝试 RX、arm 或进入等待。

错误前已成功归还的 token 保持 FREE。一个批次已经复制但尚未验证的后缀，以及还在环中的 token，都不再授权继续转移私有池状态；会话故障后不再复用这些槽位，最终由既有 session free 路径释放。TX 槽位不代表 NDIS 保留的 RX 引用，不得以停止 recycle 为由清除 Active、RX credit 或绕过真正的归还。正常 recycle 的四批/128 条预算保持不变，后续正常工作仍按原有 worker arm/recheck 继续。

确定性的宿主结果如下，不能换算为内核时间：

| 场景 | 修正前 | 修正后 |
| --- | ---: | ---: |
| 128 个旧 token 后的 Fault 调用次数 | 128 | 1 |
| 已知 recycle 故障后新发布的 TX 对象 | 1 | 0 |
| 一个合法前缀、随后坏 token 的验证次数 | 128 | 2 |
| worker 收到 recycle 失败后的 RX 尝试/TX arm/等待 | 1 / 1 / 1 | 0 / 0 / 0 |

## 普通 IP 不必执行控制响应解析

原 Send 在 `PbEthernetInspect` 之后，无条件调用 `PbEthernetControl`。后者再次解析 Ethernet 头并判断 ARP/IPv6 NS，普通 IPv4/IPv6 IP 流量最终不会产生控制响应。

当前先使用稳定的私有 prefix 完成 Inspect 和既有 IPv6 邻居报文策略判断；只有非 IP 或直接 IPv6 邻居报文才进入控制解析。普通 IP 的控制解析调用从每包一次变为零。ARP、直接 NS 的完整有效性检查和响应构造仍由同一个共享核心执行；邻居报文的原有丢弃规则也保留。共享头文件明确该回复范围与 SYS gate 的关系，未来新增控制回复家族时必须同时更新 gate 和回归，避免能力扩展被静默跳过。没有提前相信用户可变 payload，也没有省略真正需要的 checksum 验证。

## 回归与证据

扩展的 `send_window_tests.c` 执行实际 Send、recycle、引用及队列核心。回归包括合法 recycle 前缀保留、故障后缀不再授权转移、160 条合法归还仍分两次处理、坏 token/环计数错误后的发送状态与发布数，以及原有窗口预算、队列切换、暂停/脱离、重入上调、完成引用和完成后禁止访问 NBL。

新增 16 类 Ethernet 输入使用保护页源缓冲和实际共享解析器，比较原转发决定、控制回复与精确响应字节：IPv4/IPv6、普通 ICMP、VLAN、ARP、合法 NS、其他邻居消息、坏 checksum、组播源 MAC、截断输入、扩展头和错误类型。发送 Describe 在该模型中仍是资源/状态替身，完整报文校验由既有 Describe 与 packet 测试负责。普通 32 包链另验证控制调用 32→0；完整窗口预算没有变化。

`worker_poll_tests.c` 增加 recycle 失败交错，直接执行生产 worker，确认它发布故障见证且不等待；既有事件 clear、接收和 RX/TX arm 的故障交错继续运行。暂停完成进度和故障清理模型同步使用新的返回契约。Safety 增加同一发送模型，包括保护页报文和 NBL 完成后不可访问检查。

`drain_boundary_tests.c` 另将实际损坏 recycle 环、worker 和已有原生接收对象结合执行：worker 不进入 RX 就发布故障见证，已有 NDIS 接收项仍保持 Active、槽位、credit、两个 MDL 和 NBL；QUERY 必须继续返回 QUARANTINED，真正归还之后才能 STOPPED。Safety 也执行该用例。

```powershell
.\tools\audit\verify-sys-send-fault.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 使用冻结的 Send/recycle/worker 实际函数体和相同当前共享核心、宿主替身；记录实际退出码、计数及源码和二进制哈希。结果位于 `out/sys-send-fault/<当前指纹前12位>`。统一验证增加 SysSendFault，依赖历史快照的五项 A/B 与 benchmark 继续受 SkipBenchmark 控制。

本轮没有发现新的无限等待或缩短任何资源退出屏障。修正让已知 recycle 故障更直接进入原有收尾；真实 NDIS 引用、回调尾部和线程 join 仍需完成。以上为宿主操作计数和离线构建证据，内核 IRQL、NDIS 并发、Verifier、ETW/WPA、低资源及长稳仍待验收，继续保留 `kernel_runtime_tested=false` 与 `production_ready=false`。
