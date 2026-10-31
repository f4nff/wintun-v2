param([string]$BaselineDirectory='out\sys-parse-cancel-baseline\ad2b583e2e32')
# Frozen production function bodies, same hosted DDI/core; operation counts, not kernel timing.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-parse-cancel\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
$sessionPath=Join-Path $baseline 'driver\v2\session.h'
$session=[IO.File]::ReadAllText($sessionPath)
$start=$session.IndexOf('static NDIS_STATUS PbV2Describe(')
$end=if($start -ge 0) {$session.IndexOf('#include "session_send.h"',$start)} else {-1}
if($start -lt 0 -or $end -le $start) {throw 'Expected preserved Describe body before session_send.h'}
$oldDescribe=Join-Path $directory 'describe-before.h'
[IO.File]::WriteAllText($oldDescribe,$session.Substring($start,$end-$start),$encoding)
$receivePath=Join-Path $baseline 'driver\v2\session_receive.h'
$receive=[IO.File]::ReadAllText($receivePath)
$start=$receive.IndexOf('static BOOLEAN PbV2ReceiveBatch(')
if($start -lt 0) {throw 'Expected preserved ReceiveBatch body'}
# Use current helper include identities to avoid duplicating pragma-once headers
# later included by the worker. The frozen receive function itself is unchanged.
$headers=@('session_drain.h','session_coalesce.h') | ForEach-Object {
    '#include "'+(Join-Path $script:RepoRoot ('driver\v2\'+$_)).Replace('\','/')+'"'
}
$oldReceive=Join-Path $directory 'receive-before.h'
[IO.File]::WriteAllText($oldReceive,($headers -join "`n")+"`n"+$receive.Substring($start),$encoding)
Start-TestRecording $directory 'operation-results.json'
$binaries=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    foreach($case in @('describe','receive')) {
        $lines=@()
        if($variant -eq 'before') {
            if($case -eq 'describe') {$lines+=@('#define PB_DESCRIBE_BASELINE 1',('#define PB_DESCRIBE_SOURCE "'+$oldDescribe.Replace('\','/')+'"'))}
            else {$lines+=@('#define PB_RX_CANCEL_BASELINE 1',('#define PB_RECEIVE_SOURCE "'+$oldReceive.Replace('\','/')+'"'))}
        }
        $fixture=if($case -eq 'describe') {'tests\model\describe_checksum_tests.c'} else {'tests\model\drain_boundary_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $fixture).Replace('\','/')+'"'
        $source=Join-Path $objects "$case-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
        $sources=if($case -eq 'describe') {@('common\packet.c')} else {@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')}
        $exe=Join-Path $objects "$case.exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+$sources+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $binaries+=@{variant=$variant;case=$case;exe=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during parse/cancel A/B checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_session_sha256=(Get-FileHash -LiteralPath $sessionPath).Hash.ToLowerInvariant();baseline_receive_sha256=(Get-FileHash -LiteralPath $receivePath).Hash.ToLowerInvariant();
    current_describe_sha256=(Get-FileHash 'driver\v2\session_describe.h').Hash.ToLowerInvariant();current_receive_sha256=(Get-FileHash 'driver\v2\session_receive.h').Hash.ToLowerInvariant();
    method='Frozen Describe/ReceiveBatch bodies versus current production bodies, same hosted DDI and current shared packet core. Header reparses, allocation/reference counts and wait predicates; not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$binaries} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
