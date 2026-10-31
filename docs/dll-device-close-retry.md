# DLL 设备关闭重试与失败打开的 Adapter 所有权

版本保持 `2.0.0`，PE 数值版本保持 `2.0.0.0`。本轮起始完整验收指纹为 `ca77e0eb656a7b60a39a8a8b97e37df2d8916b1095619b876eae36507927e66e`。冻结源码在 `out/dll-close-retry-baseline/ca77e0eb656a/api/v2`，审计脚本固定核对 session/lifecycle/state/adapter 四个文件的 SHA256。

## 关闭失败后退回控制查询导致停滞

旧 `Destroy` 在失败 Open 的设备句柄关闭失败时，把对象交给常规生命周期执行者。执行者先要求 DRAIN/QUERY 成功，再尝试关闭句柄。PROBE 或 REGISTER 已失败的会话可能根本没有注册 V2：SYS control 在没有会话时返回 DEVICE_NOT_READY。因此，句柄已经恢复可关闭，执行者仍会反复卡在控制请求，不能重试真正失败的 CloseHandle。

成功打开的会话也有同一问题：已经确认用户 loan 归零、原生 STOPPED，进入物理关闭后 CloseHandle 失败。下一次重试重新做队列消费和 QUERY；如果控制接口已不可用，仍会阻止本来可以完成的物理关闭。

回归使用真实 Windows 事件句柄，通过 HANDLE_FLAG_PROTECT_FROM_CLOSE 使实际 CloseHandle 失败，保留原始 Win32 错误。解除保护后继续让宿主 DRAIN/QUERY 不可用，旧实现必须仍保留映射且不再尝试 CloseHandle；显式恢复宿主 IO 可用性后才允许旧实现完成测试清理。这个保护标志只是稳定复现失败的手段，不表示产品会主动设置该标志。[Microsoft：句柄保护标志](https://learn.microsoft.com/en-us/windows/win32/api/handleapi/nf-handleapi-sethandleinformation)。

当前复用已有私有 RELEASING 阶段。失败 Open 已经尝试设备关闭时，先记录该阶段、DEVICE_CLOSE 阻塞原因和原始错误，再移交给预先创建的生命周期执行者。执行者再次进入 RELEASING 时直接重试相同物理关闭操作，仍取得独占 Lifetime pin；控制查询不再成为已经进入的关闭阶段的前置条件。

未发布的失败对象从未把 session/queue/view 交给用户；原 `Destroy` 本来就直接执行设备关闭。当前只是重试它已经尝试过的操作。成功打开的正常会话仍必须先经过原有 loan、整批 token、原生 STOPPED/引用和 bookkeeping 屏障，才会进入 RELEASING。共享终止提示不能设置这个私有阶段。

CloseHandle 没有成功时，映射、池、事件、模块与运行时预算继续保留；成功后仍按原次序释放。没有通过伪造 STOPPED、清零引用或超时来回收内存。处于物理关闭阶段时也不再产生无效的完成轮询和 QUERY 重试；没有据此换算整体 CPU、吞吐或延迟。

首次停止时间现在独立于阶段初始化：失败构造可能已进入 RELEASING，BeginStop 仍须填写 Started/LastProgress，同时保持阶段单调，不回退为 STOP_REQUESTED。回归检查时间字段及原始关闭错误。

## 失败 StartAdapterSession 的 Adapter 过早释放

旧 StartAdapterSession 在 Open 成功后才把 Adapter 引用放进 session。若 Open 失败且设备关闭仍被隔离，函数会先释放临时 Adapter 引用。调用者随后 CloseAdapter 可以触发最后一次 PbAdapterRelease/SwDeviceClose，发生在仍持有的设备句柄关闭屏障之前。

SwDeviceClose 在 handle 生命周期模式下会启动设备移除；其返回不表示 PnP 移除已经完成。因此不能把过早关闭软件设备句柄当作 session 设备关闭屏障。[Microsoft：SwDeviceClose](https://learn.microsoft.com/en-us/windows/win32/api/swdevice/nf-swdevice-swdeviceclose)。本回归只记录实际 Adapter 引用路径调用宿主 SwDeviceClose 的顺序，没有进行真实 PnP 操作。

当前 OpenSession 内部帮助函数消费 StartAdapterSession 已取得的 Adapter 引用：参数/Allocate 失败时归还；Allocate 成功后立即转交给 session，早于设备打开、PROBE、REGISTER 和策略请求。每个后续失败都由已有 ReleaseResources 归还它。调用者的初始 Adapter 引用仍归调用者所有，StartAdapterSession 的入口 retain 仍覆盖 Allocate 期间。

正常成功路径、直接按路径 Open 和外部 ABI 保持一致。没有为失败关闭新增队列、线程、标志或另一套析构流程；运行时登记、单个工作对象、现有阶段和唯一 ReleaseResources 执行者继续承担回收。

## 对照与资源核对

`close_retry_tests.c` 直接包含实际 session 和 adapter 实现，保留真实引用函数、私有线程池、模块 pin、Windows 句柄、CloseHandle 和一秒诊断等待。仅设备 IO 与软件设备关闭是宿主边界；没有启用 PB_API_TESTS 的模拟原生确认捷径，也没有加载 SYS。

每个进程执行 20 个场景：

- PROBE、REGISTER、策略失败，分别组合直接 Open/Adapter start、普通/受保护句柄：12 个。
- 成功打开后的关闭，组合直接 Open/Adapter start、有/无未归还用户 loan：4 个。
- 文件打开、首次分配失败，组合直接 Open/Adapter start：4 个。

真实工作线程停在可控 park 边界后，才修复句柄或移交引用；放行后仍调用生产代码原来的 WaitForSingleObject(1000)，没有缩短诊断超时。用户 loan 存在时，关闭阶段和设备关闭调用均不能提前推进，保留的数据视图可写；取消后才能继续。

旧实现明确复现关闭停滞和失败 Adapter start 的过早软件设备关闭，随后显式恢复宿主控制接口，仅用于测试清理。当前实现不需要该恢复：控制接口保持不可用也能完成真实句柄关闭，期间没有新增 DRAIN/QUERY，Adapter 最后释放严格发生在成功设备关闭之后。

测试额外保留 shell pin，以 join 公共关闭之后的回调，再释放该 pin。每例检查实际追踪的分配、映射、事件、模块、工作对象、会话/Adapter 数量及字节预算归零。普通/保护页 × ISO/MS 的八个构造/关闭对照进程，加上八个实际 API/lifecycle 进程，共 16 个进程。

修正前的当前实现已复现新断言失败，日志为 `out/dll-close-retry-work/pre-fix.log`；包含时间字段核对的定向证据之一是 `out/dll-close-retry/772e2e5a2727/evidence.json`。最终完整验收重新绑定同一最终源码指纹，避免混用中间结果。

```powershell
.\tools\audit\verify-dll-close-retry.ps1
.\tools\audit\validate-dll-lifecycle.ps1
```

新用例接入 Tests/Safety，完整验收增加 DllCloseRetry。槽位状态机、SYS 回收/唤醒和数据热路径不变。未安装、签名或加载 SYS；真实 IRQL、NDIS/PnP、低资源、Verifier、ETW/WPA、内核 CPU 和长稳仍需实机验收，`kernel_runtime_tested`、`production_ready` 保持 false。
