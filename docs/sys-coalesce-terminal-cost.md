# SYS 遵守合并组的短分段终止状态

起始源码指纹为 `f9fdec6f74d1f74db21b8a6068fe13dbe4c0b0d2e480f49d3dc659ee4b342cc8`，完整快照保存在 `out/sys-coalesce-terminal-baseline/f9fdec6f74d1`。

## 问题与修正

共享合并核心在 UDP 组成功接收一个比首段短的末段后设置私有 `ShortFinal`，之后的 Append 必须立即拒绝继续追加。SYS 编排循环原来未检查这个状态，仍先将下一描述符的整包复制到 scratch，再由 Append 在入口拒绝；下一包随后仍属于下一个组或普通 NBL，导致无意义的复制及调用。

当前循环先检查已有私有 ShortFinal，再决定是否访问后续描述符。该字段由核心成功追加后设置，不能由用户共享数据授权。核心的解析、校验和、流兼容性、输出容量和完成逻辑没有修改；下一包仍由外层 ReceiveBatch 继续处理。没有新增状态缓存、内存分配、锁、事件、线程或等待时间。

没有根据“输出已满”提前跳过其他验证，因为那可能改变 TCP abort 统计。此次仅使用 UDP 短末段的既有终止语义；TCP、普通包及未启用 URO 的路径仍采用既有规则。

## 可复现成本

三个同流 UDP 包分别长 60,000、1,000、60,000 字节，总长 121,000，未超过 ReceiveBatch 的 128 KiB 预算。前两包构成一个合法的短末段 URO 对象，第三包单独交付：

| 项目 | 旧编排 | 当前编排 |
| --- | ---: | ---: |
| scratch 复制字节 | 121,000 | 61,000 |
| 首组合并的 Append 调用 | 2 | 1 |
| 实际组成首组的包数 | 2 | 2 |
| IPv4 合并资格校验和读取字节 | 61,016 | 61,016 |
| IPv6 合并资格校验和读取字节 | 60,984 | 60,984 |

旧的第二次 Append 在校验数据前即因 ShortFinal 拒绝，因此校验和读取没有减少。上述是实际生产函数的宿主操作计数，不能换算为内核 CPU 利用率或吞吐收益。

## 回归与所有权

- `coalesce_path_tests.c` 使用生产编排和共享核心，比较全部输出字节及组状态。新增 IPv4/IPv6 的三包、恰好两包、后续描述符无效、短包校验和失败、URO 禁用、第一个输出缓存忙及两个缓存都忙的组合；scratch、预期输出和合并缓存受保护页约束。原 TCP、普通 UDP、策略过滤、坏校验和及多流用例继续执行。
- `drain_boundary_tests.c` 执行实际接收、构造 NBL、留存引用、归还、完成和 QUERY。验证一个双包 URO NBL 加一个普通 NBL 的内容、OOB SegCount/SegSize、校验和元数据、唯一 token、状态及统计。分别覆盖合并对象分配失败、随后普通对象分配失败、indication 内同步归还及构造期间晚到故障。
- 本地取消和正常返回仍通过原有有界归还窗口，释放真正属于该批的槽、credit、引用、MDL 和缓存。故障后零引用仍不能单独确认 STOPPED，必须取得 worker 的私有见证；现有实际回调 rundown 尾部和父 MDL 解锁屏障继续验收。
- `verify-sys-coalesce-terminal.ps1` 选用冻结/当前编排，配合同一共享核心及同一个实际 ReceiveBatch 函数体，执行保护页 helper、普通接收和保护页接收的 A/B。记录源码/二进制哈希、真实退出码和日志；已有真实事件等待、满 credit/完成环、暂停及排空回归同时运行。

Tests 沿用既有两组入口；Safety 增加独立合并路径，并继续执行保护页接收。完整离线验收包含本轮 A/B，冻结一个源码指纹：

```powershell
.\tools\audit\verify-sys-coalesce-terminal.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

未安装、签名或加载 SYS；真实 IRQL、NDIS/LWF 调度、Verifier、ETW/WPA、低资源和长稳仍需内核验收。
