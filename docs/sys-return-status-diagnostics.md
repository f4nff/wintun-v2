# NBL 归还原始状态诊断（2.0.0 / x64）

已有客户端日志里的 58 个失败完成项只留下了 `PB_CORRUPT`，原始 NBL 状态已经丢失，
无法从旧日志恢复或断言实际丢包原因。此次增加诊断，并修复可独立复现的本地取消误分类。
真实 NDIS 非零状态保留原数值完成结果；分类和日志名称与 API 级队列损坏分开。

## 原机器已捕获的状态

2026-10-29 的 `5955f25e4084` 构建快照包含 24 条真实 NDIS 归还记录，全部为
`0xC000023E`（`STATUS_PROTOCOL_UNREACHABLE`）：19 条 IPv4/ICMP、576 字节，
5 条 IPv6/ICMP、1280 字节。均为普通单包，IP 声明长度与 indication 长度一致；
本地取消为 0，记录没有覆盖。状态名称与
[Microsoft NTSTATUS 定义](https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55)
及本地 SDK `ntstatus.h` 一致。

已确认的分类缺口是：非零归还被统一命名为 `PB_CORRUPT`，容易误诊成队列/内存损坏。
现在给**接收完成描述符**的数值 4 定义 `PB_RX_REJECTED` 别名；数值和 ABI 不变。
API 函数返回 4 仍然是 `PB_CORRUPT`，继续作为队列/所有权错误处理。
拒收仍计入原错误/完成统计、原样输出 raw、归还原 token，不改成成功、不重放、不切换活跃会话。
工具附加 `nbl_status_name`、`classification`、`completion_status_name`，未知值明确为未分类，
本地取消由 origin 优先分类，不把合成状态当成真实协议拒收。

这些记录能定位当次包级状态及误分类，不能恢复最初旧构建的 58 条记录，也没有覆盖稍后客户端累计的全部 44 包。
它们没有记录 ICMP Type/Code、被引用的内层协议或来源，因此不能断言具体 ICMP 生成端、远端协议支持、
防火墙策略或 Windows 丢弃原因。576/1280 的形状与客户端 reject 构造器上限相符只是线索。

## 已确认的分类问题

`session_receive.h` 可能在构造 NBL 后、正式 indication 前因停止/故障撤销整个链。
此前它写入 `NDIS_STATUS_PAUSED`，再借用正常归还函数，最终得到 `PB_CORRUPT` 和接收错误统计。
这个状态来自驱动自身，并非 NDIS 返回。现在由私有 record 标记本地取消，完成为 `PB_STOPPED`，
计入 `ifInDiscards`；清理、token、引用及 rundown 的归还路径不另起一套。

正常 `MiniportReturnNetBufferLists` 代表缓冲区所有权归还，不是对端送达确认。
参见 [Microsoft 回调约定](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ndis/nc-ndis-miniport_return_net_buffer_lists)
和 [NET_BUFFER_LIST 状态成员](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/nbl/ns-nbl-net_buffer_list)。
诊断中如出现正值、负值或未识别值，均原样记录；不能仅依据 `PB_CORRUPT` 名字判断内存或队列损坏。

## 只读诊断接口

新增独立 IOCTL `PB_RETURN_DIAG_IOCTL`，schema 1。原 probe 结构仍为 232 字节，
数据/API ABI 仍为 2.1；不占用描述符保留字段，不改变 Rust 绑定的既有内存布局。
每条记录 64 字节，快照 4224 字节，固定保留适配器生命周期内最近 64 条异常记录。
记录带 session epoch，可区分同一适配器先后建立的会话；读取不会清空历史。

每条记录包括原始 32 位 NBL 状态、原 PB 完成结果、来源、首个 token、原始包数和总字节、
indication IP 长度、IP 头声明长度、IP 版本、首层协议号以及普通/TCP 合并/UDP 合并类型。
IPv6 的协议号是基本头的 Next Header，不冒充完整扩展头解析结果。不记录地址、端口或负载。

`origin=ndis-return` 时，`ndis_status_observed=true`，原始值来自真实 NDIS 归还。
`origin=local-cancel-before-indication` 时，该标志为 false；NBL 中的 PAUSED 值是驱动自行设置的，
不能归为 NDIS 拒收。累计计数与保留窗口分开，覆盖后 `overwritten_records` 明确显示历史缺口。

归还回调在释放 NBL 前捕获状态；包形状来自构造时已有的私有前缀快照，归还时不重新读取共享包负载。
普通成功归还不进入新增诊断锁。异常批次最多增加一次独立锁，最多检查 32 个原始描述符；
不分配诊断对象、不打印日志、不读时钟、不新增等待。查询使用 METHOD_BUFFERED 输出，
不会把完整快照放到内核栈上。诊断锁不与 RxLock 嵌套，也不跨 NDIS upcall。

## 原机器采集

使用与新 SYS 同一源码指纹构建的 `pbprobe.exe`：

```powershell
.\pbprobe.exe --list
.\pbprobe.exe --inspect '<上一步的设备路径>'
.\pbprobe.exe --rx-returns '<设备路径>' | Out-File -Encoding utf8 rx-returns.json
```

这些命令只查询已有适配器，不创建适配器、安装驱动或改变卸载策略。
应在产生告警的客户端仍运行时采集，并核对 `matching_build`、epoch、队列及 generation。
先用 `--inspect` 核实实际驱动指纹、effective mask 和 URO 事件数，再分析 raw 状态、包形状及来源。
正值不能简单等同错误，未知负值也不能推测原因；必须对照原始 NDIS 定义及实际返回路径。

## 构建及签名

产品版本固定 2.0.0（PE 2.0.0，INF 2.0.0.0），x64。Rust SDK 包标识同步为 2.0.0。
SYS 修改会产生新源码指纹。原签名三件套及旧 DLL 保留不变；新 SYS 需要重新签名，
随后用同一冻结源码构建 FinalApi，嵌入签名后的 SYS/INF/CAT。旧 DLL 不能与新指纹 SYS 混配。
构建时在生成目录前给输出 INF 写入指纹注释，以区分固定版本号下的冻结包；目录生成后不得改 INF。
工具输出匹配标志不代表运行时内核签名/加载或数据正确性已经验收。

未签名测试使用单独的 `UnsignedTestApi` 阶段，输入为同指纹 Driver 阶段三件套及其 manifest：

```powershell
pwsh -File tools/build/build.ps1 -Stage UnsignedTestApi -UnsignedDriverDirectory '<构建目录>/driver' -ExpectedManifest '<构建目录>/driver/manifest.json'
```

该阶段核对三份文件 SHA256、SYS 的 x64/版本/源码身份及未签名状态，结果明确标记测试用途；
不修改 FinalApi 的签名验证，也不修改启动策略。保留冻结输出，重新编译不覆盖已签名或已有封装包。
原机器已确认旧 PacketBridge 驱动包会干扰切换：按用户要求，每次启动客户端测试前先退出客户端、
清理之前的 PacketBridge 驱动和旧设备，必要时重启；未签名测试重启继续选择 F7。

回归覆盖原始状态在 NBL 释放/复用后保持、可变负载不改变诊断元数据、本地取消、普通/合并、
IPv4/IPv6、满完成环、并发归还、查询短缓冲区、64 条覆盖、并发读写、JSON ABI 校验、
guarded 内存、token/引用/句柄清理。最初旧构建的 58 条 raw 状态仍不可恢复；本次原机快照只证明所保留的 24 条协议不可达。
