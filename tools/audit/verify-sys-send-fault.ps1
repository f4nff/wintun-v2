param([string]$BaselineDirectory='out\sys-send-fault-baseline\a5bef19f1073')
# Actual send/recycle/worker bodies with identical hosted fixtures; no SYS loaded.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-send-fault\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    foreach($case in @('send','worker')) {
        $lines=@()
        if($variant -eq 'before') {
            $lines+='#define PB_SEND_FAULT_BASELINE 1'
            if($case -eq 'send') {
                foreach($entry in @(@('PB_SEND_SOURCE','session_send.h'),@('PB_RECYCLE_SOURCE','session_recycle.h'))) {
                    $lines+='#define '+$entry[0]+' "'+(Join-Path $baseline ('driver\v2\'+$entry[1])).Replace('\','/')+'"'
                }
            } else {$lines+='#define PB_WORKER_SOURCE "'+(Join-Path $baseline 'driver\v2\session_worker.h').Replace('\','/')+'"'}
        }
        $fixture=if($case -eq 'send') {'tests\model\send_window_tests.c'} else {'tests\model\worker_poll_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $fixture).Replace('\','/')+'"'
        $source=Join-Path $objects "$case-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects "$case.exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'/link',"/out:$exe"))
        $testArguments=if($variant -eq 'before' -and $case -eq 'send') {@('--historical-send')}else{@()}
        Invoke-TestTool $exe $testArguments
        $executables+=@{variant=$variant;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
$hashes=foreach($variant in @('before','after')) {
    $root=if($variant -eq 'before') {$baseline} else {$script:RepoRoot}
    foreach($file in @('driver\v2\session_send.h','driver\v2\session_recycle.h','driver\v2\session_worker.h')) {
        @{variant=$variant;source=$file;sha256=(Get-FileHash -LiteralPath (Join-Path $root $file)).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during send/fault A/B checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    method='Frozen/current actual send, recycle and worker; shared current queue/Ethernet core and hosted DDI. Recycle-prefix ownership, post-fault publication, worker quiescence, control-call counts and guarded actual Ethernet classification. Not kernel CPU/latency/throughput.';
    historical_scope='Frozen Send/worker use their original running, budget, ownership, fault and callback contracts; current variants also run later diagnostic/persistent-pool/late-return and paused-recycle regressions. Frozen bodies remain unchanged.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;source_hashes=$hashes} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
