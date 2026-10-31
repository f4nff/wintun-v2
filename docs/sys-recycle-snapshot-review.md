# SYS 回收快照与复制方案复核

本轮基于完整验收指纹 `ee79fa4530d33d754f478fb1a8fa0efaca0289af601b998897cc3482901aa41e` 继续检查。版本保持 `2.0.0`，PE 数值版本为 `2.0.0.0`。本轮没有确认新的生产缺陷，生产实现保持本轮开始时的代码；改动限于回收边界回归及其审计说明。

## Consumer 发布之后的回收授权

TX_RECYCLE 消费者先将 token 复制到私有快照，然后发布 Consumer。生产者此时已经可以复用共享环项，而内核尚未开始逐项调用 `PbPoolTransition`。回收授权必须来自已复制的 token 与私有池校验，不能再次信任已经允许生产者复用的共享描述符。

现有 `token_queue_tests.c` 已覆盖核心复制顺序和立即复用。本轮在 `send_window_tests.c` 的实际 `PbV2RecycleLocked` 路径增加 14 组保护页用例，连同私有池和故障前缀一起核对：

- 八个已发布私有槽，先归还前四个；实际 pop 发布 Consumer 后、退休循环开始前，测试生产者立即填满八个共享环项，覆盖刚消费的全部四项。
- 第一批四个 token 仍必须从私有快照正确退休。新批次先包含一个有效 token，再依次注入错误 epoch、queue、越界 slot、generation、零 generation，或两种重复/已释放 token。
- 只允许前五个私有槽变为 FREE，后面三个仍须为 PUBLISHED；故障计数为一次、实际校验尝试为六次，消费游标反映两个已经快照的批次。
- 核对精确空闲位图 `0x1f`、未改变的 generation、有效前缀使满池缓存失效；错误后缀不授权后续工作。
- 普通游标和 UINT64_MAX 附近回绕各七种情况。共享环及私有槽数组都紧贴保护页，越界 slot 必须在解引用前被拒绝。

新增 `AfterRecyclePop` 只存在于宿主测试替身中：继续调用真实核心，只在实际 pop 返回后注入生产者复用。没有修改 SYS、DLL、核心 helper、槽位状态机或事件协议。原有回收预算、故障后发送拒绝、晚归还恢复、暂停 arm/recheck 及原生退出屏障继续由既有回归验证。

新用例接入 Tests/Safety 的现有 send-window 入口；`verify-sys-recycle-token.ps1` 当前实现也会运行它们。冻结历史实现继续只比较其原有退休/复制预算、token/游标校验及故障后接纳范围，审计 JSON 明确标明新增用例由当前实现执行。

## 未采用的批量复制实验

当前完整描述符发布按项复制并做环掩码。单独在 `out/queue-copy-experiment` 生成了按连续跨度 memcpy 的候选实现：1/2 项保留旧循环，较大批次在源/目标回绕处分段复制。候选没有写入生产源文件，也未作为通过正确性或 WDK 验证的实现交付。

实验复用相同当前 DLL、核心接口及宿主 benchmark，使用 MSVC `/O2 /volatile:iso /MT`，固定允许 CPU，预热 2000 次、计时 100000 次，五轮交替新旧次序。原始样本、候选/原核心源码、可执行文件及哈希保存在 `out/queue-copy-experiment`；取舍记录为 `decision.json`。

| 复制组合 | 批量 | 当前 ns/批 | 候选 ns/批 |
| --- | ---: | ---: | ---: |
| 连续描述符发布 + token 消费 | 32 | 48.40 | 39.75 |
| 环形 pending 源发布 + token 消费 | 4 | 21.46 | 28.94 |
| 环形 pending 源发布 + token 消费 | 8 | 25.38 | 30.70 |
| 环形 pending 源发布 + token 消费 | 32 | 49.33 | 48.25 |

SYS 完成发布使用环形 pending 源。其小批量显著回退，32 项收益很小；单一连续源的微基准改善不足以支持增加复制分支和维护成本。因此保留当前复制实现。这些数字包含队列发布及消费开销，不是纯 memcpy 时间，也不是内核 CPU、吞吐或延迟。

## 验收范围

```powershell
.\tools\audit\verify-sys-recycle-token.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

完整离线验收固定最终源码指纹，继续执行 SYS/DLL 构建、静态分析、实际宿主模型和现有成本/唤醒回归。生产实现未因微基准结果而改动。没有安装、签名或加载 SYS；真实 IRQL、NDIS/LWF、Verifier、ETW/WPA、低资源、内核 CPU 和长稳仍须实机验证，`kernel_runtime_tested`、`production_ready` 保持 false。
