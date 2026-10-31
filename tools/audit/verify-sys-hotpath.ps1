param([string]$BaselineDirectory='out\sys-hotpath-baseline\0112f76b2c1a')
# Operation-count and bounded-progress A/B checks, not a kernel timing benchmark.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-hotpath-validation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldSession=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session.h'))
$start=$oldSession.IndexOf('static BOOLEAN PbV2ReceiveBatch(')
$end=if($start -ge 0) {$oldSession.IndexOf('#include "session_worker.h"',$start)} else {-1}
if($start -lt 0 -or $end -le $start) {throw 'Expected preserved pre-boundary receive function in baseline session.h'}
$oldReceive=Join-Path $directory 'receive-before.h'
[IO.File]::WriteAllText($oldReceive,$oldSession.Substring($start,$end-$start),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$evidence=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant; New-Item -ItemType Directory -Path $objects -Force | Out-Null
    foreach($case in @('return','drain')) {
        $lines=@()
        if($variant -eq 'before') {
            if($case -eq 'return') {
                $lines+=@('#define PB_RETURN_BASELINE 1',('#define PB_NBL_SOURCE "'+(Join-Path $baseline 'driver\v2\session_nbl.h').Replace('\','/')+'"'))
            } else {
                $lines+=@('#define PB_DRAIN_BASELINE 1',('#define PB_RECEIVE_SOURCE "'+$oldReceive.Replace('\','/')+'"'),
                    ('#define PB_WORKER_SOURCE "'+(Join-Path $baseline 'driver\v2\session_worker.h').Replace('\','/')+'"'),
                    ('#define PB_QUERY_SOURCE "'+(Join-Path $baseline 'driver\v2\session_query.h').Replace('\','/')+'"'))
            }
        }
        $fixture=if($case -eq 'return') {'tests\model\return_batch_tests.c'} else {'tests\model\drain_boundary_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $fixture).Replace('\','/')+'"'
        $source=Join-Path $objects "$case.c"; $lines | Set-Content -LiteralPath $source -Encoding utf8
        $sources=@('common\v2_core.c','common\ethernet.c')
        if($case -eq 'drain') {$sources+=@('common\coalesce.c','common\packet.c')}
        $exe=Join-Path $objects "$case.exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+$sources+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $evidence+=@{variant=$variant;case=$case;exe=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
$hashes=foreach($entry in @(
    @('before','driver\v2\session_nbl.h'),@('before','driver\v2\session.h'),@('before','driver\v2\session_worker.h'),@('before','driver\v2\session_query.h'),
    @('after','driver\v2\session_return.h'),@('after','driver\v2\session_receive.h'),@('after','driver\v2\session_drain.h'),@('after','driver\v2\session_worker.h'),@('after','driver\v2\session_query.h'))) {
    $root=if($entry[0] -eq 'before') {$baseline} else {$script:RepoRoot}
    @{variant=$entry[0];source=$entry[1];sha256=(Get-FileHash -LiteralPath (Join-Path $root $entry[1])).Hash.ToLowerInvariant()}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during A/B verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;kernel_runtime_tested=$false;
    method='Frozen before function bodies and current production functions, same hosted DDI/core fixture; no driver CPU/latency/throughput measurement';
    operation_record=$script:TestResultFile;executables=$evidence;source_hashes=$hashes} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
