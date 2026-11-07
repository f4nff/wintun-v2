# 2.0.0：资源失败日志、临时目录所有权与回收

本轮修复 DLL 的资源提取失败路径。产品固定为 2.0.0，单 DLL/单 SYS、旧导出与新系统能力门控保持不变；SYS/DLL 的队列和停机同步顺序未改。

## 确认的问题

ResourceGetAddress 把 packetbridge.sys/inf/cat 别名转换成 MAKEINTRESOURCEW(101–103)，失败日志仍用 %s 格式化转换后的值。数字 ID 会被当成地址读取，在资源缺失时发生访问异常。直接传入数字 ID 的 ResourceCopyToFile 外层失败日志也有同样问题。旧 alias 测试的日志桩忽略格式和参数，因而没有发现这些错误。[FindResourceW 文档](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-findresourcew)明确区分数字标识和字符串名称。

ResourceCreateTemporaryDirectory 在 CreateDirectoryW 成功之前已经写出候选路径。辅助任务以 Directory[0] 是否非空判断是否清理，因此碰撞或创建失败时，可能尝试删除未由本次操作创建的候选目录。早于路径构造的失败也可能让调用方拿到未初始化或过期的输出。测试通过宿主目录所有权表复现这一链条，不执行真实删除。[CreateDirectoryW 文档](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createdirectoryw)将目录已存在列为失败，不能据此获得清理所有权。

冻结原实现的四组负对照分别要求退出 81/81/82/83：直接数字 ID、别名转 ID、Copy 外层格式化，以及非拥有目录候选被发布。日志用真实 CRT 格式化，SEH 捕获指定访问异常。为了单独抵达 Copy 的外层错误日志，第三组仅跳过一次内层错误格式化；它是隔离观察，不冒充实际产品日志过滤行为。

## 修复与兼容边界

- 所有数字资源 ID 以 #编号显示，字符串保留调用方原名，别名映射和资源 ID 不变。没有复制 SYS/INF/CAT，也没有新增包。
- 资源查找失败时 Size 清零；各获取阶段保留原错误，LockResource 沿用 ERROR_LOCK_FAILED。资源名 SAL 注解允许数字标识，C 签名不变。
- 临时路径先在局部构造，只有 CreateDirectoryW 成功后才发布。所有失败输出为空；GetWindowsDirectoryW 返回长度达到或超过缓冲区容量时立即拒绝，避免继续使用不完整结果。成功返回长度不含终止符，缓冲区不足返回所需容量，见 [GetWindowsDirectoryW 文档](https://learn.microsoft.com/en-us/windows/win32/api/sysinfoapi/nf-sysinfoapi-getwindowsdirectoryw)。
- 资源/文件失败中的零错误归一化为 ERROR_GEN_FAILURE，防止 WriteFile(FALSE)+0 经 RET_ERROR(TRUE,0) 被误报为成功。短写继续报告 ERROR_WRITE_FAULT，真实文件句柄始终关闭。

这里没有声称所有 LOG_LAST_ERROR 调用原来都会丢失错误。实际 LoggerLastErrorV 已恢复回调改动的最后错误；新增显式恢复主要保证零错误防御和新 LOG_ERROR 路径。相关命名空间历史测试也同步纠正，见其复核说明。

成功后的目录仍交给原调用方清理；没有在临时目录函数中越过所有权删除候选。没有增加线程、工作项、锁、周期性轮询或旧系统 API 要求，资源内存仍由原模块生命周期保护。

## 验证

`tests/safety/resource_failure_tests.c` 包含实际生产 resource.c，保留真实 logger.h 包装约定，并用真实 _vsnwprintf_s 格式化。资源、路径和目录 API 使用宿主替身，文件句柄由真实匿名事件支撑；不创建或删除真实文件/目录，不安装或操作驱动。

每种 volatile 模式执行三轮矩阵，共 1011 个场景，包括：

- ID 0、1、101–103、65535，三种别名、普通名称、大小写和 Unicode 名称，成功与全部获取失败阶段。
- 真实/零错误码、日志回调改动最后错误、Size 失败清零、写入失败、每个短写长度及句柄配对关闭。
- Windows 路径返回长度为 MAX_PATH、超长或 MAXDWORD，路径拼接失败、随机数失败、目录碰撞、失败输出含旧内容，以及宿主 MAX_PATH 容量边界。
- 每轮核实所有宿主拥有的文件/目录归还、非拥有目录删除次数为零、进程句柄不增长。

新用例纳入 Tests/UnifiedTests 和 Safety；既有真实 DLL 资源别名测试继续验证两个接口地址指向同一份 SYS/INF/CAT。

```powershell
pwsh -NoProfile -File tools/audit/verify-resource-failure.ps1
pwsh -NoProfile -File tools/audit/verify-namespace-init.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

本轮并行复核了 Active/Returns 回调尾部、worker join、故障 QUERY、暂停回收、DLL Lifetime/DataGate、兼容字节环和异步移交，未确认需要新增修改的合法调用竞态。现有屏障继续通过宿主专项验证。

真实 Win7/新 Windows、签名安装、NDIS、Driver Verifier、休眠/移除及长期内核资源验收仍待实机完成。记录保留 `kernel_runtime_tested=false`、`production_ready=false`，产物为未签名开发版本。
