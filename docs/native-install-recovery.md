> 历史实现记录：统一发布构建使用 `api/v2/adapter_unified.c` 和共享安装器。本文描述的独立现代安装/按名称恢复路径只保留用于既有测试，不用于统一 DLL。

# PacketBridgeV2 2.0.0：原生安装修复

产品版本保持 2.0.0/x64；API/Data 2.1、packet 1.1、lifecycle 1、probe 以及
RX/TX 诊断 ABI 不变。额外导出 PbCreateAdapterEx，schema 1 的 80 字节结构只用于
用户态安装诊断，不改变已有 V2 函数表或 SYS IOCTL。新增独立 PbCreateAdapterEx2，
schema 2/180 字节增加清理 API 步骤和实际 SYS probe 身份；schema 1 保持 80 字节兼容。

## 触发依据与用户策略

2026-10-30 08:58:27 的客户端已加载 a3f7ab8c5b90 的 DLL；08:58:29 创建适配器
返回 PacketBridge status 9，客户端自动回退 legacy。该状态没有携带原始 Windows
错误，现有日志不能证明是重复 OEM 包、签名、设备创建还是接口就绪失败。

13:39:16 的 3d7b3d146337 客户端记录 attempts=2、removed_devices=1、removed_packages=0、
recovery=0x2、stage=5/win32=0x51a。它已经执行恢复，但驱动包清理未成功，随后仍在
服务或 SYS 身份检查失败。旧记录没有清理 API 步骤，不能将 0x2 归到某一个 API。

按用户最新明确指令，清理目标改为配置 name（本例 VPN-Link）对应的 Net class 网卡，
删除它实际绑定的 OEM 驱动包。活动和残留同名设备都处理，不显示占用/重启提示，
然后重新安装内嵌驱动。不再使用兼容驱动列表或全局 OEM 包搜索来选择删除对象。
不是每次健康启动都清理；不存在 DLL 和用户显式选择 legacy 仍按既有选择处理。
现代 DLL 存在时的初始化失败不再自动切换到 wintun.dll。

## 安装事务

1. DLL 在保留 runtime adapter 引用期间取得管理员/SYSTEM ACL 的全局命名 mutex。
   所有新版本的设备创建与修复都使用同一锁，避免并行进程互相删除正在安装的包。
2. 暂存内嵌的 SYS/INF/CAT，然后创建软件设备并等待原有有界回调。
3. 找到接口后实际查询只读 probe，确认产品版本、ABI 和完整 source fingerprint。
   固定版本号下，SetupCopyOEMInf 成功不等于 PnP 实际绑定了这一份 SYS。
4. 如果驱动暂存、设备创建/回调、接口发现或绑定身份检查失败，在关闭失败设备前
   保存它已通过配置名验证的实际 OEM INF/DriverStore 路径；再关闭设备并等待
   SwDeviceClose 的回调退出屏障。context/event 在屏障之前保持有效，保存路径由
   本次创建事务的栈持有，没有全局缓存或指向已关闭设备的引用。
5. 自动清理一次匹配设备与驱动包，清除本进程的 Staged 缓存，再重新暂存并创建一次。
   部分清理失败也保留错误后尝试第二次创建，不无限循环，不重放数据面包。
6. 第二次失败直接返回原始诊断并终止初始化，不建立旧 Wintun 适配器或安装路由。

资源提取/临时文件准备失败发生在实际驱动安装之前，不触发卸载；内存配额或输入
校验失败同样不触发破坏性清理。mutex 等待和设备回调保留原有超时；原生 Windows
卸载调用本身由系统同步完成，不能承诺它的固定执行时长。

## 清理身份与原生 API

- 设备限定为 Net class，FriendlyName 与配置 name 大小写不敏感地完整相等；没有
  FriendlyName 才回退 DeviceDesc。VPN-Link-old、其他网卡名不匹配；硬件 ID、旧服务名
  或旧驱动类型不作为额外排除条件，允许清理同名旧 Wintun 适配器。
- 在第一次卸载前记录所有同名设备的 DEVPKEY_Device_DriverInfPath；按其实际 OEM INF
  去重。失败设备在关闭后已消失时仍使用关闭前保存的绑定，不丢失驱动包线索。
- 设备使用 DiUninstallDevice，设置 DI_QUIETINSTALL。活动和失效设备都处理；
  每次卸载前重新验证配置名，不以“活动”状态阻止用户已经授权的清理。
