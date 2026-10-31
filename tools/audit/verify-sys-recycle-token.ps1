param([string]$BaselineDirectory='out\sys-recycle-token-baseline\7b1e642ffe32')
# Actual queue/recycle readers; hosted copy accounting and native wait tests only.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-recycle-token\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
# Use one header identity when directly including the frozen implementation.
$oldCore=Join-Path $directory 'core-before.c'
$core=[IO.File]::ReadAllText((Join-Path $baseline 'common\v2_core.c'))
$header='#include "'+(Join-Path $script:RepoRoot 'common\v2_core.h').Replace('\','/')+'"'
[IO.File]::WriteAllText($oldCore,$core.Replace('#include "v2_core.h"',$header),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    foreach($case in @('queue-iso','queue-ms','recycle')) {
        $objects=Join-Path $directory "$variant-$case";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $lines=@()
        if($variant -eq 'before') {
            if($case -eq 'recycle') {
                $lines+='#define PB_RECYCLE_TOKEN_BASELINE 1'
                $lines+='#define PB_RECYCLE_SOURCE "'+(Join-Path $baseline 'driver\v2\session_recycle.h').Replace('\','/')+'"'
            } else {
                $lines+='#define PB_TOKEN_QUEUE_BASELINE 1'
                $lines+='#define PB_TOKEN_CORE_SOURCE "'+$oldCore.Replace('\','/')+'"'
            }
        }
        $test=if($case -eq 'recycle'){'tests\model\send_window_tests.c'}else{'tests\model\token_queue_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
        $source=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects 'fixture.exe';$volatile=if($case -eq 'queue-ms'){'/volatile:ms'}else{'/volatile:iso'}
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',$volatile,'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'/link',"/out:$exe"))
        $testArguments=if($variant -eq 'before' -and $case -eq 'recycle') {@('--historical-recycle')}else{@()}
        Invoke-TestTool $exe $testArguments
        $executables+=@{variant=$variant;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
$hashes=foreach($variant in @('before','after')) {
    $root=if($variant -eq 'before'){$baseline}else{$script:RepoRoot}
    foreach($file in @('common\v2_core.c','driver\v2\session_recycle.h')) {
        @{variant=$variant;source=$file;sha256=(Get-FileHash -LiteralPath (Join-Path $root $file)).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during recycle-token checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    method='Frozen full-descriptor core plus token extraction versus actual token consumer, /volatile:iso and ms; 7744 differential cases each, synthetic guarded read footprint, immediate producer reuse, arm races, 400000 concurrent tokens per run with native event waits and balanced handles. Actual frozen/current recycle and send bodies check 128-item budget, copy bytes, fault-prefix ownership and token validation. Queue ABI stride remains 64 bytes. Not kernel CPU/latency/throughput.';
    snapshot_bytes_per_128_before=8192;snapshot_bytes_per_128_after=2048;
    historical_scope='Frozen recycle predates persistent-pool proofs: compare direct retirement/copy budgets, token/cursor validation and post-fault admission; current variant also runs the later send/recovery regressions and 14 guarded snapshot/reuse cases with shared overwrite after Consumer publication, exact fault prefixes, private free-index/cache/generation checks and uint64 wrap.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;source_hashes=$hashes} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
