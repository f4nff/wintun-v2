# 2.0.0：停机重试退避与旧系统依赖加载复核

本轮确认并修复两个问题：DLL 生命周期工作线程在停放等待失败后可能高频重试；API 与安装辅助 DLL 的延迟加载钩子会在缺少搜索标志更新的 Win7 上终止进程。版本固定为 2.0.0，旧 Wintun 导出及公共 ABI 不变。

## 停机回收的失败等待

生命周期线程原来忽略 `WaitForSingleObject(ProgressEvent, 1000)` 的返回值，把指定的超时误当作一定经过的退避。等待持续失败时，线程会反复轮询完成队列、查询内核或重试关闭，消耗 CPU。冻结代码在真实生命周期工作线程、真实映射及受保护关闭句柄的测试中触发 `FAILED_PARK_RETRIED_WITHOUT_BACKOFF`（退出码 50）。

修复只在异常返回时补充 1000ms 退避。正常事件唤醒与正常超时沿用原行为；退避不持有 State、Lifetime、Control 或 DataGate。它既不清除事件，也不把等待错误当作内核回收完成，仍保留映射、设备、适配器、工作项和模块引用。已有原始关闭错误保持不变。最后一次归还在退避之前、期间或之后发生，都能在后续重查或事件等待中推进回收。

`tests/model/lifecycle_park_tests.c` 直接复用实际 session/lifecycle 和资源计数夹具，覆盖 80 个调度场景：正常唤醒、正常超时、1/2/64 次失败，WAIT_FAILED 与非预期状态，有/无最后一份用户借用，保留/交还公开会话，以及退避完成前后恢复原生关闭。每次异常都核实 1000ms 退避请求和资源保持；最后归还通过实际 CancelBatch 执行，解除真实句柄关闭保护后核实恰好一次关闭、适配器按序释放、全部计数归零。

退避通过门控确定性验证，不等待真实 64 秒，也不以时间差替代顺序证明。此测试验证失败时不会无退避重试，不代表内核或整机 CPU 性能测量。

## 缺少 DLL 搜索标志的 Win7

原有 API 与 setupapihost 钩子无条件使用 `LOAD_LIBRARY_SEARCH_SYSTEM32`，加载失败即终止进程。微软文档说明，这些标志在 Win7 上需要 KB2533623，可用 `AddDllDirectory` 等导出探测能力。两处实际生产钩子在模拟缺少标志/导出的系统时均触发 `DOWNLEVEL_DELAYLOAD_ABORTED`（退出码 49）。这是在当前宿主模拟旧系统能力，并非 Win7 实机结果。[LoadLibraryExW 官方约定](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)。

两处钩子共用 `common/system_library.h`：

- 优先使用现有受限系统目录搜索，新系统行为保持不变。
- 仅当返回 ERROR_INVALID_PARAMETER 且 kernel32 缺少 AddDllDirectory 导出时，构造 Unicode 系统目录绝对路径，使用 LOAD_WITH_ALTERED_SEARCH_PATH 加载。
- 输入仅接受链接器提供的有界 ASCII DLL 基名；拒绝路径、冒号、控制字符、非终止或过长输入，并检查目录、分隔符、名字和终止符的总长度。
- 其他加载错误直接保留。不会调用 SetDllDirectory/SetDefaultDllDirectories，也不增加缓存、句柄持有或后台线程。

旧系统回退使用该系统支持的“系统目录优先”依赖搜索，不宣称拥有现代标志的“仅系统目录”语义；这一路径只用于项目链接器声明的系统依赖。当前目录诱饵测试验证主 DLL 的来源，不代替对任意第三方 DLL 依赖链的审计。

`tests/model/system_library_tests.c` 覆盖 1330 个边界和策略用例：全部字节值、大小写扩展名、带保护页的全部名字长度和无终止输入、Unicode 路径、全部目录长度/末尾分隔符组合、零错误码、能力存在/缺失、原始错误保留及禁止非能力错误触发回退。

`tests/safety/system_loader_fixture.c` 分别包含实际 API 和 setupapihost 实现；四个真实线程并发加载系统库并核实返回模块路径，逐一等待线程退出后关闭句柄。测试进程的当前目录放置导出诱饵标记的 version.dll，实际模块不含该标记。每个 DLL 执行 18 次装卸，前两次分别预热两种系统搜索路径，随后 16 次核实 OS 句柄数无增长、模块完全卸载；API 还核实真实依赖租约和延迟导入回收。

预热分别覆盖两条搜索机制，避免把 Windows 原生加载器的首次初始化当成项目泄漏。没有通过放宽持续增长上限或忽略失败来通过资源检查。

## 重放与验收边界

```powershell
pwsh -NoProfile -File tools/audit/verify-wait-loader.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

新增用例自动纳入 Tests/UnifiedTests 和 Safety。A/B 审计校验冻结源 SHA-256，分别要求退出码 49/50 和指定标记；当前代码以 ISO/MS volatile、普通/保护页内存运行。源指纹、实际日志和二进制哈希保存在 `out/wait-loader-audit/<指纹前12位>/evidence.json`。

本轮 SYS、数据协议、公共头文件与 Rust 源码保持原样。继续回归 AllReturned/Returns rundown、并发归还、停机回调尾部唤醒、暂停期间回收和 arm/recheck；未因未经证实的内核问题调整同步顺序。新系统能力分级和卸载功能保持现状。

离线测试没有加载 SYS，也没有执行 Win7 虚拟机。真实旧/新 Windows 的签名安装、NDIS、Driver Verifier、休眠恢复、热移除及长期内核池/句柄/线程监测仍需实机验收；记录保持 `kernel_runtime_tested=false`、`production_ready=false`，产物仍为未签名开发产物。
