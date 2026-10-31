param([string]$BaselineDirectory='out\sys-receive-tail-baseline\16b66e4aa315')
# Actual receive/return/worker A/B with identical current helpers. No SYS load.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$oldPath=Join-Path $baseline 'driver\v2\session_receive.h'
if((Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant() -ne '7d32566986ca866cec00fd2c3179055a87f6b73e2dbafe2eedd3dc9f4d2ee6bb') {throw 'Unexpected receive-tail baseline'}
$directory=Join-Path $script:RepoRoot "out\sys-receive-tail\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$old=[IO.File]::ReadAllText($oldPath);$start=$old.IndexOf('static BOOLEAN PbV2ReceiveBatch(')
if($start -lt 0){throw 'Cannot locate frozen ReceiveBatch'}
$headers=@('session_drain.h','session_coalesce.h','session_rx_stats.h')|ForEach-Object {
    '#include "'+(Join-Path $script:RepoRoot ('driver\v2\'+$_)).Replace('\','/')+'"'
}
$oldReceive=Join-Path $directory 'receive-before.h'
[IO.File]::WriteAllText($oldReceive,($headers -join "`n")+"`n"+$old.Substring($start),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
    $memory=if($guarded){'guarded'}else{'ordinary'}
    $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $lines=@('#define PB_RX_TAIL_TESTS 1')
    if($variant -eq 'before') {$lines+='#define PB_RX_TAIL_BASELINE 1';$lines+='#define PB_RECEIVE_SOURCE "'+$oldReceive.Replace('\','/')+'"'}
    if($guarded) {$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\drain_boundary_tests.c').Replace('\','/')+'"'
    $source=Join-Path $objects 'fixture.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $objects 'fixture.exe'
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
        @('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c',$source,'/link',"/out:$exe"))
    Invoke-TestTool $exe @()
    $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during receive-tail checks'}
Write-TestRecording 'passed'
$hashes=foreach($path in @('driver\v2\session_receive.h','driver\v2\session_worker.h','driver\v2\session_return.h','driver\v2\session_completion.h','tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h')) {
    @{source=$path;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $path)).Hash.ToLowerInvariant()}
}
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_receive_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();
    method='Frozen/current actual ReceiveBatch, same current shared core and hosted NBL/return/worker/query functions. ISO/MS, ordinary/guarded. Count only locks acquired by the receive body: successful ordinary/coalesced/synchronous-return batches use 3 before and 2 after; local allocation failure retains 4. Real return thread holds RxLock: old receive waits for it, current receive completes while native slot/credit/Active/Returns remain held. Full completion ring freed during indication with/without consumer wake, uint64 submit wrap and 257 prior pending completions progress on bounded next worker turns before one idle wait. Native ownership, exact completions/stats, resource and joined-thread/handle checks remain. Return-window lock counts now measure actual rundown boundaries rather than subtracting an assumed receive tail. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;source_hashes=$hashes}|
    ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
