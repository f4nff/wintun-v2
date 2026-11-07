# 停机回收与背压复核

版本保持 2.0.0，DLL/SYS 数值版本保持 2.0.0.0。本轮基线为
`91ccf2ef652c01c5d24f1a8264bcb280481340e43427bcb0172a37765d3ef4ee`，
完整 596 个源文件快照位于 `out/stop-reclaim-baseline/91ccf2ef652c`。

本轮排查未确认新的生产缺陷。新增测试围绕正常背压的继续运行、初始化失败
资源归还和系统能力门槛，生产同步、旧接口与新系统功能保持不变。负控用于
验证断言能发现特定遗漏，不是当前实现存在缺陷的证据。

## 旧接口正常接收背压

旧 byte ring 容不下下一个包时，后台唯一 native queue owner 仍持有未复制的
接收批次。即使 TX_READY 还有未读包，此时也应通过 receive=false 等待外部
Wake 或完成，避免没有可消费空间却因未读包立即返回。应用归还私有旧包后，
实际后台路径继续复制和归还 native tokens；槽位再次使用必须来自真实 recycle
并增加 generation，不伪造 FULL、部分归还或无效的所有权重叠。

测试以 NDEBUG 编译完整非 PB_API_TESTS DLL 的实际 session/compat worker 路径，
测试断言继续启用。检查有限 wait/pump 调用、通知先于等待时的恢复以及最后
End/Close 的资源收尾。
这些调用计数用于检查调度逻辑，不代表真实内核 CPU 性能测量。

## Miniport 初始化与失败回滚

测试提取实际 TunInitializeEx，覆盖上下文、FDO、registration resource、NBL
pool 和必需属性设置的失败出口。必需初始化尚未成功时，Reserved 不得发布；
可选 offload 注册失败则继续提供 plain adapter。成功初始化后的初始暂停状态
及合法 Halt 路径分别检查 context、pool、resource 的精确释放。

结构版本和长度使用 SDK/WDK 定义，检查 NDIS 6.20 与后续边界。
[注册属性文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/ns-ndis-_ndis_miniport_adapter_registration_attributes)
规定 revision 2 对应 NDIS 6.30；
[通用属性文档](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/miniportgeneralattributes/ns-miniportgeneralattributes-ndis_miniport_adapter_general_attributes)
规定 revision 2 包含 NDIS 6.20 的 PowerManagementCapabilitiesEx。
测试使用宿主 DDI 边界，不把用户态同步当作实际 NDIS 或 IRQL 执行。成功案例
保持初始暂停且没有注册 session；真实 context gate 取得的 control 引用必须在
Halt 撤销 lookup 后归还，才能释放上下文。初始化失败不调用 Halt。

## 参数能力与复核方式

纯能力测试使用独立预期表锁定参数结构 revision，在 6.50、6.83、6.89 等门槛
及相邻版本检查准确结果，覆盖全部低于最低支持版本的编码、256 个 minor
编码和未来版本的上限策略。能力位与 revision 单调，现代功能不因旧系统支持
而被削减。NDEBUG 编译明确拒绝，避免 assert 被移除后产生虚假通过。
参数结构门槛见 [NDIS_OFFLOAD_PARAMETERS](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddndis/ns-ntddndis-_ndis_offload_parameters)。

```powershell
pwsh -NoProfile -File tools/audit/verify-facade-backpressure.ps1
pwsh -NoProfile -File tools/audit/verify-miniport-init.ps1
pwsh -NoProfile -File tools/build/validate-unified.ps1
```

新测试进入普通和守护内存验收；独立审计执行 ISO/MS volatile 组合，记录实际
源码投影、有限宿主适配和产物哈希。最终结果见
`out/stop-reclaim-investigation/final-review.json`。上一轮 47 组独立审计和 65 项
编译负控保留其原始指纹及证据，不标为本轮重新执行；本轮重新执行完整离线
验收、新增定向审计及其相关既有审计，并独立核对所有生产源文件不变。

本轮不安装驱动或改变系统配置。真实旧系统、NDIS/IRQL、Driver Verifier、
长期内核池、句柄、线程及 CPU 验收仍需专用系统完成。证据继续保持
`kernel_runtime_tested=false`、`production_ready=false`。
