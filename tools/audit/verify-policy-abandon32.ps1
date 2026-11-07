param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/policy-abandon32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Policy abandonment version must remain2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted='0f920bb470e314c293d212d5b26f4108f7cbb2e26086b88c6e1a5f4099788b61'
# This independent DLL group does not alter api/v2. Record only the two exact
# shared-core repairs separately approved and tested by the kernel audit.
$approvedSharedRepairs=@(
    @{path='common/v2_core.c';before='2426da12e2089d731acfd234dc7f66e5c73311da0b1734d5e7cd1cac7884e883';after='8cf8d1cf2475183a4e366f68f3586e82233673640660cda7797b2a3c9d3ca3ca'},
    @{path='common/v2_core.h';before='2994f94b4b76f0e08b92474badc8f1d05b5214eb400fb2723c035093bea062a1';after='19ed21b8d73f8fe81f3bb0a08ebb982d68bb4d2ce6b3b889a4aef91d885073d0'}
)

if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal' -or $paths.Count -ne 719 -or @($manifest.files.PSObject.Properties).Count -ne 719 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected policy abandonment accepted baseline'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$seen.Add($path)){throw 'Duplicate policy baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Policy baseline bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($path in $manifest.files.PSObject.Properties.Name){if(!$seen.Contains($path)){throw 'Policy baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Policy baseline identity differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate policy dependencies'}
$dependencies+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/policy-abandon32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($path in $dependencies){$raw=[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $path));$copy=Join-Path $baseline $path;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,$raw);$baselineHashes[$path]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($raw)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-PaDependencies {
    $actual=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate current policy closure'}
    $actual+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
    $actual=[Collections.Generic.HashSet[string]]::new([string[]]@($actual|ForEach-Object {$_.Replace('\','/')}),[StringComparer]::Ordinal)
    if(!$actual.SetEquals([string[]]$dependencies)){throw 'Policy dependency path set changed'}
    foreach($path in $dependencies){
        $before=Join-Path $baseline $path;$current=Join-Path $script:RepoRoot $path;$oldHash=(Get-FileHash $before).Hash.ToLowerInvariant();$newHash=(Get-FileHash $current).Hash.ToLowerInvariant()
        $repair=@($approvedSharedRepairs|Where-Object {$_.path -ceq $path})
        if($repair.Count){
            if($repair.Count-ne1 -or $newHash-cne$repair[0].after -or $oldHash-cne$(if($historical){$repair[0].before}else{$repair[0].after}) -or $oldHash-cne$baselineHashes[$path]){throw "Policy exact shared-core repair differs:$path"}
        }elseif($oldHash-cne$baselineHashes[$path] -or $newHash-cne$oldHash){throw "Policy unchanged dependency differs:$path"}
    }
}
Assert-PaDependencies
$reviewedDelta=@();if($historical){$reviewedDelta=@($approvedSharedRepairs)}
function Convert-PaIncludes([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-PaBuild($build){
    $e=$build.extraction;$v2=Join-Path $script:RepoRoot 'api/v2';$test=Join-Path $script:RepoRoot 'tests/model/policy_abandon32_tests.c';$place=Split-Path $build.path
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms')){throw 'Policy shipping mode differs'}
    if($build.path -cne (Join-Path $place 'policy-abandon32.exe') -or $build.fixture -cne (Join-Path $place 'policy-abandon32.dll')){throw 'Policy binary path differs'}
    foreach($name in @('session','entry','fixture_entry')){$leaf=switch($name){'session'{'session.c'}'entry'{'session-entry.h'}'fixture_entry'{'fixture.c'}};if($e.$name -cne (Join-Path $place $leaf)){throw 'Policy generated source path differs'}}
    $source=Join-Path $v2 'session.c'
    if($e.source -cne $source -or $e.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($e.session) -cne (Convert-PaIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Policy complete session body differs'}
    $lines=@('#define SleepConditionVariableSRW AbandonSleep',('#include "'+$e.session.Replace('\','/')+'"'),'#undef SleepConditionVariableSRW')
    if([IO.File]::ReadAllText($e.entry) -cne (($lines -join "`n")+"`n")){throw 'Policy SDK observer entry differs'}
    $lines=@('#define P_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Policy whole fixture entry differs'}
    $fixture=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if((Get-FileHash $fixture).Hash.ToLowerInvariant() -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'){throw 'Policy frozen native-policy ledger changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$place\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Policy exact compiler flags differ'}
    $expected=@($source,$e.session,$e.entry,$e.fixture_entry,$test,$fixture,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),(Join-Path $script:RepoRoot 'include/packetbridge_version.h'),
        (Join-Path $script:RepoRoot 'common/v2_core.c'),(Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    if(@($build.sources).Count -ne 12){throw 'Policy source count must be12'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($s in $build.sources){if(!$paths.Add([string]$s.path)){throw 'Policy duplicate source metadata'}}
    $expectedSet=[Collections.Generic.HashSet[string]]::new([string[]]$expected,[StringComparer]::Ordinal)
    if($expectedSet.Count -ne 12 -or !$paths.SetEquals($expectedSet)){throw 'Policy source path set differs'}
    foreach($s in $build.sources){if((Get-FileHash -LiteralPath $s.path).Hash.ToLowerInvariant() -cne $s.sha256){throw 'Policy source raw hash differs'}}
    foreach($key in @('path','fixture')){$hash=if($key-eq'path'){$build.sha256}else{$build.fixture_sha256};if((Get-FileHash -LiteralPath $build.$key).Hash.ToLowerInvariant() -cne $hash){throw 'Policy binary hash differs'}}
    foreach($key in @('session','entry','fixture_entry')){if((Get-FileHash $e.$key).Hash.ToLowerInvariant() -cne $e.($key+'_sha256')){throw 'Policy extraction raw hash differs'}}
}
function Test-PaMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;Assert-PaBuild ($json|ConvertFrom-Json);$results=@()
    foreach($kind in @('deleted-source','duplicate-source','compiler-flags','generated-path')){
        $candidate=$json|ConvertFrom-Json
        $expected=switch($kind){
            'deleted-source'{$candidate.sources=@($candidate.sources)[0..10];'Policy source count must be12'}
            'duplicate-source'{$candidate.sources[11]=$candidate.sources[0];'Policy duplicate source metadata'}
            'compiler-flags'{$candidate.flags=@($candidate.flags)+'/DPB_API_TESTS=1';'Policy exact compiler flags differ'}
            'generated-path'{$candidate.extraction.entry=$candidate.extraction.session;'Policy generated source path differs'}
        }
        $reason=$null;try{Assert-PaBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Policy metadata accepted/wrong rejection:$kind ($reason)"};$results+=@{name=$kind;rejected=$true;reason=$reason}
    };return $results
}
function Get-PaRows([string]$text,[int]$family){
    if([regex]::Matches($text,'(?m)^FAMILY: ').Count -ne 1 -or ![regex]::IsMatch($text,"(?m)^FAMILY: IPv$family`r?$")){throw 'Policy family witness differs'}
    $rows=[regex]::Matches($text,'(?m)^CASE ip=([46]) protocol=(6|17) failure=([01]) forget=([01]) measured=([01]) loans=32 published=32 sync_failure=1 sync_join=1 old_stale=2 poll_join=2 owner_join=1 io_calls=2 released=32 reconciled=32 prepared=(\d+) final=1 callbacks=(\d+) owned=0 unload=1\r?$')
    if($rows.Count-ne16 -or [regex]::Matches($text,'(?m)^CASE ').Count-ne16){throw 'Policy case count/format differs'};$callbacks=0
    for($i=0;$i-lt16;++$i){$shape=$i%8;$g=$rows[$i].Groups;$prepared=[int]$g[6].Value;$joined=[int]$g[7].Value
        if([int]$g[1].Value-ne$family -or [int]$g[2].Value-ne$(if($shape-ge4){17}else{6}) -or [int]$g[3].Value-ne([int][Math]::Floor($shape/2)%2) -or [int]$g[4].Value-ne($shape%2) -or [int]$g[5].Value-ne[int]($i-ge8) -or $prepared-lt1 -or $prepared-ge8 -or $joined-ne($prepared+1)){throw "Policy family/shape/ownership row differs:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: policy-abandon32 cases=(\d+) warm=(\d+) measured=(\d+) owner_joins=(\d+) sync_joins=(\d+) poll_joins=(\d+) callback_joins=(\d+) defensive_failures=(\d+) owned=0 true_unload=1\r?$')
    $handles=[regex]::Matches($text,'(?m)^HANDLES before=(\d+) after=(\d+)\r?$')
    if($summary.Count-ne1 -or [regex]::Matches($text,'(?m)^PASS:').Count-ne1 -or $handles.Count-ne1){throw 'Policy summary missing/extra'};$g=$summary[0].Groups
    if([int]$g[1].Value-ne16 -or [int]$g[2].Value-ne8 -or [int]$g[3].Value-ne8 -or [int]$g[4].Value-ne16 -or [int]$g[5].Value-ne16 -or [int]$g[6].Value-ne32 -or [int]$g[7].Value-ne$callbacks -or [int]$g[8].Value-ne16 -or $handles[0].Groups[1].Value-cne$handles[0].Groups[2].Value){throw 'Policy exact sums/handles differ'}
    return [ordered]@{cases=16;warmup_schedules=8;measured_schedules=8;owner_os_thread_joins=16;sync_os_thread_joins=16;poll_os_thread_joins=32;callback_os_thread_joins=$callbacks;defensive_wait_failures=16;old_stale_results=32;native_policy_io_calls=32;published_reconciled=512;user_loans_returned=512;process_handles_before=[int]$handles[0].Groups[1].Value;process_handles_after=[int]$handles[0].Groups[2].Value}
}
Start-TestRecording -Directory $directory -Name 'operation-results.json';$builds=@();$firstLog=$null
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $build=Build-PolicyAbandon32Tests -Directory (Join-Path $directory "ipv$family-$mode-$guarded") -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-PaBuild $build;$builds+=,$build
    $psi=[Diagnostics.ProcessStartInfo]::new($build.path);$psi.ArgumentList.Add($build.fixture);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(150000)){$process.Kill($true);$process.WaitForExit();throw 'Policy process watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$family,$mode,$guarded)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));if($process.ExitCode){throw "Policy process exit$($process.ExitCode):$log"}
        $metrics=Get-PaRows $output $family
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$family;mode=$mode;guarded=$guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')});if(!$firstLog){$firstLog=$log}
    } finally { $process.Dispose() }
}}}
$metadata=Test-PaMetadata $builds[0];$original=[IO.File]::ReadAllText($firstLog);$parsers=@()
foreach($kind in @('wrong-family','missing-row','duplicate-row','wrong-callbacks')){
    $candidate=switch($kind){
        'wrong-family'{$original.Replace('CASE ip=4 ','CASE ip=6 ')}
        'missing-row'{[regex]::Replace($original,'(?m)^CASE ip=4 protocol=6 failure=0 forget=0 measured=0 [^\r\n]*\r?\n','')}
        'duplicate-row'{$original+"`n"+[regex]::Match($original,'(?m)^CASE [^\r\n]*').Value+"`n"}
        'wrong-callbacks'{[regex]::Replace($original,'(?m)^(CASE [^\r\n]* prepared=)\d+','${1}0')}
    }
    $expected=if($kind-in@('missing-row','duplicate-row')){'Policy case count/format differs'}else{'Policy family/shape/ownership row differs:0'}
    $reason=$null;try{Get-PaRows $candidate 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason-cne$expected){throw "Policy parser accepted/wrong rejection:$kind ($reason)"};$parsers+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();foreach($kind in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $out=Join-Path $directory "compile-contracts/$kind";New-Item -ItemType Directory -Path $out -Force|Out-Null;$flags=@($builds[0].flags|Where-Object {$_ -notlike '/Fo*'})
    $diagnostic=switch($kind){
        'pb-api-tests'{$flags+='/DPB_API_TESTS=1';'Shipping policy abandonment required'}
        'missing-ndebug'{$flags=@($flags|Where-Object {$_-cne'/DNDEBUG'});'Shipping NDEBUG required'}
        'missing-fingerprint'{$flags=@($flags|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'});'shipping fingerprint required'}
        'invalid-family'{$flags=@($flags|Where-Object {$_-notlike'/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0';'Select one IP family'}
    }
    $lines=@(& cl.exe @flags /c (Join-Path $script:RepoRoot 'tests/model/policy_abandon32_tests.c') "/Fo$out/contract.obj" 2>&1);$exit=$LASTEXITCODE;$log=Join-Path $out 'compiler.log';$lines|Set-Content -LiteralPath $log -Encoding utf8
    if(!$exit -or ![regex]::IsMatch(($lines-join"`n"),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($diagnostic))){throw "Policy compiler contract rejected wrong condition:$kind"}
    $contracts+=@{contract=$kind;expected_failure=$true;exit_code=$exit;expected_diagnostic=$diagnostic;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-PaDependencies
if((Get-SourceFingerprint)-cne$script:SourceFingerprint){throw 'Sources changed during policy abandonment audit'}
Write-TestRecording 'passed'
$families=@(foreach($family in @(4,6)){$rows=@($script:TestExecutions|Where-Object {$_.ip_version-eq$family});[ordered]@{ip_version=$family;executed_processes=$rows.Count;cases=($rows|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;owner_os_thread_joins=($rows|ForEach-Object {$_.metrics.owner_os_thread_joins}|Measure-Object -Sum).Sum;callback_os_thread_joins=($rows|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{schema_version=1;version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$contracts;parser_checks=$parsers;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=($dependencies.Count-$reviewedDelta.Count);reviewed_delta=$reviewedDelta;reviewed_production_repairs=@($reviewedDelta|ForEach-Object {$_.path});production_closure_changed=[bool]$reviewedDelta.Count;production_closure_change_reference=$(if($historical){'accepted-baseline'}else{'current-snapshot'});approved_shared_repair_contracts=$approvedSharedRepairs;api_runtime_body_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='No additional DLL defect or DLL production repair confirmed; independent approved common/v2_core.c/h scoped-generation repair is recorded as an exact before/after pair, never a broad ignore. Whole unchanged native-policy ledger executes shipping session/queue/lifecycle/control in an unloadable DLL with NDEBUG. One owner actually Reserve/Commit32 valid1500B packets and Receive32 independent hosted TX_READY packets. Actual packet extension negotiation precedes experimental/check-segments Open flags. Synchronous SetPolicy issues ticket1/nonzero family-checksum policy. One explicit CV failure releases State, waits for real IO1 entry, restores State, and returns ERROR_OPERATION_ABORTED; actual SetPolicy auto-Forget abandons its hidden ticket while IO remains owned. The caller joins and its thread marker is cleared before new OS threads can reuse its ID. Two old PollPolicy callers execute real SDK CV waits. After actual IO1 fails, their SDK return observers release State on two hard gates; completion clears the abandoned occupied slot. Real work-callback join stabilizes ticket1 completion before a second actual IO is re-armed only through test-owned gate/mode fields. Ticket2 stays occupied while old Forget(1)/Poll(1) and both resumed old waiters return STALE without touching outputs or clearing the new request. Their post-gate sleep counters do not grow; earlier SDK spurious wakes are not constrained. BeginStop occurs after ticket2 entered IO; late success/failure is consumed or explicitly forgotten without running a third request. Original owner validates held payload, ReleaseBatch32 and joins; hosted NativeActive1 independently prevents preparation until set0 at a lifecycle park. Only a trusted STOPPED acknowledgement permits Published32 reconciliation. Actual work-callback waits establish Prepared before final Close; all owner/sync/poller callers join before that successful call consumes the public reference. Ordinary LoadLibrary covers DLL calls; after dropping it, only SDK host gates and every captured callback OS-thread handle are used until real DLL detach/unload and inherited exact resource ledgers become0. Native IO/reference/capability values and CV failure are modeled boundary inputs, not actual Windows failures, SYS/NDIS/IRQL/old-system execution. Fixed6.20 capability model supports the selected family checksum mask; no modern-NDIS or old-layout proof claimed. Guard protects inherited native core/slot/map allocations; caller config/stack and SDK objects retain original allocators. SDK wait-entry/return gates are not kernel scheduler-park proof. Counts concern API/ownership/wait attempts, not payload memcpy, native CPU, throughput, latency or sustained driver leaks. Default mode snapshots and authenticates current raw closure without historical out. Explicit719-file0f92 typed-Ordinal baseline is fully authenticated; only the two individually reviewed common files may differ by their exact SHA pairs, all other dependency bytes remain identical. Version2.0.0 preserved.';
    audit_sources=@('tests/model/policy_abandon32_tests.c','tools/build/policy-abandon32-tests.ps1','tools/audit/verify-policy-abandon32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
