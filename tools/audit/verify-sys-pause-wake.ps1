param([string]$BaselineDirectory='out\sys-pause-wake-baseline\e9bc036520f1')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$oldSession=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'driver\v2\session.h')).Path
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'driver\v2\session_pause.h')).Path
if((Get-FileHash -LiteralPath $oldSession).Hash.ToLowerInvariant() -ne 'a95fa2e1a75eaf0881df164434e3bdf2638aec7c547429fde5a2597132341bd5' -or (Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne 'df669b0d0bb201cf59e768f47c3221a7448e9808946c31195430f82bd53380f9'){throw 'Unexpected pause-wake baseline'}
$currentSession=Join-Path $script:RepoRoot 'driver\v2\session.h'
$current=Join-Path $script:RepoRoot 'driver\v2\session_pause.h'
$oldBody=[IO.File]::ReadAllText($oldSession)
$start=$oldBody.IndexOf('static VOID PbV2Pause(TUN_CTX *ctx)');$end=$oldBody.IndexOf('#include "session_query.h"',$start)
if($start -lt 0 -or $end -le $start){throw 'Missing frozen pause body'}
$function=$oldBody.Substring($start,$end-$start)
$expected="/* SPDX-License-Identifier: GPL-2.0 */`r`n#pragma once`r`n"+$function.TrimEnd()+"`r`n"
if([IO.File]::ReadAllText($old) -cne $expected){throw 'Frozen pause body was changed while extracted'}
$expected=$oldBody.Substring(0,$start)+"#include `"session_pause.h`"`r`n`r`n"+$oldBody.Substring($end)
# Account for the separately reviewed unified/downlevel changes since this
# historical pause extraction. Keep an explicit finite transformation ledger,
# so an unrelated aggregate-header change still fails rather than being ignored.
$expected=$expected.Replace("`r`n","`n")
$expected=$expected.Replace("#pragma once`n","#pragma once`n#include `"platform_nbl.h`"`n#include `"platform_mdl.h`"`n")
$expected=$expected.Replace('ExAllocatePool2(POOL_FLAG_NON_PAGED,','ExAllocatePoolZero(NonPagedPool,')
$expected=$expected.Replace('NET_BUFFER_LIST_INFO(nbl, TcpRecvSegCoalesceInfo) = NULL;','PB_NBL_SET(nbl, TcpRecvSegCoalesceInfo, 30, NULL);')
$expected=$expected.Replace('NET_BUFFER_LIST_INFO(nbl, UdpRecvSegCoalesceOffloadInfo) = NULL;','PB_NBL_SET(nbl, UdpRecvSegCoalesceOffloadInfo, 89, NULL);')
$expected=$expected.Replace('NET_BUFFER_LIST_INFO(nbl, RscTcpTimestampDelta) = NULL;','PB_NBL_SET(nbl, RscTcpTimestampDelta, 30, NULL);')
$expected=$expected.Replace('MmGetSystemAddressForMdlSafe(s->Mdl, NormalPagePriority | MdlMappingNoExecute)','PbV2MapMdl(s->Mdl)')
# The pause/unregister regression includes the exact production unregister
# helper. Verify its extraction rather than allowing an aggregate-header gap.
$unregisterStart=$expected.IndexOf('/* Called with RegistrationLock exclusively held')
$unregisterEnd=$expected.IndexOf('#include "session_pause.h"',$unregisterStart)
if($unregisterStart -lt 0 -or $unregisterEnd -le $unregisterStart){throw 'Missing original unregister body'}
$unregisterBody=$expected.Substring($unregisterStart,$unregisterEnd-$unregisterStart).TrimEnd()
$unregisterHeader="/* SPDX-License-Identifier: GPL-2.0 */`n#pragma once`n"+$unregisterBody+"`n"
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_unregister.h')).Replace("`r`n","`n") -cne $unregisterHeader){throw 'Unregister changed beyond exact extraction'}
$expected=$expected.Substring(0,$unregisterStart)+"#include `"session_unregister.h`"`n`n"+$expected.Substring($unregisterEnd)
if([IO.File]::ReadAllText($currentSession).Replace("`r`n","`n") -cne $expected){throw 'Session changed outside pause/unregister extraction and reviewed downlevel pool/NBL/MDL adaptations'}
$insert=@'
    /* A worker may have sampled Running before pause, then skipped receive
     * after it. Its old wait set excludes RunningEvent. Recheck via the queue
     * event so paused retirement and the later restart cannot lose progress.
     * The existing pin keeps every queue/event alive after dropping the lock. */
    if (held) PbV2Wake(s);
'@
$anchor='    PbWaitIndications(ctx, 0); PbWaitIndications(ctx, 1);'
$expected=[IO.File]::ReadAllText($old).Replace("`r`n","`n").Replace($anchor,$insert.Replace("`r`n","`n")+"`n"+$anchor)
$snapshotStart=@'
    /* Unregister detaches V2 before waiting for its outstanding receives. Join
     * that owner before treating NULL as drained; otherwise pin its snapshot.
     * The Returns pin, not RegistrationLock, covers the subsequent waits. */
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&ctx->Device.RegistrationLock, TRUE);
'@
$snapshotEnd=@'
    ExReleaseResourceLite(&ctx->Device.RegistrationLock);
    KeLeaveCriticalRegion();
'@
$expected=$expected.Replace('    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);',$snapshotStart.Replace("`r`n","`n")+"`n"+'    KIRQL irql = ExAcquireSpinLockShared(&ctx->TransitionLock);')
$snapshotAnchor='    ExReleaseSpinLockShared(&ctx->TransitionLock, irql);'
$expected=$expected.Replace($snapshotAnchor,$snapshotAnchor+"`n"+$snapshotEnd.Replace("`r`n","`n"))
if([IO.File]::ReadAllText($current).Replace("`r`n","`n") -cne $expected){throw 'Pause changed beyond the documented wake and short registration snapshot'}
$directory=Join-Path $script:RepoRoot "out\sys-pause-wake\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
 $memory=if($guarded){'guarded'}else{'ordinary'}
 $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $lines=@();if($variant -eq 'before'){$lines+='#define PB_PAUSE_BASELINE 1'}
 if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
 $header=if($variant -eq 'before'){$old}else{$current}
 $lines+='#define PB_PAUSE_SOURCE "'+$header.Replace('\','/')+'"'
 $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\pause_wake_tests.c').Replace('\','/')+'"'
 $source=Join-Path $objects 'pause.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'pause.exe'
 Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
 Invoke-TestTool $exe @()
 $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during pause/wake verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;
 baseline_session=$oldSession;baseline_session_sha256=(Get-FileHash -LiteralPath $oldSession).Hash.ToLowerInvariant();baseline_pause=$old;baseline_pause_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();
 current_session=$currentSession;current_session_sha256=(Get-FileHash -LiteralPath $currentSession).Hash.ToLowerInvariant();current_pause=$current;current_pause_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 extraction_review='Frozen full session/header hashes are checked; frozen helper matches exact original Pause extraction. Current aggregate is byte-checked after LF normalization, explicit reviewed pool-zero/NBL-version/MDL-NX adaptations and byte-equivalent unregister extraction. Current pause differs only by the documented wake/comment and short shared-registration snapshot, released before all waits. The separate pause/unregister audit verifies that retirement barrier.';
 method='Actual worker, receive, pause, completion and reference helpers with same current core and hosted DDIs; real Windows locks/events/threads. Eight stale-decision schedules: worker sampled running before pause, actual receive then skips flush, stale normal wait excludes RunningEvent. Legitimate four-batch flush leaves one of 129 completions pending; consumer drains first 128 before worker clears old wake. Frozen first selected wait has no ready event even after restart; explicit unrelated queue rescue is required for teardown. Current Pause signals the queue while holding its existing rundown pin; first wait consumes that signal, next bounded turn publishes the last completion without new packet admission or polling. Sync/manual kernel events and cursor wrap. Four extra controls cover absent session, both queues, Active/AllReturned blocking and detach while pin held. Eight ISO/MS ordinary/guarded processes each execute 12 cases. No native NDIS/IRQL or CPU measurement.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
