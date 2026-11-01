# Windows 用户态／内核交付参数对照

测量日期：2026-11-01（Asia/Singapore）。以下保留默认切换前的基准数据及证据；测量时生产批宽为 32、共享区上限为 64 MiB。

切换后的独立复测见 [新旧默认组合吞吐复测](default-throughput-comparison.md)：重新交替执行新旧参数各九轮，完整保留原始样本与波动范围。

后续已将项目默认切换为 **4 队列、每队列每方向 256 个 slot、ring 512、批宽上限 64，HostRx/HostTx 字节预算 512/128 KiB，共享区上限 256 MiB**。C 的 `PB_CONFIG_INIT` 与 Rust `Config::default()` 同步；接收 worker 的批次快照移入队列私有内存，避免 64 项数组扩大内核接收栈。下面的 127-slot 组合是切换前、旧 64 MiB 上限内的候选，表格速率属于历史宿主基准，不是新默认下真实 SYS/NDIS 吞吐的实测。

切换后完整 Tests（53 个执行进程）、Safety（45 个执行进程）及 SYS/DLL 构建均通过。Rust 已验证 64 项收取、提交、逆序完成回收和 65 项拒绝；实际 SYS 接收函数的宿主测试覆盖 64 小包／16 个 32-KiB 对象的单次预算、快照投毒及 NDIS 独立副本归还。静态分析保留的 8 条 C28197 所有权告警与既有分析记录一致，无新增项目告警。详见 [默认切换验证记录](throughput-defaults-validation.json)。

## 测量口径

- Intel Core Ultra 9 285H，16 个可用逻辑处理器；Windows 11 Pro，10.0.26300。每队列一条用户线程和一条宿主对端线程，固定到不同逻辑处理器；四队列使用八条线程。
- DLL `session_queue.h` 函数体及共享 `v2_core.c` 静态编入宿主，保留真实 SRW 锁、自动复位 Windows 事件和 owner/lease/token 检查。不是加载成品 DLL/SYS 的测量，编译器可以执行宿主内联优化。
- HostTx = Windows 协议栈方向的宿主生产者 → 用户态；HostRx = 用户态 → 代表 Windows 协议栈方向的宿主消费者。两方向分别测量，不是全双工或多跳往返吞吐。
- 对端立即完成／归还；HostRx 使用实际 `PbAdmitReceive` 和带字节预算的出环；HostTx 逐项 `PbPoolTransition` 回收，与实际 `session_recycle.h` 一样不额外做整批 token 校验。
- 数据区预触页。每个对象仅写／读首尾合计 16 字节的序号标记；64/1500/32768 是描述符长度场景，不是完整 IP 包解析、完整载荷复制或真实 GSO/RSC 请求。未运行加密、NDIS 指示/归还、真实内核 worker、MDL 或链路。
- 每项预热 8192 个对象；最终每配置 7 轮、每轮 4194304 个对象，轮次间正反次序交替。表中单位为 **百万描述符对象/秒**，方括号为七轮的四分位范围（Q1–Q3）；保留波动，不转换为实际 Gbps。
- SetEvent 调用、阻塞等待调用分别计数，均不能直接当成上下文切换次数。CPU 比较采用线程周期；短样本的线程 CPU 时间分辨率不足，未用于排名。
- 初筛用于选择候选；发现对端冗余校验后重新执行了全部最终候选和深度／内存对照。此文和 CSV **仅采用校准后的 `confirmed-*` 数据**，不引用早期筛选／中断运行的速率。

最终共 **106 个配置、742 个计时样本**；各实验的 35 个功能／独立模型测试进程全部退出 0。完整数据见 [CSV](exchange-throughput.csv)、[重放配置](exchange-throughput-cases.json)、[证据索引](exchange-throughput-evidence.json)。

## 1500 字节对象：深度与批宽的组合

以下均使用筛选式去重器。slot 为每队列、每方向的数量；共享区一列为真实布局大小，不是编译上限。表格记录切换前的配置分类，最后一行组合现已成为项目默认。

