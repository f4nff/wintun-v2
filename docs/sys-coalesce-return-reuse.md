# 2.0.0：归还回调中的合并缓冲区复用

产品版本固定为 2.0.0，PE 文件版本固定为 2.0.0.0。
Data/API 2.1、packet 1.1、lifecycle 1、共享布局和导出函数表均不变。

## 确认的问题

起始指纹为 `7f54476eae84d9e89ee3eb1057c0672fe91221c859e437c19f38394b623f99f8`。
旧归还、构造和合并函数保存在 `out/sys-coalesce-reuse-baseline/7f54476eae84`。
旧 `session_return.h` 的 SHA-256 为
`3f78ac4500fff23c57e4d7b9dfd5f45279b455d3601839be2edbc083e4dda124`。

两个复制合并缓冲区均被已 indication 的 NBL 占用时，归还其中一个 NBL 会先
释放它的两个 MDL 和 NBL，然后进入诊断锁、RxLock 和完成发布阶段。原实现到
释放私有 record 时才清除 CoalesceBusy。因此已经没有 NBL/MDL 使用的复制
缓冲区，在这段回调中仍显示忙碌。

真实宿主线程执行生产归还函数，在 RxLock 释放后暂停回调；另一线程执行同一
队列的实际合并和 NBL 构造。旧实现下，第二个仍被持有的缓冲区保持忙碌，已归还
缓冲区也被延迟的标志挡住，新的两个合格报文只能构造两个普通 NBL。要求立即
复用的回归在旧函数上以退出码 1 失败，冻结旧函数的对照模式则明确核对这一退化。

这确认的是临时延长私有缓冲区占用导致的合并退化；没有据此宣称存在共享槽永久
泄漏、丢唤醒、历史业务丢包或确定的内核 CPU 百分比问题。

## 修正及所有权边界

`PbV2ReturnNbls` 在释放该 NBL 的全部 MDL 和 NBL 后立即清除其 CoalesceBusy，
早于诊断/队列锁和完成发布。后续只读取私有 record 中的描述符和创建时的元数据
快照，不再读取这个复制缓冲区。record 的原释放位置保持不变。

回调尾部不再清除 CoalesceBusy。接收 worker 此时可能已经取得同一缓冲区并将新
NBL 交付给上层；第二次清除会破坏新所有者的独占权。回归故意让新 NBL 跨越旧
回调尾部保持存活，检查标志仍为 1、复制内容不变，直到新 NBL 自己实际归还。

| 所有权/等待条件 | 本轮保留的依据 |
| --- | --- |
| 复制合并缓冲区 | 对应 NBL 已实际返回且全部 MDL/NBL 已释放后才可复用；仍被持有的另一个 NBL 的缓冲区不能复用 |
| 原始 RX 共享槽 | 仍由 RxLock 下的 Active/credit、完成发布及 DLL 的 token 校验推进；释放复制缓冲区不释放原始槽 |
| 满完成环 | 原始完成仍保留在 pending，credit 不因复制缓冲区空闲而减少；继续依赖消费者空间通知 |
| 正常/错误/本地取消统计 | 来源仍是原始描述符、NDIS 状态与 record 快照，不能从已被复用的缓冲区重新解析 |
| 故障后的 native 所有权 | 活跃槽、Active、AllReturned 和 Returns rundown 保持既有屏障；故障不授权新的接收 |
| worker 和 DLL 唤醒 | 事件、arm/recheck、full→usable 判断、暂停回收、预算及有限 DRAIN 均不变 |
| 映射和队列资源 | 最终仍须 Device Close、worker join、AllReturned、Returns rundown 后释放 |

生产变更只移动一次既有 InterlockedExchange，没有新增缓存、字段、分配、线程、
锁、事件或轮询。将复制缓冲区的释放与最后一次实际使用放在同一阶段，减少它对
后续诊断、记账和 record 析构阶段的耦合。

## 回归和成本证据

`return_coalesce_tests.c` 执行实际归还、完成、引用、合并与 NBL 构造函数，只有
DDI 是宿主替身。每个变体执行 44 个场景：IPv4/IPv6、TCP/UDP、成功/错误/本地
取消、满完成环和故障，以及在进入诊断锁前暂停的交错。

- 在 NBL 释放前仍必须保持缓冲区 busy，不能提前授权复用。
- 在 RxLock 释放后暂停时，旧原始槽已清退到既有完成路径；在诊断锁之前暂停时，
  原始槽仍 Active，验证复制缓冲区与原始共享槽的所有权没有混淆。
- 诊断交错将新报文换成不同 IP、协议和长度，先覆盖复制缓冲区再继续旧回调，
  核对旧诊断和字节统计仍来自原有快照。
- 保留另一个真实 NBL，检查其缓冲区、Active 和 AllReturned；新替代 NBL 跨越
  旧回调尾部继续持有缓冲区，最后逐个真实归还。
- 核对精确 token/完成顺序、状态、字节/包/错误/取消统计、锁次数、rundown、
  KernelEvent/UserEvent 通知次数、所有 slot/credit/pending 和资源/句柄平衡。

对于这一“两个缓冲区均忙、其中一个已经实际归还”的交错，两个新报文的构造数为：

| 宿主确定性操作计数 | 旧归还时点 | 当前归还时点 |
| --- | ---: | ---: |
| NBL | 2 | 1 |
| MDL | 4 | 2 |
| 私有 record 分配 | 2 | 1 |
| 上述构造 API 调用合计 | 8 | 4 |

这只适用于已启用合并且新报文合格的该交错。合并本身还会复制和校验 payload，
不能从对象数直接推导净 CPU、吞吐或延迟收益；关闭合并时的普通接收路径不因
这次修改增加合并工作。

```powershell
.\tools\audit\verify-sys-coalesce-reuse.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 脚本校验旧文件哈希，在普通/保护页和 ISO/MS volatile 四种配置下分别执行
冻结/当前函数，并单独记录旧函数要求复用时的预期失败。Tests/Safety 已纳入新
回归，完整离线入口纳入 A/B。最终验收冻结一个源码指纹，保留真实退出码、日志
和二进制哈希。没有安装、签名或加载 SYS；真实 IRQL、NDIS/LWF 调度、Verifier、
低资源、ETW/WPA 和长稳仍需实机验收。
