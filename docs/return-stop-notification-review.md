# 归还与停机通知复核

版本保持 2.0.0，DLL/SYS 数值版本保持 2.0.0.0。本轮基线为
`e6c767571bed1daf8e27ce06c802cf79730bba583f269329ca5298452fa986ac`，
完整 603 个源文件快照位于 `out/return-stop-baseline/e6c767571bed`。

本轮未确认新的生产缺陷。新增回归执行之前未直接组合的启动、完成队列和
日志所有权路径。生产同步、旧接口、系统能力门控和热路径保持不变。

## 注册失败中的实际 worker

已有注册失败测试使用只等待 Start/Stop 的线程替身；新增测试连接逐字提取的
实际 worker、Register 和 Free，覆盖 1、2、8 个队列的必需资源失败及成功后
初始暂停状态下的注销。worker 在调用前、Start 等待入口或实际等待返回后
暂停，实际回收先发布 Stopping 并唤醒，再让线程继续。每个已创建 worker 都
必须只执行一次 Start 等待和终止，不能进入 clear/recycle/receive/arm。

真实自有 OS 线程全部汇合后，才能释放队列、事件引用、MDL 和上下文。负控
只省略 Free 的 Start 唤醒；在仍持有完整对象且已开始 join 的边界观察遗漏，
通过同一事件接口补偿一次，再执行实际退出、join 和回收。正常构建不补偿。

## completion 分轮通知及队列进度

完整非 PB_API_TESTS DLL 使用 NDEBUG，测试检查仍启用。真实队列所有者先
Reserve/Commit 合法槽位，再退出并汇合。实际生命周期线程先消费初始 stop
提示并进入等待，宿主 native 边界随后发布有效完成和一次 progress 通知。
大队列超过每轮四批的预算时，继续通知必须让剩余完成得到消费；轻量队列
也应在同一轮得到处理，不能只依赖一秒诊断回退。

观察器保留事件真实语义，检查通知来自实际完成/继续路径。最后一份 native
所有权仍会阻止关闭；最后真实完成和 progress 才允许收敛。测试遵循合法
slots/ring/runtime 配额，不伪造 FULL 或修改私有计数。负控只省略预算继续
通知，借真实最后完成与通知安全收尾。

PrepareClose 与后台准备完成相交时，准备阶段可以有一或两次回调，最终关闭
再有一次。测试逐例记录这两个阶段并汇合全部已捕获线程句柄；跨环形状先等
第一回调返回再调用 PrepareClose，明确覆盖额外检查。并行验收发现原审计
固定“每例两次回调”的假设过严，现改为检查逐阶段账本，生产实现保持不变。

## 错误日志分配与回调

完整实际 logger.c 使用真实 FormatMessage、CRT 和 LocalFree。逐一覆盖系统
消息、拼接消息的分配失败，长前缀、回调改写线程错误码和有限重入。每次
调用都检查其实际 Local 分配归还；直接 LoggerError 的返回值与 LAST_ERROR
包装器保存线程错误码的行为分别验证，不把私有注解差异当作新的外部缺陷。
守护模式保护调用方前缀，不声称保护 SDK 内部 Local 分配的布局。

负控只省略系统消息释放，待实际调用返回且其余所有者已结束后，通过真实
LocalFree 归还恰一次。全部资源检查通过后才返回预期失败码；正常测试没有
补偿，不编辑资源计数。

## 排除的关闭顺序疑点与验收

当前 SYS 在 IRP_MJ_CLOSE 处理 FILE 注销。微软说明该请求到达时，在途 I/O
已经完成或取消，因此不能将“关闭早于注册 I/O 完成”的 cleanup 时序直接
套在此处构造泄漏。参见 [IRP_MJ_CLOSE](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mj-close)
和 [IRP_MJ_CLEANUP](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/irp-mj-cleanup)。
现有映射由锁页 MDL 的内核视图持有，本轮没有改动这条回收路径。

```powershell
pwsh -NoProfile -File tools/audit/verify-worker-start.ps1
pwsh -NoProfile -File tools/audit/verify-completion-turn.ps1
pwsh -NoProfile -File tools/audit/verify-logger-owner.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

新测试进入普通/守护内存验收，定向审计执行 ISO/MS volatile 组合，记录逐字
源码、有限宿主适配、单处负控和产物哈希。历史审计保留原始指纹及证据，不
标为本轮重跑。最终结果位于 `out/return-stop-investigation/final-review.json`。

NDIS、内核 DDI、IOCTL 和 IRQL 为宿主边界；Windows 锁、事件、线程等待和
指定 SDK 调用实际执行。资源账本收敛与通知次数不是实机内核泄漏或 CPU
测量。本轮不安装驱动或更改系统配置；真实旧系统、Driver Verifier、长期
内核池、句柄、线程与 CPU 验收仍待专用系统完成。证据保持
`kernel_runtime_tested=false`、`production_ready=false`。