| 配置 | 队列 | slot | ring | 批宽 | RX/TX 字节预算 KiB | 共享区 MiB | HostRx 中位数 [Q1,Q3] | HostTx 中位数 [Q1,Q3] |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| 默认基线 | 1 | 32 | 64 | 32 | 128/128 | 4.02 | 2.56 [2.49, 2.78] | 2.30 [2.11, 2.76] |
| 单队列增加深度 | 1 | 128 | 128 | 32 | 128/128 | 16.04 | 12.79 [12.51, 14.54] | 17.22 [15.38, 19.69] |
| 四队列、浅池 | 4 | 32 | 64 | 32 | 128/128 | 16.08 | 7.91 [7.05, 8.93] | 8.37 [7.51, 8.74] |
| 四队列、两批深度 | 4 | 64 | 128 | 32 | 128/128 | 32.14 | 20.25 [17.88, 20.69] | 14.50 [13.22, 16.59] |
| 当前可用候选 | 4 | 127 | 128 | 32 | 128/128 | 63.64 | 37.91 [29.96, 39.75] | 57.75 [55.55, 60.81] |
| 只扩大批宽 | 4 | 127 | 128 | 64 | 128/128 | 63.64 | 33.74 [23.99, 37.05] | 25.90 [24.48, 28.09] |
| 128 MiB 上限实验 | 4 | 255 | 256 | 64 | 512/128 | 127.77 | 46.29 [44.61, 48.33] | 72.89 [54.06, 81.67] |
| 本轮最高中位数候选 | 4 | 256 | 512 | 64 | 512/128 | 128.52 | 55.62 [50.13, 56.13] | 85.81 [72.81, 93.08] |

单队列只增加深度（32 → 128 slot），HostRx 从 2.56 到 12.79、HostTx 从 2.30 到 17.22；批宽仍是 32，线程数也不变。四队列的 32 → 64 → 127 slot 对照同样表明在途深度对减少停等的重要性。

四队列、127 slot 只把批宽从 32 调成 64，吞吐反而下降；127 slot 只容纳一个完整的 64 项批次加 63 项，而能容纳三个完整的 32 项批次加 31 项。不能只按更大的批宽推算收益。

扩展候选为 **4 队列、256 slot、ring 512、批宽 64**：该布局实际占 128.52 MiB，略超过 128 MiB，本轮通过 256 MiB 编译上限启用；不要求实际分配 256 MiB。较省内存的 4 队列／255 slot／ring 256／批宽 64，在 128 MiB 上限下也已对照。

1500 字节、64 项总计 96000 字节，128 KiB 预算已经不触顶；此场景 128/512 KiB 的速率差不能解释为预算带来的收益。32768 字节对象时，两预算分别限制对端每次最多 4/16 个对象，才会改变实际工作批次。

## 通知与等待计数

1500 字节对象、生产筛选器；每百万已交付对象的七轮中位数。

| 场景 | 方向 | SetEvent 调用/百万对象 | 等待调用/百万对象 | 线程周期/对象 |
|---|---|---:|---:|---:|
| 默认基线 | rx | 93724 | 93703 | 931.8 |
| 默认基线 | tx | 62487 | 62487 | 910.5 |
| 单队列增加深度 | rx | 43723 | 20816 | 390.0 |
| 单队列增加深度 | tx | 12827 | 12826 | 341.9 |
| 当前可用候选 | rx | 45825 | 26461 | 461.6 |
| 当前可用候选 | tx | 5898 | 5838 | 347.4 |
| 本轮最高中位数候选 | rx | 22813 | 13066 | 343.9 |
| 本轮最高中位数候选 | tx | 3475 | 3426 | 284.7 |

## 128/256 批宽为什么不能直接成为默认值

当前 `ShouldScanEarlierSlots` 使用槽号低六位筛选。连续合法槽位超过 64 个时，在重复位命中后回扫之前的槽；128 项和 256 项在单个校验函数中分别需要 6112、30624 次额外槽号比较。这是源码循环的操作数分析，不是整驱动吞吐测量。

`-TokenValidation Bitmap` 只在输出目录生成每槽一位的精确位图版本，所有权检查、首错优先级和整批导入事务保持原顺序。三个区域上限下均通过已有独立模型的 578816 个矩阵用例、32736 个重复位置/优先级用例，以及新增的宽批次保护页和并发测试。它用于说明扩展方向，没有修改生产核心。

