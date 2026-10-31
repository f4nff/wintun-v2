# SYS / DLL 最后一代 token 归还与回收边界复核

从完整验收指纹 `6ab47f7fa66206c6db46701933a11b23fdda00690f74aab704d08635766c0f66` 继续检查，产品版本保持 **2.0.0**，PE 数值版本 **2.0.0.0**。本轮未确认新的生产缺陷，104 个生产源码及版本属性文件逐项保持一致。此前 Drain 等待失败修正、槽位换代、rundown、暂停唤醒和关闭重试实现均保留。

## 核对结论

池允许最后一次有效 generation `UINT32_MAX` 被保留、发布和正常归还。下一次分配必须永久退役该槽位，返回 FULL 时不改写输出 token；不得把 generation 加到零后重新发布。原有 pool model 已覆盖全部 512 种池大小、非二次幂尾部、稀疏空闲索引和代次耗尽，本轮补充实际 DLL／SYS 队列路径的组合。

TX 的 native 私有池与 DLL 镜像池可以处于不同但合法的状态：实际 recycle 后 native 池在再次分配时退役最后一代槽位，DLL 镜像仍可为同代 FREE。DLL TX 镜像只导入原生发布的新 token，不会自行分配该槽位；这不授权零代 token 或旧代 token 再次进入队列。

native STOPPED 与 DLL 借用也不能合并成一个条件。RX NBL 已归还、提交边界与完成发布都已结束时，QUERY 可以返回 STOPPED，但应用仍可能持有 TX lease。反方向，DLL loan 已全部归还时，实际 RX NBL 仍可使 QUERY 保持 DRAINING。现有关闭流程分别检查用户借用、可信原生确认和物理关闭屏障；不据此调整其顺序。

## 新增 24 个组合

扩展既有 `return_reclaim_tests.c`，保留原有 64 个清理交错和 72 个同槽换代交错，新增三种 RX 路径 × 两种归还顺序 × 两种 KernelEvent × 两种游标起点，共 24 个场景：

- 普通 IPv4 TCP 单包、四个原始包的 IPv4 TCP RSC、四个原始包的 IPv6 UDP URO。RX 执行实际 Reserve／Commit／ReceiveBatch／Return／Poll，合并经过真实解析、校验和及 NBL 构造。
- 在首次操作前将一致、空闲的私有代次置为 `UINT32_MAX-1`，模拟已完成此前轮次的边界，不执行数十亿次收发，也不在运行中改写活动 token。两方向实际取得最后一代 token。
- TX producer 由 fixture 使用实际 pool/ring helper 构造，DLL 执行实际 ReceiveBatch／ReleaseBatch，SYS 执行实际 recycle；不执行 SYS Send 或真实 NDIS send completion。该模型对应发送已经完成、复制槽位仍由用户借用的阶段。
- 游标从零或 `UINT64_MAX` 开始；后者连单包也必须跨越回绕。检查四条环两侧的 private cursor 和 shared producer／consumer，永久退役不得产生新条目。

RX 先归还时，实际 DLL Poll 只释放 RX Published，保留仍存在的 TX loan。运行状态下再次 Reserve 必须将耗尽的 RX 槽位退役，返回 FULL、count=0，不改变 TX loan 或提交游标。随后开始 native drain，QUERY 返回 STOPPED 且 Active=0，同时断言 TX loan 仍存在。最终实际 Release／Recycle 才归还这些借用。

TX 先归还时，实际 Release 唤醒已 arm 的 recycle consumer，实际 recycle 归还私有 native 槽位并撤销旧的满池缓存。再次 native reserve 永久退役全部耗尽槽位，FULL 保留输出哨兵；重复用户归还必须返回 BUSY、accepted=0，不产生第二次 recycle。随后开始 drain，用户 loan 已为零，但 RX NBL 仍使 QUERY 保持 DRAINING／Active=1，直到实际归还。

停止后的 Reserve 一律返回 STOPPED，不改变 RX 私有池和索引：运行时已退役者仍为 RETIRED，尚未再次分配者仍为最后一代 FREE。两类状态均不能绕过停止接纳。每例检查末代 completion 的完整 token、Native Active／AllReturned／rundown、credit／pending、合并租约、通知和全部跟踪资源／进程句柄平衡，再执行实际 native Free。

本轮复用并抽出 user 侧 fixture 构造／释放 helper；原同槽换代场景的行为和断言保留。Poll helper 新增显式预期 loan 参数，原场景仍要求零 loan，新场景明确验证 RX completion 不会清除独立 TX loan。

## 反例与维护收益

既有 `verify-sys-return-reclaim.ps1` 继续作为唯一专项入口。四个当前实现进程按 ISO／MS volatile、普通／守护内存运行全部 64 + 72 + 24 场景。保留原有 16 个屏障／旧尾部错误反例，增加四个仅存在于 `out` 的 core 反例：只删除永久退役的 exhaustion guard。

新反例单独运行末代场景，必须在第一次耗尽后的 native reserve 处以退出码 1 命中精确断言；尚未向任何环发布回绕 token。崩溃、超时或其他断言失败不算通过。脚本记录 24 个进程，其中四个正向、20 个预期拒绝，另保存 core/header／可执行文件哈希并校验源树指纹。

标准 Tests、Safety 和完整生命周期审计沿用已有入口，测试进程数不增加。这样后续调整空闲索引、代次检查、满池缓存或回收条件时，能自动捕获跨两侧私有状态的回归，无需新增生产状态或同步分支。

本轮没有增加生产锁、轮询、事件、定时器或热路径指令，没有修改 FULL／STOPPED 的 ABI 语义，也不自动把临时满池等同于代次耗尽。通知与分配次数仅为宿主行为断言，不是 CPU 测量，不宣称 CPU 降幅。

DDI 使用宿主替身，锁／事件／测试线程使用真实 Windows 实现；user session 由 fixture 组装，不运行完整 DLL open／close 或 queue worker。真正与清理线程交错的 AllReturned／rundown 屏障仍由原 64 场景验证；完整 DLL 关闭与发送上层回调生命周期由既有专项覆盖。没有安装、签名或加载 SYS；真实内核 CPU、NDIS／IRQL、Verifier、低资源和长稳仍待实机验收，`kernel_runtime_tested=false`、`production_ready=false`。
