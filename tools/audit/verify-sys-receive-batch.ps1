param([string]$BaselineDirectory='out\sys-receive-batch-baseline\1a1a72e2e21c')
# Actual ReceiveBatch A/B with real hosted NBL/return/core paths, including guards.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-receive-batch\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'driver\v2\session_receive.h'
$old=[IO.File]::ReadAllText($oldPath);$start=$old.IndexOf('static BOOLEAN PbV2ReceiveBatch(')
if($start -lt 0) {throw 'Expected frozen ReceiveBatch function'}
# Preserve the old function, using current helper include identities to avoid
# duplicate pragma-once definitions in the current worker fixture.
$headers=@('session_drain.h','session_coalesce.h') | ForEach-Object {
    '#include "'+(Join-Path $script:RepoRoot ('driver\v2\'+$_)).Replace('\','/')+'"'
}
$oldReceive=Join-Path $directory 'receive-before.h'
[IO.File]::WriteAllText($oldReceive,($headers -join "`n")+"`n"+$old.Substring($start),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    foreach($guarded in @($false,$true)) {
        $lines=@();$case=if($guarded) {'guarded'} else {'ordinary'}
        if($variant -eq 'before') {$lines+=@('#define PB_RX_BATCH_BASELINE 1',('#define PB_RECEIVE_SOURCE "'+$oldReceive.Replace('\','/')+'"'))}
        if($guarded) {$lines+='#define PB_GUARD_TESTS 1'}
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\drain_boundary_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects "$case-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects "$case.exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
            @('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c',$source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during receive batch A/B checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_receive_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();current_receive_sha256=(Get-FileHash 'driver\v2\session_receive.h').Hash.ToLowerInvariant();
    current_stats_sha256=(Get-FileHash 'driver\v2\session_rx_stats.h').Hash.ToLowerInvariant();
    method='Frozen/current actual ReceiveBatch, same hosted NBL allocation/return/reference/worker/query and packet/coalescing core. Count stats record reads/atomics and private-cancel locks/rundown/wakes; guarded failure, real native ownership and synchronous upcall return. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
