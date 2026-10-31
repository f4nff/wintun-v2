# SYS 第四轮并发、资源与 CPU 审查

日期：2026-10-29。范围为实际构建的 PacketBridgeV2 x64 SYS，重点是 `driver/v2`、`driver/wintun.c`、链接的 `common` 及必要的 API 退出关系。版本保持 1.0.1，版权截至 2026；没有修改实际 Rust VPN 项目，没有安装、加载或签名驱动。

本轮新确认并修正一个暂停状态下的 completion 进度缺口，实施两项范围较小的 CPU 改动。没有新确认的内存破坏型竞态、内存泄漏、句柄泄漏或永久线程泄漏。SYS 不含 Rust/Tokio 协程，本轮检查的是系统线程和回调生命周期。离线结果不能证明真实内核环境中不存在其他问题。

## 新确认的进度缺口

`session_worker.h` 原先在适配器暂停且未排空时直接等待 StopEvent/KernelEvent/RunningEvent，不执行 completion 刷新。若已有完成项积压、用户消费完成环并通过 KernelEvent 告知空位，worker 虽被唤醒，仍会再次进入暂停等待，pending/credit 不前进。该状态可持续到 Restart 或 Drain；它是完成通知停滞，不能称为已证明的永久内存或线程泄漏。

修正：在暂停分支睡眠前，短暂持有 RxLock 调用同一 completion 刷新函数。只推进已完成项，不接纳新包，不向 NDIS indication，不增加 NBL/MDL 或线程。

新增回归直接执行实际 Worker、实际 completion helper 和共享队列核心。构造过程为：

1. 通过原始 ABI 的接纳规则生成四条非法 epoch 的错误完成项，填满 completion 环。
2. 接纳一个有效对象，在其完成时因环满留下一个 pending。
3. 适配器保持暂停，消费者取走已有四条完成项、arm 通知并唤醒 worker。
4. 检查有效对象的完成项能够送达，pending/credit 归零，且普通接收处理未执行。

相同测试使用保存的旧 Worker 头文件编译后失败，当前版本通过。标准 C API 会提前拒绝上述非法提交；本轮覆盖的是原始 ABI 异常提交形成背压后的进度，**没有在普通 C API 真实流量中复现**。这不是已确认的内存破坏型竞态，也没有依据把它升级为通用暂停死锁。

旧源快照保存在 `out/audit4-baseline/3824cedc372b`，关联上一轮指纹 `3824cedc372b1f6a70fb7f4178affe293f79f595f0ec62fc822ebadabaf06f4a`。旧 Worker SHA256 为 `14ded794944eb5337c2d4c3e11bed423527b7b031b9ea50d1f54f7e35d30df39`。`out/audit4-repro/paused-baseline.exe --paused` 和日志保留该失败；测试使用当前完成逻辑搭配旧 Worker，以隔离暂停分支的影响。没有调用真实 NDIS。

## completion 刷新预算与 CPU 开销

此前 PbV2FlushLocked 在一次 RxLock 持有期间循环，直到全部 pending 发布或环满；pending 上限为 ring capacity，最多 4096 项。消费者并发释放空间时，一次持锁可能处理较多描述符。这是持锁工作量偏大的候选，不是无限循环：原代码受私有 pending/credit 上限约束。

现在每次最多四个批次，每批最多 32 项，即最多 128 项：

- 只有预算耗尽且仍有 pending、会话未故障时，设置 KernelEvent 安排后续处理。
- 因环满停止时，等待消费者释放空间的通知；不持续自唤醒。
- 每轮累计 Completions 后只做一次原子加，合并 UserEvent 通知；满四批时，该统计从四次原子加减为一次。
- 暂停分支也执行相同预算，预算后的自唤醒可以继续处理剩余项，不依赖重新开始网络收包。
- 原来的 token 所有权、credit 释放时机、NDIS 归还、Active/AllReturned 和 rundown 保护不变。这里没有提前重用 NDIS 仍持有的内存。

`completion_progress_tests.c` 的六个场景执行生产 helper：正常与回绕各 4096 项，每轮最多 128 项，各 32 轮完成且内容顺序不变；满环不自唤醒、消费后继续；损坏共享计数进入一次性故障；上述暂停背压；暂停时 384 项跨三个预算自动完成。正常/回绕合计 8192 个描述符，并检查 pending/credit、统计与通知。

这些测试使用确定性的宿主 DDI 替身，不模拟真实 IRQL 和调度时间。预算限制的是一次 helper 的工作量，不是整个回调或线程总执行时间；新增调度也有成本。不能因此宣称吞吐一定提升或 DPC 延迟已达到某个数值。

## 单队列选择优化与微基准

PbSelectQueue 原先即使只有一个队列，也计算 IPv4/IPv6 地址哈希再对 1 取模。现在单队列直接返回 0。队列数 0、非法参数仍返回 0，2—8 队列保留既有算法与顺序；没有改成新的五元组哈希。

`tools/audit/bench-flow.ps1` 分别链接保存的旧 flow.c 与当前 flow.c，使用相同测试源、MSVC x64 /O2 /MT /W4 /WX、独立翻译单元、不启用 LTO。每项 200 万次、五轮交替顺序，线程固定到进程允许的最低编号 CPU，记录 QPC 与线程周期中位数，并检查前后队列选择结果一致。

