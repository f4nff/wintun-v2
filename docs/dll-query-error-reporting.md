# DLL 无效 QUERY 确认的关闭诊断

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮从完整验收指纹 `7bbec4504e4b33eb1e3b490e87decee548f8e79459a5fd7d5c4ad997f3204adf` 继续检查 SYS/DLL 槽位、回收、唤醒和 CPU 路径。

本轮确认并修正的是回收阻塞原因的诊断：没有新确认的提前复用、提前释放或漏唤醒。上一轮 SYS 发送窗口初始化修正保留；此次生产改动仅在 `api/v2/session_status.h`。

## 已复现的问题

`StatusInternal` 原先把 DeviceIoControl 成功、返回长度、Size 和 Epoch 的检查写在同一个布尔表达式中。真正 I/O 失败及成功后发现响应无效，均返回 PB_CORRUPT；成功但无效的分支没有设置相应 Win32 错误。

LifecycleWorker 随即读取 GetLastError，并发布到 `PB_CLOSE_PROGRESS.LastError`。因此短响应、错误 Size 或旧会话 Epoch 可以被报告为残留的 ERROR_BUSY，甚至 ERROR_SUCCESS，误导对原生停止确认和关闭阻塞的排查。旧代码正确拒绝这些确认并保留资源；问题是阻塞原因不准确。

GetLastError 保存调用线程的最近错误，成功调用不保证清除或产生适合此次校验失败的错误。不能把成功 I/O 后的残值当作响应校验错误。[Microsoft：GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror)。

独立宿主复现执行实际非 PB_API_TESTS 的 DLL session/lifecycle，只将设备 I/O 换成可控响应：QUERY 返回成功、STOPPED/零引用、正确 Size/Epoch，但返回字节数少一。旧 GetStatus 返回 PB_CORRUPT，同时错误为残留的 170（ERROR_BUSY）；关闭进度也存入 170，设备关闭次数为零、映射仍保留。日志为 `out/dll-query-error-review/pre-fix.log`。这是本地错误传播的复现，不表示实机 SYS 已出现上述无效响应。

## 局部修正

`StatusInternal` 现在明确区分三个出口：

- DeviceIoControl 失败：直接返回 PB_CORRUPT，保留其真正 Win32 错误。
- I/O 成功且既有长度/Size/Epoch 校验通过：返回 PB_OK，保持原有成功行为。
- I/O 成功但校验失败：设置 ERROR_INVALID_DATA，再返回 PB_CORRUPT。

原有校验条件和关闭授权保持一致；没有扩大哪些响应可以确认 STOPPED。没有清空或伪造返回状态，没有因错误诊断而直接清退 Published、强制释放映射、绕过 loan/Active/rundown/Device Close，也没有改变 worker 的进度事件、重试或一秒诊断等待。新 SetLastError 只在控制查询的无效响应分支执行，不增加包处理热路径工作，也不宣称 CPU 降幅。

## 新旧对照与回收屏障

`query_error_tests.c` 复用既有真实 Windows 句柄、私有线程池和资源追踪 fixture，执行实际非测试替身的 StatusInternal、GetStatus、关闭 worker 和释放函数。设备 I/O、SwDeviceClose 为宿主边界；不存在用 TestActive/TestFault 直接伪造 QUERY 确认的捷径。

每个进程执行 24 组场景：

| 维度 | 取值 |
| --- | --- |
| QUERY 响应 | 返回字节数过短、过长、Size 错误、Epoch 错误、真实 I/O 失败、有效对照 |
| 成功响应留下的错误残值 | 0、ERROR_BUSY |
| session 来源 | 直接 Open、StartAdapterSession |

过长字节数只改变宿主返回长度，不越界写入缓冲区。I/O 失败注入 ERROR_DEVICE_NOT_CONNECTED，验证它不会被 ERROR_INVALID_DATA 覆盖。有效对照验证原成功路径及残值行为未改变。

无效确认场景先检查公开 GetStatus，再让真实关闭 worker 处理同一响应。即使响应内容声称 STOPPED 且 Active=0，关闭进度仍须停在 PREPARED 之前、BlockStage=KERNEL、Result=PB_CORRUPT，设备关闭次数为零，Region 和运行时预算保留。适配器场景还提前释放调用者的 adapter 引用，确认 session 仍保留它。之后只纠正 QUERY 响应，原有关闭路径必须完成恰好一次设备关闭，并在设备关闭后释放适配器；追踪的分配、区域、事件、模块、work、session/adapter 预算归零。

冻结头为 `out/dll-query-error-baseline/7bbec4504e4b/api/v2/session_status.h`，SHA256 为 `bff862250846d1b79bae501ae4a639f51634041ac6790afe08d0b717c5f0606d`。`verify-dll-query-error.ps1` 对新旧状态头使用相同当前 session/lifecycle，仅在 out 副本中解析包含路径并替换状态头；原始 session.c、控制和释放实现不改。ISO/MS volatile、普通/保护页模式共八个实际进程，旧实现必须复现残值，新实现必须报告准确错误，双方都保留真实失败码和关闭屏障。

```powershell
.\tools\audit\verify-dll-query-error.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

新用例接入 Tests/Safety，进程数分别为 44、36；完整离线验收新增 DllQueryError，共 38 项。最终结果绑定同一源码指纹，保留原始静态诊断并与起始验收比较，不增加告警抑制。没有改变共享协议、函数表、ABI、版本或 SYS 实现。

未安装、签名或加载 SYS；真实 IRQL、NDIS、Verifier、ETW/WPA、CPU、低资源及长稳仍待实机验证，`kernel_runtime_tested`、`production_ready` 保持 false。
