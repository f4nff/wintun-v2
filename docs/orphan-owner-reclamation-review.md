# 2.0.0：拥有者身份、孤立设备回收与停机取消复核

本轮确认并修复 DLL 后台设备清理的错误回收判断。产品版本固定为 2.0.0，公开 Wintun/v2 接口、SYS、共享数据协议和能力门控保持不变。

## 已确认的问题

Win7 路径原来把 OpenProcess 的任何失败视为拥有者失效，可能因权限拒绝或资源不足删除仍有活跃拥有者的适配器。拥有者属性读取失败、错误类型或短记录也可能进入删除流程，短记录的创建时间甚至没有被完整初始化。相反，拥有者已经退出但进程对象仍被其他句柄引用时，OpenProcess/GetProcessTimes 仍可能成功，原代码只比较创建时间，会继续保留孤立设备。

同组代码还存在这些可复现边界：枚举持续失败时不断递增索引重试；现代系统节点状态查询失败时仍尝试删除；清理在属性读取中收到取消后仍发起后续删除；设备名属性不是字符串或没有终止符时被作为字符串传入日志。旧设备硬件 ID 的类型和 MULTI_SZ 边界也需要明确验证。

冻结旧实现的七组负对照分别触发 53/54/56/57/58/59 和指定标记。删除函数全部被观察器替代，没有操作真实设备；触发负对照退出前，真实测试子进程仍会被信号唤醒、等待退出，避免留下孤立测试进程。

## 修复后的判定边界

`api/orphan_owner.h` 保留原拥有者属性 GUID 和 12 字节 PID/FILETIME 布局。查询使用 Win7 支持的有限查询权限及 SYNCHRONIZE，零超时等待进程对象：真正的退出信号可证明终止，包括合法退出码 259。存活时继续在同一个已打开对象上比较创建时间，识别 PID 已属于另一实例的情况。打开返回 ERROR_INVALID_PARAMETER 才视为不存在的用户 PID；拒绝访问、资源错误、时间查询或等待失败均不能授权删除。每条成功打开的路径都关闭查询句柄。

依据：[OpenProcess 权限/错误约定](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-openprocess)、[GetProcessTimes 的身份信息](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocesstimes)、[WaitForSingleObject 的进程退出信号与 SYNCHRONIZE 权限](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitforsingleobject)。

`api/orphan_cleanup.h` 集中承载三条扫描路径，继续由原调度器、安装互斥量及运行时租约管理：

- Win7 只有完整、正确类型的拥有者记录才能参与身份判断；未知或损坏记录保留。真正的 ERROR_NOT_FOUND 仍按中断安装留下的无拥有者设备处理，保留原恢复用途；扫描与项目创建流程共享安装互斥量。
- 现代节点必须成功读到 DN_HAS_PROBLEM，查询错误不会被当作问题状态。
- 意外枚举错误结束本次扫描并释放列表，不以高频循环等待错误消失。未知单个设备的属性则跳过该条，继续后续条目。
- 设备名按类型、字节数、UTF-16 边界和终止符核实；无效时用固定占位文字。旧硬件 ID 只接受完整 REG_MULTI_SZ，逐字符以显式偏移限制扫描和比较，不依赖 wcslen 推导下一指针；首个列表终止符之后必须全为零，完整验证之后才允许删除。
- 读取完成后、发起删除之前再次检查取消。该边界阻止取消已经在读操作期间发生时启动后续删除，不声称能撤销已经开始的系统调用；原有回调所有权仍覆盖未返回的系统调用。

私有旧设备扫描函数改为报告是否完整完成。取消、枚举/属性错误或删除失败不会设置 LegacyDone，后续正常清理请求可以重试。它不会自我排队或增加定时后台任务。公开导出与函数表布局不变。

抽取代码时保留原版权声明。审计用有限变更清单逐字核对安装器其余部分：Win7 创建/等待代码未改变；adapter.c 仅增加模块引用、迁移清理函数及消费扫描完成标志。

## 测试与重放

`tests/safety/orphan_owner_tests.c` 包含实际生产清理模块，SetupAPI/节点状态和删除调用使用宿主替身，进程、线程、事件与互斥量使用真实 Windows 对象。每组执行 448 次场景检查，包括：

- 存活拥有者、打开/等待/时间查询失败、零错误码、PID 0、创建时间不匹配；后者模拟 PID 重用，不冒称实际触发了系统 PID 重用。
- 保留真实进程句柄直到拥有者退出后再次扫描，分别验证退出码 0 和 259，确认真正孤立设备可回收。
- 拥有者记录所有短尺寸、错误类型/过长报告、真实缺失；Unicode、最大名字、无终止/错误类型/奇数或超大尺寸。
- Win7、现代和旧枚举路径的持续失败、提前取消、逐条跳过、删除失败及下一次扫描恢复；MULTI_SZ 类型、长度和终止边界，完整缓冲区容量下的匹配/非匹配/空列表，大小写、相邻前缀、重复 ID 与单终止符。
- 真正清理线程在名字/硬件属性查询中暂停，控制线程设置真实停止事件后恢复；删除未启动，列表和互斥量仍在返回前持有，线程加入后才释放测试上下文。
- 拥有者输入与名字输出置于保护页边界，身份输入设为只读；每次核实查询句柄、设备列表和互斥量归零，真实子进程和扫描线程均加入、关闭，重复矩阵无进程句柄增长。

```powershell
pwsh -NoProfile -File tools/audit/verify-orphan-owner.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

新增用例纳入 Tests/UnifiedTests 与 Safety。A/B 审计核实旧文件哈希、抽取边界、属性声明布局及指定失败标记，以 ISO/MS volatile 执行；日志、源指纹和二进制哈希写入 `out/orphan-owner-audit/<指纹前12位>/evidence.json`。

本轮仅改变 DLL 的冷清理路径，没有修改数据面循环、SYS 同步/回收顺序或新系统卸载能力，也没有新增常驻线程、线程池或定时轮询。关闭前无法证明拥有者失效的设备可能继续保留，等待后续可验证的扫描或显式管理操作，这是避免误删的预期行为。

离线验证不加载 SYS、不安装/移除设备，也不是 Win7 虚拟机验收。真实旧/新 Windows 的签名安装、NDIS、Driver Verifier、休眠恢复、热移除和长期内核资源监测仍待实机完成：`kernel_runtime_tested=false`、`production_ready=false`，产物为未签名开发产物。
