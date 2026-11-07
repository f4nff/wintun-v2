param([string]$BaselineDirectory='out\stop-progress-baseline\124a7befbedb',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\drain-policy-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Drain/policy audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='9d06e10637c4291c0479735b8ab0e54d41706dbfc0b1e923b4c3e42e00c8b459'
 'api\v2\session_control.h'='cd327833ea04c09e9e857389766f36ee7f2c3b4d052035fdda1ec13b7b88d85e'
 'tests\safety\native_policy_fixture.c'='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
}
foreach($path in $frozen.Keys){if((Get-FileHash (Join-Path $baseline $path)).Hash.ToLowerInvariant() -ne $frozen[$path]){throw "Unexpected frozen source:$path"}}
$dependencies=@(rg --files api/v2 common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
 'tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){
    if($path.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $path}
    else{if((Get-FileHash (Join-Path $baseline $path)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $path)).Hash){throw "Unchanged dependency changed:$path"}}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\drain-policy\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-DrainPolicyProjection($build) {
    $api=Join-Path $script:RepoRoot 'api\v2';$original=[IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session_control.h'));$expected=$original
    $before='        if (!SleepConditionVariableSRW(&s->Changed, &s->State, INFINITE, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; break; }'
    $after='        if (!SleepConditionVariableSRW(&s->Changed, &s->State, INFINITE, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; /* Observer123: omit only the failed-wait break. */ }'
    if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'Frozen Drain failure branch not unique'}
    if($build.variant -eq 'without-drain-wait-break') {
        $expected=$expected.Replace($before,$after)
        if($build.mutation.offset -ne $original.IndexOf($before) -or $build.mutation.original -cne $before -or $build.mutation.changed -cne $after){throw 'Mutation ledger mismatch'}
    }elseif($null -ne $build.mutation){throw 'Current has an unexpected mutation'}
    if([IO.File]::ReadAllText($build.extraction.selected) -cne $expected){throw 'Control changed beyond the unique failed-wait break omission'}
    if($build.variant -eq 'current' -and $build.extraction.selected_sha256 -ne $frozen['api\v2\session_control.h']){throw 'Current control is not byte-identical'}
    $marker='static PB_RESULT WINAPI Drain(PB_SESSION *s)';$offset=$original.IndexOf($marker)
    if([IO.File]::ReadAllText($build.extraction.drain_original) -cne $original.Substring($offset) -or $build.extraction.offset -ne $offset){throw 'Actual Drain extraction changed'}
    $session=[IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session.c'))
    $session=[regex]::Replace($session,'(?m)^#include "([^"]+)"',{
        param($match);$include=if($match.Groups[1].Value -eq 'session_control.h'){$build.extraction.selected}else{[IO.Path]::GetFullPath((Join-Path $api $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    if([IO.File]::ReadAllText($build.session) -cne $session){throw 'Complete session projection changed beyond includes'}
    $entry=@'
#define WaitForMultipleObjects OwnerWait
#define SleepConditionVariableSRW DrainSleep
#include PB_DRAIN_POLICY_SESSION_SOURCE
#undef WaitForMultipleObjects
#undef SleepConditionVariableSRW
'@
    if([IO.File]::ReadAllText($build.entry) -cne $entry){throw 'Session boundary entry changed'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_DRAIN_POLICY_SESSION_SOURCE "'+$build.session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$build.entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\drain_policy_fixture.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Fixture include overrides changed'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -ne $source.sha256){throw 'Source/artifact hash changed'}}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-DrainPolicyProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start Drain/policy test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Drain/policy external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-drain-policy.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='DRAIN_WAIT_FAILURE_RETRIED: single defensive failure retried instead of returning; subsequent real SDK wait released State, late policy and actual Drain completed, every public owner/caller and callback joined before final close/unload, zero owned residue'
        $summary=[regex]::Match($output,'PASS: (\d+) actual nonPB synchronous Drain/policy schedules, (\d+) queue owners and (\d+) Drain callers joined, (\d+) callback OS handles joined; (\d+) defensive failures/(\d+) retry witnesses; actual unload, handles (\d+)/(\d+)')
        $full=$Arguments.Count -eq 1;$count=if($full){24}else{1};$owners=if($full){88}else{8};$failures=if($full){12}else{1};$retries=if($Expected -eq 123){1}else{0}
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 123 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $owners -and [int]$summary.Groups[3].Value -eq $count -and [int]$summary.Groups[4].Value -eq $count -and [int]$summary.Groups[5].Value -eq $failures -and [int]$summary.Groups[6].Value -eq $retries}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 123 -and $correct);
            watchdog_ms=30000;log=$log;schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});owner_joins=$(if($summary.Success){[int]$summary.Groups[2].Value}else{0});
            drain_caller_joins=$(if($summary.Success){[int]$summary.Groups[3].Value}else{0});callback_thread_joins=$(if($summary.Success){[int]$summary.Groups[4].Value}else{0});
            defensive_failures=$(if($summary.Success){[int]$summary.Groups[5].Value}else{0});retry_witnesses=$(if($summary.Success){[int]$summary.Groups[6].Value}else{0});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Drain/policy test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-drain-wait-break')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-DrainPolicyTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-DrainPolicyProjection $built
    if($variant -eq 'current'){Invoke-DrainPolicyProcess $built.path @($built.fixture);Invoke-DrainPolicyProcess $built.path @($built.fixture,'123')}
    else{Invoke-DrainPolicyProcess $built.path @($built.fixture,'123') 123}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during Drain/policy audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-1);
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;operations=$script:TestResultFile;executables=$executables;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    reviewed_dependency_repairs=@('common/offload_policy.c');
    method='Original session paths and native-policy host/ledger remain unchanged; common/offload_policy.c alone includes the separately reviewed full-file LSO/checksum dependency repair. Actual non-PB_API_TESTS full session/lifecycle/control/queue executes in a real unloadable DLL with real Win7-target SDK locks/events/condition waits and private max-one thread pool. One/two/eight distinct queue owners import one hosted publication and reserve one RX loan, then select actual Wait(INFINITE). Wait and condition-variable entry markers are SDK boundary observations, not claims of opaque kernel scheduler parking. Nonzero policy is held after actual IO entry while Control remains owned; actual synchronous Drain calls BeginStop, which wakes queue owners before real DRAIN IO can execute. Both actual late policy success/failure are covered. A separate defensive one-shot ERROR_OPERATION_ABORTED input returns from a condition wait with State held, matching its failure ownership contract; it is not a naturally observed Windows fault. Current returns once without consuming the still-outstanding policy ticket; normal Drain keeps waiting and completes only after actual DRAIN acknowledgement. PolicyOccupied/requested/completed and Drains0 are observed while policy remains in flight. After policy release the actual worker drains, retains user loans, then independently retains nativeActive1 after all owners cancel/release and join. Every external queue/Drain caller joins before finalClose consumes the handle; actual callback OS-thread handles, deferred tail pin, exact DLL detach resource ledger and real unload are checked. Full processes warm12 then measure12 schedules with process handle equality. Guarded mode protects native core/slot allocations and mapping; shared host configuration is ordinary live host storage, not claimed guarded. The negative changes only the unique Drain non-timeout wait failure break, retaining result=PB_CORRUPT. Its unexpected retry is observed while policy remains in flight; the wrapper calls genuine SleepConditionVariableSRW, releasing/reacquiring State without a held-State hard gate. Late policy/real Drain then permit safe completion, all public callers/owners join, normal native0/finalClose and full callback joins/unload precede123. No Draining/reference counter is edited and no post-consumption public call or unsafe finalClose-with-Wait is used. Native registration/IO/reference state are hosted; no real device/driver/registry/NDIS/IRQL, Windows7 runtime or CPU-performance proof. Both host and DLL reject PB_API_TESTS and statically require the shipping65-byte source fingerprint; version remains2.0.0. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    audit_sources=@('tests\model\drain_policy_fixture.h','tests\safety\drain_policy_fixture.c','tests\model\drain_policy_tests.c','tools\build\drain-policy-tests.ps1','tools\audit\verify-drain-policy.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
