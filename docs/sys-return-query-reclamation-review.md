# SYS 最后一次归还、QUERY 与物理回收交错复核

版本保持 **2.0.0**，PE 数值文件版本保持 **2.0.0.0**。本轮从完整验收指纹 `9bae81ac5c44a66f81e52c918808433a9ecb528b7dc75024db5ce8691542bc46` 继续检查，未确认新的生产缺陷，未修改 SYS/DLL 生产实现。补强了实际归还、QUERY 和清理函数之间的回归覆盖。

## 核对结论

`PbV2ReturnNbls` 在释放 NBL/MDL 及活动引用前取得 `Returns` rundown pin。它先释放数据对象、生成或故障清退完成项、更新统计并归还 Active，最后才可能通知 KernelEvent，再释放 rundown。`Active == 0` 和 `AllReturned` 置位都可能早于回调尾部结束。

`PbV2Free` 先请求 worker 退出并 join，然后等待 `AllReturned`，再执行 rundown wait，之后才释放事件、缓冲区、锁定页和 MDL。两个等待分别处理尚未进入归还回调的 NBL，以及已经归还 Active 但仍使用会话／事件的尾部。不能只保留其中一个条件，或者用 QUERY 的 STOPPED 替代它们。

实际 `PbV2Query` 还可以在完成项已经发布但尚未被消费时返回 STOPPED。正常 drain 的 credit 在发布时释放，并非在消费时释放；故障路径则可以清退尚未交付的 pending。这与上一轮 DLL 关闭修复依赖的边界一致，见 [DLL 关闭完成项消费](dll-close-completion-consumption.md)。

本轮没有根据这些状态快照推导新的提前释放漏洞，也没有调整正确的同步屏障。生产侧 103 个源文件及版本属性文件逐项哈希保持一致，起始记录保存在 `out/return-reclaim-review/baseline.json`。

## 新增实际函数回归

`tests/model/return_reclaim_tests.c` 复用已有 NBL fixture，直接执行生产 `PbV2Receive`、`PbV2ReturnNbls`、completion/reference、Fault/quiescence、`PbV2Query` 和 `PbV2Free`。DDI 使用宿主替身，锁、事件和测试线程使用真实 Windows 实现。

64 个组合覆盖：

- 正常 drain／Fault。
- 一个普通 NBL／一个包含四个原始描述符的合并 NBL；测试聚焦已构造对象的生命周期，不重新测试完整合并解析算法。
- 最后一次 KernelEvent 通知之前／rundown release 之前暂停实际归还函数。两处均已释放队列和引用锁；暂停只是宿主调度控制，不是在真实 DISPATCH_LEVEL 阻塞。
- 先 detach 并开始清理、稍后才进入归还回调／先查询 STOPPED、再 detach 并开始清理。
- 同步／手动复位 KernelEvent，以及普通／回绕完成游标。

提前清理的场景先确认 Active=1、AllReturned 未置位且 callback 尚未取得 rundown pin。清理必须停在 AllReturned；此时不能启动 rundown，否则会阻止后续回调取得保护。

回调尾部暂停时，Active 已为零、AllReturned 已置位、NBL/数据 MDL/私有 record 已释放，但 pin 仍为 1，映射和父 MDL 仍存活。rundown 替身先尝试取得真实 SRW 独占锁；失败后发出测试事件，再等待，因此测试验证实际锁冲突，不依赖任意短 sleep 判断“应该还没释放”。清理必须停在此处，保留所有会话资源。

后开始清理的场景先执行实际 QUERY：正常路径已经发布的完成项仍未消费，QUERY 仍可返回 STOPPED。detach 后 QUERY 必须返回 DEVICE_NOT_READY，且不修改输出哨兵。放行归还尾部后，两条测试线程结束，事件、池、NBL、MDL、锁定区域和进程句柄数均回到起点。

新 fixture 不启动 queue worker；worker join 和部分构造失败仍由既有 `sys_cleanup_tests.c` 的 77 个获取前缀及八个宿主 worker 回归覆盖。未对真实 NDIS、原生 rundown 内部实现或 IRQL 作仿真等价声明。

## 反例验证与维护收益

`tools/audit/verify-sys-return-reclaim.ps1` 在 ISO/MS volatile、普通／守护内存组合运行四个当前进程，每进程完整执行 64 场景。另生成八个仅存在于 `out` 的反例，分别删除清理函数中唯一的 AllReturned wait 或唯一的 rundown wait；生产 header 不变。

删除 AllReturned wait 必须在启动 rundown 时因 Active 仍为 1 被观察器拒绝。删除 rundown wait 必须在第一个事件对象释放之前因 pin 仍为 1 被拒绝。反例要求实际退出码 1 和对应精确断言同时匹配；崩溃、超时或其他断言失败不能算通过。后者刻意在第一次物理资源释放前观察，避免反例先关闭仍在使用的事件而让测试结果依赖调度。

脚本记录 12 个进程的退出码、日志、可执行文件及 header 哈希，并核对源码指纹未变化。标准 Tests、Safety 和完整生命周期审计均接入新回归。这样后续简化同步逻辑时，既能发现两类错误屏障删除，又能保留“原生 STOPPED／完成消费／回调尾部／物理释放”各自的边界。

本轮不增加生产锁、状态、事件、定时器或热路径指令，不宣称 CPU 降幅。新 fixture 中满 credit 到可用的最后通知恰好一次，只是操作计数，没有执行真实 worker，也不是 CPU 或调度成本测量。既有成本基准随完整审计重新运行；真实内核 CPU、NDIS、IRQL、Verifier、低资源和长稳仍须实机验证。没有安装、签名或加载 SYS，`kernel_runtime_tested=false`、`production_ready=false`。
