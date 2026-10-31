param([string]$BaselineDirectory='out\sys-reference-store-baseline\5028998bf71e')
# Same lock/event/ownership protocol, frozen RMW updates versus release stores.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-reference-store\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'driver\v2\session_references.h'
$old=[IO.File]::ReadAllText($oldPath)
# One progress-helper include identity in the actual send/receive fixtures.
$progress='#include "'+(Join-Path $script:RepoRoot 'driver\v2\session_progress.h').Replace('\','/')+'"'
$oldSource=Join-Path $directory 'references-before.h'
[IO.File]::WriteAllText($oldSource,$old.Replace('#include "session_progress.h"',$progress),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    foreach($case in @('reference-iso','reference-ms','send','receive','receive-guarded')) {
        $objects=Join-Path $directory "$variant-$case";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $lines=@()
        if($variant -eq 'before') {
            $lines+='#define PB_REFERENCE_RMW_BASELINE 1'
            $lines+='#define PB_REFERENCE_SOURCE "'+$oldSource.Replace('\','/')+'"'
        }
        if($case -eq 'receive-guarded'){$lines+='#define PB_GUARD_TESTS 1'}
        $test=if($case -like 'reference-*'){'tests\model\receive_reference_tests.c'}elseif($case -eq 'send'){'tests\model\send_window_tests.c'}else{'tests\model\drain_boundary_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
        $source=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $sources=@();if($case -like 'receive*'){$sources+=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')}
        $exe=Join-Path $objects 'fixture.exe';$volatile=if($case -eq 'reference-ms'){'/volatile:ms'}else{'/volatile:iso'}
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',$volatile,'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+$sources+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during reference-store checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_helper_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();current_helper_sha256=(Get-FileHash 'driver\v2\session_references.h').Hash.ToLowerInvariant();
    method='Frozen/current actual reference helpers with unchanged ReferenceLock and AllReturned. Count 130 overlapping ownership updates, assert every write/event owns the lock and count is published before event changes. Each reference run adds 300000 concurrent retain cycles, 100000 actual QUERY observations, single/batched releases, native waiter and zero handle delta. Both volatile modes; actual send and ordinary/guarded receive/return/worker/query paths run with both helpers. Not kernel CPU/latency/throughput.';
    extra_reference_rmws_per_130_updates_before=130;extra_reference_rmws_per_130_updates_after=0;
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
