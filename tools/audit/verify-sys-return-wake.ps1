param([string]$BaselineDirectory='out\sys-return-wake-baseline\b260b7c381cb')
# Frozen/current actual return functions, identical NBL construction and hosted
# receive/worker/query/DDI; includes real waiters and guarded allocations.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-return-wake\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'driver\v2\session_return.h';$old=[IO.File]::ReadAllText($oldPath)
$receive=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_nbl.h'))
$start=$old.IndexOf('typedef struct PB_RETURN_ITEM');$receiveStart=$receive.IndexOf('static NET_BUFFER_LIST *PbV2Receive(')
if($start -lt 0 -or $receiveStart -lt 0){throw 'Cannot locate preserved return/receive functions'}
$oldNbl=Join-Path $directory 'nbl-before.h'
[IO.File]::WriteAllText($oldNbl,$old.Substring($start)+"`n"+$receive.Substring($receiveStart),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    foreach($guarded in @($false,$true)) {
        $mode=if($guarded){'guarded'}else{'ordinary'}
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        foreach($case in @('return','wait')) {
            $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
            if($variant -eq 'before') {$lines+=@('#define PB_RETURN_WAKE_BASELINE 1',('#define PB_NBL_SOURCE "'+$oldNbl.Replace('\','/')+'"'))}
            $test=if($case -eq 'return'){'tests\model\return_batch_tests.c'}else{'tests\model\drain_boundary_tests.c'}
            $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
            $source=Join-Path $objects "$case-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
            $exe=Join-Path $objects "$case.exe"
            $sources=@('common\v2_core.c','common\ethernet.c');if($case -eq 'wait'){$sources+=@('common\coalesce.c','common\packet.c')}
            Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
                $sources+@($source,'/link',"/out:$exe"))
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;guarded=$guarded;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during return-wake checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_return_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();
    current_return_sha256=(Get-FileHash 'driver\v2\session_return.h').Hash.ToLowerInvariant();
    method='Frozen/current actual return functions, same NBL construction, completion/reference/core and actual worker/query. Count KernelEvent writes and idle receive scans. Real host threads/events cover normal/paused idle, full credit, before-wait notification, normal drain, full completion backpressure and two concurrent return chains. Ordinary/guarded runs verify exact tokens/statistics, real native ownership and balanced resources. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
