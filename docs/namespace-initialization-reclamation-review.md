# 2.0.0：命名空间初始化的取消、期限与资源回收

本轮修复 DLL 安装/后台清理共用的私有命名空间初始化。产品版本固定为 2.0.0，公开 Wintun/v2 接口、SYS 和数据面协议保持不变。

## 确认的问题

CreatePrivateNamespace 返回 ERROR_ALREADY_EXISTS 后，原创建者可能已经退出或关闭命名空间，OpenPrivateNamespace 随即返回 ERROR_PATH_NOT_FOUND。旧实现立即重试，没有退避；后台清理没有 CurrentOperation 时也没有该循环自身的期限。显式清理停止事件只传到了后面的安装互斥量等待，没有参与初始化重试。因此该竞争持续发生时，会空转且不能仅靠显式停止事件终止初始化。

另外，初始化错误会在持有 Initializing 临界区时调用用户日志回调。日志回调若等待其他需要初始化的线程，会形成锁依赖。

冻结旧 namespace.c 的五组负对照分别要求退出 71/72/73/75/76，验证无退避、显式停止被忽略、日志仍持有初始化锁，以及创建调用中已发生停止或越过期限时仍进入打开调用。第 4 组改为退出 0 的正对照：即使日志回调覆盖线程错误，真实 logger.h 的 LoggerLastErrorV 仍会恢复互斥量创建原本的 ERROR_ACCESS_DENIED。各组结束前同样回收全部测试资源，没有用强制结束掩盖归还问题。

复核更正：先前“日志覆盖互斥量创建错误”的断言撤回。旧测试把 LOG_LAST_ERROR 直接替换为 CaptureLog，遗漏了生产 LoggerLastErrorV 的错误恢复，因而退出 74 不能证明生产缺陷。现在保留实际 logger.h 包装，只替换 LoggerErrorV 回调，并让该回调主动覆盖线程错误。当前生产代码中的显式恢复保留；其零错误映射为 ERROR_GEN_FAILURE 的防御行为继续由矩阵验证。

依据：[私有对象命名空间的创建者关闭语义](https://learn.microsoft.com/en-us/windows/win32/sync/object-namespaces)、[OpenPrivateNamespaceW](https://learn.microsoft.com/en-us/windows/win32/api/namespaceapi/nf-namespaceapi-openprivatenamespacew)、[必须立即保存 GetLastError 的约定](https://learn.microsoft.com/en-us/windows/win32/api/errhandlingapi/nf-errhandlingapi-getlasterror)。

## 修复边界

- 仅在上述创建/打开竞争中等待 50 ms 后重试；本次初始化的重试预算为 15 s，并继续服从更短的外层操作期限及运行时停止。普通创建成功、打开成功或缓存复用只做状态检查，不执行该退避等待。
- 显式停止事件参与初始化检查和退避等待。创建返回后、进入打开之前再次检查停止和期限；系统调用中发生的停止或越过期限，会在返回后关闭新取得的私有资源，再返回失败，不发布新状态。
- 边界描述符和命名空间先由局部变量拥有，完整就绪后才在原临界区内一起发布。失败时只清理本次获取；已经缓存的模块资源保留原生命周期。模块卸载后清空两个私有字段。
- 失败原因在回收和日志调用之前保存。初始化错误日志在清理局部资源、释放临界区之后执行；互斥量失败同样保留原错误，零错误的防御场景映射为 ERROR_GEN_FAILURE。

没有新增锁、事件、线程或定时任务，也没有改动内核锁顺序。仍用原来的初始化临界区：等待该临界区的调用者需要等当前初始化返回；本次改动不宣称可以中断尚未返回的系统调用。已有运行时与回调所有权继续覆盖这段时间，超时不能授权提前卸载。

## 测试与限制

`tests/safety/namespace_init_tests.c` 包含实际生产 namespace.c。SID/边界描述符、互斥量、事件、临界区和线程调用使用真实 Windows API；私有命名空间创建/打开/关闭用真实匿名事件句柄支撑的宿主替身，不操作项目的命名空间或设备。时钟和外层操作等待使用脚本化替身，让期限及回绕测试可重放；实际停止事件和线程加入不使用时间模型代替。

ISO/MS 两种 volatile 模式各执行 210 次场景检查，覆盖：

- SYSTEM/管理员边界，四个获取阶段及零/内存不足/拒绝访问/句柄不足，失败后同一实例重试恢复。
- 暂时消失后恢复、正常打开、终止错误、持续竞争、50 ms 退避及 15 s 期限、更短外层期限、32/64 位 tick 回绕、系统调用跨过期限。
- 调用前停止、创建/打开返回前停止、退避中停止、缓存复用时停止，确认未完成的资源不发布，缓存资源不被其他调用的取消撤销；创建阶段已经观察到停止或期限时，不再进入后续打开调用。
- 八线程首次初始化共享同一已发布资源；创建期间停止时所有线程退出，句柄归还后才销毁测试上下文。独立线程观察日志回调时临界区已释放。
- 互斥量创建/等待失败保留错误，每次矩阵核实描述符、命名空间、互斥量及测试线程归属清零，进程句柄无增长。

用例已纳入 Tests/UnifiedTests 和 Safety。审计脚本核实旧文件 SHA-256、未修改的依赖头、五组指定失败码/标记与一组成功码/标记，以及最终源码和测试二进制哈希。ISO/MS 各运行五个旧源负对照、一个旧源正对照和一个当前源码完整矩阵，共 14 个测试进程；两个当前源码矩阵合计 420 次场景检查。

```powershell
pwsh -NoProfile -File tools/audit/verify-namespace-init.ps1
pwsh -NoProfile -File tools/audit/verify-sys-return-reclaim.ps1
pwsh -NoProfile -File tools/audit/verify-sys-pause-wake.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

SYS 的并发归还、回调尾部保护、故障停机及暂停唤醒继续复核和回归，本轮没有据未证实的怀疑修改它们。单 DLL/单 SYS、旧导出、新系统卸载能力门控保持不变，没有增加旧系统 API 要求。

离线测试不代表真实 Win7/新 Windows、NDIS 或内核 CPU 验收。签名安装、Driver Verifier、休眠/热移除与长期内核资源监测仍需实机完成，记录保留 `kernel_runtime_tested=false`、`production_ready=false`；产物为未签名开发版本。
