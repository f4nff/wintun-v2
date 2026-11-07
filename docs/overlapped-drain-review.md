# 2.0.0：异步 I/O 终止、事件唤醒与资源回收复核

本轮确认并修复 DLL 跨架构辅助调用的异步管道回收缺陷。产品版本固定为 2.0.0，旧 Wintun 导出、v2 ABI、单 DLL/单 SYS、系统能力分级和新系统卸载功能保持现有约定。

## 确认的问题

原实现把 `GetOverlappedResult(..., TRUE)` 的失败直接当作请求结束，清除 `Pending` 并关闭读句柄和事件，随后可以释放包含 OVERLAPPED 和读取缓冲区的任务。阻塞查询的内部等待失败，并不能证明请求已完成。取消请求成功或返回 `ERROR_NOT_FOUND` 也不能代替完成屏障。

使用真实未完成的重叠管道读取，延迟取消完成并注入等待错误，冻结旧实现能确定性触发 `PENDING_IO_RELEASED`（退出码 46）。另外分别向连接、退出后排空的阻塞查询注入错误，确认旧实现会拒绝连接（47）或丢弃输出（48）；这两个负对照验证共享修复覆盖全部调用点，不代表在真实系统中观察到这些错误。负对照在退出前真实取消并收割未完成请求，不遗留异步访问测试内存。

依据：[CancelIoEx 的完成与生命周期约束](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex)、[GetOverlappedResult 的非等待查询约定](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult)。

## 修复后的所有权边界

`api/overlapped_wait.h` 统一处理连接完成、取消后的收割和退出后排空。该内部函数要求独占且有效的文件句柄、OVERLAPPED、缓冲区及手动复位事件。每次用非等待查询判定实际完成；只有终态才能清除 `Pending` 和释放资源。

等待流程为“查询—复位事件—再次查询—等待—重查”。第二次查询捕获第一次查询与事件复位之间完成的请求，避免抹掉唯一唤醒。事件信号本身不证明请求完成；残留信号在复位后重新核实。等待或复位异常保留全部所有权并请求 1000ms 退避，记录首个错误，直至请求真实结束。正常已完成请求只需一次查询；正常空闲请求阻塞等待，不增加定时轮询或后台线程。

只有 WAIT_FAILED 读取 GetLastError；无限事件等待返回其他异常状态时归一为 ERROR_GEN_FAILURE，避免误用此前 I/O 查询残留的错误值。零错误码的失败同样归一，已有业务错误继续优先。参见 [WaitForSingleObject 返回值约定](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)。

长期无法确认完成时继续由既有有界任务持有资源。这不是宣称系统故障时也能立即回收：提前释放仍在使用的内存会造成越界访问。辅助任务上限和调用方 shutdown 超时语义保持不变。

只使用既有 Win7 可用的同步/重叠 I/O API；没有引入 Win8 的 GetOverlappedResultEx。SYS 的双重回收屏障、MDL/NDIS 能力分级和现代卸载能力未修改。

## 测试与重放

`tests/safety/helper_io_fixture.c` 直接包含实际 helper 实现，使用真实重叠管道、私有 OVERLAPPED、真实取消/写入/断开以及完成后加入的测试线程；只在 OS 边界注入故障和调度，不以另写的算法模型替代生产代码。

- 取消受理、未找到请求、拒绝访问；之后分别真正取消、正常完成或对端关闭，分别启用/禁用输出排空。
- WAIT_FAILED、WAIT_TIMEOUT、WAIT_ABANDONED 和零错误码；完成发生在首次、第二次退避或后续阻塞等待。
- 请求在事件复位之前或之后完成；残留事件信号；请求在第一次查询前已经结束。
- 等待/复位连续失败 64 次，每次验证 1000ms 退避请求、未完成状态、事件有效和三个管道资源仍在持有；首个业务错误不被后续错误覆盖。
- 已完成连接和排空读取以 pending 返回的竞态，检查原始输出字节完整。
- 每轮 199 个场景，40 次 DLL 装卸共 7960 个场景；每次检查任务分配/句柄/工作项计数归零、模块完全卸载和预热后 OS 进程句柄数无增长。所有直接创建的测试完成线程均核实终止后加入、关闭，创建数与加入数一致。

退避测试用确定性门控代替实际睡眠，不以墙钟时间推算正确性。它验证调用退避及资源保持，不代表真实故障恢复延迟或 CPU 性能测量。

进程线程快照只记录范围，不要求总数固定：并行回归追踪到原生 `ntdll.dll` 入口线程随负载增加，即使测试自建线程已经逐一加入。把 Windows 自身的加载器/线程池伸缩等同于 DLL 线程泄漏会造成误报。测试核实每个可归属线程的真实终止、句柄关闭及模块完全卸载，并保留进程快照诊断。

```powershell
pwsh -NoProfile -File tools/audit/verify-overlapped-drain.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

I/O A/B 审计验证冻结旧头文件 SHA-256，要求三个指定负对照失败，当前实现使用 ISO/MS volatile 两种编译模式通过全部场景。源指纹、二进制哈希及执行日志写入 `out/overlapped-drain-audit/<指纹前12位>/evidence.json`。常规 Tests/UnifiedTests 和 Safety 均自动执行新增用例。

SYS 回归继续验证 AllReturned 与 Returns rundown 的独立屏障、回调尾部唤醒、并发归还/停止、暂停期间回收和 arm/recheck。检查中未确认需要修改的新 SYS 缺陷，保持既有生产同步顺序。

这些都是离线/宿主验证，不能代替真实 Win7/现代 Windows 的签名安装、NDIS、Driver Verifier、休眠恢复、热移除和长时间内核池/句柄/线程监测。记录保持 `kernel_runtime_tested=false`、`production_ready=false`，测试产物为未签名开发产物。
