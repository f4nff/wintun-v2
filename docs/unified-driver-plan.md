# Unified DLL and v2 driver

The shipping architecture has one DLL per process architecture and one native
SYS in its driver package. `Wintun*` retains the published userspace contract;
`Pb*` retains the versioned batch ABI. Both use the v2 kernel protocol. The
compatibility adapter owns private byte rings and one joined queue-owner worker;
no legacy ring registration is sent to the driver.

The native compatibility target is Windows 7 SP1 x64 through current Windows.
This is an engineering target, not a runtime certification. Each supported OS
must pass signed installation, connectivity, Driver Verifier, suspend/resume,
adapter removal and sustained resource accounting before release. Newer kernel
imports must not prevent the minimum OS from loading the binary. Unsupported
offloads are absent from capability reports, not merely disabled by default.

Implementation gates:

1. Pure platform capability policy with tests for every NDIS version boundary.
2. Runtime NDIS structure revisions, downlevel allocation, INF and import audit.
3. Wintun byte-ring compatibility over v2, ordered multithreaded publication,
   bounded backpressure, deterministic worker join and failure rollback tests.
4. Shared installer/device identity, combined exports and single-package build.
5. Full hosted model/safety/native Rust regressions, DLL/SYS builds and import
   checks. Runtime driver acceptance remains separately recorded as untested
   until executed on each target OS.

The implementation now includes these gates in `tools/build/validate-unified.ps1`.
The isolated release directory contains `wintun.dll` with one numeric-resource
SYS/INF/CAT package. Legacy export ordinals 1–14 are frozen; extensions follow
them. Both APIs use the same downlevel installer, with a stable hash-derived
instance identity for the batch API and original requested-GUID behavior for
Wintun callers. The old standalone installer is retained only as a historical
regression fixture and is absent from the shipping link.

Compatibility tests exercise four producers and four consumers, 4,000 ordered
or unique packets in each direction, maximum-sized payloads and wrapping,
out-of-order publication/return, null/duplicate pointers, every constructor
acquisition failure, CNG provider/hash failure, exhausted ring admission,
full-ring cancellation, delayed native references and repeated complete unload.
The owner worker is joined before private rings are freed. Native cleanup is
handed to the existing pinned runtime if kernel references outlive the public
close deadline. Persistent native references remain visible as retained state;
they never authorize freeing memory still owned by NDIS.

`out/unified-validation/summary.json` records actual runs and keeps kernel
runtime acceptance false. Static analysis retains raw SARIF including WDK SAL
and ownership-transfer diagnostics for review; a successful analysis invocation
does not mean it emitted no diagnostics. No signing certificate, installation,
boot-policy change or kernel runtime success is inferred from hosted tests.

Old official DLLs are not clients of this new SYS. Existing applications load
the new compatibility DLL. Existing official Wintun devices are never silently
adopted, upgraded or removed. Native v2 clients keep their packet extensions and
explicit experimental offload opt-in. The C DLL baseline does not lower the
minimum OS required by an application's Rust runtime or other dependencies.
