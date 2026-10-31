# 停止后的 TX 归还边界复核

本轮从完整验收指纹 `28074c4f8627c0f5c38cb43cd4c399341682a0507a181fab2f852cb46632a0e2` 继续检查 SYS/DLL 槽位归还、内核回收、唤醒和 CPU 成本。产品版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。

本轮没有确认新的生产缺陷。生产 SYS、DLL 和公共核心保持原实现；改动限于实际路径的边界回归、审计记录和本说明。上一轮已经修正的完成环空间通知与停止后满回收环本地归还，见 [DLL 关闭归还与通知说明](dll-close-progress-wake-cost.md)。

## 归还的整批授权与部分发布

扩展现有 `close_wake_tests.c`，直接执行 DLL `ReleaseBatch`、生命周期工作线程、公共环核心和 SYS `PbV2RecycleLocked`。没有增加生产测试钩子或状态。

| 维度 | 取值 |
| --- | --- |
| 停止来源 | 普通 BeginStop、宿主原生终止提示且 DLL Fatal 仍为零 |
| 回收环剩余空间 | 零、两项；四个合法用户 loan |
| 游标 | 零、UINT64_MAX−3 附近回绕 |

每个进程执行八种组合。原始 ABI 垃圾项用于构造不合作的回收环；在正常协议下，容量八、仅四个未归还 loan 不会自行产生这种满环。

每种组合先执行九个无效批次：尾部错误 epoch、generation、slot、queue、零 generation、重复 token，以及零长度、超过 PB_MAX_BATCH 和空指针。四个实际 token 紧邻保护页；非法 count 必须在读取越界输入之前被拒绝。72 个无效批次都要求 accepted=0，整个 DLL 池、槽状态、共享环内容、Producer/Consumer/Armed 及私有游标逐字节不变，loan 数和 Fatal 不变。另有真实线程调用同一队列，必须因 owner 不符返回 PB_BUSY；线程被 join 并关闭句柄。

有两项空间时，实际发布只接受前两个 token：仅前两个 DLL view 退休，尾部两个 loan 仍保留。下一次遇满环，当前实现本地退休剩余 view；旧实现必须复现 PB_FULL 的预期停滞，随后仅为测试清理显式触发其原有 Fatal 捷径。满环本地归还不能移动 Producer、改写回收项或消费 Armed；有空间的真实发布仍按 Armed 通知。这里将 Armed 置一只控制通知谓词，不替代现有真实 arm/recheck 并发回归。

测试丢弃预先注入的原始垃圾项后，使用实际 SYS recycler 处理真实发布前缀，核对精确空闲位图和 TxPoolExhausted：只有经过验证的前缀退休才使满池缓存失效，本地归还尾部的原生槽仍是 PUBLISHED。重复归还必须失败且不改变池或 loan，当前实现也不生成无进展通知。

上述矩阵保留独立的宿主原生 acknowledgement；即使所有 DLL loan 已清退，PrepareClose 仍不能释放资源。该矩阵没有将 fake acknowledgement 当作真实 SYS QUERY；既有跨层 RX 用例另外接入实际 SYS NBL/Flush/DRAIN/QUERY，继续验证真实 pending/credit 归零才能确认 STOPPED。

## Consumer 发布的隔离实验

原 Consumer 游标使用完整屏障的原子交换。仅在忽略目录 `out/consumer-release-experiment` 生成 native x64 候选：将 Consumer 发布替换成编译器屏障包围的对齐 store，Producer 和 Armed 保持原样，其他架构保留原交换。候选没有写入生产源码。

原实现和候选配合同一当前 DLL、宿主 peer 及现有 benchmark，以 MSVC `/O2 /volatile:iso`、固定允许 CPU、2,000 次预热、100,000 次计时，七轮交替执行。下表是顺序槽位的每批中位数，单位 ns；不是纯游标写入时间。

| 实际宿主循环 | 批量 | 当前 | 候选 |
| --- | ---: | ---: | ---: |
| DLL receive/release | 1 | 91.71 | 83.66 |
| DLL receive/release | 8 | 188.18 | 177.72 |
| DLL receive/release | 32 | 459.31 | 445.70 |
| DLL reserve/commit/completion | 1 | 228.42 | 219.34 |
| DLL reserve/commit/completion | 8 | 394.05 | 379.55 |
| DLL reserve/commit/completion | 32 | 920.73 | 905.16 |

测量保存在 `measurements.json`，原始日志、候选/原核心和可执行文件均保留，`decision.json` 记录取舍及哈希。候选仅执行计时循环的内容一致性检查，没有完成队列正确性、WDK、驱动或真实内核验收。

这些宿主循环存在数纳秒收益和样本波动。修改会扩大全部 Pop 路径的内存发布证明范围；现有确定性发布/立即复用观察器还直接拦截完整屏障原语，迁移时必须保留这些证明。当前证据不足以支持这项维护成本，因此保留生产 Consumer 交换和既有握手，不将候选纳入发行构建。

对齐 64 位读写的原子性本身不保证跨线程同步；Windows 同步操作的排序要求还包括内存屏障。Microsoft 也明确区分编译器重排约束，并将 `_ReadWriteBarrier` 标为 deprecated。继续沿用当前已验证实现，有助于保持本轮修改范围可审查。[Microsoft：Interlocked 访问](https://learn.microsoft.com/en-us/windows/win32/sync/interlocked-variable-access)、[Microsoft：多处理器同步](https://learn.microsoft.com/en-us/windows/win32/sync/synchronization-and-multiprocessor-issues)、[Microsoft：编译器屏障](https://learn.microsoft.com/en-us/cpp/intrinsics/readwritebarrier?view=msvc-170)。

## 验收

```powershell
.\tools\audit\verify-dll-close-wake.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

扩展场景沿现有入口同时进入 Tests、Safety 和 ISO/MS 普通/保护页新旧对照，完整验收继续绑定最终同一份源码指纹。版本和全部 ABI 不变。未安装、签名或加载 SYS；真实内核 CPU、NDIS/IRQL、Verifier、ETW/WPA、低资源和长稳仍需实机验证，`kernel_runtime_tested`、`production_ready` 保持 false。
