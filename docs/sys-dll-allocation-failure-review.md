# 2.0.0：分配失败后的槽位归还与批量校验成本复核

产品版本保持 2.0.0，数字 PE 文件版本保持 2.0.0.0；协议、共享布局和导出表不变。
本轮未确认新的生产缺陷，没有修改 SYS/DLL 生产实现。新增可重复执行的组合测试，
并保留一次未采用的 CPU 优化候选对照，避免为了减少局部操作引入常见路径回退。

## 分配失败与真实持有者的组合

起始源码指纹为 `9fed4fbfb20c0c339883c3cd58798dd2bbd89a10ed64cbfcd3aad64ca6ca604d`。
此前单独的 NBL 构造和 SYS receive 测试已覆盖分配失败，本轮将它们与实际 DLL
Reserve/Commit/Poll、SYS Receive/Return/QUERY/Free 串联。用户会话由测试组装，
DDI 为宿主替身；这不是完整 DLL Open/Close 或实际内核低资源测试。

`tests/model/return_reclaim_tests.c` 新增 48 个场景：四个 NBL 构造分配失败点，
分别交叉普通包、四原始包 RSC4、四原始包 URO6、同步/手动复位 KernelEvent、
普通/跨 uint64 回绕的四条环游标。失败点依次为私有 record、数据 MDL、头 MDL、NBL。

每个场景先让队列 0 的真实 receive 路径构造并保留一个 NBL；队列 1 通过 DLL 提交
后注入一次分配失败。检查以下实际结果：

- 失败组没有 NBL、MDL、record、Active 引用或 Returns pin 残留；credit、pending
  和两个合并缓冲 busy 位归零。另一队列的 NBL、MDL、Active 和槽位仍然有效。
- DLL Poll 成功消费失败组的 PB_FULL 完成，逐项核对 epoch、queue、slot、generation、
  status；Loans/Published 归零，不能因为包分配失败而把整个会话判为损坏。
- completion 发布产生一次用户通知；DLL 消费完成实际设置 KernelEvent。区分内核
  KeSetEvent 调用计数和 DLL SetEvent 产生的可观察事件状态，不把两者混为同一计数。
- 测试把空闲候选 Next 置回 0，再用实际 Reserve/Commit/Receive 重用相同槽位的
  generation 2；普通/RSC/URO 构造均成功，证明失败组没有占住对应槽位或复制缓冲。
- 开始真实 native DRAIN 后，先归还恢复成功的队列 1 NBL。队列 0 的旧 NBL 仍使
  QUERY 返回 DRAINING、Active=1，AllReturned 保持复位。只有实际归还旧 NBL 后，
  才有一次 AllReturned 信号与 STOPPED。成功 NBL 共两次 rundown 获取/释放。
- 实际 Free 在线程中完成；跟踪 pool、MDL、NBL、事件对象、锁页区域及进程句柄
  最终平衡。该场景的回调已全部返回，不能替代此前并发回调尾部的 rundown 测试。

共用 `PrepareGeneration` 组装包和校验保留状态，避免在新旧场景中复制不同的包构造
逻辑。此前的 64 个回收、72 个同槽换代、24 个末代退休、32 个多队列场景继续执行；
每个当前进程合计 240 个场景。标准和保护页入口均自动覆盖新增场景。

`verify-sys-return-reclaim.ps1` 另生成两个只存在于 out 的错误 receive 变体：
删除 PB_FULL 完成发布，或删除失败后合并缓冲 busy 释放。分别在 ISO/MS volatile
和普通/保护页模式执行 `--allocation-only`，8 个进程必须以 1 退出，并命中对应的
credit/pending 或 CoalesceBusy 断言。它们没有改写生产文件。加上此前 24 个负向
控制和 4 个当前进程，本专项合计 36 个进程，预期拒绝 32 个。

## 完整位图候选未采用

当前大批量重复槽位校验使用 64 位筛选；低六位冲突时仍回扫先前 token 精确比较。
合法的稀疏批次可以接近原来的比较上限：32 个不同槽位按
`(i & 3) + (i >> 2) * 64` 排列时，每个校验函数有 490 次精确比较，上限为 496。
该工作有 PB_MAX_BATCH=32 的边界，不是新发现的无限循环、槽泄漏或漏唤醒。

本轮在 out 中生成一个完整 512 位局部集合候选；1–8 项保留精确回扫，大批次使用
集合直接判重。当前/候选各执行一次独立逐槽模型，覆盖 578,816 个矩阵用例、
32,736 个重复位置/错误顺序用例，以及输入/池后的保护页边界，均通过。
这些结果针对正常初始化的私有池，不能扩展为所有损坏私有状态下的等价性声明。

未插桩的实际 DLL 收取/归还循环固定同一允许 CPU，在九轮交替执行中测量。
以下为本机各九个样本的中位数，单位 ns/批次；原始数据、源码、可执行文件和原生
代码哈希见 `out/token-bitset-review/experiment/evidence.json`，实验生成脚本为
`out/token-bitset-review/experiment.ps1` 和 `assembly.ps1`。

| 槽位排列/批次 | 当前 | 完整位图候选 |
| --- | ---: | ---: |
| 连续，1 项 | 97.54 | 98.34 |
| 连续，8 项 | 201.84 | 202.00 |
| 连续，16 项 | 291.17 | 302.31 |
| 连续，32 项 | 508.71 | 513.51 |
| 跨 64 槽，32 项 | 695.72 | 505.30 |
| 交错跨 64 槽，32 项 | 727.33 | 512.98 |

候选改善了高冲突批次约 27%–29%，但连续 16/32 项分别出现约 3.8%/0.9% 的回退，
周期计数同样没有证明常见路径普遍获益。该编译器输出中，两个函数的显式 rsp
减量各从 0 增至 80 字节，并各增加四次 16 字节向量清零；这不是完整调用链栈占用。
没有实际业务槽位分布和 native CPU profile，无法证明值得承担这项维护及常见路径
代价，因此候选不进入生产。本轮不声称降低了内核 CPU、端到端延迟或吞吐开销。

## 复核与验收范围

```powershell
.\tools\audit\verify-sys-return-reclaim.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

冻结指纹后执行完整离线入口，复核 104 个生产文件（含版本属性）与起始版本逐文件
一致，并比较 DLL/SYS 项目静态诊断集合，不能只用总数声称没有新问题。
完整结果及人工复核记录随输出指纹保存在 out 中。

槽位退休仍由实际 owner/token、完成消费和 native 屏障决定。保持正常 DRAIN 的
有限 producer 边界、故障队列的私有 quiescence witness、AllReturned 后的 Returns
rundown，以及物理 Device Close 成功后的 DLL 资源释放顺序。本轮未改变其锁、
唤醒、故障退休规则或每轮预算。

没有安装、签名或加载 SYS。真实 NDIS/IRQL、Verifier、低资源、长稳及 ETW/WPA CPU
验收仍待实机执行；宿主分配失败注入与事件调用计数不代表这些结果。
