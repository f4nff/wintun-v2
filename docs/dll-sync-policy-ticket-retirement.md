# DLL 同步策略等待失败后的请求槽位归还（2.0.0）

本轮从完整验收指纹 `2312cfc0d593302e650b84858c7dee954af5c3e735325294c7033c2d00cc3838` 继续检查。产品版本保持 **2.0.0**，数字 PE 文件版本保持 **2.0.0.0**，ABI 不变。确认并修正的是 C 同步策略接口的请求槽位错误退出，未证明新的报文槽位提前复用或内核资源提前物理释放。

## 已确认的问题

`SetPolicy` 内部调用 `BeginSetPolicy` 取得 ticket，再用无限期限的 `PollPolicy` 等待。正常 Poll 成功会消费结果并释放 `PolicyOccupied`；底层策略 I/O 自身失败也通过一次成功的 Poll 返回操作错误，因此同样正常释放槽位。

但当等待本身返回非超时错误时，`PollPolicy` 返回 `PB_CORRUPT`，没有消费结果。旧同步 wrapper 直接返回错误，也没有调用 `ForgetPolicy`。ticket 只存在于该函数内部，未交给同步调用者。随后即使实际 I/O 已完成，worker 只在 `PolicyAbandoned` 为 TRUE 时自动清空占用；旧路径没有设置它，后续策略请求一直得到 BUSY，直到会话关闭或另行显式恢复。

微软文档定义条件变量等待可返回失败并提供扩展错误，超时另有 ERROR_TIMEOUT；本实现已经为非超时失败提供错误返回分支。本轮通过注入该失败来验证分支清理，没有声称观察到自然发生的 Windows 等待失败、真实低资源故障或 NDIS 故障。[Microsoft：SleepConditionVariableSRW](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleepconditionvariablesrw)。

## 修正与边界

生产修改仅在 `api/v2/session_control.h`：Poll 未能成功确认结果时，保存当前 Win32 错误，调用既有 `ForgetPolicy(s, ticket)`，再恢复错误并返回原 PB_RESULT。

复用既有 ticket 检查和放弃逻辑：

- I/O 排队或执行中：标记放弃结果，保留占用，直到实际 I/O 完成才允许复用。没有取消或覆盖进行中的操作。
- I/O 已完成但结果未被消费：立即清空仍属于该 ticket 的占用。
- 原 ticket 已退还并有后续请求：ticket 不匹配，清理不改变新请求的占用或放弃标记。
- Poll 正常返回：保留原来的结果确认路径，包括成功确认一个失败的底层 I/O。Begin 参数拒绝仍在取得 ticket 前返回。

没有手工清零占用、增加队列／锁／事件／定时器，或修改 worker、实际策略 I/O、ForgetPolicy、报文热路径、用户借用、原生引用及物理关闭屏障。新增清理只发生在同步等待错误出口；不宣称 CPU 降幅。104 个原生产文件中，除该 control header 外其余 103 个文件字节保持一致。

## 实际函数回归

`sync_policy_tests.c` 直接包含实际 session/lifecycle/control，实现真实 Windows SRW 锁、事件、调用线程和私有单线程池。原生策略 I/O 使用 PB_API_TESTS 宿主边界。等待替身返回一次 ERROR_OPERATION_ABORTED，保留返回时持有独占 State 的条件；普通成功／I/O 失败控制仍执行真实条件变量等待。

每进程 13 个场景：

- I/O 尚未开始、正在执行、已经完成，以及原票据已被后续正在执行／已经完成的请求替换，分别组合原 I/O 成功和失败，共十个错误等待场景。
- 普通成功、普通 I/O 失败、Begin 参数拒绝三个控制场景。

完成／替换交错只在替身释放 State 后，通过实际 worker、Forget 和 Begin 接口安排，之后恢复锁再报告等待失败，不直接改写会话字段。替换场景验证清理的 ticket 身份边界，不声称这是安全 Rust PolicyChange 的并发故障；Rust wrapper 使用不同的异步请求／Drop 路径。

旧实现明确验证：错误返回后占用未放弃，实际 I/O 完成后仍 BUSY。只有旧代码测试显式恢复内部 ticket，才能继续后续请求和有界清理。当前实现无需此恢复：未完成操作仍 BUSY，完成后可提交下一请求；后续 ticket 的原有 backpressure 和结果确认保持有效。等待错误码仍为注入的原错误，未被清理覆盖。

`verify-dll-sync-policy.ps1` 固定核对旧 control header SHA-256，与相同当前 session/lifecycle 配对，执行旧／新 × ISO/MS volatile × 普通／守护内存八个进程。生成的 session 仅变更 include 解析和所选 control header。每例最终执行原 Close 流程，并核对运行时会话、保留字节、Adapter 计数和守护分配归零。新测试已接入 Tests、Safety 及完整生命周期审计。

没有安装、签名或加载 SYS。受控失败注入与宿主线程／锁验证不能替代真实内核 CPU、NDIS、IRQL、低资源及长稳验收；`kernel_runtime_tested=false`、`production_ready=false`。
