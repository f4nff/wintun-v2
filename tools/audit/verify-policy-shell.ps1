param([string]$BaselineDirectory='out\retirement-retry-baseline\03bf04fcdbb3',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\policy-shell-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Policy shell audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='9d06e10637c4291c0479735b8ab0e54d41706dbfc0b1e923b4c3e42e00c8b459'
 'api\v2\session_lifecycle.h'='33806814b12535da7304b63d544aa0b31c9862a8c7b465eb004d374a9455392d'
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
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\policy-shell\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-PolicyShellProjection($build) {
    $api=Join-Path $script:RepoRoot 'api\v2';$original=[IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session_lifecycle.h'));$expected=$original
    $before='        if (!s->PolicyOccupied || ticket != s->Progress.PolicyRequested) { result = PB_STALE; break; }'
    $after='        if (ticket != s->Progress.PolicyRequested) { result = PB_STALE; break; }'
    if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'Frozen occupied check not unique'}
    if($build.variant -eq 'without-occupancy-check') {
        $expected=$expected.Replace($before,$after)
        if($build.mutation.offset -ne $original.IndexOf($before) -or $build.mutation.original -cne $before -or $build.mutation.changed -cne $after -or $build.mutation.length -ne $before.Length){throw 'Mutation ledger changed'}
    }elseif($null -ne $build.mutation){throw 'Current has an unexpected mutation'}
    if([IO.File]::ReadAllText($build.extraction.selected) -cne $expected){throw 'Lifecycle changed beyond the unique occupied check omission'}
    if($build.variant -eq 'current' -and $build.extraction.selected_sha256 -ne $frozen['api\v2\session_lifecycle.h']){throw 'Current lifecycle is not byte-identical'}
    $first='static PB_RESULT WINAPI PollPolicy(PB_SESSION *s, uint64_t ticket, uint32_t timeout, PB_RESULT *operation)'
    $next='static PB_RESULT WINAPI GetCloseProgress(PB_SESSION *s, PB_CLOSE_PROGRESS *out)';$offset=$original.IndexOf($first);$end=$original.IndexOf($next,$offset)
    if([IO.File]::ReadAllText($build.extraction.poll_original) -cne $original.Substring($offset,$end-$offset) -or $build.extraction.offset -ne $offset -or
        $build.extraction.length -ne $end-$offset -or $build.extraction.first -cne $first -or $build.extraction.next -cne $next){throw 'Actual PollPolicy extraction changed'}
    $session=[IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session.c'))
    $session=[regex]::Replace($session,'(?m)^#include "([^"]+)"',{
        param($match);$include=if($match.Groups[1].Value -eq 'session_lifecycle.h'){$build.extraction.selected}else{[IO.Path]::GetFullPath((Join-Path $api $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    if([IO.File]::ReadAllText($build.session) -cne $session){throw 'Complete session changed beyond includes'}
    $entry=@'
#define SleepConditionVariableSRW PolicySleep
#include PB_POLICY_SHELL_SESSION_SOURCE
#undef SleepConditionVariableSRW
'@
    if([IO.File]::ReadAllText($build.entry) -cne $entry){throw 'Sleep observer entry changed'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_POLICY_SHELL_SESSION_SOURCE "'+$build.session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$build.entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\policy_shell_fixture.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Fixture include overrides changed'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -ne $source.sha256){throw 'Source/artifact hash changed'}}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-PolicyShellProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start Policy shell test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Policy shell external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-policy-shell.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='CLOSED_POLICY_ALREADY_RETIRED_ACKNOWLEDGED: occupancy guard omitted; completed ticket acknowledged after peer consume/forget, all pollers joined before finalClose and actual unload, every owned resource closed'
        $summary=[regex]::Match($output,'PASS: (\d+) actual nonPB policy Closed-shell schedules; (\d+) poller joins, (\d+) callback OS-thread joins; pub-only ref/pin preserved through physical close/runtime removal, winner/consume/forget outputs, final Close strictly after reader joins, DLL unload; handles (\d+)/(\d+); repeat confirmations(\d+)')
        $full=$Arguments.Count -eq 1;$count=if($full){36}else{4};$readers=if($full){132}else{32};$repeats=if($Expected -eq 126){32}else{0}
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 126 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $readers -and
            [int]$summary.Groups[3].Value -eq $count -and $summary.Groups[4].Value -eq $summary.Groups[5].Value -and [int]$summary.Groups[6].Value -eq $repeats}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 126 -and $correct);
            watchdog_ms=30000;log=$log;schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});poller_joins=$(if($summary.Success){[int]$summary.Groups[2].Value}else{0});
            callback_thread_joins=$(if($summary.Success){[int]$summary.Groups[3].Value}else{0});repeat_confirmations=$(if($summary.Success){[int]$summary.Groups[6].Value}else{0});
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Policy shell test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-occupancy-check')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-PolicyShellTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-PolicyShellProjection $built
    if($variant -eq 'current'){Invoke-PolicyShellProcess $built.path @($built.fixture);Invoke-PolicyShellProcess $built.path @($built.fixture,'126')}
    else{Invoke-PolicyShellProcess $built.path @($built.fixture,'126') 126}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during Policy shell audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-1);
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;operations=$script:TestResultFile;executables=$executables;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    reviewed_dependency_repairs=@('common/offload_policy.c');
    method='Original session paths and native-policy host/ledger remain unchanged; common/offload_policy.c alone includes the separately reviewed full-file LSO/checksum dependency repair. Actual non-PB_API_TESTS full DLL session/lifecycle/control executes with real SDK locks/events/condition waits, private max-one thread pool, module pins and unload. While nonzero policy is held after actual IO entry, Close(0) first returns non-success with no public calls active; one/two/eight new PollPolicy users then begin on the still externally owned public handle. Each genuine SDK condition wait wakes on actual policy completion. An observer briefly unlocks State before gating and reacquires it before returning, modeling another caller winning reacquisition without editing state or adding references. These are SDK call-boundary observations, not claims of opaque kernel parking. NativeActive1 first prevents release; after native0, actual background close frees region/device, removes runtime/registry ownership, transfers the tail pin and drops work ownership while every poller remains paused without Lifetime. A real WaitForThreadpoolWorkCallbacks is called using the still-public shell, confirming callback completion before observing References1/PublicReleased0/Pins1/RuntimeCount0/Regions0 and one live core/work/pool. No extra Retain is used. Those tracked pins describe core ownership after tail transfer, not raw loader reference-count enumeration. With the closed public shell still alive, either one poller alone consumes the completed ticket, a peer Poll consumes first, or Forget abandons it. Current old waiters return STALE with output sentinel unchanged; actual BeginSetPolicy rejects new requests after stop, so no synthetic replacement is introduced. Both actual late native policy success/failure are covered. All public pollers join before final successful Close consumes the handle; actual work/pool destruction, each retained callback OS-thread handle join, ordinary FreeLibrary and real aggregate DLL detach prove exact owned-resource convergence. Full workload warms18 then measures18 cases; CLI126 consumes/forgets in two checked warm and two measured cases. Every process requires strict process-handle equality after warmup, never excludes or closes unknown SDK handles. Guarded mode protects native core/slots/mapping; live host config is ordinary storage. Negative126 omits only PolicyOccupied in the real PollPolicy loop while keeping ticket validation. On completed closed-shell tickets already consumed/forgotten, it records32 extra acknowledgements, then follows identical public caller joins, finalClose, callback OS joins and actual unload before126. No reference/counter repair or UAF, no finalClose concurrent public calls, no new production defect claim. Native IO/registration/reference counts are hosted; no real device/driver/registry/NDIS/IRQL, Windows7 runtime or CPU-performance proof. Both host and DLL reject PB_API_TESTS and statically require the shipping65-byte source fingerprint. Version remains2.0.0. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    audit_sources=@('tests\model\policy_shell_fixture.h','tests\safety\policy_shell_fixture.c','tests\model\policy_shell_tests.c','tools\build\policy-shell-tests.ps1','tools\audit\verify-policy-shell.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
