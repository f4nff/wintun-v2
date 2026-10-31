# SYS 空接收与合并开销

后续校验解析复用与故障接收撤销见 [SYS 校验解析复用与故障接收撤销](sys-parse-cancel-cost.md)。

本轮从 `1e9c5350e158ac99b9801625034ed29dc2756afda9d8416a95fc63e810560202` 开始，原源码快照保存在 `out/sys-empty-coalesce-baseline/1e9c5350e158`。范围是当前 x64 PacketBridgeV2 SYS 的实际接收函数、合并编排和 worker 等待。没有安装、签名或加载 SYS。

## 1 空 RX 仍执行完整后处理

TX recycle 唤醒、空队列检查等情况下，ReceiveBatch 已经刷新现有 completion，并确认没有 RX 描述符，仍继续取得一次空的接纳事务锁、读取 offload 配置、再取得锁刷新一次 completion。单次空接收的 RxLock 获取为三次，offload 配置读取为一次。

现在在首次刷新、弹出、额度处理并释放 RxLock 后，若 count 为零，就释放 TransitionLock 返回。没有构造 NBL、登记新引用或生成新完成项的路径不再进入后处理。空接收的队列锁获取降到一次，offload 配置读取为零。

首次 completion 刷新的预算和继续执行规则仍然有效：

- 预算耗尽且还有 pending 时，刷新函数已设置 KernelEvent。worker 在下一轮继续，不依赖第二次机会性刷新。
- 完成环已满时不自唤醒；消费者释放空间并通知后继续。
- 真实 NDIS 归还、提交 ring 的 arm/recheck、暂停/重启、DRAIN 边界及故障唤醒继续通过原有谓词决定进展。

新增实际生产函数回归用 257 条 pending 检查预算续跑。当前首次空接收发布 128 条，剩余 129 条经 worker 继续处理，最终 pending/credit 为零。满 512 条的完成环中另有 32 条私有 pending 时，测试确认 worker 在消费者动作之前没有自唤醒，消费者一次释放 128 个位置并通知后，32 条 pending 全部发布。测试等待替身先消费真实宿主事件，只有无信号时才模拟停车，避免把已存在的续跑通知误当作睡眠。

## 2 不支持当前包类型的合并尝试

原路径先执行 CoalesceBegin，再判断其 IP 家族/协议是否被当前策略启用。例如仅启用 IPv4 TCP RSC，32 个合法的 IPv4 UDP 包仍执行 31 次 Begin；最后一个包没有后继，不尝试合并。每次成功 Begin 都验证校验和并复制到合并输出，随后才因 URO 未启用而退回普通 NBL。

修正把合并编排集中到 `session_coalesce.h`。先按原路径复制完整包到内核私有 scratch，再从这份稳定副本读取最小 IP/协议字段，检查对应 RSC4/RSC6/URO 位。未启用的类型不进入 Begin。类型允许时仍调用原有完整长度、eligibility、校验和验证，并保留 Begin 后的策略检查；预筛选本身不授权任何 NDIS 元数据。默认所有合并功能关闭时，接收调用方直接保留普通包路径。helper 强制内联，保留原有调用层次，避免拆分源码给未合并候选或输出缓冲占用路径增加函数调用；编译结果另以 PDB 函数符号核对。

确定性的宿主计数如下，单位为字节或调用次数，不是内核 CPU 时间：

| 32 个输入包，长度 1500，仅启用 RSC4 | 原 Begin 调用 | 当前 Begin 调用 | 原 eligibility 校验字节 | 当前 | 原合并输出复制 | 当前 |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| IPv4 UDP | 31 | 0 | 46748 | 0 | 46500 | 0 |
| IPv6 TCP | 31 | 0 | 46252 | 0 | 46500 | 0 |

这两个用例的 scratch 快照复制仍各为 46500 字节，普通 NBL 数仍为 32。没有省略现有用户输入快照，也没有改变普通接收对象的分配、校验和元数据或所有权。

## 3 回归与证据

`coalesce_path_tests.c` 直接执行生产编排和实际共享合并核心，scratch 与两个合并输出使用保护页分配。它统计 Begin/Append、快照/输出复制字节，以及 eligibility 的校验字节；finalization 的校验不混入该计数。

覆盖 IPv4/IPv6 TCP RSC、两种 UDP URO、64 字节小包、功能全关、两个输出都由 NDIS 持有、错误校验和，以及连续八组不同流。启用的稳定输入维持相同分组、payload、长度、校验状态与 RSC abort 数；错误校验和仍不能产生合并输出。拆分后的相对描述符索引也由多组流覆盖。

`drain_boundary_tests.c` 在原有正常空闲 arm、原始 ABI 持续追加、回绕、暂停、多队列、快照损坏、额度归还时序之外，增加上述空接收操作计数、预算续跑和满完成环消费者唤醒。Safety 继续执行其保护页版本。内存、句柄、线程 join、Active/AllReturned 和 rundown 释放规则未被重写。

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\build\build.ps1 -Stage Safety
.\tools\audit\verify-sys-receive-cost.ps1 -BaselineDirectory out\sys-empty-coalesce-baseline\1e9c5350e158
.\tools\audit\validate-dll-lifecycle.ps1
```

新的 A/B 入口用冻结的旧接收函数和旧合并编排，与当前函数运行同一宿主替身和共享核心。旧编排的抽取只将外层描述符索引改成相对索引并补充局部会话指针，不改变算法。结果、实际退出码、二进制和来源哈希写入 `out/sys-receive-cost/<当前指纹前12位>`。完整离线验证增加 SysReceiveCost，仍在同一冻结指纹运行；失败或源文件变化不能记为通过。

本轮没有确认新的内部死锁。预算续跑、满环唤醒、原有故障/事件时序及退出屏障均纳入回归；外部 NDIS 归还或上调永久停滞仍可能保留必要等待。实际 IRQL、Verifier、PoolMon、ETW/WPA、低资源及长稳尚待内核环境验收。上述计数不能换算成真实驱动的 CPU 百分比、延迟或吞吐，清单继续保留 `kernel_runtime_tested=false` 与 `production_ready=false`。
