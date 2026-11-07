# 注册、回调交接与日志属性所有权复核

产品版本保持 2.0.0，DLL/SYS 文件版本为 2.0.0.0。本轮基线为
`74362d336bb1a317a24c1c8b12a1d20f4fd54821a3bfde148b9c93b6d39f87f1`。

## 确认的名称属性问题

启用、停用适配器的日志路径原来忽略 `SetupDiGetDevicePropertyW` 的返回值和类型，
直接将名称缓冲区交给 `%s`。名称是自定义设备属性；
[自定义属性](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/creating-custom-device-properties)
可以使用 [DEVPROP_TYPE_BINARY](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devprop-type-binary)。
读取成功并不代表取得了字符串。

合法的二进制值可以填满整个名称缓冲区且没有宽字符终止符。精确旧函数与真实 CRT
格式化复现了两条路径把这种值当成字符串；这要求格式化越过名称缓冲区寻找终止符。
复现没有调用真实设备属性写入，也不据此声称本机已崩溃。

修复复用孤立设备日志原有的类型、字节长度和终止符检查：只有读取成功、类型为
`DEVPROP_TYPE_STRING`、长度适当且末尾终止的值才用于日志，其余显示 `<unknown>`。
共同的名称读取 helper 避免三条日志路径以后分别维护验证规则。
正常日志名称、适配器启停、枚举和失败清理策略保持。

## 补齐实际所有权路径的测试

此前 SYS 清理测试用手工构造的 acquisition prefix 执行真实 `PbV2Free`，没有执行
实际注册函数。本轮从当前生产源提取 `PbV2Register`，将输入验证、对象引用、别名拒绝、
分配、线程启动、发布及失败回滚连接到同一测试。句柄和对象身份分别建模，区分
同一 handle 与多个 handle 指向同一 event，验证回滚归还每次取得的对象引用。

DLL 增加策略回调尾部交接测试：旧 callback 已清除 `Scheduled` 并解锁 State，
但尚未归还 work 引用或返回时，另一个请求可以排队。测试检查私有单线程池、core、
registry 和模块引用在这个窗口仍然有效，后续停止和释放能够完成。

这些测试针对原有实现的具体覆盖缺口，本轮未确认新的注册或回调竞态，生产内核
和生命周期同步保持原样。注册的启动顺序对照检查 StartEvent 的发信号位置，
替身 worker 不执行实际数据处理；它不能证明真实 worker 曾提前读取队列。

策略 work 引用的负对照在安全 gate 观察到缺少引用后，仅为完成测试清理补回这一个
引用，记录补偿次数，再执行实际停止、线程汇合和 DLL 卸载。正常实现禁止补偿。
该对照用于检验观察器，不是本轮生产缺陷。

## 验收与限制

新增测试与此前的跨队列归还、暂停/注销、外层 Return/Halt、槽位复用、generation
耗尽、分配失败及事件唤醒回归一起验收。最终执行数量、源码指纹、负对照、编译模式、
静态诊断比较、导出/导入、版本和产物哈希见
`out/registration-owner-investigation/final-review.json`。

新增注册和回调测试绑定构建的 64 字符源码指纹，并以静态断言要求含终止符的
65 字节布局，防止独立编译遗漏选项后回落到 `unknown`。重放入口：

```powershell
pwsh -NoProfile -File tools/audit/verify-adapter-name-log.ps1
pwsh -NoProfile -File tools/audit/verify-kernel-register-owner.ps1
pwsh -NoProfile -File tools/audit/verify-callback-handoff.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

SetupAPI、内核对象和 DDI 使用宿主替身；真实 Windows 堆、锁、事件、线程及 DLL
装卸只在测试明确记录的范围内使用。不安装或加载驱动，不修改真实设备或注册表。
Windows 7 SP1 x64 仍是最低工程目标；原生旧系统、NDIS/IRQL、Verifier、长期内核池、
句柄、线程和 CPU 验收仍待完成。证据保持 `kernel_runtime_tested=false`、
`production_ready=false`，不据宿主结果宣称全系统无泄漏。
