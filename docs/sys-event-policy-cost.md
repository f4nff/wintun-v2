# SYS 事件转换与无 offload 报文策略读取

本轮从 `d052ec1415db361baad671371fa305084247990289a27b052d9ed060ededb752` 继续检查 SYS 热路径与等待边界。冻结源码保存在 `out/sys-event-policy-baseline/d052ec1415db`。没有安装、签名或加载 SYS。

## 忙碌期间重复清除空闲事件

`PbV2RetainReferences` 原来在每次增加 Active 后清除 AllReturned；`PbBeginIndication` 每次增加代内计数后清除 IndicationIdle。只要计数原本大于零，对应私有事件就已经未触发，这些重复清除不会改变等待谓词，却仍进入 dispatcher 事件操作。

现在只在计数零→正数时清除事件，最后一次释放仍在正数→零时设置事件：

- ReferenceLock 内用 InterlockedExchangeAdd 的旧值判断是否为第一个引用；同一锁继续覆盖最后一次释放的减计数和 SetEvent。
- IndicationLock 内用原代内计数判断是否为第一个 indication；同一锁继续覆盖最后退出及 SetEvent。TransitionLock 与原有串行旋转/等待协议不变。

事件均为内核私有对象，初始化为零计数且已触发；生产写点只有初始化及上述留存/归还 helper。没有从共享用户提示推断事件状态，也没有无锁检查事件。锁的串行关系保证：若新持有者先于最后释放进入，则计数仍为正、事件保持未触发；若最后释放先完成，则新持有者看到零并重新清除。不会借减少事件操作缩短真实引用或上调尾部。

64 次同一忙碌期的重叠留存，事件清除由 64 次降为 1 次；64 次同代 indication 也由 64 次降为 1 次。计数、rundown、AllReturned 等待、旧代屏障和最后唤醒的含义保持一致。新的流量进入另一代，不延长正在等待的旧代屏障。

## 普通报文没有 offload 策略需求

Describe 的末尾原来无条件执行 `required & ~PbEffectiveMask(ctx)`，即使 required 为零也会读取 AllowedMask、NdisMask、EncapsulationMask、V2SessionActive，并执行依赖规则解析。零与任意策略结果按位与都为零，普通报文始终返回成功，但仍付出了策略观察成本。

现在在报文复制、长度/填充检查及元数据处理完成之后，若 required 为零直接返回成功。所有真正的 checksum/LSO/USO 请求仍通过末尾的当前有效策略检查；未提前缓存策略，也未因请求被禁用而跳过原有报文验证或校验和工作。发送的运行/停止接纳、会话引用和发布屏障仍由 Send 的原有事务规则保证；原来零需求表达式也不根据策略值拒绝普通包。

有效普通报文的策略观察由一次降为零，输出字节和描述符长度不变。带 offload 请求的有效报文仍观察一次，错误元数据或报文仍按原顺序失败。测试在 checksum 完成时模拟策略被禁用，确认末尾仍观察到禁用并返回 NOT_SUPPORTED，防止把这个局部优化扩展成过早策略快照。

## 回归与证据

`receive_reference_tests.c` 直接执行生产引用 helper，新增 64 个重叠引用和真实等待线程：部分释放及再次留存不能放行，最后释放必须恰好设置一次事件。原有二十万轮并发留存继续混合单个/批量归还，锁内检查 Active 与 AllReturned 一致；每个完成忙碌期的 clear/set 收支一致，线程、事件句柄全部归还。

`indication_gate_tests.c` 直接执行生产 gate，新增 64 个旧代所有者、一个新代所有者与真实旧代等待线程。新代仍忙碌时，最后旧代退出应只释放旧代等待。另用两个线程执行十万次 indication，同时串行执行一万次旋转，最后两代的零计数都对应已触发事件。原有十万次平衡旋转及重入转换回归继续执行，检查句柄平衡。

`describe_checksum_tests.c` 的保护页输入新增普通 IPv4/IPv6、TCP/UDP、VLAN/填充、IPv4 ICMP 分片、IPv6 扩展头、错误长度和错误版本；逐字节核对输出，直接记录策略读取。原有 offload、错误偏移/家族、冲突请求及校验和回归继续执行，新增 checksum 完成后禁用策略的交错。另验证四种 LSO/USO 家族在启用/禁用策略、错误偏移/家族以及解析后禁用时仍正确检查策略；测试替身的 LSO/USO 重叠字段按本地 WDK 定义对齐，实际驱动继续使用真实 WDK 类型编译。Tests 与 Safety 都执行该保护页模型。没有新增生产异步任务、缓存或资源类型。

```powershell
.\tools\audit\verify-sys-event-policy.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 分别使用冻结旧引用、gate、Describe 函数与当前函数，配合同一宿主事件/锁/等待线程及实际 packet 核心；按内核使用的 `/volatile:iso` 编译。原常规回归仍覆盖默认编译模式。结果、实际退出码、计数和源码/二进制哈希位于 `out/sys-event-policy/<当前指纹前12位>`。统一验证增加 SysEventPolicy；依赖本地历史快照的七项 A/B 与 benchmark 继续受 SkipBenchmark 控制。

本轮未确认新的内部无限等待，未改变任何必要等待的退出谓词。以上为操作计数与宿主并发证据，不能直接换算为内核 CPU、吞吐或延迟；实际 IRQL、NDIS 并发、Verifier、ETW/WPA、低资源及长稳仍待验收。清单保留 `kernel_runtime_tested=false` 和 `production_ready=false`。
