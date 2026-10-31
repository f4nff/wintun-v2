# 2.0.0：完成消费中的槽位退休

产品版本保持 2.0.0，PE 文件版本保持 2.0.0.0。
Data/API 2.1、packet 1.1、lifecycle 1、共享布局和导出函数表均不变。

后续组合回归修正了损坏完成的空间通知：只要 Pop 已发布 Consumer，token 校验失败也必须唤醒生产者；私有池仍零部分退休。见 [DLL 关闭与损坏完成唤醒](dll-close-progress-wake-cost.md)。以下成本快照保留原始测量范围。

## 确认的问题及修正

起始指纹为 `40884c0cb15589575c5d4e960d7a886f67316c590dfb39c54f8bd745b5408a46`。
旧队列、会话和核心保存在 `out/dll-completion-retire-baseline/40884c0cb155`。
旧 `api/v2/session_queue.h` 的 SHA-256 为
`de2da8de2fcdac9f6c613a3d8a49391100fb66aa06415e31a21e1c7ed260c3cf`。

`PollInternal` 在取出完成项并完整验证 token 批次后，每项依次调用：

```text
PbPoolTransition(PUBLISHED, COMPLETE)
PbPoolTransition(COMPLETE, FREE)
```

每次转换都会重新检查 epoch、queue、slot、generation 和当前状态。DLL 的有限
队列操作由 owner 和 DataGate 串行保护，生命周期消费完成时持有排他的 DataGate。
两个转换之间没有 I/O、事件发布或外部回调；合法观察者无法看到 COMPLETE 中间态。

现在使用核心已有的、带完整校验和空闲索引维护的 `PUBLISHED → FREE` 转换。
整批 token 仍在任何槽位修改之前验证，没有增加未经校验的释放捷径，也没有
复制一份位图维护逻辑。共用核心的 COMPLETE 常量和其他状态转换保留。

生产变更只简化完成退休循环；32 项完成的转换调用从 64 次降为 32 次。若按源码
调用链计入整批验证，每项原来验证 token 三次，现在两次。这个计数不是所有
指令、总 CPU 或整个完成路径成本减半。

## 归还、回收与唤醒边界

| 环节 | 保留的依据 |
| --- | --- |
| 完成项获取 | 先快照整个接受批次，再发布共享 Consumer；后续生产者复用 ring 不得改写本地快照 |
| token 整批验证 | 旧/错 epoch、错 queue/slot/generation、重复和错误状态仍在任何退休之前拒绝 |
| 私有槽退休 | 每项仍使用检查后的核心转换更新 State 和 FreeBits；代次不在这里增加或回绕 |
| 完成状态 | PB_RX_REJECTED 的旧 wire 值 4 仍是包结果，不能按 API PB_CORRUPT 对待；完整描述符原样交付 |
| 部分消费 | 只退休实际消费的前缀、减少相同数量 Published；未完成的槽仍不能预留 |
| 完成空间通知 | 成功消费非空批次后仍设置 KernelEvent，空轮询不通知；不依赖消费前的“环未满”快照删减通知 |
| DLL 故障收尾 | 无效完成不产生部分退休；用户 loan 和真实 QUERY/原生引用屏障继续限制关闭 |
| SYS 回收和最终释放 | 原有 recycle 预算、暂停 arm/recheck、full→usable 唤醒、有限 DRAIN、worker join、AllReturned 和 Returns rundown 均保留 |

PUBLISHED 的普通退休仍需真实完成项。生命周期在可信 STOPPED 后清退遗留 Published
是另一个已有路径，本轮没有改动。共享 terminal 位、期限或 DLL 私有状态仍不能
替代真实原生关闭屏障。

## 实际 API 回归和资源检查

`completion_retire_tests.c` 直接执行生产队列/生命周期函数及真实核心，只把 native
接收方作为宿主 peer/引用 stand-in，不运行 SYS。它记录实际槽位转换并透传真实
Windows 事件调用，分别核对冻结/当前实现：

- 1、2、4、8、16、32 项，单项/整批消费，逆序完成和 uint64 游标回绕；输出后接
  保护页，逐字节校对全部完成字段与成功/拒绝/取消状态。
- 每个已消费前缀的代次与空闲索引、立即复用及取消；未完成槽继续 Published。
  空轮询不得调用转换或通知内核，成功非空轮询恰有一次空间通知。
- 2、8、32 项的首/中/尾错误：epoch、generation、重复、越界 slot 和仍为 RESERVED
  的合法 loan。无效完成必须零部分转换并保持全部私有状态/位图。原快照测试要求零空间通知；后续已确认该要求会滞留原生 pending，当前回归要求一次空间通知，冻结旧实现仍按原行为核对。
- 非法完成后保留一个用户 loan 和 native 引用，分别验证取消/真实停止见证之前
  不能完成关闭；受信 STOPPED 后才由既有收尾路径清退 Published。
- 在第一个槽已转换、Published 计数尚未更新时并发 PrepareClose，检查 DataGate
  不能放行部分退休状态，也不能提前释放映射。
- 错误 owner 不得消费或改槽；UINT32_MAX 代次完成后，下次预留永久退役该槽，
  不回绕到零。

保护页模式覆盖会话、镜像槽数组和共享区域。会话自己创建/关闭的 device/event
句柄、work/pool 对象、准入及保护页分配逐项配对；线程和测试事件也实际关闭。
Windows 线程池的后台清退会影响进程总句柄瞬时值，因此不把它当作本模块的所有权
计数。夹具额外持有 shell 引用以安全 join 回调后销毁测试对象；实际 mapping/native
关闭屏障照常执行，完整 DLL 卸载回归仍独立覆盖生产中的异步回调尾部。

## 成本对照与复现

`verify-dll-completion-retire.ps1` 校验旧文件哈希，使用相同当前核心和生命周期，
在普通/保护页、ISO/MS volatile 四种配置运行冻结/当前队列、API 和 lifecycle。
原生代码列表也核对 PollInternal 循环内的转换 call site 从两个变为一个。

未插桩宿主基准执行完整的 Reserve → Commit → peer 消费/完成发布 → PollCompletions，
保留真实 Windows 事件调用。六种批量长度、顺序/逆序完成，固定允许 CPU，七轮
交替测量。初次 A/B 证据为 `out/dll-completion-retire/1a521302a353/evidence.json`：

| 完成顺序/批次 | 旧中位数 ns | 当前 ns |
| --- | ---: | ---: |
| 顺序，1 项 | 243.40 | 242.53 |
| 顺序，8 项 | 413.95 | 407.40 |
| 顺序，32 项 | 1001.35 | 932.76 |
| 逆序，1 项 | 242.40 | 246.90 |
| 逆序，8 项 | 410.06 | 395.36 |
| 逆序，32 项 | 1009.37 | 947.94 |

保留全部原始样本、线程周期、源码/二进制哈希和真实退出码。小批次波动不作统一
加速结论；这些结果不含 driver I/O、NDIS/LWF 调度和业务 payload 工作，不能
推广为实际内核 CPU、端到端延迟或吞吐结果。

```powershell
.\tools\audit\verify-dll-completion-retire.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

Tests/Safety 已纳入新夹具，完整离线入口纳入 A/B。最终验收冻结一个源码指纹。
未安装、签名或加载 SYS；真实 IRQL、NDIS、Verifier、低资源、ETW/WPA 和长稳
仍需实机验收。
