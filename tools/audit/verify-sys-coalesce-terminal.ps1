param([string]$BaselineDirectory='out\sys-coalesce-terminal-baseline\f9fdec6f74d1')
# Frozen/current orchestration with the same coalescing core and NBL lifecycle.
# Copy/call counts are hosted evidence, never kernel performance measurements.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-coalesce-terminal\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldHelper=Join-Path $baseline 'driver\v2\session_coalesce.h'
$receive=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_receive.h'))
$start=$receive.IndexOf('static BOOLEAN PbV2ReceiveBatch(')
if($start -lt 0){throw 'Cannot locate actual receive function'}
$headers=@(('#include "'+(Join-Path $script:RepoRoot 'driver\v2\session_drain.h').Replace('\','/')+'"'),
    ('#include "'+$oldHelper.Replace('\','/')+'"'),
    ('#include "'+(Join-Path $script:RepoRoot 'driver\v2\session_rx_stats.h').Replace('\','/')+'"'))
$oldReceive=Join-Path $directory 'receive-before.h'
[IO.File]::WriteAllText($oldReceive,($headers -join "`n")+"`n"+$receive.Substring($start),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    foreach($case in @('coalesce','receive','receive-guarded')) {
        $objects=Join-Path $directory "$variant-$case";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $lines=@();if($case -eq 'receive-guarded'){$lines+='#define PB_GUARD_TESTS 1'}
        if($variant -eq 'before') {
            $lines+='#define PB_COALESCE_TERMINAL_BASELINE 1'
            if($case -eq 'coalesce'){$lines+='#define PB_COALESCE_SOURCE "'+$oldHelper.Replace('\','/')+'"'}
            else{$lines+='#define PB_RECEIVE_SOURCE "'+$oldReceive.Replace('\','/')+'"'}
        }
        $test=if($case -eq 'coalesce'){'tests\model\coalesce_path_tests.c'}else{'tests\model\drain_boundary_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
        $source=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $sources=@('common\v2_core.c','common\coalesce.c');if($case -ne 'coalesce'){$sources+=@('common\packet.c','common\ethernet.c')}
        $exe=Join-Path $objects 'fixture.exe'
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
            $sources+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during short-final checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_helper_sha256=(Get-FileHash -LiteralPath $oldHelper).Hash.ToLowerInvariant();
    current_helper_sha256=(Get-FileHash 'driver\v2\session_coalesce.h').Hash.ToLowerInvariant();
    method='Frozen/current actual kernel coalescing helper; same shared core, actual ReceiveBatch/NBL/reference/return/completion/query and hosted DDI. Count scratch snapshots/append calls, compare entire output/state to direct core, check IPv4/IPv6 OOB, allocation failures, synchronous native return and late fault. Standalone buffers always guarded; receive runs ordinary and guarded. Existing real wait and drain cases run in both receive variants. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
