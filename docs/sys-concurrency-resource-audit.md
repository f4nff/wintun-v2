# SYS 并发与资源审查

后续复审见 [第二轮审查](sys-concurrency-resource-audit-round2.md)。第二轮用每适配器 rundown 替代本文第一轮的全局锁跨等待保护，并进一步修正状态查询等待环、回调 IRQL 边界和槽位分配游标。

日期：2026-10-28。范围是实际构建的 PacketBridgeV2 x64 SYS：driver/wintun.c 的 PACKETBRIDGE_PROBE 路径、driver/v2、链接的 common 模块及必要的 DLL 故障通知衔接。版本保持 1.0.1，签名继续延后。历史输出不覆盖。

结论：发现了可以从源码时序确认的竞态和资源清理缺口，并进行了修正。没有依据宣称新驱动已经无竞态、无泄漏。新 SYS 尚未安装或运行，本轮结果来自逐路径审查、MSVC 静态分析、共享逻辑测试，以及使用宿主 DDI 替身执行的实际内核辅助函数。

## 已修正的问题

| 编号 | 级别 | 原时序与影响 | 修正 |
| --- | --- | --- | --- |
| A1 | P1 | InitializeEx 在资源初始化完成前设置 FDO Reserved。后续 NBL pool/属性注册失败直接释放 Ctx，会留下悬空入口；并发控制请求也可能看见半初始化对象 | 全部失败点通过后，在 CtxGuard 排他保护下发布上下文 |
| A2 | P1 | Halt 先注销、释放 NBL pool，之后才关闭控制入口。两步之间可能再次注册会话，或让控制请求使用已释放资源 | Halt 开始先撤销入口并等待已有控制读者；之后注销、等待归还、释放 pool；最终再等待进程回调读者后释放 Ctx |
| A3 | P1 | 进程退出通知释放设备列表锁后使用裸 Ctx；可能与 Halt 释放竞争。原代码仅清理第一个适配器，并可能把已经更换的 owner 强制注销 | 用 CtxGuard 保持对象生命周期；循环处理全部匹配适配器；在 RegistrationLock 内重查 PID |
| A4 | P1 | 发送 completion、接收 indication 和 ARP/NDP indication 跨调用持有 TransitionLock。上层可以同步重入发送，违反 EX_SPIN_LOCK 不可递归要求；暂停/关闭竞争时可能死锁 | 上层回调前释放转换锁。NDIS outstanding send、会话 rundown/引用及工作线程 join 分别保护生命周期；控制报文回调后重新核对会话身份 |
| A5 | P1 | 重复或畸形 RX 描述符引用仍由 NDIS 持有的 token 时，为重复项发布失败 completion，可能让用户态提前回收原块 | 新的实际接纳函数 PbAdmitReceive 拒绝为旧/重复/仍活跃的 token 再发布 completion；原提交保留唯一完成权，错误转入隔离 |
| A6 | P2 | 多个控制来源可能让较旧的卸载状态覆盖较新的关闭状态；排空中也可能被另一控制句柄重新开启。将 indication 移出锁后还必须保留 URO 关闭屏障 | 串行化配置转换和状态报告；排空撤销可用会话标记；以双代计数等待旧 indication，允许新普通包批次继续。Pause 等待两代结束 |
| A7 | P2 | 原 PnP 代码按全系统句柄快照强关其他进程句柄。快照与关闭存在重用窗口，Verifier 分支还跳过对象核对 | 私有 SYS 的 PnP 路径交给 NDIS，不再扫描/关闭其他进程句柄。由 Pause/Halt 和 I/O manager 管理对象生命周期；未将此行为扩展到历史二进制 |
| A8 | P2 | 适配器初始化失败清理缺少已初始化 RegistrationLock 的 ExDeleteResourceLite；资源初始化返回值也未处理 | 记录初始化成功状态，失败分支成对删除资源；全局资源初始化也检查返回值；注销函数自行进入 critical region |
| A9 | P2 | 不同队列或两个方向可引用同一事件，造成通知被错误消费者取走。损坏队列反复唤醒还可能形成空转 | 注册时按内核事件对象拒绝别名；错误通知仅发一次，损坏队列工作线程退出；在排空/重查前合并 kernel event 通知，防止手动复位事件保持置位造成空转 |
| A10 | P2 | 驱动检测到队列故障后，用户态 Wait(INFINITE) 未必得知会话已排空，可能继续等待 | SYS 唤醒全部用户事件；DLL 等待返回后查询会话状态，把停止/隔离状态传给既有故障停止流程 |

此外，接收头部元数据改为同一私有快照；明确清理所使用的 NBL flags/OOB；控制报文释放 NBL 后不再通过已释放的指针值判断结果。锁页状态使用独立 PagesLocked 标记，统一由清理路径解锁，不用映射地址是否非空推断锁页所有权。

## 资源所有权核对

