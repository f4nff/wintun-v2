# 终止后的分批归还与 Rust 描述符重定位

版本固定为 **2.0.0**。本轮从 `02e1234fcd09`、694 个文件的完整字节基线继续复核，未确认新的生产缺陷。运行时源码保持原实现；Rust 文件的变化仅为其 `cfg(all(test, feature = "native-tests"))` 模块追加测试，原有 55744 字节逐字保留。

新增 [DLL 32 贷款归还回归](../tests/model/local_retirement32_tests.c)比较两种终止边界：仅发布 native terminal 提示而 DLL Fatal=0，以及在合法私有句柄的 SDK 等待边界显式注入 WAIT_FAILED，使实际 DLL 故障处理置 Fatal=1。后者是防御输入，不表示观察到 Windows 自然失败或生产缺陷；前者也不冒充完整 SYS Fault/Quiesce。

同一 owner 持有 32 个真实贷款，按调用者选择的 1/7/24 三批归还，实际 accepted 均等于请求。正常 terminal 分支的 recycle producer 累计为 1/8/32；Fatal 分支本地退役，producer 保持 0/0/0。两者贷款数均为 31/24/0。测试检查每一阶段消费旧停止提示后的真实 Progress 状态、五次实际 lifecycle 等待及零超时；非最终 PrepareClose 在贷款未清时返回 BUSY，清空后仍受独立 NativeActive=1 障碍约束。公开 owner 全部退出并汇合后才最终 Close，之后汇合捕获的回调线程对象并验证真实模块卸载、原资源账本归零和进程句柄相等。

新增 [Rust 两族回归](../rust/packetbridge/src/native_tests.rs)使用两个各含 16 段的 GSO 描述符，共输出 32 段。每次提供 32 个 caller 缓冲，但 C 实际 written=16；返回长度与未使用的 16 个缓冲分别核对。首描述符经真实 `release_prefix(1)` 归还后，同槽 generation 2 被不同的 plain 对象复用；原尾描述符重定位为 index 0，其指针、metadata、plan、源字节及随后生成的 16 段必须保持正确。测试不再读取已归还首贷款的数据，三个真实贷款最终全部归还，且没有 Reserve/Commit/Cancel 调用。每族 TCP/UDP 分案例，两个族由现有独立 cargo 进程执行。

SYS 复核还确认一个几何限制：正常唯一槽位所有权、ringCapacity≥SlotCount、每个 Return 窗口最多 64 个原始描述符且在同一 RxLock 内立即 flush 的条件下，普通 32 包不能自然耗尽四批 completion 预算。未消费的完成项和尚待完成的唯一槽互斥，不能填入额外完成项后称作正常 FULL。既有测试已单独覆盖四批续醒、满环等待、并发 Return 合并通知及实际 DLL Wait；本轮没有为这个不成立的反例扩大宿主。

包内 64 位标记仍是数据，未引入发送时间 API。Rust writer 每个描述符仅调用一次 checked batch writer；前缀归还移动的是 lease 元数据，不能把测试 oracle 的 clone 或必要的 caller 输出复制称为数据路径重复复制。USO 输入 checksum=0 的禁用计算指令也按 [Microsoft USO 合同](https://learn.microsoft.com/en-us/windows-hardware/drivers/network/udp-segmentation-offload-uso-)保留，不能与普通线上 IPv6 UDP 校验规则混用。

新 C 回归接入 UnifiedTests 和保护页 Safety，两族独立；Rust 原有 family/full 命令自动发现追加测试。专项入口为 [verify-local-retirement32.ps1](../tools/audit/verify-local-retirement32.ps1)，默认从当前源码独立生成，显式基线才要求完整历史身份。本轮封存与验收记录位于 `out/terminal-retirement-review/`。

以上为 hosted 验证：DLL 组运行实际队列、生命周期、SDK 等待与卸载，内核 I/O/状态由原 fixture 模拟；Rust 组使用测试 shim。SDK 入口和事件状态不等同内核线程已休眠，保护页也只覆盖声明的宿主对象。旧系统实机、真实 NDIS/IRQL、Driver Verifier、持续资源压力和 CPU 性能仍需目标机器验证。