| 匹配的 128 批宽对照 | 方向 | 当前筛选器 | 实验精确位图 |
|---|---|---:|---:|
| 64 MiB 上限 / 1 队列 / 510 slot | rx | 10.62 | 13.69 |
| 64 MiB 上限 / 1 队列 / 510 slot | tx | 17.25 | 14.35 |
| 128 MiB 上限 / 2 队列 / 510 slot | rx | 15.84 | 22.11 |
| 128 MiB 上限 / 2 队列 / 510 slot | tx | 27.83 | 26.61 |
| 256 MiB 上限 / 4 队列 / 510 slot | rx | 36.16 | 52.17 |
| 256 MiB 上限 / 4 队列 / 510 slot | tx | 57.01 | 56.94 |

大批次位图对照并不支持把它作为 32/64 批宽的必选优化。更大的批宽还会扩大现有内核栈数组：一个 64 字节描述符数组在 32/64/128/256 项时分别占 2/4/8/16 KiB，尚未计入其他局部变量和调用栈。生产扩展需要拆分 API 批宽、内核工作预算，并考虑队列私有预分配，不能只改一个全局宏。

## 实际采用范围

- 当前默认：`Config { queues: 4, slots: 256, ring_capacity: 512, ... }`，调用批宽最高 64，HostRx/HostTx 字节预算 512/128 KiB。旧的 127-slot 配置仍可显式指定。对单地址对负载，可参考上面的单队列对照。
- 多队列速率要求流量实际分布于多个队列；当前 `common/flow.c` 只哈希源／目的 IP 地址，同一地址对即使端口不同也会进入同一队列。本基准按队列均分对象，没有证明单流可以获得四队列的聚合速率。
- API/DLL/SYS/Rust 收发批次已统一为最高 64；分段写出仍独立限制为 32，Rust 对 65 项收发和 33 项分段写出分别提前拒绝。默认卸载掩码保持为 0，URO/RSC 合并上限和 USO/LSO 负载上限不由本次默认配置修改。
- 本轮没有测定 USO/LSO 最大负载、URO/RSC 真实 NDIS 命中或最终网络吞吐最优值；这些数值不应从描述符基准外推。

## 重放与测试

```powershell
& .\tools\build\build.ps1 -Stage Tests
& .\tools\build\build.ps1 -Stage Safety
& .\tools\audit\bench-exchange.ps1 -Stage Matrix -Rounds 3 -Objects 262144 -RegionMiB 64
# 在隔离版本测试更大的区域或精确位图：
& .\tools\audit\bench-exchange.ps1 -Stage Matrix -RegionMiB 128 -TokenValidation Bitmap
```

精确重放最终配置（各组分别编译，保留生产宏）：

```powershell
$groups = Get-Content docs\exchange-throughput-cases.json -Raw | ConvertFrom-Json
New-Item out\exchange-replay -ItemType Directory -Force | Out-Null
foreach ($group in $groups) {
    $cases = "out\exchange-replay\$($group.name)-cases.json"
    $group.cases | ConvertTo-Json -Depth 6 | Set-Content $cases -Encoding utf8
    & .\tools\audit\bench-exchange.ps1 -Stage Confirm -Rounds 7 -Objects 4194304 `
        -RegionMiB $group.region_mib -TokenValidation $group.token_validation `
        -Candidates $cases -OutputDirectory "out\exchange-replay\$($group.name)"
}
$samples = Get-ChildItem out\exchange-replay -Recurse -Filter confirm-samples.jsonl | Select-Object -ExpandProperty FullName
python tools\audit\analyze-exchange.py @samples --expected-rounds 7 --output out\exchange-replay\summary
```

原始证据保留在 `out/exchange/confirmed-*`。脚本保存真实退出码、原始 JSONL、源码指纹、生成核心哈希及二进制哈希；输出中 `kernel_runtime_tested=false`。新增 `exchange_tests.c` 已接入标准 Tests/Safety。完整项目检查结果见同目录构建记录及本次任务交付说明。

本次完整检查全部通过：Tests 52 个执行进程、Safety 44 个执行进程；最终矩阵脚本的 588 个配置也完成了短样本功能检查（未纳入上面的性能排名）。记录为 `out/packet-path-v2/2.0.0/b37e12cd33d0/test-results.json`、同目录 `safety/safety-results.json`，以及 `out/exchange/matrix-smoke/matrix-evidence.json`。
