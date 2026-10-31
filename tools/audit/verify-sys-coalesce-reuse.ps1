param([string]$BaselineDirectory='out\sys-coalesce-reuse-baseline\7f54476eae84')
# Frozen/current return bodies, same actual NBL/coalesce/core and hosted DDI.
# Counts construction calls and ownership transitions; no kernel CPU claim.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('session_return.h','3f78ac4500fff23c57e4d7b9dfd5f45279b455d3601839be2edbc083e4dda124'),
    @('session_nbl.h','63756841498f957ce1c32083d52c1a57b561f2348c37acdd172d6f1a3512599e'),
    @('session_coalesce.h','5e4f762a8dfa3c1968b699b3e7249efda6a8355050bf77a3356b2d347e0cb426'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "driver\v2\$($entry[0])")).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected coalesce-reuse baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\sys-coalesce-reuse\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldReturn=Join-Path $baseline 'driver\v2\session_return.h'
$oldNbl=Join-Path $directory 'nbl-before.h'
$receive=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session_nbl.h'))
$start=$receive.IndexOf('static NET_BUFFER_LIST *PbV2Receive(');if($start -lt 0){throw 'Cannot locate frozen NBL constructor'}
[IO.File]::WriteAllText($oldNbl,[IO.File]::ReadAllText($oldReturn)+"`n"+$receive.Substring($start),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('iso','ms')) {
    foreach($guarded in @($false,$true)) {
        $memory=if($guarded){'guarded'}else{'ordinary'}
        foreach($variant in @('before','after')) {
            $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force | Out-Null
            $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
            if($variant -eq 'before'){$lines+=@('#define PB_COALESCE_REUSE_BASELINE 1',('#define PB_NBL_SOURCE "'+$oldNbl.Replace('\','/')+'"'))}
            $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_coalesce_tests.c').Replace('\','/')+'"'
            $source=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
            $exe=Join-Path $objects 'fixture.exe'
            Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
                @('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c',$source,'/link',"/out:$exe"))
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
            if($variant -eq 'before') {
                $lines=@(& $exe --require-reuse 2>&1 | ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
                $log=Join-Path $objects 'expected-regression.log';$lines | Set-Content -LiteralPath $log -Encoding utf8
                $expected=$code -eq 1 -and ($lines -join "`n").Contains('q->CoalesceBusy[0]==(RequireReuse?0:1) && q->CoalesceBusy[1]==1')
                $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--require-reuse');exit_code=$code;expected_exit_code=1;expected_regression_observed=$expected;log=$log})
                Write-TestRecording $(if($expected){'running'}else{'failed'})
                if(-not $expected){throw 'Frozen return did not reproduce the expected stale buffer lease'}
                Write-Output "PASS: frozen callback reproduces delayed coalesce reuse ($mode/$memory), exit=$code"
            }
        }
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during coalesce-reuse verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_directory=$baseline;
    baseline_return_sha256=(Get-FileHash -LiteralPath $oldReturn).Hash.ToLowerInvariant();
    current_return_sha256=(Get-FileHash 'driver\v2\session_return.h').Hash.ToLowerInvariant();
    method='Frozen/current actual return and same actual coalesce/NBL construction; native return thread paused after RxLock release or before diagnostic locking, new live replacement retained across old callback tail. 44 IPv4/IPv6 TCP/UDP normal/error/local-cancel, full-completion and fault cases per variant, ordinary/guarded, ISO/MS volatile; exact tokens/status/stats, held buffer, Active/rundown, locks/events and balanced resources/handles. Diagnostic-phase replacement changes IP/protocol/length before the old metadata is read. Frozen require-reuse is expected to fail. Construction-count comparison applies only to this enabled-coalescing interleaving, not a universal CPU improvement; coalescing also copies and checksums payload.';
    replacement_nbls_before=2;replacement_nbls_after=1;replacement_mdls_before=4;replacement_mdls_after=2;
    replacement_ddi_acquisitions_before=8;replacement_ddi_acquisitions_after=4;
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
