# SYS 暂停转换遗漏 worker 唤醒（2.0.0）

版本保持 **2.0.0**，PE 数值文件版本保持 **2.0.0.0**，ABI 不变。本轮从完整验收指纹 `e9bc036520f1bb71190c5d1964d4f940761b605e323a0c8d41a2990549803301` 继续检查，确认并修正了暂停转换中的进度竞态。未证明因此发生内核提前物理释放。

## 已复现的交错

`PbV2Worker` 先根据 Running/Draining 决定是否处于 paused 分支，随后才调用 `PbV2ReceiveBatch`。接收函数独立取得 TransitionLock，并再次检查 Running。两次判断可以跨越一次真正的 Pause：

1. worker 清除旧 KernelEvent，看到 Running=true，选择正常分支。
2. `TunPause` 在独占 TransitionLock 内发布 Running=false、清除 RunningEvent；随后调用 `PbV2Pause`。旧 Pause 没有通知队列。
3. 接收函数看到 Running=false、尚未 drain，立即返回 FALSE，跳过 completion flush。
4. worker 仍按第 1 步的选择 arm 提交／回收环，进入只含 StopEvent、KernelEvent 的正常等待，未监听 RunningEvent。
5. 此时没有新的提交或回收项来触发下一轮。已有 pending completion 停留。Restart 只设置 Running=true 和 RunningEvent，不能唤醒这个旧等待集合。

测试执行实际 flush 留下合法积压：129 个已完成的私有项，第一次四批 flush 发布 128 项，剩余一个 pending/credit；消费方实际取走前 128 项并发出空间通知。worker 清除这个旧通知后，才被调度到上述暂停窗口。并非向完成环填入伪造 junk 来制造背压。

旧实现的实际零超时等待返回 WAIT_TIMEOUT：选中的两个事件均未就绪，最后一项仍为 pending。即使 RunningEvent 已因 Restart 置位，结果依旧如此。只有测试显式补发一次无关队列通知，旧代码才继续并完成清理。这证明该等待没有可用的恢复通知，不是用一个短时间内没看到进度来推断永久停滞。未来其他队列工作、DRAIN 或注销仍可恢复，不称为无条件永久死锁。

## 修正范围

`PbV2Pause` 在既有 TransitionLock 下取得当前会话的 Returns rundown pin，释放锁后立即调用既有 `PbV2Wake`，再进入原有 indication bank 和 AllReturned 等待。每个队列获得一次已有 KernelEvent 通知。

worker 若已清除旧通知并读到运行状态，暂停通知会保留到它的旧等待；若之后才清除通知，后续状态读取和接收函数会看到已发布的暂停状态。下一轮走正确分支，完成暂停期间的归还／完成刷新，并在需要时监听 Restart。测试确认只增加一次受预算约束的重查，没有新包接纳、忙轮询或定时器。

Returns pin 保护解锁之后使用的会话及队列事件。无会话／无法取得 pin 时不访问队列；原 indication 等待、活动引用等待和最后 pin 释放顺序保留。没有改变正常运行的每包路径、共享结构、槽位状态机、回收授权或物理释放屏障。

为直接测试实际函数，原 Pause 函数从 `session.h` 原位置移入 `session_pause.h`，由原位置 include。审计逐字核对完整 session header 只发生此抽取，并核对新 helper 除说明注释和新增的 `if (held) PbV2Wake(s);` 外与旧函数一致；仅为比较统一 LF/CRLF。不是在测试中复制另一套暂停算法。

## 回归与 CPU 边界

`pause_wake_tests.c` 每个进程执行 12 个场景：八个竞态组合覆盖保持暂停／提前 Restart、同步／手动复位 KernelEvent、普通／回绕完成游标；另四个验证无会话、两队列均被通知、原生 Active 阻止 Pause 返回，以及等待期间 detach 后原 pin 仍保留。

worker 在实际接收函数之前被 gate，未修改它选出的 paused 分支和等待集合。首次零超时等待若消费了就绪事件，按实际阻塞等待的同一语义返回；若没有，则继续等待完全相同的两个事件。新实现无需测试补发通知，首次等待消费暂停通知，第二轮发布最后一个完成项，pending/credit 归零；只有两次等待入口，恢复后的等待集合和接收调用次数也与状态对应。守护模式覆盖共享区域、私有 RX 槽、pending 数组和事件包装对象；分配及进程句柄数归零。

`verify-sys-pause-wake.ps1` 固定核对旧完整 session header 与原 Pause 函数 SHA-256，使用相同当前 worker/receive/core，运行旧／新 × ISO/MS volatile × 普通／守护内存八个进程。旧实现有意重现等待集合缺少通知，随后用显式 rescue 清理；当前实现必须直接前进。测试接入 Tests、Safety 和完整生命周期审计。

新增开销是每次暂停、每个现有队列的一次事件通知，换取暂停退休和恢复进展。没有新增运行热路径指令，不宣称 CPU 降幅。未启动真实 NDIS、执行原生 IRQL 或测量内核 CPU；宿主使用实际生产函数和真实 Windows 锁／事件／测试线程，DDI 与 indication 等待为替身。没有安装、签名或加载 SYS，`kernel_runtime_tested=false`、`production_ready=false`。
