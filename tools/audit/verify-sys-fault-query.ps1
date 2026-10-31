param([string]$BaselineDirectory='out\sys-fault-query-baseline\838f9da30fa3')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
 @('session_query.h','f65df836a159bfb078bdf3518336304f3caaff8dea236e32add997596aeae6de'),
 @('session_send.h','f73f43a6692348553489866ce3d902165eac91f56adc97644fa2a492eefc7d3b'),
 @('session_references.h','2a146a0ddd8c7723dfe9bd56e5663098a2fc1d4e21681c00aacaed0f95a916d8'))){
 if((Get-FileHash -LiteralPath (Join-Path $baseline ('driver\v2\'+$entry[0]))).Hash.ToLowerInvariant() -ne $entry[1]){throw "Unexpected fault-query baseline: $($entry[0])"}
}
$old=Join-Path $baseline 'driver\v2\session_query.h'
$directory=Join-Path $script:RepoRoot "out\sys-fault-query\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')){foreach($mode in @('iso','ms')){foreach($case in @('fault-query','session-query')){
 $objects=Join-Path $directory "$variant-$mode-$case";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $lines=@()
 if($variant -eq 'before'){
  $macro=if($case -eq 'fault-query'){'PB_FAULT_QUERY_SOURCE'}else{'PB_QUERY_SOURCE'}
  $lines+='#define '+$macro+' "'+$old.Replace('\','/')+'"'
  if($case -eq 'fault-query'){$lines+='#define PB_FAULT_QUERY_BASELINE 1'}
 }
 $test=if($case -eq 'fault-query'){'tests\model\fault_query_tests.c'}else{'tests\model\session_query_tests.c'}
 $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
 $source=Join-Path $objects 'query.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'query.exe'
 $support=if($case -eq 'fault-query'){@('common\v2_core.c','common\ethernet.c','common\flow.c','common\packet.c')}else{@()}
 Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source)+$support+@('/link',"/out:$exe"))
 Invoke-TestTool $exe @()
 $executables+=@{variant=$variant;volatile=$mode;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
# Negative controls validate the host mode/saved-level observer itself. These
# mutated headers live only in out; no production file is edited or installed.
$queryBody=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_query.h'))
$release='ExReleaseSpinLockExclusive(&ctx->TransitionLock, irql);'
if([regex]::Matches($queryBody,[regex]::Escape($release)).Count -ne 2){throw 'Cannot locate the two exclusive release exits for observer controls'}
foreach($mode in @('iso','ms')){foreach($mutation in @('wrong-release-mode','wrong-saved-level')){
 $objects=Join-Path $directory "observer-$mode-$mutation";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $replacement=if($mutation -eq 'wrong-release-mode'){'ExReleaseSpinLockShared(&ctx->TransitionLock, irql);'}else{'ExReleaseSpinLockExclusive(&ctx->TransitionLock, (KIRQL)0);'}
 $header=Join-Path $objects 'query-mutant.h';[IO.File]::WriteAllText($header,$queryBody.Replace($release,$replacement),[Text.UTF8Encoding]::new($false))
 $source=Join-Path $objects 'observer.c'
 $wrapper=@()
 $wrapper+='#define PB_FAULT_QUERY_SOURCE "'+$header.Replace('\','/')+'"'
 $wrapper+='#include "'+(Join-Path $script:RepoRoot 'tests\model\fault_query_tests.c').Replace('\','/')+'"'
 $wrapper|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'observer.exe'
 Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'common\v2_core.c','common\ethernet.c','common\flow.c','common\packet.c','/link',"/out:$exe"))
 $lines=@(& $exe 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
 $log=Join-Path $objects 'expected-observer-rejection.log';$lines|Set-Content -LiteralPath $log -Encoding utf8
 $condition=if($mutation -eq 'wrong-release-mode'){'HeldTransition==lock && HeldMode==1 && ModelLevel==2 && irql==SavedLevel'}else{'HeldTransition==lock && HeldMode==2 && ModelLevel==2 && irql==SavedLevel'}
 $expected=$code -eq 1 -and ($lines -join "`n").Contains($condition)
 $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@();exit_code=$code;expected_exit_code=1;expected_observer_rejection=$expected;mutation=$mutation;log=$log})
 Write-TestRecording $(if($expected){'running'}else{'failed'})
 if(-not $expected){throw "Mode/saved-level observer failed its negative control: $mode/$mutation"}
 $executables+=@{variant='observer-negative-control';volatile=$mode;case=$mutation;expected_exit_code=1;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 Write-Output "PASS: observer rejects isolated $mutation ($mode), actual exit=$code"
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during fault-query checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_directory=$baseline;
 baseline_query_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_query_sha256=(Get-FileHash 'driver\v2\session_query.h').Hash.ToLowerInvariant();
 method='Actual current send admission, rundown/reference helpers, fault and private worker quiescence with frozen/current QUERY; real Windows locks/events/threads in ISO/MS volatile modes. Sender is parked after its admission reads and rundown acquisition but before Active is retained. Old QUERY falsely acknowledges STOPPED before later Active=1; current fault-only exclusive transition snapshot waits for the in-flight lookup, keeps the completion callback reentrant and only acknowledges after release. Running/normal-drain queries retain shared snapshots. Current-only escalation detach makes old session PAGE_NOACCESS before reacquisition to prove fresh ctx->V2 lookup and quiet DEVICE_NOT_READY output. Basic actual query regressions run on both bodies. Hosted NDIS/IRQL, not a demonstrated physical reclamation failure or kernel CPU measurement.';
 replacement_review='Current-only escalation cases cover both ctx->V2 detach and replacement while the old guarded session is PAGE_NOACCESS. Reacquisition returns DEVICE_NOT_READY without output mutation, or a fresh replacement RUNNING snapshot with epoch 84. No old pointer or pre-escalation snapshot authorizes either result.';
 lock_mode_review='Thread-local host observers model transition-lock acquisition/release mode and saved/restored caller levels 0/1/2. Real SRW locks still serialize threads. No-session, normal, mid-snapshot fault, send lookup, reentrant callback at modeled level 2 and six detach/replacement combinations verify matching release and restoration; all observed acquisition/release counts balance. Four isolated out-only negative controls must exit 1 at the exact mode/saved-level assertion. This does not emulate actual kernel IRQL or scheduling; production code is unchanged by this coverage extension.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
