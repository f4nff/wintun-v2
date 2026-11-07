param([string]$BaselineDirectory='out\return-stop-baseline\e6c767571bed',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\completion-turn-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Completion turn audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path $BaselineDirectory).Path
$frozen=@{'api\v2\session.c'='9d06e10637c4291c0479735b8ab0e54d41706dbfc0b1e923b4c3e42e00c8b459';
 'api\v2\session_lifecycle.h'='33806814b12535da7304b63d544aa0b31c9862a8c7b465eb004d374a9455392d';'tests\safety\native_policy_fixture.c'='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'}
foreach($path in $frozen.Keys){if((Get-FileHash (Join-Path $baseline $path)).Hash.ToLowerInvariant() -ne $frozen[$path]){throw "Frozen source changed:$path"}}
$dependencies=@(rg --files api/v2 common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
 'tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){
    if($path.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $path}
    else{if((Get-FileHash (Join-Path $baseline $path)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $path)).Hash){throw "Unchanged dependency changed:$path"}}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\completion-turn\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Assert-CompletionTurnProjection($build){
    $e=$build.extraction;$api=Join-Path $script:RepoRoot 'api\v2';$original=[IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session_lifecycle.h'))
    if([IO.File]::ReadAllText($e.lifecycle_original) -cne $original -or (Get-FileHash $e.lifecycle_original).Hash.ToLowerInvariant() -ne $frozen['api\v2\session_lifecycle.h']){throw 'Original complete lifecycle snapshot changed'}
    $before='                if (batch == 3) SetEvent(s->ProgressEvent);';$after='                /* Observer132: omit budget continuation only. */'
    if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'Actual continuation not unique'}
    $expected=$original
    if($build.variant -ne 'current'){
        if($build.mutation.original -cne $before -or $build.mutation.changed -cne $after -or $build.mutation.offset -ne $original.IndexOf($before) -or $build.mutation.length -ne $before.Length){throw 'Continuation mutation ledger changed'}
        $expected=$expected.Replace($before,$after)
    }elseif($null -ne $build.mutation){throw 'Current unexpectedly mutated'}
    if([IO.File]::ReadAllText($e.lifecycle) -cne $expected){throw 'Lifecycle changed beyond selected one-line continuation omission'}
    if($build.variant -eq 'current' -and (Get-FileHash $e.lifecycle).Hash.ToLowerInvariant() -ne $frozen['api\v2\session_lifecycle.h']){throw 'Current lifecycle not byte-identical'}
    $session=[IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session.c'))
    $session=[regex]::Replace($session,'(?m)^#include "([^"]+)"',{
        param($m);$path=if($m.Groups[1].Value -eq 'session_lifecycle.h'){$e.lifecycle}else{[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'
    })
    if([IO.File]::ReadAllText($e.session) -cne $session){throw 'Session changed beyond exact include projection'}
    $entry=@'
#define PbQueuePop TurnPop
#undef WaitForSingleObject
#define WaitForSingleObject TurnWait
#undef AcquireSRWLockExclusive
#define AcquireSRWLockExclusive TurnAcquire
#include PB_COMPLETION_TURN_SESSION_SOURCE
#undef PbQueuePop
'@
    if([IO.File]::ReadAllText($e.entry) -cne $entry -or !$build.ndebug -or $build.pb_api_tests){throw 'Shipping observation entry/flags changed'}
    $lines=@('#define CT_FIXTURE 1');if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_COMPLETION_TURN_SESSION_SOURCE "'+$e.session.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\completion_turn_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Final compilation entry changed'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -ne $source.sha256){throw 'Source/artifact hash changed'}}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-CompletionTurnProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start completion turn test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Completion turn watchdog expired30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-completion-turn.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) completion turn cases, owner joins(\d+) preparation callbacks(\d+) final callbacks(\d+) callback joins(\d+) forced prepare(\d+) missing(\d+) handles(\d+)/(\d+); NDEBUG bounded256 per queue, direct event-state witness, actual SDK wakes and final native progress')
        $full=$Arguments.Count -eq 1;$cases=if($full){12}else{3};$owners=if($full){20}else{6};$forced=if($full){6}else{3};$missing=if($Expected -eq 132){3}else{0}
        $marker='COMPLETION_TURN_SELF_WAKE_MISSING: non-consuming event query observed no continuation after budget while ready completions remained; final genuine native progress rescued normal cleanup before132'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 132 -or $output.Contains($marker))
        $records=[regex]::Matches($output,'(?m)^CASE: completion turn queues(\d+) heavy(\d+) wrap([01]) preparation(\d+) final(\d+) joins(\d+) missing([01])\r?$')
        $caseLines=@($output -split '\r?\n'|Where-Object {$_.StartsWith('CASE: completion turn')})
        $correct=$correct -and $records.Count -eq $cases -and $caseLines.Count -eq $cases
        $stages=[Collections.Generic.List[object]]::new();$prepSum=0;$finalSum=0;$joinSum=0;$ownerSum=0;$forcedSum=0;$missingSum=0
        for($i=0;$i -lt $records.Count;++$i){
            $row=$records[$i];$queues=[int]$row.Groups[1].Value;$heavy=[int]$row.Groups[2].Value;$wrap=[int]$row.Groups[3].Value;$prep=[int]$row.Groups[4].Value
            $final=[int]$row.Groups[5].Value;$joined=[int]$row.Groups[6].Value;$absent=[int]$row.Groups[7].Value
            # Independently reconstruct the exact warm/measure shape order.
            $shape=if($full){$i%6}else{5};$shapeQueue=if($shape%3 -eq 0){1}else{2};$shapeHeavy=if($shape%3 -eq 2){1}else{0}
            if(!$full){$shapeQueue=2;$shapeHeavy=0};$shapeWrap=if($shape -ge 3){1}else{0}
            $correct=$correct -and $queues -eq $shapeQueue -and $heavy -eq $shapeHeavy -and $wrap -eq $shapeWrap -and
                ($prep -eq 1 -or $prep -eq 2) -and ($wrap -eq 0 -or $prep -eq 2) -and $final -eq 1 -and $joined -eq $prep+$final -and $absent -eq [int]($Expected -eq 132)
            $prepSum+=$prep;$finalSum+=$final;$joinSum+=$joined;$ownerSum+=$queues;$forcedSum+=$wrap;$missingSum+=$absent
            $stages.Add([ordered]@{queues=$queues;heavy=$heavy;wrap=[bool]$wrap;preparation_callbacks=$prep;final_callbacks=$final;callback_joins=$joined;missing=[bool]$absent})
        }
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $owners -and $ownerSum -eq $owners -and
            [int]$summary.Groups[3].Value -eq $prepSum -and [int]$summary.Groups[4].Value -eq $finalSum -and $finalSum -eq $cases -and
            [int]$summary.Groups[5].Value -eq $joinSum -and $joinSum -eq $prepSum+$finalSum -and [int]$summary.Groups[6].Value -eq $forcedSum -and $forcedSum -eq $forced -and
            [int]$summary.Groups[7].Value -eq $missingSum -and $missingSum -eq $missing -and $summary.Groups[8].Value -eq $summary.Groups[9].Value}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 132 -and $correct);watchdog_ms=30000;log=$log;
            schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});owner_joins=$ownerSum;preparation_callbacks=$prepSum;final_callbacks=$finalSum;callback_joins=$joinSum;forced_prepare_cases=$forcedSum;case_stages=@($stages);
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Completion turn failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-budget-continuation')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$built=Build-CompletionTurnTests -Directory (Join-Path $directory "$mode-$memory\$variant") -Mode $mode -Guarded:$guarded -Variant $variant
    Assert-CompletionTurnProjection $built;$executables+=,$built
    if($variant -eq 'current'){Invoke-CompletionTurnProcess $built.path @($built.fixture);Invoke-CompletionTurnProcess $built.path @($built.fixture,'132')}
    else{Invoke-CompletionTurnProcess $built.path @($built.fixture,'132') 132}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during completion turn audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-1);operations=$script:TestResultFile;executables=$executables;
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;ndebug=$true;
    callback_accounting=[ordered]@{schema='per-case-preparation-plus-final-v2';preparation_callbacks=@(1,2);final_callbacks=1;
        forced_second_prepare='Every wrap shape waits for the first real callback to finish before actual PrepareClose, then waits for its second preparation callback. Other shapes preserve the legal call/worker race.';
        verification='At each real prepared Work wait: ScheduledFALSE, ActiveCallbacks0, Finished==CallbackCount, complete slots/freebits scan. Each case captures exactly preparation+1 contiguous callback handles/IDs and joins/closes every one after true detach. Independent audit reconstructs shape order, validates every stage row and exact stage/summary sums; no fixed two-callback assumption.'};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    reviewed_dependency_repairs=@('common/offload_policy.c');
    method='Coverage-only nonPB/NDEBUG completion continuation and per-queue service. Frozen complete session/lifecycle and original native-policy ledger remain unchanged; current header is byte-identical and full session only normalizes quoted includes. One/two queues with512 slots/ring each are admitted under the real256MiB runtime quota. Distinct actual queue owner threads Reserve/Commit512 heavy submissions and one light submission, then join before any stop/close. Hosted native consumer pops the actual submitted descriptors. Empty lifecycle polls first enter a real SDK park, consume the genuine initial stop hint, and reach a second empty park; no event is reset or consumed manually. A single coalesced native progress notification accompanies511 heavy and one light legal completions while a final heavy packet/reference remains native-owned. Actual lifecycle first turn retires256 heavy and all light completions in either queue order. Non-consuming NtQueryEvent on its auto-reset Progress event witnesses the production continuation hint without stealing it. Current releases only the observer gate, then the real WaitForSingleObject consumes the hint and immediately continues without a fallback timeout:255 remaining readable completions retire, leaving one Published native-owned token. A bounded empty turn settles to no hint, not a self-spin. Final actual hosted completion/reference progress permits preparation. The real callback completes while a public shell reference protects Work; a test-only post-callback scan checks every RX slot FREE and all free-index bits restored without adding that scan to shipping ValidOwnership. All public owners have joined before final successful Close; exact native core/map/pool/work/module/handle accounting, actual callback OS-thread joins and true DLL unload execute. Full6 checked warm+6measured shapes cover1/2 queues, heavy-first/heavy-last and uint64 cursor wrap. CLI132 uses2 checked warm+1measured shape. Every process requires strict process-handle equality after warmup. Negative omits only the unique batch==3 continuation SetEvent; at the first budget-limited park NtQueryEvent reads zero while255 completions are readable, Published256 includes the last native-owned token and NativeActive1 blocks preparation. This is a direct state witness, not timeout absence. Last genuine modeled native completion/reference notification naturally rescues progress before normal cleanup and132, without reference/event/counter repair or unknown SDK handle closure. Only diagnostic pop counters reset on genuine DataGate acquisition; production ownership never changes in observers. Guarded mode uses original native core/slots/mapping guards, host config remains ordinary live storage. Native producers/IOCTL/reference state are hosted, no actual SYS/NDIS/driver/device/registry/Win7 runtime, scheduler fairness or CPU performance guarantee is claimed. NDEBUG and shipping65-byte fingerprint are enforced, PB_API_TESTS is rejected; version2.0.0. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    audit_sources=@('tests\model\completion_turn_tests.c','tools\build\completion-turn-tests.ps1','tools\audit\verify-completion-turn.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 16|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
