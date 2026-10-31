# 2.0.0：DLL 槽位归还发布成本

版本继续固定为 2.0.0，PE 文件版本为 2.0.0.0。Data/API 2.1、packet 1.1、lifecycle 1、共享布局和导出函数表均保持原值。

## 确认的问题及修正

起始源码指纹为 `96ef1d64980b0df47e18c256eeb8bbafcdcb8d8bba40fcbd8e251b12223b5324`。
修改前的共享核心和 DLL 归还函数保存在 `out/dll-return-publish-baseline/96ef1d64980b`，
复现脚本校验三个基线文件的 SHA-256 后才执行对照。

`ReleaseBatch` 原来在验证整批 token 后，无论实际归还几个槽，都清零一个
32 × 64 字节的描述符数组，再将 token 写入数组，最后复制到 TX_RECYCLE。
SYS 只用 token 验证和回收私有槽。这是确定的重复构造和复制成本；本轮没有
把它归因为槽泄漏、丢唤醒或实际业务丢包。

共享核心增加内部 `PbQueuePushTokens`：先校验消费者游标和可用空间，再直接
构造实际接受前缀的完整描述符。每项的 token 以外仍全部为零，64 字节环步长不变，
不会把历史元数据留给旧的完整描述符读取者。DLL 不再构造 2 KiB 中间数组。
普通连续发布、私有 pending 环发布和 token 发布共用内联的空间校验与发布收尾，
避免维护三份游标/通知协议。

一个完整 32 项归还仍向共享环写入 2,048 字节。去掉的是 DLL 中间数组的
2,048 字节清零、token 写入及随后对该数组的读取；不能将其描述为共享环流量减少
75%。当前 x64 MSVC 发出的 token 循环为每项读取 16 字节 token，写入 token
及三个零向量，没有批量描述符栈数组。

对照生成的原生代码中，`ReleaseBatch` 的显式 `sub rsp` 预留由 2,152 字节变为
56 字节；新调用的 `PbQueuePushTokens` 另有 64 字节显式预留。这些是该编译配置的
函数级证据，不是整个 DLL 调用链或内核栈的节省量。

## 归还、回收与等待条件

核对两条方向的所有权链路：

| 环节 | 本轮保留的进度和释放依据 |
| --- | --- |
| DLL ReleaseBatch | 先完整验证 epoch、queue、slot、generation、状态及重复 token；发布接受前缀后才退休同样数量的本地 loan |
| DLL 故障归还 | 永久停止接纳后可只退休本地 loan；不依赖损坏的回收环，物理释放仍需真实原生关闭屏障 |
| SYS TX 回收 | TxLock 下每次最多四批、128 个 token；逐项验证后才将 PUBLISHED 变为 FREE，有效前缀之后的坏项立即 fault |
| 运行/暂停 worker | 清事件后执行实际回收，两条等待路径均 arm/recheck TX_RECYCLE；暂停不消费 RX_SUBMIT，没有周期轮询 |
| RX 归还 | 真实 NDIS 回调释放 Active 槽、生成 completion；信用满到可用的转换唤醒既有等待者，满完成环继续保留 pending/credit |
| 完成消费者 | 每次实际释放 completion 空间后仍通知 KernelEvent；消费前的未满快照不足以证明生产者不会随后阻塞 |
| 正常/故障排空 | 正常路径使用有限提交边界，故障使用各 worker 私有见证；未归还的 NBL、credit、Active 和 Returns rundown 仍阻止提前释放 |
| 最终清理 | Device Close、worker join、AllReturned、Returns rundown 之后才释放映射/MDL/事件等真实资源 |

`ReleaseBatch` 接受 token 只证明用户视图已归还并完成发布，不等于 SYS 已执行回收。
新发布函数与原函数一样，在所有条目写完后发布一次 Producer，再交换 Armed，
通知次数、事件类型、超时、预算和回调屏障均没有变化。

## 对照和成本证据

`verify-dll-return-publish.ps1` 分别执行冻结/当前核心和冻结/当前实际 ReleaseBatch，
其他 DLL 生命周期代码使用同一当前实现。它不把混合夹具称为完整历史 DLL。
每个变体在 `/volatile:iso` 和 `/volatile:ms` 下执行：

- 7,360 个逐字节对照：容量 1 至 4,096 的边界、部分接受、满环、游标回绕、
  Armed 状态、零元数据和未接受后缀；输入及目标后接保护页。
- 非法参数、错误方向及损坏/回退的消费者游标不能发布、覆盖条目或改变 Armed。
- 在 Producer 发布之前观察所有完整条目，发布之后立即消费并重新 arm；覆盖
  写入前、arm 后及等待前的归还，验证单次发布和通知。
- 容量 1 和 64 的真实宿主线程各传递 200,000 个 token，完整读取并验证全部
  64 字节描述符，使用真实事件等待并检查进程句柄平衡。
- 实际 API 和 lifecycle 回归继续覆盖前缀归还、剩余 loan、重复/旧/错误 owner
  token、损坏回收环后的本地归还、原生引用、异步关闭和晚到回调。

另以固定允许 CPU，七轮交替执行旧/新宿主基准，保存全部原始样本和中位数。
初次完整基准证据在 `out/dll-return-publish/f4e8b982d613/evidence.json`：

| 宿主每批成本，中位数 ns | 原实现 | 当前实现 |
| --- | ---: | ---: |
| 1 项生产及消费循环 | 23.76 | 17.83 |
| 32 项生产及消费循环 | 55.52 | 44.81 |
| 1 项 DLL 收取、归还及核心消费循环 | 102.89 | 95.80 |
| 32 项 DLL 收取、归还及核心消费循环 | 659.38 | 642.86 |

普通描述符及 pending 环的控制基准也保留，32 项分别为 46.08 → 45.85 ns、
46.78 → 47.34 ns。小幅差异不作普遍加速或退化结论。以上不是实际内核 CPU、
端到端延迟或吞吐结果；完整 DLL 循环还包含池镜像、锁和所有权校验。

```powershell
.\tools\audit\verify-dll-return-publish.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

普通 Tests 和 Safety 已纳入新生产者回归，完整离线入口纳入该 A/B。
原始静态分析日志还暴露了验收脚本问题：`analyze-dll.ps1` 的单项语言参数被
PowerShell 解包为字符串，原生 splat 将 `/std:c17` 展开为字符，编译器忽略这些
参数后仍返回成功。现明确声明字符串数组，并将 D9002/D9024/D9027 参数忽略
告警升级为验收失败。两个既有 DLL 数据流告警仍由基线对照和边界/生命周期
测试复核，不通过关闭告警获得通过；八个 SYS 所有权告警对照实际析构路径复核。
最终验收冻结一个源码指纹，保留实际退出码、测试日志、原生列表和二进制哈希。
未安装、签名或加载 SYS；真实 IRQL、NDIS/LWF 调度、Verifier、低资源、
ETW/WPA 和长稳仍需内核验收，`kernel_runtime_tested` 与 `production_ready` 继续为 false。
