# SYS 共享游标读取与等待协议

后续 recycle 故障传播与发送控制解析见 [SYS recycle 故障传播与发送控制解析](sys-send-fault-control-cost.md)。

本轮从 `55aed283acc826e9b02243579d4a8298af267971164af24aaa86927cd9399335` 继续检查。冻结源码位于 `out/sys-cursor-baseline/55aed283acc8`；范围是 SYS/DLL 共用队列核心及其 worker 唤醒关系，没有安装、签名或加载 SYS。

## 已确认的 CPU 热路径

`PbQueueSpace`、`Available`（由 Pop、SnapshotProducer、Arm 使用）原来通过 `_InterlockedCompareExchange64(p,0,0)` 读取对端游标。它虽然取得正确原子值，却让只读观察也执行锁定读改写。SYS 空接收、recycle 检查和休眠前 recheck 都使用该路径，正常数据收发也经过它。游标分占缓存行的布局不能消除这种访问自身对缓存写所有权的要求。

当前在原生 MSVC x64 上使用对齐的 64 位 volatile 读取，随后显式执行编译器 acquire 屏障；不依赖编译选项默认的 volatile 顺序语义。`PbAttachQueue` 已要求 64 字节对齐的区域和计算出的布局；布局从 4096 开始，各环跨度为共享头加整批描述符，本轮加入字段偏移及两种结构大小均保持 8 字节对齐的编译期断言。小容量环的起点不一定是 64 字节对齐，原布局不变；测试检查所有四种环的游标均满足原子读取所需的 8 字节对齐。其他目标，包括 ARM64EC，保留原 Interlocked 实现。

这里没有把 `Store64` 或 `Exchange32` 改成普通存取。条目写完才发布 Producer，读完才发布 Consumer；Armed 的交换仍然具有完整屏障。每个私有 view 的串行所有权、环边界检查、共享结构布局、描述符验证和原有错误返回保持一致。

## 顺序与维护边界

本实现依赖 Windows 原生 x64 对自然对齐 64 位访问的原子性，以及 x64 的 load-acquire 硬件顺序。显式编译器屏障阻止后续条目读取或槽位复用写入越过游标观察，因此 `/volatile:iso` 下也成立。发布游标和 Armed 交换的原有完整屏障仍负责 copy/publication 和 arm/recheck 的配对。[Microsoft：Interlocked Variable Access](https://learn.microsoft.com/en-us/windows/win32/sync/interlocked-variable-access)、[Microsoft：Lockless Programming Considerations](https://learn.microsoft.com/en-us/windows/win32/dxtecharts/lockless-programming)。

`_ReadWriteBarrier` 是 MSVC 编译器屏障，不是通用 CPU 栅栏。Microsoft 已将其标为 deprecated，并建议线程通信使用标准原子。本仓库的 WDK 共享 C 核心仍使用非 C11 的编译模式，不能直接采用 C++ `std::atomic`；标准 C 原子在当前 MSVC 工具链还需要额外支持选项。此次将依赖局限在一个有明确架构条件的内部 helper 中，不更改全局 volatile 选项或跨边界共享结构类型。未来迁移编译器/标准原子时，应整体迁移队列发布、观察和握手，并运行同一回归，不能单独删掉屏障。[Microsoft：编译器屏障](https://learn.microsoft.com/en-us/cpp/intrinsics/readwritebarrier?view=msvc-170)、[Microsoft：volatile 选项](https://learn.microsoft.com/en-us/cpp/build/reference/volatile-volatile-keyword-interpretation?view=msvc-170)。

## 等待和复用的回归

`queue_order_tests.c` 直接包含生产队列核心，替身仅统计旧的读改写调用，并在真实 Armed/Consumer 交换周围注入对端动作。三次观察分别调用 QueueSpace、SnapshotProducer 和空 Pop。修正前统计到三次锁定读改写，且只读测试页上均出现写访问异常；修正后两项计数均为零。该页保护是观察副作用的实验，不是新增的只读共享环支持；push、非空 pop 和 arm 仍需要可写映射。

确定性交错覆盖发布早于 arm、发生在 Armed 置位前后、以及 arm 已返回空后才发布。前两类由 recheck 发现可读内容，最后一类必须由真正的宿主事件唤醒。另在 Consumer 发布空闲位置后立即让 producer 覆盖容量为 1 的槽位，验证 Pop 已复制的完整输出不会被覆盖。每组交错分别从零游标和 UINT64_MAX 开始执行。

三个双线程用例每个传输 20 万条描述符：容量为 1 的普通/回绕环，以及容量为 64、每批 32 的回绕环。每条描述符逐字节比较；等待有失败超时，检查线程结束、游标和句柄收支。新测试始终以 `/volatile:iso` 加入 Tests 和 Safety；A/B 另将旧/新核心各以 iso/ms 编译运行。原有百万条并发队列、worker 故障交错、完成环满时停车、消费者唤醒、排空边界与引用归还测试继续执行。

```powershell
.\tools\audit\verify-sys-cursor.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

A/B 从冻结旧文件复制函数，仅适配 include 的文件身份，使用相同当前头文件与测试。另编译未插桩的核心并检查 x64 汇编：旧代码存在锁定 cmpxchg64 读取，当前核心不存在该指令；发布与 Armed 的 xchg 仍存在。真实 SYS 的最终 PDB/反汇编也单独检查。结果、实际退出码及源码、二进制和汇编哈希保存在 `out/sys-cursor/<当前指纹前12位>`。统一验证增加 SysCursor，依赖本地历史快照的四项 A/B 与 benchmark 仍受 SkipBenchmark 控制。

本轮未确认新的内部等待死锁，没有改变事件、引用或资源退出屏障。以上是操作副作用、宿主并发正确性和离线代码生成证据，不能换算为真实驱动的 CPU 百分比或吞吐；内核 IRQL、Verifier、ETW/WPA、低资源和长稳仍未验收。清单保留 `kernel_runtime_tested=false` 与 `production_ready=false`。
