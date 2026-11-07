# 2.0.0：普通管道读取的唤醒、空转与回收

本轮确认并修复 DLL 辅助进程普通读取路径的事件空转问题。产品固定为 2.0.0，公开 Wintun/v2 接口、SYS、共享数据协议和新系统能力门控保持不变。

## 确认与负对照

HelperPump 遇到挂起读时查询 GetOverlappedResult(FALSE)，若仍为 ERROR_IO_INCOMPLETE 就直接返回。手动复位事件若此时已触发，原代码没有清除它，随后的等待持续立即返回；普通循环不断查询同一个未完成读，消耗 CPU。结束阶段的 WaitForOwnedIo 已有复位/复查保护，普通读取没有共用这个边界。

测试以真实 overlapped 命名管道提交挂起读，再显式触发其真实手动事件，注入“唤醒不等于完成”的场景。冻结旧 HelperPump 后可连续重现，指定退出码 77；这不是声称实际捕获了系统自动产生的虚假完成通知。负对照退出前完成真实 I/O 并关闭读取相关句柄。

只加入 ResetEvent 仍不完整：完成可能发生在第一次查询后、复位前。复位会清掉该完成的事件，若不再次查询，线程会等待一个已经完成但不再有信号的读。审计用一处输出目录内的“只复位不复查”变体验证此边界，必须退出 78。这个变体没有写入生产代码，也不冒充旧实现的另一处独立故障。

依据：[GetOverlappedResult 的非等待查询及 ERROR_IO_INCOMPLETE](https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult)、[手动事件的持续触发语义](https://learn.microsoft.com/en-us/windows/win32/sync/event-objects)、[重叠管道 I/O 的独立事件约定](https://learn.microsoft.com/en-us/windows/win32/ipc/synchronous-and-overlapped-input-and-output)。

## 实现与所有权

`api/overlapped_wait.h` 的私有 PollOwnedIo 统一非阻塞准备事务：查询，仍挂起时复位事件，然后再次查询。最多两次查询和一次复位，不在这里等待 I/O。普通 HelperPump 使用它，结束阶段的 WaitForOwnedIo 也复用同一实现，避免两个路径分别维护关键竞态。

复位失败不能证明 I/O 结束。该函数保留挂起状态，并单独报告复位错误；调用方保留文件、OVERLAPPED、缓冲区和事件，记录首个错误并请求 1000 ms 退避。零错误码按 ERROR_GEN_FAILURE 处理。只有终态查询才撤下挂起所有权；即使原生完成恰好已发生、复位又失败，也会保留所有者直到下一次查询。

普通泵没有改为阻塞等待一个流完成。没有复位错误时，一个流仍挂起不会阻止另一个流继续处理，也不会绕开工作线程原来的取消/进程退出等待。异常退避沿用既有等待路径的策略，没有新增线程、计时任务或周期性轮询。正常包循环未修改，不提供内核 CPU 或吞吐优化结论。

所有 API 都属于既有 Win7 目标接口，没有引入 GetOverlappedResultEx。一个 DLL、一套 SYS，旧导出和卸载能力门控均保留。

## 测试范围与复现

扩展 `tests/safety/helper_io_fixture.c`，继续包含实际生产辅助任务代码，使用真实异步命名管道、文件/事件句柄、完成/取消和加入的测试线程。观察器只注入事件信号、指定 API 失败和精确时序，不把“模型完成”当成真实 I/O 终态。

每种 volatile 模式执行 40 次完整 DLL 装卸，共 9080 个 I/O 场景、3200 个实际加入的测试线程。新增覆盖包括：

- 普通挂起读取中的一次/连续虚假唤醒，每次泵最多两次查询、一次复位，不能隐藏阻塞等待。
- 复位前/后完成，正常写入、取消和写端关闭；Payload、响应字节及挂起标志核实。
- 连续 64 次复位失败、零错误和已有首个错误；实际原生完成恰在复位失败中发生时，读取和事件仍由原所有者持有。
- 一个流保持挂起且事件已清除时，另一个流仍能处理完整日志；不等待第一个流完成才服务第二个。
- 原有连接、取消、结束收集、等待失败和输出边界矩阵继续运行。

每次 DLL 装卸都核实全部跟踪的堆对象、工作项及 OS 句柄归零、测试线程已等待退出、DLL 已卸载；首轮后的进程句柄计数保持不变。实时进程总线程数仍作为诊断记录，不能代替逐个测试线程的归属和加入检查。

已有 Tests/UnifiedTests 和 Safety 自动运行扩展矩阵。审计校验冻结 helper/等待头 SHA-256，记录原始/复位变体指定失败标记、ISO/MS 结果、源码指纹和二进制哈希。

```powershell
pwsh -NoProfile -File tools/audit/verify-helper-pump.ps1
pwsh -NoProfile -File tools/audit/verify-overlapped-drain.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

本轮同时复核 SYS/DLL 槽位归还、引用、停机和唤醒交界，没有据未证实的怀疑改动内核。真实 Win7/新 Windows、签名安装、NDIS、Driver Verifier、休眠/热移除及长期内核资源监测仍待实机验收：`kernel_runtime_tested=false`、`production_ready=false`，产物为未签名开发版本。
