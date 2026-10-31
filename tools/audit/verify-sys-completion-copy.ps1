param([string]$BaselineDirectory='out\sys-completion-copy-baseline\2aa1358ac8da')
# Frozen/current actual completion flush; shared publication and DDI rules remain
# common. Guarded ring tests exercise the new source path in both volatile modes.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-completion-copy\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'driver\v2\session_completion.h'
$old=[IO.File]::ReadAllText($oldPath);$start=$old.IndexOf('static BOOLEAN PbV2RetirePendingLocked')
if($start -lt 0){throw 'Cannot locate frozen completion ledger functions'}
$oldSource=Join-Path $directory 'completion-before.h'
[IO.File]::WriteAllText($oldSource,$old.Substring($start),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($volatile in @('iso','ms')) {
    foreach($variant in @('before','after','queue-ring')) {
        $objects=Join-Path $directory "$variant-$volatile";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $lines=@();$sources=@()
        if($variant -eq 'before') {$lines+=@('#define PB_COMPLETION_COPY_BASELINE 1',('#define PB_COMPLETION_SOURCE "'+$oldSource.Replace('\','/')+'"'))}
        if($variant -eq 'queue-ring') {$test='tests\model\queue_ring_tests.c'}
        else {$test='tests\model\completion_progress_tests.c';$sources+='common\v2_core.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
        $source=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects 'fixture.exe'
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$volatile",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
            $sources+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;volatile=$volatile;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during completion-copy checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_completion_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();
    current_completion_sha256=(Get-FileHash 'driver\v2\session_completion.h').Hash.ToLowerInvariant();
    current_core_sha256=(Get-FileHash 'common\v2_core.c').Hash.ToLowerInvariant();
    method='Frozen/current actual completion functions, same hosted worker/DDI/current core. Verify guarded circular source contents, single publication/arm exchange, partial/full/corrupt rings, <=128 flush budget, paused progress and real concurrent event waits in both volatile modes. Staged bytes count private-to-stack descriptors offered by the frozen flush; direct descriptors count accepted private-ring descriptors. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
