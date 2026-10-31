param([string]$BaselineDirectory='out\paused-recycle-baseline')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$oldWorker=Join-Path $baseline 'driver\v2\session_worker.h'
$oldHash=(Get-FileHash -LiteralPath $oldWorker).Hash.ToLowerInvariant()
if($oldHash -ne '87f7d9af49ea9e21da23b0392a18323b71f5cd8d65016c571d6904da95f476d9') {throw 'Unexpected paused-recycle baseline worker'}
$directory=Join-Path $script:RepoRoot "out\sys-paused-recycle\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('ms','iso')) {
    $objects=Join-Path $directory "before-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $source=Join-Path $objects 'worker.c'
    @(('#define PB_WORKER_SOURCE "'+$oldWorker.Replace('\','/')+'"'),
        ('#include "'+(Join-Path $script:RepoRoot 'tests\model\worker_poll_tests.c').Replace('\','/')+'"')) |
        Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $objects 'worker.exe'
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\driver\v2", "/Fo$objects\")+$script:VersionArguments+@($source,'/link',"/out:$exe"))
    $lines=@(& $exe 2>&1 | ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
    $log=Join-Path $objects 'expected-regression.log';$lines | Set-Content -LiteralPath $log -Encoding utf8
    $expected=$code -eq 1 -and ($lines -join "`n").Contains('PublishedReturn && !LostRecycleWaits && !LostFaultWaits && Exits==1 && !s.Faulted')
    $script:TestExecutions.Add([ordered]@{program=$exe;exit_code=$code;expected_exit_code=1;expected_regression_observed=$expected;log=$log})
    Write-TestRecording $(if($expected){'running'}else{'failed'})
    if(-not $expected) {throw "Frozen worker did not reproduce the expected paused-return failure: $mode"}
    $executables+=@{variant='before';volatile=$mode;case='worker';path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    Write-Output "PASS: frozen worker reproduces paused-return regression ($mode), actual exit=$code"
    foreach($guarded in @($false,$true)) {
        $memory=if($guarded){'guarded'}else{'ordinary'}
        $objects=Join-Path $directory "after-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        foreach($case in @('worker','drain')) {
            if($guarded -and $case -eq 'worker') {continue}
            $test=if($case -eq 'worker'){'tests\model\worker_poll_tests.c'}else{'tests\model\drain_boundary_tests.c'}
            $source=Join-Path $objects "$case.c";$lines=@()
            if($guarded) {$lines+='#define PB_GUARD_TESTS 1'}
            $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
            $lines | Set-Content -LiteralPath $source -Encoding utf8
            $sources=if($case -eq 'worker'){@($source)}else{@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c',$source)}
            $exe=Join-Path $objects "$case.exe"
            Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+$sources+@('/link',"/out:$exe"))
            Invoke-TestTool $exe @()
            $executables+=@{variant='after';volatile=$mode;case=$case;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during paused-recycle verification'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_worker_sha256=$oldHash;
    current_worker_sha256=(Get-FileHash 'driver\v2\session_worker.h').Hash.ToLowerInvariant();
    method='Expected frozen-worker regression failure; actual current worker/recycle/core, deterministic schedules and real host waits in MS/ISO volatile modes, ordinary/guarded NBL fixtures. Operation counts only, no kernel CPU measurement.';
    paused_stop_review='Current drain fixtures also run 16 real-host-wait combinations: sync/manual kernel event, gated-before-wait/ungated-host-wait path, ordinary/wrapped RX and recycle cursors, DRAIN/Fault. All four private TX slots are actually reserved/published before setting the full-pool proof. One raw return token is published with its separate event notification deliberately deferred; exactly one DRAIN/Fault wake must suffice. Normal drain retires exactly that TX token and only the three initial RX descriptors, then parks without idle spin; fault exits with private quiescence but leaves all unconsumed TX slots published and non-reusable. QUERY ignores later hostile shared tails. No production implementation change or native IRQL/CPU claim.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
