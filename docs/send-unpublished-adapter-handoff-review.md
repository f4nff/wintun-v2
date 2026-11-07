# 发送尾部、未发布会话与适配器所有权交接

版本保持 2.0.0，DLL/SYS 数值版本为 2.0.0.0。本轮基线为
`9334ef9d25d1cb48c14cdecd68a34b8adc8fa4e6c1b0c4f5174bfa69f5f20ace`，
完整快照位于 `out/ownership-handoff-baseline/9334ef9d25d1`。

本轮没有确认新的生产缺陷。回归补充合法生命周期窗口的直接覆盖，生产实现、
旧接口、卸载能力门控及数据路径保持不变。

## 发送完成、暂停及注销的三段屏障

使用当前 Send、引用、Fault、Pause、公共文件拥有者注销及 Free 实现。
测试在合法的发送 lookup 与第二次准入之间触发故障，因此拒绝后续数据工作，
仍必须保留已取得的 Active 和 Returns 所有权。它不构造空 NBL，不改变生产
准入条件，也不声称执行了实际网络发送。

完成上调归还并释放守护内存中的原始 NBL、NB、MDL 和载荷；完成后的发送尾部
不得再访问这些对象。[发送完成的所有权交接](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nf-ndis-ndismsendnetbufferlistscomplete)
与会话本身的寿命分别验证：Pause 和注销先等待 Active；Active 归零后，
Pause 可以完成，Free 仍必须等待发送的 Returns 尾部退出。

省略 Free 的 Returns wait 的测试变异停在首个物理释放之前，先汇合真实
Send 和 Pause，再执行剩余释放并报告预期失败码 117。没有在 Pause 未完成时
执行 Halt，没有把不合法的 NDIS 生命周期当成生产问题。

## 未发布 Open 失败后独立回收及卸载

实际非 PB_API_TESTS DLL 执行 PROBE I/O 失败、成功但身份不匹配及未发布的
REGISTER 失败。宿主的真实私有 event 设置 PROTECT_FROM_CLOSE，使首次清理
无法完成；这是防御性关闭失败输入，不声称正常私有设备句柄自然产生该错误。

Open 返回 NULL 且保留原始错误，没有公开会话持有者或测试额外 Retain。
实际 Destroy 发布 runtime 所有者，ReleaseSession 转交后由 registry/work
及预取得的模块引用保留核心、设备、映射和准入预算。GetRetained 等调用全部
在普通 LoadLibrary 引用仍有效时完成；随后先归还普通加载引用，再仅由宿主
SDK 清除保护、放行事件、汇合实际回调线程并观察真实 DLL 卸载。

未注册的清理直接重试 Close，不发送 DRAIN/QUERY。[回调返回后的模块归还](https://learn.microsoft.com/en-us/windows/win32/api/threadpoolapiset/nf-threadpoolapiset-freelibrarywhencallbackreturns)
及最后核心销毁分别记账，不能用 runtime 登记移除替代模块和线程汇合。
缺少 ReleaseSession 转交的变异先观察 construction/registry 仍持有资源且
没有回调；普通加载引用仍有效时，以真实 ReleaseSession 进行一次负例清理，
最终释放、线程汇合和卸载完成后才返回 118。当前构建不执行这项补偿。

## 统一适配器与运行时租约

统一 PB_ADAPTER 还通过实际 OpenAdapterInternal 的运行时准入、保留和
RuntimeOwned 转交建立底层所有者。设备创建工厂由宿主转向该发现路径；它
不执行真实设备创建，也不由测试手工伪造租约计数。统一 wrapper、失败的
StartAdapterSession、WintunCloseAdapter 和 MainRuntime 状态机执行当前源码。

调用方关闭 adapter 后，失败会话仍须保留唯一的 wrapper 和底层依赖租约。
MainRuntimeShutdown 的零预算检查和实际条件变量等待分别验证尚未归还的
租约；设备关闭恢复后，最终 adapter 释放触发依赖卸载。卸载尚在进行时，
shutdown 仍不得完成。会话核心、adapter 预算、回调线程及 DLL 卸载另行汇合，
不能把依赖租约归零等同于整个原生会话已经销毁。

遗漏 WintunCloseAdapter 的 MainRuntimeRelease 的变异先验证原生核心及 adapter
均已回收，而真实 DependencyUsers 仍为 1；在普通加载引用仍有效时，用真实
MainRuntimeRelease 补偿一次，再验证卸载、shutdown、所有调用线程和 DLL
销毁收敛后返回 119。当前构建不进行该补偿。覆盖 PROBE、REGISTER 和初始
POLICY 失败，实际设备安装与命名空间、孤立设备清理准入仍为宿主边界。

## 验收入口及边界

```powershell
pwsh -NoProfile -File tools/audit/verify-send-completion.ps1
pwsh -NoProfile -File tools/audit/verify-unpublished-close.ps1
pwsh -NoProfile -File tools/audit/verify-adapter-runtime.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

构建器记录原始源码、完整生产片段、有限宿主适配及产物哈希；回归以 Win7
目标、ISO/MS volatile 和普通/守护内存组合运行。最终记录位于
`out/ownership-handoff-investigation/final-review.json`。

宿主提供设备 I/O、NDIS、IRQL 和设备发现边界，不安装驱动、不改变设备、
注册表或启动策略。真实旧系统、Verifier、长期内核池、句柄、线程和 CPU
验收仍待专用系统完成。证据保持 `kernel_runtime_tested=false`、
`production_ready=false`；新增测试不引入生产热路径开销。
