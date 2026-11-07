# 终止广播与 helper 响应边界复核

版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮从源码指纹
`e16627eaddcc98149ff28b62e01cce336ea838f3bb9e6c84c524888044a33dbe`
继续复核；基线快照保存在 `out/terminal-wake-baseline/e16627eaddcc`。

## SYS 到 DLL 的终止唤醒

此前普通无限等待测试由 `PbTestFault` 发布终止，SYS 的实际 Progress/Fault
和 DLL 的实际等待路径没有在同一共享映射、同一组事件上共同执行。
新增回归补齐这一边界，不使用 `PB_API_TESTS` 的终止替身。

测试构造就绪的 SYS/DLL 状态，执行生产终止发布与队列等待代码，以实际 Windows
事件和线程固定检查、arm 与等待之间的交错。终止是永久条件，事件只是重查提示：
已经等待的队列依赖广播醒来，先观察到终止的队列则锁存 ClosingEvent。
普通进度通知不能被误认为终止。队列的线程所有权仍须遵守，所有等待者在释放
事件和映射前汇合。

负对照移除终止广播，观察缺少必要唤醒，再显式救援等待者，完成汇合和资源清理
后报告预期失败。不得靠直接退出进程来掩盖负对照的线程或句柄残留。
这组测试没有执行驱动加载、实际注册、NDIS、IRQL 或 I/O 管理器调度。

## helper 响应容量契约

[Windows 的设备实例 ID 契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/device-instance-ids)
规定不含 NUL 的长度小于 `MAX_DEVICE_ID_LEN`；
[SetupDiGetDeviceInstanceIdW](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdeviceinstanceidw)
按字符接收缓冲区大小并返回 NUL 结尾字符串。当前 SDK 的最大有效正文是 199 字。

helper 原成功格式是十六进制状态、空格和实例 ID，没有附加 CRLF。
因此最大成功响应正文为 `1 + 1 + 199 = 201` 字，另需 NUL；父端原来的
`MAX_DEVICE_ID_LEN + 1` 容量只能接收 200 字正文。既有 host 测试使用更大的
独立输出缓冲区，未把结果交给实际父端传输和解码，遗漏了这个不一致。

本轮统一内部响应容量定义，保持现有 wire 格式；完整接收合法 ID 后才拷贝到
原有大小的调用方缓冲区。扩容后超过 ID 上限的成功响应必须被拒绝，不能由
`_TRUNCATE` 静默变成另一个标识。公开接口、类安装器仅返回错误码的响应和
设备创建策略保持不变。

复现和回归由宿主 SetupAPI 返回合法边界数据，经过实际 host 格式化、传输消费
和父端解码。它确认内部协议容量契约的问题，不证明固定 PacketBridgeV2 ROOT
生成器在真实 Win7 上产生过 199 字 ID。目前发布流水线仍只验收原生 x64，
保留的跨架构 helper 路径不能据此视作其他架构已获验收。

## 验收范围

执行记录、负对照、源码和二进制哈希以及最终版本检查保存在
`out/terminal-wake-investigation/final-review.json`。复核命令：

```powershell
pwsh -NoProfile -File tools/audit/verify-terminal-wake.ps1
pwsh -NoProfile -File tools/audit/verify-helper-response.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

扩展回归发现旧孤立设备审计的整体源码校验尚未记录此前的四处独立提取。
本轮补齐 Win7 等待、适配器元数据、名称和发现函数的精确边界变换；保留冻结
原文件哈希、整体逐字比较和原有七个负对照，不以放宽校验绕过失败。
首次结构校验失败的记录保存在 `out/terminal-wake-investigation/attempt-9004d153a7e8`。

真实旧系统安装、NDIS/IRQL、Driver Verifier、长期内核池、句柄、线程和 CPU
验收仍待专用系统完成。离线证据保持 `kernel_runtime_tested=false`、
`production_ready=false`，不把宿主资源计数或事件操作次数当成内核 CPU 测量。