一次结果如下；原始逐轮样本位于 `out/audit4-bench/e5aa9e5f82b5/benchmark-results.json`：

| 输入 / 队列数 | 前 ns/调用 | 后 ns/调用 | 解释 |
| --- | ---: | ---: | --- |
| IPv4 / 1 | 3.468 | 0.608 | 辅助函数跳过无意义哈希 |
| IPv6 / 1 | 14.201 | 0.533 | 同上，原先需遍历更多地址字节 |
| IPv4 / 4 | 3.432 | 3.552 | 本次约慢 3.5%，无改善依据 |
| IPv6 / 4 | 14.140 | 14.261 | 本次约慢 0.9%，线程周期 51.310→51.161，存在测量噪声 |

旧 flow.c SHA256 为 `9ac1e38385acfb02758e38efe0f75208102117feb77c410e9cc7dd496fe771f8`，当前为 `720d6b33c8aedaf77a4cef612948d4fbf0415c655dcf65b530f35167ff6fcb0e`。这是用户态辅助函数微基准；驱动的 /kernel/LTO、CPU 频率与其他工作会影响结果，不能换算成驱动 CPU 或 VPN 吞吐提升。

## 资源、锁与退出再次核对

| 核对项 | 本轮结果 |
| --- | --- |
| 用户可变共享区域 | 内核仍以私有 Layout/Cursor/slot 状态决定范围；先快照描述符再验证，所有数据偏移均受队列、slot、长度及 region 上限约束；本轮未发现新的越界路径 |
| Ctx 与控制请求 | 先在短全局锁内取得 ControlRundown 引用，Halt 撤销入口后在锁外等待；进程通知按引用保护对象，并在 RegistrationLock 内复查 owner |
| 未发布接收链 | Worker 和 TransitionLock 保护构造阶段；批量登记引用发生在发布或本地取消前。Pause 的排他转换、注销的 detach/join 避免释放半构造对象 |
| NDIS 归还与会话清理 | 先回收实际归还对象并发布唯一完成，再递减 Active；最后返回尾部仍在 Returns rundown 内。停止并 join 线程、等 AllReturned 和 rundown 后才解锁父 MDL/释放映射与事件引用 |
| 锁顺序 | 新增暂停路径只短暂取得 RxLock，不在锁内等待线程或 NDIS；不会把 TransitionLock/队列锁跨越网络上调。刷新沿用现有队列保护和一次性故障通知 |
| 句柄与线程 | 没有新增系统线程、驱动句柄或事件引用。既有失败清理和注销仍按取得次数释放，先 join 再 ZwClose |

76 个会话获取前缀、48 个逐 NBL 资源场景、十万轮并发引用、上下文/indication gate、故障事件时序、查询与 detach、C API/Rust 原生 FFI/布局/编译失败及百万并发描述符测试继续通过。宿主追踪的资源、对象引用和句柄差值归零，不等于真实内核 PoolMon 结果。

重新分析 7 个内核翻译单元，仍为 57 条诊断：49 条在 SDK/WDK，项目 8 条 C28197，仍是会话、五类所属队列缓冲区、Ctx、NBL pool 的成功移交路径。已再次核对 owner 与清理出口，保留原始 SARIF/日志；没有新增其他项目诊断，也不称为零告警。

仍保留的限制：NDIS/LWF 永不归还或回调永不返回时，安全清理必须继续等待；用户态故障隔离可能有意保留未能证明可回收的资源。SYS 没有协程，不能用本轮 SYS 结果替代整个 Rust VPN 的异步任务审计。

## 后续 CPU 候选与验证边界

仍需 ETW/WPA 采样后决定的候选：逐 NBL 的 record/两个 MDL/NBL 获取释放、按队列归并归还与唤醒、RSC/URO 私有复制和 checksum、全局引用/indication/统计缓存行竞争。当前地址哈希把同一 IP 地址对的不同端口流放入同队列，可能限制横向扩展；进一步改变分流还必须处理分片和流顺序，不在本轮直接重写。共享 ring 的强原子发布和 Armed 握手仍保留，不能为了减少指令而省略其同步语义。

复现（PowerShell 7）：

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\audit\analyze-sys.ps1
.\tools\audit\bench-flow.ps1
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
```

源码和文档变化生成独立指纹目录，使用同一指纹的 SYS/DLL/probe，不混用旧包。开发 DLL 不内嵌驱动，最终嵌入包仍待以后完成签名再生成。本轮保持未签名。

尚未执行真实内核 IRQL、Verifier、PoolMon、ETW/WPA、长稳、低资源、NDIS pause/restart/Halt、动态 OID、进程强退和过滤驱动组合测试。因此不能保证已排除所有竞态/泄漏，也没有驱动吞吐验收结论。

契约参考：[NDIS 暂停期间的操作要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_pause)、[队列自旋锁与 IRQL](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-keacquireinstackqueuedspinlock)。项目缺口与预算选择来自本地源码和回归测试；微软文档没有规定本项目应采用 128 项预算。
