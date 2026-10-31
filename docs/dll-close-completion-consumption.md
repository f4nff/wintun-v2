# DLL 关闭时完成项的消费与记账清退（2.0.0）

本轮确认并修正了 DLL 关闭路径中的有效完成项误判。产品版本保持 **2.0.0**，PE 数字文件版本保持 **2.0.0.0**，ABI 不变。

## 已确认的问题

`LifecycleWorker` 每轮每队列调用 `PollInternal` 最多四次，每次最多 32 项。原实现达到这个预算后仍继续查询内核；收到可信 `STOPPED` 且没有用户借用和内核活动引用时，直接将全部剩余 `Published` 私有槽位转为 `FREE`。

`driver/v2/session_query.h` 的正常 drain 判定检查已捕获的提交边界、`Credits` 和活动引用；故障 drain 还检查私有 worker quiescence。`PbV2FlushLocked` 在完成项**发布**后归还 credit。因而 `STOPPED` 确认原生所有权和交付已结束，并不证明 DLL 已**消费**完成环。正常、未损坏的环完全可能保留超过 128 项。

使用实际 DLL/core 和 Windows 线程池的宿主复现中，提交并完成 256 项后，旧 `PrepareClose` 返回 `PB_OK`，但只消费了 128 项并清退了 256 项记账。随后仍合法的 `PollCompletions` 读到旧 token，其私有槽位却已是 `FREE`，因而返回 `PB_CORRUPT` 并设置 `Fatal`。`PrepareClose` 保留公开句柄，接口允许此时执行归还／完成轮询；最终 `Close` 前仍要求调用者停止并 join 所有使用者。

还有同一根因下的时间窗口：最初轮询为空之后，原生侧发布完成项，然后 QUERY 返回 `STOPPED`。只根据是否耗尽四批预算决定继续轮询，无法覆盖这一窗口。第二组确定性复现在 QUERY 宿主边界停住 worker，在空轮询之后、STOPPED 回复之前发布 129 项，旧实现同样提前清退。

这是关闭记账和合法 token 的错误分类；本轮没有证明因此发生内核资源提前物理释放。

## 修正与维护边界

生产修改仅在 `api/v2/session_lifecycle.h`：

- 保留每队列每轮四批、最多 128 项的轮询预算，用局部位图记录本轮已报损坏的队列。
- 在可信 `STOPPED` 回复之后、记账清退之前，持有原有独占 `DataGate`，通过既有 `PbQueueSnapshotProducer` 重新检查仍有 `Published` 且本轮未报损坏的完成环。该 helper 保留游标范围、前进和回绕校验。
- 存在可读完成项时，通过原有 `ProgressEvent` 继续下一轮，优先消费这些完成项，再考虑记账清退。检查和清退处在同一把锁的临界区内。未新增持久状态、锁、事件、定时器或协议字段。
- 已损坏的队列、无可读完成项的遗留记账仍可由可信 `STOPPED` 授权清退。损坏 producer 快照会记录真实故障。健康队列的积压不会因另一个队列损坏而提前清退；故障队列不靠反复读取攻击者可写的环来证明静止。
- 用户借用、原生引用、最终 `Lifetime` 独占和设备真实关闭屏障维持原有条件。共享提示仍不能授权物理回收。

新检查仅发生在关闭阶段、仍有发布记账且已经收到可信 STOPPED 的路径。包处理热路径未增加工作。保持既有预算和低频诊断等待；正常积压由事件立即续跑，测试不经过一秒诊断超时。本轮修复不宣称 CPU 降幅；真实 NDIS、IRQL 和 CPU 成本仍须实机内核运行验证。

## 回归证据

`tests/model/close_completion_budget_tests.c` 每进程覆盖 24 个场景：

- 127、128、129、256、510 项有效积压，普通游标与 `UINT64_MAX` 回绕。510 项接近当前布局可用上限；512 槽位加环和元数据超过 ABI 的 64 MiB 区域限制。
- 两队列积压，以及非法 token／损坏 producer 与另一健康积压队列共存。
- 空轮询后、STOPPED QUERY 回复前到达的完成项。
- 缺失和部分交付的原有异常回收，用户借用与宿主原生引用分别阻止 PREPARED／资源释放。

观察实际 Pop 调用，验证每轮每队列不超过四次／128 项，健康积压的续跑没有诊断超时；最终资源、运行时会话计数、保留字节及守护分配归零。旧实现有意复现提前清退及合法 token 的后续 `PB_CORRUPT`；当前实现消费健康完成环后才清退遗留记账。

`tools/audit/verify-dll-completion-budget.ps1` 使用固定 SHA-256 的修复前 lifecycle，与相同当前 session/core 组成 A/B，分别运行 ISO/MS volatile 和普通／守护内存共八组。生成的 session 只替换 include 解析和所选 lifecycle header。标准、守护和完整生命周期审计均接入新测试。

证据范围是实际 DLL/core、真实 Windows 锁／事件／私有线程池及宿主原生 I/O 边界。没有安装或加载 SYS，`kernel_runtime_tested=false`、`production_ready=false`。
