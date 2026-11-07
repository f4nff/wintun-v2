param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/retirement-hint32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Retirement hints version must remain2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted='4c7c7832d59cc9cd09e8b480eb902aea3ae6c26979c688d26db9dccfed0c168b'
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal' -or $paths.Count -ne 727 -or @($manifest.files.PSObject.Properties).Count -ne 727 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected retirement hints accepted baseline'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$seen.Add($path)){throw 'Duplicate status baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Hint baseline bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($path in $manifest.files.PSObject.Properties.Name){if(!$seen.Contains($path)){throw 'Hint baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Hint baseline identity differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate status dependencies'}
$dependencies+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/retirement-hint32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($path in $dependencies){$raw=[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $path));$copy=Join-Path $baseline $path;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,$raw);$baselineHashes[$path]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($raw)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-RhDependencies {
    $actual=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate current status closure'}
    $actual+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
    $actual=[Collections.Generic.HashSet[string]]::new([string[]]@($actual|ForEach-Object {$_.Replace('\','/')}),[StringComparer]::Ordinal)
    if(!$actual.SetEquals([string[]]$dependencies)){throw 'Hint dependency path set changed'}
    foreach($path in $dependencies){$before=Join-Path $baseline $path;$current=Join-Path $script:RepoRoot $path;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $baselineHashes[$path] -or (Get-FileHash $current).Hash -cne (Get-FileHash $before).Hash){throw "Hint unchanged dependency differs:$path"}}
}
Assert-RhDependencies
function Convert-RhIncludes([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-RhBuild($build){
    $e=$build.extraction;$v2=Join-Path $script:RepoRoot 'api/v2';$test=Join-Path $script:RepoRoot 'tests/model/retirement_hint32_tests.c';$place=Split-Path $build.path
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms')){throw 'Hint shipping mode differs'}
    if($build.path -cne (Join-Path $place 'retirement-hint32.exe') -or $build.fixture -cne (Join-Path $place 'retirement-hint32.dll')){throw 'Hint binary path differs'}
    foreach($name in @('session','entry','fixture_entry')){$leaf=switch($name){'session'{'session.c'}'entry'{'session-entry.h'}'fixture_entry'{'fixture.c'}};if($e.$name -cne (Join-Path $place $leaf)){throw 'Hint generated source path differs'}}
    $source=Join-Path $v2 'session.c'
    if($e.source -cne $source -or $e.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($e.session) -cne (Convert-RhIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Hint complete session body differs'}
    $lines=@('#define SetEvent HintSignal','#define WaitForMultipleObjects HintWait','#undef WaitForSingleObject','#define WaitForSingleObject HintProgress',('#include "'+$e.session.Replace('\','/')+'"'),'#undef SetEvent','#undef WaitForMultipleObjects','#undef WaitForSingleObject')
    if([IO.File]::ReadAllText($e.entry) -cne (($lines -join "`n")+"`n")){throw 'Hint SDK observer entry differs'}
    $lines=@('#define H_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Hint whole fixture entry differs'}
    $fixture=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if((Get-FileHash $fixture).Hash.ToLowerInvariant() -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'){throw 'Hint frozen native-policy ledger changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$place\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Hint exact compiler flags differ'}
    $expected=@($source,$e.session,$e.entry,$e.fixture_entry,$test,$fixture,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),(Join-Path $script:RepoRoot 'include/packetbridge_version.h'),
        (Join-Path $script:RepoRoot 'common/v2_core.c'),(Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    if(@($build.sources).Count -ne 12){throw 'Hint source count must be12'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($s in $build.sources){if(!$paths.Add([string]$s.path)){throw 'Hint duplicate source metadata'}}
    $expectedSet=[Collections.Generic.HashSet[string]]::new([string[]]$expected,[StringComparer]::Ordinal)
    if($expectedSet.Count -ne 12 -or !$paths.SetEquals($expectedSet)){throw 'Hint source path set differs'}
    foreach($s in $build.sources){if((Get-FileHash -LiteralPath $s.path).Hash.ToLowerInvariant() -cne $s.sha256){throw 'Hint source raw hash differs'}}
    foreach($key in @('path','fixture')){$hash=if($key-eq'path'){$build.sha256}else{$build.fixture_sha256};if((Get-FileHash -LiteralPath $build.$key).Hash.ToLowerInvariant() -cne $hash){throw 'Hint binary hash differs'}}
    foreach($key in @('session','entry','fixture_entry')){if((Get-FileHash $e.$key).Hash.ToLowerInvariant() -cne $e.($key+'_sha256')){throw 'Hint extraction raw hash differs'}}
}
function Test-RhMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;Assert-RhBuild ($json|ConvertFrom-Json);$results=@()
    foreach($kind in @('deleted-source','duplicate-source','compiler-flags','generated-path')){
        $candidate=$json|ConvertFrom-Json
        $expected=switch($kind){
            'deleted-source'{$candidate.sources=@($candidate.sources)[0..10];'Hint source count must be12'}
            'duplicate-source'{$candidate.sources[11]=$candidate.sources[0];'Hint duplicate source metadata'}
            'compiler-flags'{$candidate.flags=@($candidate.flags)+'/DPB_API_TESTS=1';'Hint exact compiler flags differ'}
            'generated-path'{$candidate.extraction.entry=$candidate.extraction.session;'Hint generated source path differs'}
        }
        $reason=$null;try{Assert-RhBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Hint metadata accepted/wrong rejection:$kind ($reason)"};$results+=@{name=$kind;rejected=$true;reason=$reason}
    };return $results
}
function Get-RhRows([string]$text,[int]$family){
    if([regex]::Matches($text,'(?m)^FAMILY: ').Count -ne 1 -or ![regex]::IsMatch($text,"(?m)^FAMILY: IPv$family`r?$")){throw 'Hint family witness differs'}
    $rows=[regex]::Matches($text,'(?m)^CASE ip=([46]) protocol=(6|17) ndis=(20|89) first=([01]) measured=([01]) release=32 cancel=32 stop_waits=2 signals=2 merged_state=1 sdk_wakes=1 data_free=1 lifetime_free=0 prepared_pins=2 owner_join=2 prepared=(\d+) final=1 callbacks=(\d+) owned=0 unload=1\r?$')
    if($rows.Count-ne16 -or [regex]::Matches($text,'(?m)^CASE ').Count-ne16){throw 'Hint case count/format differs'};$callbacks=0
    for($i=0;$i-lt16;++$i){$shape=$i%8;$g=$rows[$i].Groups;$prepared=[int]$g[6].Value;$joined=[int]$g[7].Value
        if([int]$g[1].Value-ne$family -or [int]$g[2].Value-ne$(if($shape-ge4){17}else{6}) -or [int]$g[3].Value-ne$(if([int][Math]::Floor($shape/2)%2){89}else{20}) -or [int]$g[4].Value-ne($shape%2) -or [int]$g[5].Value-ne[int]($i-ge8) -or $prepared-lt1 -or $prepared-ge8 -or $joined-ne($prepared+1)){throw "Hint family/shape/ownership row differs:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: retirement-hint32 cases=(\d+) warm=(\d+) measured=(\d+) owner_joins=(\d+) callback_joins=(\d+) coalesced_wakes=(\d+) owned=0 true_unload=1\r?$')
    $handles=[regex]::Matches($text,'(?m)^HANDLES before=(\d+) after=(\d+)\r?$')
    if($summary.Count-ne1 -or [regex]::Matches($text,'(?m)^PASS:').Count-ne1 -or $handles.Count-ne1){throw 'Hint summary missing/extra'};$g=$summary[0].Groups
    if([int]$g[1].Value-ne16 -or [int]$g[2].Value-ne8 -or [int]$g[3].Value-ne8 -or [int]$g[4].Value-ne32 -or [int]$g[5].Value-ne$callbacks -or [int]$g[6].Value-ne16 -or $handles[0].Groups[1].Value-cne$handles[0].Groups[2].Value){throw 'Hint exact sums/handles differ'}
    return [ordered]@{cases=16;warmup_schedules=8;measured_schedules=8;owner_os_thread_joins=32;callback_os_thread_joins=$callbacks;release_calls=16;cancel_calls=16;tx_loans_released=512;rx_loans_cancelled=512;successful_retirement_hints=32;coalesced_sdk_wakes=16;stopped_user_wait_calls=32;prepared_lifetime_pins_per_case=2;process_handles_before=[int]$handles[0].Groups[1].Value;process_handles_after=[int]$handles[0].Groups[2].Value}
}
Start-TestRecording -Directory $directory -Name 'operation-results.json';$builds=@();$firstLog=$null
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $build=Build-RetirementHint32Tests -Directory (Join-Path $directory "ipv$family-$mode-$guarded") -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-RhBuild $build;$builds+=,$build
    $psi=[Diagnostics.ProcessStartInfo]::new($build.path);$psi.ArgumentList.Add($build.fixture);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(150000)){$process.Kill($true);$process.WaitForExit();throw 'Hint process watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$family,$mode,$guarded)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));if($process.ExitCode){throw "Hint process exit$($process.ExitCode):$log"}
        $metrics=Get-RhRows $output $family
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$family;mode=$mode;guarded=$guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')});if(!$firstLog){$firstLog=$log}
    } finally { $process.Dispose() }
}}}
$metadata=Test-RhMetadata $builds[0];$original=[IO.File]::ReadAllText($firstLog);$parsers=@()
foreach($kind in @('wrong-family','missing-row','duplicate-row','wrong-callbacks')){
    $candidate=switch($kind){
        'wrong-family'{$original.Replace('CASE ip=4 ','CASE ip=6 ')}
        'missing-row'{[regex]::Replace($original,'(?m)^CASE ip=4 protocol=6 ndis=20 first=0 measured=0 [^\r\n]*\r?\n','')}
        'duplicate-row'{$original+"`n"+[regex]::Match($original,'(?m)^CASE [^\r\n]*').Value+"`n"}
        'wrong-callbacks'{[regex]::Replace($original,'(?m)^(CASE [^\r\n]* prepared=)\d+','${1}0')}
    }
    $expected=if($kind-in@('missing-row','duplicate-row')){'Hint case count/format differs'}else{'Hint family/shape/ownership row differs:0'}
    $reason=$null;try{Get-RhRows $candidate 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason-cne$expected){throw "Hint parser accepted/wrong rejection:$kind ($reason)"};$parsers+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();foreach($kind in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $out=Join-Path $directory "compile-contracts/$kind";New-Item -ItemType Directory -Path $out -Force|Out-Null;$flags=@($builds[0].flags|Where-Object {$_ -notlike '/Fo*'})
    $diagnostic=switch($kind){
        'pb-api-tests'{$flags+='/DPB_API_TESTS=1';'Shipping retirement hints required'}
        'missing-ndebug'{$flags=@($flags|Where-Object {$_-cne'/DNDEBUG'});'Shipping NDEBUG required'}
        'missing-fingerprint'{$flags=@($flags|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'});'shipping fingerprint required'}
        'invalid-family'{$flags=@($flags|Where-Object {$_-notlike'/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0';'Select one IP family'}
    }
    $lines=@(& cl.exe @flags /c (Join-Path $script:RepoRoot 'tests/model/retirement_hint32_tests.c') "/Fo$out/contract.obj" 2>&1);$exit=$LASTEXITCODE;$log=Join-Path $out 'compiler.log';$lines|Set-Content -LiteralPath $log -Encoding utf8
    if(!$exit -or ![regex]::IsMatch(($lines-join"`n"),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($diagnostic))){throw "Hint compiler contract rejected wrong condition:$kind"}
    $contracts+=@{contract=$kind;expected_failure=$true;exit_code=$exit;expected_diagnostic=$diagnostic;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-RhDependencies
if((Get-SourceFingerprint)-cne$script:SourceFingerprint){throw 'Sources changed during retirement hints audit'}
Write-TestRecording 'passed'
$families=@(foreach($family in @(4,6)){$rows=@($script:TestExecutions|Where-Object {$_.ip_version-eq$family});[ordered]@{ip_version=$family;executed_processes=$rows.Count;cases=($rows|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;owner_os_thread_joins=($rows|ForEach-Object {$_.metrics.owner_os_thread_joins}|Measure-Object -Sum).Sum;callback_os_thread_joins=($rows|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{schema_version=1;version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$contracts;parser_checks=$parsers;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only, no production defect/fix/before variant. Whole unchanged native-policy resource ledger includes actual NDEBUG shipping session through absolute includes and three lexical SDK observers for SetEvent/WaitForMultipleObjects/WaitForSingleObject. Two unique real public queue-owner threads hold32 valid1500B leases each: q0 Receive32, q1 Reserve32. Both call actual Wait and observe BeginStop as STOPPED through the real selected SDK wait; entry markers do not claim kernel scheduler park. The background worker first consumes the genuine initial BeginStop Progress hint, and non-consuming NtQueryEvent verifies EventState0 at the next park before measuring returns. In both explicit orders each original owner performs actual successful Release32 or Cancel32. Its real Progress SetEvent has already returned before a test tail gate blocks the finite API, whose DataGate shared pin has been released while Lifetime shared is still held. Two actual hints coalesce as one auto-reset signaled state; no manual reset, counters, references or owner bookkeeping is repaired. Real worker SDK wait consumes precisely one additional wake and queries hosted NativeActive1, independently preventing preparation. NativeActive0 then permits a trusted STOPPED acknowledgement/Prepared while both finite API tails still retain Lifetime. Actual work-callback joins and a non-final PrepareClose establish stable Prepared, DataGate exclusive is available, Lifetime exclusive remains unavailable and mapping/module/registry resources remain owned. Only after both actual callers return and their OS threads join does final Close consume the public handle. Ordinary LoadLibrary ownership covers all DLL calls; after release only host SDK events and every captured callback OS-thread handle are used through actual detach/unload and inherited exact resource/quota ledger0. Callback stages are recorded dynamically as prep plus one final callback. No fakeFULL, malformed reply, native completion generation replay or finalClose concurrency is modeled. Native IO/reference/capability values are hosted; NDIS6.20/6.89 variables do not prove either target OS execution. Guard protects inherited native core/slot/map allocations; caller/SDK objects retain their allocators. Counts are API/ownership/event observations, not payload-copy/nativeCPU/throughput/latency or sustained driver leak measurements. No actual SYS/NDIS/IRQL/registration worker is executed. Current mode authenticates raw dependency snapshots and whole source identity without old out; explicit727-file4c7c typed-Ordinal baseline is fully authenticated and all51 declared dependencies must match raw bytes. Version2.0.0 preserved.';
    audit_sources=@('tests/model/retirement_hint32_tests.c','tools/build/retirement-hint32-tests.ps1','tools/audit/verify-retirement-hint32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
