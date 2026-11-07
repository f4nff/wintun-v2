# 等待退役、卸载处理器及运行时初始化复核

版本固定为 2.0.0，DLL/SYS 数值版本为 2.0.0.0。本轮基线为
`9d75adca3c0b0701630c9525d4069796f856015869e8c220e050fedb40e7e51c`，
完整快照位于 `out/wait-retirement-baseline/9d75adca3c0b`。

本轮没有确认新的生产缺陷。新增回归执行当前生产源码的有关片段及完整 DLL
生命周期，保留原有接口、系统能力门控与同步实现。测试变异只用于检验断言
是否能识别屏障或租约归还的遗漏，不能当成当前实现存在缺陷的证据。

## 卸载策略、常规 OID 和状态上报

逐字抽取实际 POLICY、参数和封装处理器，以及有效掩码、指示屏障与状态上报。
使用 WDK 的参数、封装及状态指示布局，验证 NDIS 6.20、6.30、6.83、6.89
对应的旧结构大小和新功能门控。精确大小的旧参数放在守护页边界，分别检查
拒绝不支持的功能、启用支持的功能、会话未激活及注册失败时的非零策略拒绝，
以及零策略停用行为。

POLICY 与并发常规 OID 必须通过 OffloadMutex 串行化，先等待旧指示 bank；
切换后的新 bank 引用不能延长本次旧 bank 的等待。TransitionLock 在等待前
必须释放。状态上报检查真实快照和上调的锁顺序，宿主只替代 NDIS 上调与
内核调度边界；模拟 IRQL 不是实际内核 IRQL 验收。

曾检查持 OffloadMutex 上报状态时的同线程常规 OID 重入假设。实际上报还持有
StatusLock 自旋锁，上调处于 DISPATCH_LEVEL；[常规 MiniportOidRequest](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_oid_request)
要求 PASSIVE_LEVEL。[常规、Direct 和 Synchronous OID 的契约](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/synchronous-oid-request-interface-in-ndis-6-80)
也区分这几类调用。本项目的 Direct/Synchronous 入口不进入这些修改处理器。
因此没有用任意递归的宿主回调制造“生产死锁”，也没有据此移动生产锁。

省略 POLICY 旧 bank 等待的变异先观察真实旧引用仍在时处理器已经返回，
再释放引用、汇合调用者并回收全部资源，最后报告预期失败码 120。

## 多队列等待者和最后一次退役

实际非 PB_API_TESTS DLL 创建 1、2、8 个队列。每个队列由独立 owner 线程执行
实际 Receive、Reserve 和无限 Wait；同一 owner 在唤醒后归还和取消自己的批次。
分别覆盖 BeginStop 广播及单队列故障触发的 ClosingEvent 广播。故障用例注入
不合法的原生完成记录，属于防御性对端输入，不声称正常 API 会生成该记录。

在最后 owner 已完成归还、loan 归零但仍持有 DataGate/Lifetime 时设置门闩，
验证生命周期 worker 的真实排他快照仍不能进入。放行并汇合所有 owner 后，
独立的 NativeActive 引用仍阻止 PREPARED；原生引用归零后才允许准备完成。
最终 Close 在所有公开调用退出之后执行，不放宽公开句柄的关闭契约。

省略 worker 首次 DataGate 排他快照锁对的变异观察提前 QUERY，再通过独立
原生引用阻止物理回收。先安全退出 owner，再完成关闭、真实回调线程汇合和
DLL 卸载，最后返回 121。这检验快照串行化，不声称 PrepareClose 必须等待
所有 API 返回尾部，也不执行已经释放的句柄访问。

## 初始化失败后的并发租约与重试

完整实际 main.c 执行运行时准入、真实 InitOnce、私有堆、token 和两个 SDDL
分配。对七处初始化取得边界分别注入失败，检查错误码、Ready、描述符及资源
清理，然后验证真实重试成功。部分失败点仅是防御性注入，不能由此推断 SDK
在合法输入下自然失败，或伪造 SDK 在失败后分配输出。

并发用例让第二调用者先真实取得 DependencyUsers 租约，再到达 InitOnce
调用边界；不把 SDK 内部不可见的等待状态当成已观察事实。第一调用者失败
后，第二调用者完成真实重试，第一调用者的失败出口必须仅归还自己的租约。
最后一个成功调用者归还后才卸载延迟依赖，并实际卸载 DLL。

省略失败出口 MainRuntimeRelease 的变异观察无人持有的残留租约，在普通
加载引用仍有效时，每个新加载的 DLL 实例调用实际 MainRuntimeRelease 恰做一次
负例清理；正常构建
不进行补偿。线程汇合、依赖卸载及 DLL 销毁完成后才返回 122。守护内存保护
测试自有的跨 DLL 配置和快照，SDK 的堆、token 和 SDDL 仍由真实 SDK 管理。

## 验证与剩余边界

```powershell
pwsh -NoProfile -File tools/audit/verify-offload-handler.ps1
pwsh -NoProfile -File tools/audit/verify-wait-retirement.ps1
pwsh -NoProfile -File tools/audit/verify-runtime-init.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

回归以 Win7 目标、ISO/MS volatile、普通/守护内存组合执行，记录原始源码、
完整抽取、有限测试变异和构建产物哈希。正式验证检查同一源码指纹下的旧导出
及序号、导入集合、数值版本和历史回归。最终记录位于
`out/wait-retirement-investigation/final-review.json`。

生产数据路径未增加锁、线程、定时器或周期唤醒。宿主调度次数和事件断言不是
实际内核 CPU 测量。本轮不安装驱动或修改系统设置；真实旧系统、NDIS/IRQL、
Driver Verifier、持续内核池、句柄、线程及 CPU 验收仍需专用系统完成。
证据保持 `kernel_runtime_tested=false`、`production_ready=false`。
