# 进程所有权、非零策略与改名失败复核

版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮基线为
`89081d142dbf270e095b513e218ab2c019c71fa0c618ab3e41cbb831cc105dae`。

## 确认的改名失败问题

旧 `NciSetAdapterName` 处理名称冲突时，会尝试移动占用名称的另一个适配器。
如果系统改名返回 `ERROR_ACCESS_DENIED` 等错误，旧代码仍先调用 `RenameByNetGUID`
修改它的自定义名称元数据，再为本适配器选择后缀名称。这可能让另一个适配器的
系统名称与自定义名称不一致，即使本适配器的创建最终成功。

精确原函数的宿主复现确认了这个分支，所有 NCI、设备和注册表操作均由观察器替代。
修复只在系统改名成功后更新对应元数据。名称后缀、长度限制、原有取消检查点和
元数据更新失败时的 best-effort 日志策略保持，不引入跨适配器回滚事务。

## 进程和文件所有权的实际回归

新增测试执行生产的进程通知、文件关闭、共同注销和上下文 rundown 代码，补齐
此前只运行内层会话回收、手工模拟 owner/list 移除的缺口。

测试构造已就绪会话，不在这一组中执行注册入口。它固定两类迟到操作：退出通知
已取得旧上下文引用后，模拟另一个进程发布替代会话；旧文件的 Close 到达时，
该上下文已属于新文件。PID 和文件身份必须在注册锁内重新确认，迟到操作不能
注销新会话。真实注册入口仍由单独的注册回归覆盖。

[IRP_MJ_CLOSE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mj-close)
不保证运行在关闭句柄的进程上下文；测试只执行现有 Close handler，不虚构 Cleanup
handler，也不以调用线程的当前 PID 替代文件身份。

另一组交错令会话和归还回调已经结束，但进程或 Close 回调仍持有 Control pin。
Halt 必须先撤销新的设备查找，再等待该 pin，之后才可释放上下文。
这与保护 NBL 回调尾部的 Return rundown 是两层独立的寿命边界。

## 非零策略的真实 DLL 路径

新增策略测试不启用 `PB_API_TESTS`。它通过宿主驱动 I/O 边界执行生产 DLL 的
非零 `PolicyIo`、ticket 和生命周期控制流，避免测试开关覆盖实际 I/O 结果。
最低能力驱动模型返回不支持时，消费失败 ticket 后仍可应用零掩码，保持会话可用。

测试固定 worker 采样未停止、尚未取得 Control 锁的窗口，再发起停止请求：
非零策略必须在锁后重新检查停止状态。已经进入 I/O 的迟到成功或失败也不能将
DLL 生命周期恢复到 Running。迟到成功可以先更新驱动模型的掩码，随后由 DRAIN
清零；测试不把 DLL 阶段与内核状态等同。用户借用和原生 active 见证分别阻止提前回收。

这些测试针对具体覆盖缺口，本轮仅在确认生产问题后修改相应实现。
替身驱动的能力和 active 状态不代表在真实旧系统执行过 NDIS 操作。

## 验收范围

新增测试与此前的注册、回调交接、归还、暂停/注销、外层 Halt 和唤醒矩阵一起验收。
最终源码指纹、执行数量、负对照、编译模式、静态诊断比较、导出/导入、版本和
产物哈希见 `out/process-owner-investigation/final-review.json`。

```powershell
pwsh -NoProfile -File tools/audit/verify-adapter-rename.ps1
pwsh -NoProfile -File tools/audit/verify-kernel-process-owner.ps1
pwsh -NoProfile -File tools/audit/verify-native-policy.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

测试使用真实 Windows 锁、事件、堆、已汇合线程及明确记录的 DLL 装卸；
对象管理、页锁定、设备与驱动 I/O 仍是宿主替身。不操作真实设备、注册表或启动策略，
不安装或加载驱动。Windows 7 SP1 x64 仍是最低工程目标，NDIS/IRQL、旧系统安装、
Driver Verifier、长期内核池、句柄、线程和 CPU 验收仍待完成。
证据保持 `kernel_runtime_tested=false`、`production_ready=false`。
