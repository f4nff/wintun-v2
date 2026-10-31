# SYS TX recycle token snapshots

The TX recycle path uses only `PB_DESCRIPTOR.Token` to validate and release a
private published slot. It previously copied each entire 64-byte descriptor into
a 32-entry stack array. `PbQueuePopTokens` now snapshots the 16-byte tokens into
a 512-byte array instead of a 2048-byte descriptor array. A full 128-item recycle
budget copies 2048 descriptor bytes rather than 8192. These are copy-byte and
source-array sizes, not measured kernel CPU or total stack-frame savings.

The shared ABI still uses 64-byte descriptors. Producers and ring layout do not
change. Both consumer APIs use the same `Available` cursor validation/acquire
observation and `PopCommit` release publication. Every token is copied before
publishing the consumer cursor, so immediate producer reuse cannot overwrite the
private snapshot. The token reader does not examine descriptor length or other
unused metadata; the previous unlimited full reader did not use them to reject
a recycle either.

`PbV2RecycleLocked` still runs under the caller's TX lock, validates every token
against the private pool's epoch, queue, slot, generation and published state,
and stops on the first fault. Successful prefixes remain retired; later tokens
cannot authorize work after a fault. Each call retains its four-batch/128-item
budget. No event policy, wait set, deadline, drain boundary, reference release or
NDIS callback ordering changes.

## Reproducible verification

`tools/audit/verify-sys-recycle-token.ps1` compares the frozen pre-change readers
with current code. The queue fixture runs 7744 differential cases per variant
for tiny/4096-entry rings, partial batches, arbitrary unused metadata and uint64
wrap. Guard pages bound the output and separately measure the 16-byte source
read footprint using a deliberately partial synthetic entry; real attached
rings map complete descriptors. Deterministic schedules force publication
before/during/after arm and slot reuse before the read returns. Concurrent native
threads transfer 400000 exact tokens per run through capacity-1 and capacity-64
rings using event waits. Both MSVC volatile modes and balanced handles are checked.

The actual recycle/send fixture compares 8192 versus 2048 copied bytes per
128-item budget and checks valid prefixes, corrupt cursors, stale/duplicate
tokens across ring and cursor wrap, remaining slot ownership and post-fault send
admission. Existing worker, backpressure, return, finite drain and callback-tail
tests remain part of full offline validation. The new queue test is included in
ordinary and guarded safety builds.

These checks use hosted DDI fixtures, offline WDK builds, static analysis and
native emitted-code inspection. They do not execute the SYS in a kernel or
establish real IRQL, Verifier, ETW/WPA, CPU, latency, throughput or long-stress
results.