| 资源 | 获取与所有者 | 释放条件与出口 |
| --- | --- | --- |
| Ctx、NBL pool、RegistrationLock | InitializeEx；成功后归 NDIS 适配器 | 初始化失败逆序清理；成功后由 Halt 在控制读者、工作线程和归还回调结束后释放 |
| 会话 s、TX/RX slots、pending 数组、scratch/合并缓存 | Register；发布后由 Ctx.V2 持有 | 注册任意步骤失败走实际 PbV2Free；正常注销同一出口 |
| 父 MDL、锁页及系统映射 | Register 分阶段取得 | 明确 PagesLocked；先停止并 join 工作线程，再等 NDIS 引用与返回回调结束，之后解锁/释放 MDL |
| RX record、头 MDL、payload MDL、NBL | 每次接收准备 | 分配失败立即回滚；成功提交后等待实际 NDIS 归还，再释放并发布唯一 completion |
| ARP/NDP buffer、MDL、NBL | 同步 RESOURCES indication | indication 调用返回后释放；跨调用期间会话引用和分代计数有效 |
| 内核线程 HANDLE | 每队列最多一个，最多 8 个/会话 | 注册失败或注销都先唤醒 start/stop、逐个等待退出，再 ZwClose |
| 用户事件内核引用 | ObReferenceObjectByHandle，最多两份/队列 | 成功、部分初始化失败、事件别名拒绝均按取得次数 ObDereferenceObject；不把对象引用误当成需 ZwClose 的句柄 |
| 进程通知、全局 ERESOURCE | DriverEntry | 注册失败对应撤销；Unload 撤销通知后注销 NDIS，再删除资源 |

SYS 是 C 代码，没有 Rust/Tokio 协程。这里检查的是系统线程、内核对象引用和回调生命周期；本轮没有把 Rust 隧道所有异步任务纳入完整审计。

## 已执行的验证

1. 原有分段/checksum 13 项、合并 8 项、Ethernet/ARP/NDP 6 项测试继续通过。
2. 共享核心覆盖百万并发描述符、十万所有权转换；新增十万次重复接收所有权循环，断言活跃块不会被第二次完成。
3. sys_cleanup_tests.c 直接包含实际 session_cleanup.h，并以宿主 DDI 替身覆盖 76 个初始化获取前缀：未分配 MDL、MDL 未锁页、锁页但映射失败、每个队列的对象/内存/线程部分初始化。额外测试别名对象双引用回滚、一次性故障唤醒，以及 AllReturned 已置位但最后回调仍在 rundown 中的时序。所有被追踪资源和进程句柄差值归零，8 个工作线程均已 join。
4. indication_gate_tests.c 包含实际 indication_gate.h：关闭等待旧代时，上层回调可重入转换路径；新代不延长旧代等待；十万次代切换后计数归零。
5. C API 新增由内核状态变化唤醒无限等待的测试；原有关闭无限等待、乱序完成、隔离重试及 Rust 原生 FFI/布局/编译失败测试继续通过。
6. 对 7 个内核翻译单元运行 MSVC /analyze:only，保留原始 SARIF、日志和 analysis-summary.json。项目源码剩余 8 条 C28197：会话及其所属缓冲区、Ctx、NBL pool 的成功移交路径。逐条核对了后续 owner 与清理出口，未用全局禁用警告掩盖；仍属于静态分析告警，不能称为静态分析零告警。SDK/WDK 自身诊断同样保留。

复现命令（PowerShell 7）：

```powershell
.\tools\build\build.ps1 -Stage Tests
.\tools\audit\analyze-sys.ps1
.\tools\build\build.ps1 -Stage Probe
.\tools\build\build.ps1 -Stage Api
```

每次源码变化生成独立指纹目录。SYS、DLL 和 probe 必须来自同一指纹，不能混用此前的 b28a3dae896d 开发包。

## 尚不能关闭的验收项

- **NDIS 永不归还的终止性**：内核清理必须等待仍被 NDIS 持有的块及正在执行的 indication；不能强制解锁。若过滤驱动永久不归还或上层回调永不返回，关闭、进程退出或卸载可能长期等待。用户态观察超时并不是内核强制回收时限；排空 IOCTL 自身也可能先等待回调。这是明确的剩余运行风险。
- **损坏队列的隔离保留**：无法确认完成所有权时，DLL 可能保留句柄/映射至后续安全回收或进程退出。这与正常路径泄漏不同，也不能当作“所有情况下资源已归零”。
- **真实系统检查**：仍需签名后在可恢复测试机运行并发 register/close、进程强退、多适配器、NDIS pause/restart、OID 动态关闭、surprise removal、乱序/延迟归还及分配失败注入；使用 Verifier/PoolMon/句柄与线程计数长稳观察。宿主 DDI 替身不模拟真实 IRQL、NDIS/LWF 和系统 PTE。
- **旧编译分支**：历史 NdisMediumIP/旧 ABI 分支不在当前私有 SYS 的活动路径中；保留源码不等于已对历史二进制或所有兼容分支完成同等运行验收。

参考契约：[ExAcquireSpinLockShared 禁止递归](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/nf-wdm-exacquirespinlockshared)、[发送完成与上层回调](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismsendnetbufferlistscomplete)、[Halt 的请求与清理边界](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/halting-a-miniport-adapter)、[URO 动态关闭要求](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-rsc-offload)。

版权：项目源码现有年份范围已更新至 2026，补齐新构建资源中的版权字段；保留原始署名和许可证正文。官方 DLL、历史输出、归档和第三方依赖没有改写。
