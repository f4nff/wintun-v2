$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$directory=Join-Path $script:RepoRoot "out\sys-return-reclaim\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$cleanup=Join-Path $script:RepoRoot 'driver\v2\session_cleanup.h'
$body=[IO.File]::ReadAllText($cleanup)
$mutations=[ordered]@{
 'without-all-returned'='    KeWaitForSingleObject(&s->AllReturned, Executive, KernelMode, FALSE, NULL);'
 'without-rundown'='    ExWaitForRundownProtectionRelease(&s->Returns);'
}
$headers=@();$executables=@()
foreach($variant in @('current','without-all-returned','without-rundown')) {
 $header=$cleanup
 if($variant -ne 'current') {
  $line=$mutations[$variant]
  if([regex]::Matches($body,[regex]::Escape($line)).Count -ne 1){throw "Cannot isolate $variant barrier"}
  $header=Join-Path $directory "$variant.h"
  [IO.File]::WriteAllText($header,$body.Replace($line,'    /* observer negative control: omitted barrier */'),[Text.UTF8Encoding]::new($false))
 }
 $headers+=@{variant=$variant;path=$header;sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
  $memory=if($guarded){'guarded'}else{'ordinary'}
  $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
  $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_RETURN_RECLAIM_CLEANUP_SOURCE "'+$header.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_reclaim_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'reclaim.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'reclaim.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\driver\v2","/Fo$objects\")+$script:VersionArguments+@($source,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
  if($variant -eq 'current') {Invoke-TestTool $exe @()}
  else {
   $output=@(& $exe 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
   $log=Join-Path $objects 'expected-observer-rejection.log';$output|Set-Content -LiteralPath $log -Encoding utf8
   $condition=if($variant -eq 'without-all-returned'){'!ReadAcquire(&Current->Active)'}else{'!ReadAcquire(&Current->Returns.References)'}
   $expected=$code -eq 1 -and ($output -join "`n").Contains($condition)
   $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@();exit_code=$code;expected_exit_code=1;expected_observer_rejection=$expected;mutation=$variant;guarded=$guarded;log=$log})
   Write-TestRecording $(if($expected){'running'}else{'failed'})
   if(-not $expected){throw "Return/reclamation observer failed its control: $variant/$mode/$memory"}
   Write-Output "PASS: observer rejects $variant ($mode/$memory), actual exit=$code"
  }
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();expected_exit_code=$(if($variant -eq 'current'){0}else{1})}
 }}
}
## Exercise the same-slot reuse observers independently of cleanup controls.
## Mutations exist only in out/: an old callback wrongly clears a reused slot
## or copied-buffer lease after dropping RxLock. A failure elsewhere is rejected.
$returnHeader=Join-Path $script:RepoRoot 'driver\v2\session_return.h'
$returnBody=[IO.File]::ReadAllText($returnHeader)
$nblBody=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_nbl.h'))
if([regex]::Matches($nblBody,[regex]::Escape('#include "session_return.h"')).Count -ne 1){throw 'Cannot isolate the NBL return include'}
$anchor='        ExFreePoolWithTag(record, TUN_MEMORY_TAG);'
if([regex]::Matches($returnBody,[regex]::Escape($anchor)).Count -ne 1){throw 'Cannot isolate the post-unlock private-record tail'}
foreach($variant in @('late-slot-clear','late-buffer-clear')) {
 $injected=if($variant -eq 'late-slot-clear') {
  '        for (ULONG j = 0; j < record->Count; ++j) q->RxSlots[record->Descriptors[j].Token.Slot].Active = FALSE;'
 } else {
  '        if (record->CoalesceIndex < 2) InterlockedExchange(&q->CoalesceBusy[record->CoalesceIndex], 0);'
 }
 $header=Join-Path $directory "$variant.h"
 [IO.File]::WriteAllText($header,$returnBody.Replace($anchor,$injected+"`r`n"+$anchor),[Text.UTF8Encoding]::new($false))
 $nbl=Join-Path $directory "$variant-nbl.h"
 [IO.File]::WriteAllText($nbl,$nblBody.Replace('#include "session_return.h"','#include "'+$header.Replace('\','/')+'"'),[Text.UTF8Encoding]::new($false))
 $headers+=@{variant=$variant;path=$header;sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant();nbl=$nbl;nbl_sha256=(Get-FileHash -LiteralPath $nbl).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
  $memory=if($guarded){'guarded'}else{'ordinary'}
  $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
  $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_NBL_SOURCE "'+$nbl.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_reclaim_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'reclaim.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'reclaim.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\driver\v2","/Fo$objects\")+$script:VersionArguments+@($source,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
  $output=@(& $exe --reuse-only 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
  $log=Join-Path $objects 'expected-observer-rejection.log';$output|Set-Content -LiteralPath $log -Encoding utf8
  $condition=if($variant -eq 'late-slot-clear'){'q->RxSlots[i].Generation==2 && q->RxSlots[i].Active==active'}else{'native->CoalesceBusy[0]==(LONG)(format!=0)'}
  $expected=$code -eq 1 -and ($output -join "`n").Contains($condition)
  $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--reuse-only');exit_code=$code;expected_exit_code=1;expected_observer_rejection=$expected;mutation=$variant;guarded=$guarded;log=$log})
  Write-TestRecording $(if($expected){'running'}else{'failed'})
  if(-not $expected){throw "Slot-reuse observer failed its control: $variant/$mode/$memory"}
  Write-Output "PASS: observer rejects $variant ($mode/$memory), actual exit=$code"
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();expected_exit_code=1}
 }}
}
## Omit only the exhaustion guard in an out-only core. The terminal fixture
## must reject a wrapped reservation before the token can enter any ring.
$coreBody=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'common\v2_core.c'))
$guard='    if (s->Generation == 0xffffffffu) { s->State = PB_SLOT_RETIRED; return 0; }'
if([regex]::Matches($coreBody,[regex]::Escape($guard)).Count -ne 1){throw 'Cannot isolate the generation exhaustion guard'}
$wrapped=Join-Path $directory 'generation-wrap.c'
[IO.File]::WriteAllText($wrapped,$coreBody.Replace($guard,'    /* observer negative control: omitted generation exhaustion guard */'),[Text.UTF8Encoding]::new($false))
$coreControls=@(@{variant='generation-wrap';path=$wrapped;sha256=(Get-FileHash -LiteralPath $wrapped).Hash.ToLowerInvariant()})
foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
 $memory=if($guarded){'guarded'}else{'ordinary'}
 $objects=Join-Path $directory "generation-wrap-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
 $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_reclaim_tests.c').Replace('\','/')+'"'
 $source=Join-Path $objects 'reclaim.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'reclaim.exe'
 Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\common","/Fo$objects\")+$script:VersionArguments+@($source,$wrapped,'common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
 $output=@(& $exe --terminal-only 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
 $log=Join-Path $objects 'expected-observer-rejection.log';$output|Set-Content -LiteralPath $log -Encoding utf8
 $condition='PbPoolReserve(&native->TxPool,&sentinel)==PB_FULL && !memcmp(&sentinel,&unchanged,sizeof(sentinel))'
 $expected=$code -eq 1 -and ($output -join "`n").Contains($condition)
 $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--terminal-only');exit_code=$code;expected_exit_code=1;expected_observer_rejection=$expected;mutation='generation-wrap';guarded=$guarded;log=$log})
 Write-TestRecording $(if($expected){'running'}else{'failed'})
 if(-not $expected){throw "Terminal-generation observer failed its control: $mode/$memory"}
 Write-Output "PASS: observer rejects generation-wrap ($mode/$memory), actual exit=$code"
 $executables+=@{variant='generation-wrap';volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();expected_exit_code=1}
}}
## A global STOPPED decision must inspect every private queue witness.
$queryBody=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_query.h'))
$queueLoop='        for (ULONG i = 0; i < s->Layout.QueueCount; ++i) {'
if([regex]::Matches($queryBody,[regex]::Escape($queueLoop)).Count -ne 1){throw 'Cannot isolate the QUERY queue walk'}
$firstQueue=Join-Path $directory 'first-queue-query.h'
[IO.File]::WriteAllText($firstQueue,$queryBody.Replace($queueLoop,'        for (ULONG i = 0; i < 1; ++i) {'),[Text.UTF8Encoding]::new($false))
$queryControls=@(@{variant='first-queue-only';path=$firstQueue;sha256=(Get-FileHash -LiteralPath $firstQueue).Hash.ToLowerInvariant()})
foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
 $memory=if($guarded){'guarded'}else{'ordinary'}
 $objects=Join-Path $directory "first-queue-only-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
 $lines+='#define PB_RETURN_RECLAIM_QUERY_SOURCE "'+$firstQueue.Replace('\','/')+'"'
 $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_reclaim_tests.c').Replace('\','/')+'"'
 $source=Join-Path $objects 'reclaim.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'reclaim.exe'
 Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
 $output=@(& $exe --multi-only 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
 $log=Join-Path $objects 'expected-observer-rejection.log';$output|Set-Content -LiteralPath $log -Encoding utf8
 $condition='status.State!=PB_SESSION_STOPPED && !status.ActiveReferences && !Current->Queues[1].FaultQuiesced'
 $expected=$code -eq 1 -and ($output -join "`n").Contains($condition)
 $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--multi-only');exit_code=$code;expected_exit_code=1;expected_observer_rejection=$expected;mutation='first-queue-only';guarded=$guarded;log=$log})
 Write-TestRecording $(if($expected){'running'}else{'failed'})
 if(-not $expected){throw "Multi-queue observer failed its control: $mode/$memory"}
 Write-Output "PASS: observer rejects first-queue-only ($mode/$memory), actual exit=$code"
 $executables+=@{variant='first-queue-only';volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();expected_exit_code=1}
}}
# Allocation-failure composition controls: a failure must return its completion
# credit and copied-buffer lease without touching an NBL held by another queue.
$receivePath=Join-Path $script:RepoRoot 'driver\v2\session_receive.h'
$receiveBody=[IO.File]::ReadAllText($receivePath)
$receiveControls=@()
$allocationMutations=[ordered]@{
 'missing-failure-completion'='                PbV2CompleteLocked(q, items + i + j, PB_FULL);'
 'retained-failure-buffer'='            if (copied < 2) InterlockedExchange(&q->CoalesceBusy[copied], 0);'
}
foreach($variant in $allocationMutations.Keys) {
 $line=$allocationMutations[$variant]
 if([regex]::Matches($receiveBody,[regex]::Escape($line)).Count -ne 1){throw "Cannot isolate allocation control: $variant"}
 $header=Join-Path $directory "$variant.h"
 [IO.File]::WriteAllText($header,$receiveBody.Replace($line,'            /* observer negative control: omitted failure retirement */'),[Text.UTF8Encoding]::new($false))
 $receiveControls+=@{variant=$variant;path=$header;sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
  $memory=if($guarded){'guarded'}else{'ordinary'}
  $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
  $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_RETURN_RECLAIM_RECEIVE_SOURCE "'+$header.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_reclaim_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'allocation.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'allocation.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\driver\v2","/Fo$objects\")+$script:VersionArguments+@($source,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))
  $output=@(& $exe --allocation-only 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
  $log=Join-Path $objects 'expected-observer-rejection.log';$output|Set-Content -LiteralPath $log -Encoding utf8
  $condition=if($variant -eq 'missing-failure-completion'){'!native->Credits && !native->PendingCount'}else{'!native->CoalesceBusy[0] && !native->CoalesceBusy[1]'}
  $expected=$code -eq 1 -and ($output -join "`n").Contains($condition)
  $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--allocation-only');exit_code=$code;expected_exit_code=1;expected_observer_rejection=$expected;mutation=$variant;guarded=$guarded;log=$log})
  Write-TestRecording $(if($expected){'running'}else{'failed'})
  if(-not $expected){throw "Allocation observer failed its control: $variant/$mode/$memory"}
  Write-Output "PASS: observer rejects $variant ($mode/$memory), actual exit=$code"
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();expected_exit_code=1}
 }}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during return/reclamation verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;production_implementation_changed=$false;
 method='Actual receive NBL allocation, bounded return, completion/reference/fault/quiescence, QUERY and Free helpers with hosted DDIs and real Windows locks/events/threads. Four ISO/MS ordinary/guarded processes each execute 64 cleanup, 72 same-slot reuse, 24 terminal-generation and 32 multi-queue return schedules, plus 48 allocation-failure compositions (240 total). Multi-queue cases use actual DLL Reserve/Commit/Poll and SYS ReceiveBatch to build one ordinary and one four-original RSC NBL on separate queues; two callback threads independently gate after RxLock release and before rundown release. Independent last-reference and last-pin orders, drain/fault, sync/manual event and cursor wrap: one AllReturned signal, both fault witnesses required, cleanup actually blocks with two pins and remains blocked after one drops. Exact queue identities/completions, references/events and balanced resources. User session state is fixture-assembled; fault-undelivered private bookkeeping is discarded only after trusted native STOPPED, not through modeled DLL Close. Terminal TX producer uses actual pool/ring helpers, not actual SYS Send. No full DLL open/close, queue worker, native NDIS/IRQL or CPU execution. Eight cleanup, eight tail-clear, four exhaustion-guard and four first-queue-only QUERY controls require exact assertions and exit 1. Production source unchanged.';
 allocation_failure_method='Four ISO/MS ordinary/guarded current processes additionally execute 48 DLL/SYS allocation-failure schedules (240 total per process): four NBL construction failure sites, plain/RSC4/URO6, sync/manual event and cursor wrap. Actual DLL Reserve/Commit/Poll consumes PB_FULL completions, repeats the same slots at generation 2, and uses actual native receive/return/query/free while a separate queue holds its NBL. Exact credit, copied-buffer, reference and native/DLL event checks; held NBL prevents STOPPED until its real return. Eight out-only receive controls omit failure completion or buffer release and must fail at their exact observer with exit 1. Hosted injection, not actual low-resource kernel execution.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;headers=$headers;core_controls=$coreControls;query_controls=$queryControls;receive_controls=$receiveControls;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
