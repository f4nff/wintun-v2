# SYS 故障 QUERY 锁模式与恢复级别复核

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮起始完整验收指纹为 `e1d312e643c1f8f6c132829ed3ee44d5c968533023f048e6474f3416145ffb58`。

本轮未确认新的生产缺陷，未修改 SYS/DLL 生产实现。继续复核槽位归还、引用与物理回收屏障、唤醒和 CPU 成本后，新增工作集中在上一轮[故障 QUERY 准入修正](sys-fault-query-admission.md)的回归检查：明确验证锁模式和保存/恢复级别，防止今后维护锁升级出口时引入错误。本轮开始时记录的 103 个生产源文件及版本属性文件逐项核对 SHA256，确保实现保持不变；记录位于 `out/sys-query-lock-review/baseline.json`。

## 检查的边界

故障 QUERY 先释放 shared，再取得 exclusive，重新查找 `ctx->V2`。调用者不能递归取得已经持有的同一个锁，释放操作必须匹配获取模式，并恢复获取接口返回的原 IRQL。[Microsoft：shared](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exacquirespinlockshared)、[Microsoft：exclusive](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exacquirespinlockexclusive)。

`tests/model/fault_query_tests.c` 的线程局部观察器记录当前 TransitionLock、shared/exclusive 模式和获取时保存的调用级别。获取前检查没有重复持锁；释放前检查锁、模式、当前模拟级别和保存参数全部匹配。获取/释放仍使用真实 Windows SRW locks，计数必须逐项平衡；调用者退出时必须恢复到入口级别且没有遗留持锁。

入口级别分别建模为 0、1、2。这里检验的是接口参数与锁模式契约；Windows 宿主线程、等待、保护页仍在用户态运行，不能据此断言真实内核 IRQL、抢占或调度行为已通过。

## 新增覆盖

- 每个入口级别直接执行五次实际 QUERY：无会话、正常运行、正常排空、shared 快照期间发生故障、随后再次查询故障状态，共 15 次。无会话输出用非零哨兵逐字节检查。正常路径保持 shared；快照期间发生故障不能确认 STOPPED，下一次查询才进入故障锁模式。
- 三个入口级别分别执行实际发送准入、rundown/reference、Fault、私有 quiescence 和 QUERY。旧 QUERY 仍须复现过早 STOPPED，当前 QUERY 须等待准入记账并看到 Active=1。完成回调在模拟级别 2 实际重入 QUERY，引用覆盖整个回调，最终释放后才确认 STOPPED。故障后报文读取、描述、控制回复及发布均不能发生。
- 当前实现执行六个升级窗口：三个入口级别分别组合移除或替换会话。旧会话设为 PAGE_NOACCESS；重新获取 exclusive 后只能安静返回 DEVICE_NOT_READY，或取得新会话的 RUNNING/epoch=84 快照。移除分支对非零输出哨兵逐字节核对，排除部分输出写入或旧快照残留。

`tools/audit/verify-sys-fault-query.ps1` 保留新旧 QUERY、ISO/MS volatile、两类查询测试的八个正常进程，并新增四个独立负向对照：只在忽略的 `out` 目录生成错误头文件，分别将 exclusive 释放改为 shared，或错误传入保存级别 0，再各自以 ISO/MS 模式编译运行。每个对照必须以退出码 1 在对应的精确模式/保存级别断言处被拒绝；编译失败、其他崩溃或不匹配的断言不能当作成功。生产文件不会被这些对照改写。

因此定向验收共 12 个进程：八个正常退出 0，四个按预期拒绝并退出 1。执行记录显式保存 `expected_exit_code` 和 `expected_observer_rejection`，避免将预期失败当作测试遗漏。Tests/Safety 也执行扩展后的当前源码用例。

## 维护与验收结论

本轮没有增加生产锁、原子操作、唤醒、缓存或常驻状态；没有提出新的 CPU 降幅。既有槽位状态机、有限回收边界、full-to-room 唤醒、关闭重试、AllReturned 和 Returns rundown 的物理回收屏障继续由完整离线验收覆盖。后续优化仍应以可复现的问题或测量为前提。

```powershell
.\tools\audit\verify-sys-fault-query.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

完整验收在最终同一源码指纹上执行 36 项检查；定向负向对照只生成宿主程序。静态分析与起始完整验收比较，不新增抑制或修改分析选项。未安装、签名或加载 SYS；真实 NDIS、IRQL、Verifier、ETW/WPA、内核 CPU、锁争用和长稳验证仍未执行，`kernel_runtime_tested`、`production_ready` 保持 false。
