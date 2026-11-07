param([string]$BaselineDirectory='out\reclamation-baseline\31293a872711')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Pause/unregister audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$oldPause=Join-Path $baseline 'driver\v2\session_pause.h';$oldSession=Join-Path $baseline 'driver\v2\session.h'
if((Get-FileHash -LiteralPath $oldPause).Hash.ToLowerInvariant() -ne 'bc191078e46ffe05699f78832225fc2c63c9a762a05344fbea03c09ec7be2b04' -or
   (Get-FileHash -LiteralPath $oldSession).Hash.ToLowerInvariant() -ne '934b93718e93b18aed26b7f36215e6259064144e274acd10e0b743f94a0000cd'){throw 'Unexpected pause/unregister baseline'}
foreach($name in @('session_cleanup.h','session_return.h','session_references.h','session_nbl.h','session_completion.h','session_progress.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "driver\v2\$name")).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "driver\v2\$name")).Hash){throw "Reclamation dependency changed: $name"}
}
$directory=Join-Path $script:RepoRoot "out\pause-unregister\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$body=[IO.File]::ReadAllText($oldSession).Replace("`r`n","`n")
$start=$body.IndexOf('/* Called with RegistrationLock exclusively held');$end=$body.IndexOf('#include "session_pause.h"',$start)
if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen unregister'}
$function=$body.Substring($start,$end-$start).TrimEnd()
$header="/* SPDX-License-Identifier: GPL-2.0 */`n#pragma once`n"+$function+"`n"
$currentUnregister=Join-Path $script:RepoRoot 'driver\v2\session_unregister.h'
if([IO.File]::ReadAllText($currentUnregister).Replace("`r`n","`n") -cne $header){throw 'Unregister extraction changed behavior'}
$expected=$body.Substring(0,$start)+"#include `"session_unregister.h`"`n`n"+$body.Substring($end)
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session.h')).Replace("`r`n","`n") -cne $expected){throw 'Aggregate session changed beyond unregister extraction'}
$oldUnregister=Join-Path $directory 'unregister-before.h';[IO.File]::WriteAllText($oldUnregister,$header)
$prefix=@'
    /* Unregister detaches V2 before waiting for its outstanding receives. Join
     * that owner before treating NULL as drained; otherwise pin its snapshot.
     * The Returns pin, not RegistrationLock, covers the subsequent waits. */
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&ctx->Device.RegistrationLock, TRUE);
'@
$suffix=@'
    ExReleaseResourceLite(&ctx->Device.RegistrationLock);
    KeLeaveCriticalRegion();
'@
$expected=[IO.File]::ReadAllText($oldPause).Replace("`r`n","`n")
$anchor='    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);'
$expected=$expected.Replace($anchor,$prefix.Replace("`r`n","`n")+"`n"+$anchor)
$anchor='    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);'
$expected=$expected.Replace($anchor,$anchor+"`n"+$suffix.Replace("`r`n","`n"))
$currentPause=Join-Path $script:RepoRoot 'driver\v2\session_pause.h'
if([IO.File]::ReadAllText($currentPause).Replace("`r`n","`n") -cne $expected){throw 'Pause changed beyond short shared registration snapshot'}
$executables=@()
foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {foreach($variant in @('before','after')) {
    $memory=if($guarded){'guarded'}else{'ordinary'}
    $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=Join-Path $objects 'entry.c';$lines=@()
    if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $pause=if($variant -eq 'before'){$oldPause}else{$currentPause}
    $unregister=if($variant -eq 'before'){$oldUnregister}else{$currentUnregister}
    $lines+='#define PB_PAUSE_SOURCE "'+$pause.Replace('\','/')+'"'
    $lines+='#define PB_UNREGISTER_SOURCE "'+$unregister.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\pause_unregister_tests.c').Replace('\','/')+'"'
    $lines|Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $objects 'pause.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source)+$script:VersionArguments+@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
    if($variant -eq 'after'){Invoke-TestTool $exe @();Invoke-TestTool $exe @('1')}
    else {
        $output=@(& $exe 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 86 -and ($output -join "`n").Contains('PAUSE_COMPLETED_BEFORE_RETIRED_RECEIVE_RETURN')
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=86;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected retired-session pause baseline, exit=$code"}
    }
    $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during pause/unregister audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Whole frozen Pause and session hashes; byte-equivalent unregister extraction, unchanged actual free/receive/return/reference/completion helpers. ISO/MS ordinary/guarded each require old exit 86 and exact premature-pause marker after all resources reclaimed; current full 48 schedules and repaired CLI1 exit zero. Actual helpers build and return one NBL on each of one/two queues; NDIS ownership is fixture-assembled, no native indication. Real events, locks and joined Windows threads host the DDIs. Unregister-first detaches and blocks on AllReturned; Pause must join the exclusive owner before accepting NULL. Pause-first pins then releases shared registration ownership; unregister demonstrably reaches its own AllReturned while Pause remains gated. Multi-queue last return, manual/auto queue wake consumption, joined worker handles, Active/Returns notification tail and guarded mappings/pool objects are checked. Private callback-tail pin delays physical reclamation. No native IRQL, NDIS runtime, signed driver or old-OS execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
