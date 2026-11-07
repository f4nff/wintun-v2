# 原生启动错误与兼容层、暂停屏障复核

版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮接受基线为
`a4e6a58f27c18a0202b20e4f4bbcc7a55f2128f28b7ccd61a8827730b4f3570d`，
完整快照位于 `out/queue-stop-baseline/a4e6a58f27c1`。

## 已确认的旧接口启动错误

实际 `WintunStartSession` 经默认 `PB_COMPAT_OPEN`、发布版 v2 `OpenSession`
进入 `Allocate`。原实现遇到 CRT 分配失败或实际构造额度已满时只返回
`PB_FULL`，没有设置可供旧接口读取的 Win32 错误。零 LastError 会被旧接口
映射成 `ERROR_NOT_SUPPORTED`；无关的非零 LastError 也会直接流向旧调用方。

回归没有伪造 Api.Open 返回值：它在真实分配边界注入合法的 NULL/ENOMEM，
另用实际 Allocate 构造占满 16 个准入名额。两种情况都复现了错误分类丢失，
并在全部资源归还及线程汇合后报告旧基线失败。非零初始错误是验证线程局部
依赖的调度输入，不声称 Windows 新线程必然以某个特定错误码启动。

v2 的公开结果仍是 PB_RESULT；修复在内部 Allocate 建立完整的构造失败错误
约定，使旧接口能消费可靠的错误。CRT 失败报告内存不足，实际准入额度不足
报告额度不足；随机数生成失败及零 epoch 分别报告一般失败及数据无效。
每个 Win32 申请立即检查并保存错误，队列事件和槽位数组不再在首个失败后
继续申请。所有已转交资源仍由原 Destroy 清理，清理结束后恢复首错。
没有新增导入、公开 ABI 或数据路径锁。

这与 Microsoft 对 [calloc 的错误约定](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/calloc?view=msvc-170)
及 [GetLastError 的有效范围](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror)
一致：不能仅凭 PB_FULL 推断当前线程已得到相应的 Win32 错误。

## 实际兼容工作线程与延迟回收

新增 DLL 回归把当前旧接口、worker/pump 与非 PB_API_TESTS 原生会话连接，
使用默认 PB_COMPAT_OPEN。工厂边界保留真实 Open/Close，提供 I/O 与观察门；
原 native-policy 资源账本只包含一次，旧接口的私有 heap、ring、事件、线程、
路径、模块引用与 runtime 另行记账。守护内存覆盖原生对象和映射，旧 ring
仍使用真实 VirtualAlloc。

发送侧固定 Wake 已发生而 native wait 尚未执行、Reserve 已借出 8 个槽位、
提交前终止、真实 Commit 返回 STOPPED 且接受数为零、同一 owner Cancel 全部
借用的交接。接收侧只发布一次 64 个有效最大报文，实际 pump 复制并归还
首个 native loan，剩余 63 个在停止时由 CompatReturnPending 归还。
公开读者持有的私有副本可在 worker 已退出且 End 尚未调用时归还；读者汇合
后才 End，不在会话销毁后使用旧句柄或包指针。

真实 Close 的 5000 毫秒预算未缩短。正常关闭等待 native-active 归零；
QUERY 失败时，真实 Close 返回 CORRUPT，ReleaseSession 转交原生所有权。
旧 worker 汇合后可释放私有 ring，而原生登记与工作引用仍独立保留设备和
映射。恢复 QUERY 并释放 native-active 后，完成物理回收、回调线程汇合及
实际 DLL 卸载。缺少 pending return 的测试变异在同一 owner 的 Close 边界
观察剩余借用，再显式归还已知 token，仅用于安全清理后报告失败码 116。

## NBL 已归还不等于上调已返回

[MiniportPause 的契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_pause)
同时要求收回未归还的接收 NBL，并等待所有 receive indication 调用返回。
新增回归组合当前 ReceiveBatch、外层 Return、indication bank 和 TunPause /
PbV2Pause：NBL 和 Active/Return pins 均已归零时，上调仍停在 EndIndication
之前，Pause 必须继续等待 bank idle。

覆盖 bank0、实际轮换后的 bank1、两个 bank 同时有未退出上调，配合单/双 NBL
及 auto/manual 事件。省略 bank wait 的负对照只观察 Pause 提前完成，随后
先放行上调、汇合所有测试线程，再释放会话资源；不在 Pause 未完成时运行
Halt。本轮未确认 indication 或归还路径的新生产缺陷。

## 验收与维护边界

```powershell
pwsh -NoProfile -File tools/audit/verify-native-start-error.ps1
pwsh -NoProfile -File tools/audit/verify-facade-stop.ps1
pwsh -NoProfile -File tools/audit/verify-indication-pause.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

历史审计继续锁定原整文件、运行负对照及整体比较；共享的有限变换只允许
已单独审计的 Allocate 区间替换，原函数与新函数哈希固定，其他字节不得漂移。
最终证据、数值版本、导出/导入和产物哈希位于
`out/queue-stop-investigation/final-review.json`。

设备 I/O、NDIS、IRQL 和旧接口 MainRuntime 边界按各回归说明由宿主提供；
v2 会话准入、引用及失败清理执行当前生产实现。
没有安装驱动或改变设备、注册表、启动策略。真实旧系统、Verifier、长期内核池、
句柄、线程和 CPU 验收仍待专用系统完成。证据保持 `kernel_runtime_tested=false`、
`production_ready=false`。SYS、兼容 pump、队列和生命周期热路径均保持不变。
