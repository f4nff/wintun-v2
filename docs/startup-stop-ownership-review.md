# 启动通知与任务预留复核

版本保持 2.0.0，DLL/SYS 数值版本保持 2.0.0.0。本轮基线为
`d4faced72bc16d0b7db5a4baad4cbb23f1956e117c492265441b8b57982ff80c`，
完整 619 个源文件快照位于 `out/stop-edge-baseline/d4faced72bc1`。

本轮未确认新的生产缺陷。新增回归验证两个此前没有直接组合的构造期边界；
生产源码、旧接口、同步及运行时卸载能力门控保持不变。

## Open 返回前的终止通知

注册成功后，内核可以在用户 Open 返回前终止会话。普通 Open 仍可返回一个
有效的公开会话；首次实际队列调用观察共享终止字段，并停止接纳或返回空
完成。终止通知本身不能替代 native 关闭屏障。

扩展测试覆盖 1/2/8 队列和不同首次调用：Reserve、Receive、Wait、Poll。
真实调用者线程访问实际 DLL 源码中的队列实现，全部退出并汇合后才最终 Close。
这组宿主为 EXE，模块引用账本不单独证明实际 DLL 的卸载行为；实际 DLL
等待与卸载由同时重跑的 terminal-wake 等回归验证。
队列保持生产使用的自动复位事件，关闭事件保持手动复位；测试不通过改变
事件类型或补写私有计数制造结果，也不要求通知必须保持未消费状态。

带初始 USO 策略时，另外区分两种顺序：native 已接受策略并确定成功后才
发布终止通知，随后 SDK 交付成功结果；以及注册后先终止、策略请求返回
失败。前者可以发布会话并由首次队列调用停止，后者必须保留失败错误、
不返回公开会话并完整清理。策略失败错误是明确的宿主输入，不宣称覆盖了
真实 NTSTATUS 到 Win32 错误的映射。

共享终止字段、进度序列和 Windows 事件遵循实际通知顺序；native 注册、
策略执行及物理关闭仍是宿主边界。测试不把成功 Open、共享 Terminal 或
无用户 loan 解释为 native 资源已经可以释放。

## 控制任务尚未分配对象时的 shutdown

MainRunOperation 在分配任务对象之前已经预留名额，并持有两个实际事件和
私有线程池。任务对象、工作项、模块引用和运行时租约此时尚未取得。
新增回归在首个任务 HeapAlloc 边界暂停，实际 shutdown 必须等待该名额；
随后分配失败，实际失败路径应归还名额，保存 ERROR_OUTOFMEMORY，并允许
shutdown 重试成功。

测试复用冻结的真实 main.c、operation_runtime.h 和旧所有权宿主，通过有限
的 SDK 边界门闩固定顺序。普通 loader 引用保留至所有导出调用与调用线程
结束；最终执行实际 detach，检查事件、线程池和其余所有权账本归零。
未创建工作项的失败路径不执行项目回调，不能将这个事实扩展成对 SDK 内部
线程的枚举或生命周期证明。

安全负控只省略首个任务分配失败时的 OperationUnreserve。观察仍被占用的
名额和未发信号的 idle 后，在完整对象仍存活且失败调用者已汇合的边界，
调用同一实际释放函数补偿恰一次，再完成 shutdown、detach 和测试线程汇合。
正常构建不补偿，不直接改写计数。

## 回收与唤醒复核范围

本轮重新对照了 Return 的同队列并发唤醒、同槽跨代重用、coalesce 缓冲区
归还后重借、构造失败及故障快照。已有 return_batch、return_coalesce、
return_reclaim、drain_boundary 和 fault_query 测试直接覆盖这些边界；未发现
新的合法反例，因此没有复制这些宿主或改动内核回收流程。

```powershell
pwsh -NoProfile -File tools/audit/verify-operation-construction.ps1
pwsh -NoProfile -File tools/audit/verify-terminal-wake.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

新增测试进入普通及守护内存验收，定向矩阵覆盖 ISO/MS volatile。最终结果与
精确测试、资源范围记录在 `out/stop-edge-investigation/final-review.json`。
历史证据保留原始指纹，不标为本轮重跑。

Windows 锁、事件、DLL 加载和指定测试线程实际执行；NDIS、内核 DDI、IOCTL
及 IRQL 仍是宿主边界。没有增加生产锁、分配、线程或轮询。真实旧系统、
Driver Verifier、设备移除、长期内核资源及 CPU 测量仍待专用系统验收。
证据保持 `kernel_runtime_tested=false`、`production_ready=false`。