- 驱动包严格来自同名设备的实际绑定，不从排序后的兼容列表推测。卸载前通过
  SetupGetInfDriverStoreLocationW 保存 DriverStore 路径；解析失败仍直接使用 OEM 路径。
  若 DriverStore 路径卸载返回 ERROR_FILE_NOT_FOUND，再用已知 OEM 路径尝试一次。
  只有对应路径均已确认不存在时才记录 already_absent，不能把权限错误或删除失败
  当作成功。一个包供多个同名设备使用时只卸载一次。
- 使用 DiUninstallDriverW(flags=0) 解除绑定并从 Driver Store 移除。该 API 适用于
  当前 INF 使用的 DIRID 13；不依靠会被 DIRID 13 忽略的 SUOI_FORCEDELETE。
- 卸载始终传入非空 NeedReboot 输出，防止 newdev 自动弹出重启对话框。
  系统延后处理标记仅留作原始诊断，不触发提示或自动重启。继续执行一次重新安装，
  以真实创建和 probe 匹配结果判定成功；不能把原生失败改写为成功。
- 一个已验证候选卸载失败后继续处理其他已验证候选，保留首个清理错误。
  不调用 pnputil/sc/PowerShell 子进程，不手工删除注册表服务、SYS 文件或 DriverStore。
- 非内嵌的开发 DLL 没有可重新安装的资源，因此不执行驱动删除。

## 诊断

PbCreateAdapterEx 返回 Size/Schema、最终 stage/attempts、原始 Win32/HRESULT、
首次失败 stage/Win32/HRESULT、首次与最终设备 ProblemCode/ProblemStatus/known bits、
清理原始错误、已移除设备/包数及系统延后处理标记。unknown 不冒充成功。
Rust SDK 新方法 create_adapter_with_diagnostics 保留这些位值；旧 create_adapter 继续
保持原有返回类型。客户端 adapter-created 或最终失败日志输出这些字段，故障不再
压缩成只有 PacketBridge status 9。

新版 SDK 优先使用 Ex2，旧 DLL 只有 Ex 时继续使用 schema 1。成功结果仍返回原有
80 字节 Rust 前缀；失败的附加 details 输出 recovery_api、matched_devices、found_packages、
already_absent_packages、probe_mismatch、observed_source/version/data_abi/probe_schema。
recovery_api 只标记第一个无法恢复的清理错误；probe_mismatch 的 bit 0..6 依次对应
返回长度、Size、magic、schema、产品版本、data ABI、完整 source fingerprint。

stage：0 校验，1 安装锁，2 驱动暂存，3 创建设备，4 等回调，5 接口/绑定，
6 设备卸载，7 驱动包卸载，8 完成，9 资源准备。
problem_fields_known 的 bit 0/1 分别表示 ProblemCode/ProblemStatus 已成功取得。
deferred_actions 为 Windows 原生卸载的延后处理标记，不是应用发起的操作。

## 验证与边界

driver_recovery_tests 执行生产恢复逻辑，系统设备和破坏性 API 全部替换为有检查的替身。
覆盖配置名完整匹配、大小写、DeviceDesc 回退、不同名称排除、活动+残留同名设备、
卸载前绑定快照、共享 OEM 去重、关闭后设备消失但绑定仍保留、DriverStore/OEM 回退、
包已不存在、安静卸载参数、最多一次恢复、失败回调屏障、部分清理后再创建、资源准备
失败不卸载、原始 HRESULT/Win32/NTSTATUS/清理 API 保留及进程句柄平衡。

既有 callback 测试增加失败 HRESULT 不被坏 instance 字符串覆盖；真实线程/事件验证
成功、失败、超时、取消和关闭屏障。Rust 测试覆盖 80 字节 ABI、额外导出、无效输入在
安装前被拒绝、原始错误显示及缺少扩展时的明确错误。完整常规/安全测试继续执行。

构建机不安装/卸载真实 SYS；实际目标机安装恢复效果仍需测试。签名和 kernel policy
验证不放宽，FinalApi 继续只接受经原有签名校验的冻结输入。未签名测试包沿用已授权
测试方式，自动清理不能绕过 Windows 签名策略。

参考：[DiUninstallDevice](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diuninstalldevice)、
[DiUninstallDriverW](https://learn.microsoft.com/en-us/windows/win32/api/newdev/nf-newdev-diuninstalldriverw)、
[SetupUninstallOEMInfW 的 DIRID 13 限制](https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupuninstalloeminfw)。
