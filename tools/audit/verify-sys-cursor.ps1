param([string]$BaselineDirectory='out\sys-cursor-baseline\55aed283acc8')
# Actual core A/B, volatile-mode coverage and compiler output; no driver loading.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-cursor\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'common\v2_core.c'
$old=[IO.File]::ReadAllText($oldPath)
if(-not $old.Contains('_InterlockedCompareExchange64((volatile __int64 *)p, 0, 0)')) {throw 'Expected frozen RMW cursor reader'}
# Only adapt include identity, preventing duplicate pragma-once type declarations
# in the fixture; do not edit the frozen functions or their shared-memory protocol.
$oldCore=Join-Path $directory 'core-before.c'
$old=$old.Replace('#include "v2_core.h"',('#include "'+(Join-Path $script:RepoRoot 'common\v2_core.h').Replace('\','/')+'"'))
[IO.File]::WriteAllText($oldCore,$old,[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@();$assembly=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $lines=@()
    if($variant -eq 'before') {$lines+=@('#define PB_CURSOR_RMW_BASELINE 1',('#define PB_CORE_SOURCE "'+$oldCore.Replace('\','/')+'"'))}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\queue_order_tests.c').Replace('\','/')+'"'
    $source=Join-Path $objects 'queue-fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
    foreach($mode in @('iso','ms')) {
        $exe=Join-Path $objects "queue-$mode.exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;volatile_mode=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
    $core=if($variant -eq 'before') {$oldCore} else {Join-Path $script:RepoRoot 'common\v2_core.c'}
    $listing=Join-Path $objects 'core.asm'
    Invoke-BuildTool 'cl.exe' @('/nologo','/c','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\core.obj",'/FAs',"/Fa$listing",$core)
    $instructions=[IO.File]::ReadAllText($listing)
    $rmw=[regex]::Matches($instructions,'(?m)^\s+lock\s+cmpxchg\s+QWORD\b').Count
    if(($variant -eq 'before' -and $rmw -eq 0) -or ($variant -eq 'after' -and $rmw -ne 0)) {throw "Unexpected x64 cursor RMW emission: $variant $rmw"}
    $publish=[regex]::Matches($instructions,'(?m)^\s+xchg\s+QWORD\b').Count
    $handshake=[regex]::Matches($instructions,'(?m)^\s+xchg\s+DWORD\b').Count
    if(-not $publish -or -not $handshake) {throw 'Cursor publication/Armed exchange instructions missing'}
    $assembly+=@{variant=$variant;listing=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant();emitted_locked_cmpxchg64_instructions=$rmw;
        emitted_cursor_exchange64_instructions=$publish;emitted_handshake_exchange32_instructions=$handshake}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during cursor A/B checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_core_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();current_core_sha256=(Get-FileHash 'common\v2_core.c').Hash.ToLowerInvariant();
    method='Frozen and current actual core with identical hosted API/intrinsic instrumentation. Three read observations, deterministic arm/reuse schedules, 600000 concurrent descriptors per executable, both volatile modes; separate uninstrumented x64 compiler listings. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;assembly=$assembly} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
