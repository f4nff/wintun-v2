param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/retire-retry32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Retry ownership version must remain2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted='a0b750b05183a7075317cc2c1dcd2458770edc937d7c780f2b0814785eb0dc19'
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal' -or $paths.Count -ne 764 -or @($manifest.files.PSObject.Properties).Count -ne 764 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected retry ownership accepted baseline'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$seen.Add($path)){throw 'Duplicate retry baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Retry baseline bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($path in $manifest.files.PSObject.Properties.Name){if(!$seen.Contains($path)){throw 'Retry baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Retry baseline identity differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate retry dependencies'}
$dependencies+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/retire-retry32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($path in $dependencies){$raw=[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $path));$copy=Join-Path $baseline $path;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,$raw);$baselineHashes[$path]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($raw)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-RrDependencies {
    $actual=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate current retry closure'}
    $actual+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
    $actual=[Collections.Generic.HashSet[string]]::new([string[]]@($actual|ForEach-Object {$_.Replace('\','/')}),[StringComparer]::Ordinal)
    if(!$actual.SetEquals([string[]]$dependencies)){throw 'Retry dependency path set changed'}
    foreach($path in $dependencies){$before=Join-Path $baseline $path;$current=Join-Path $script:RepoRoot $path;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $baselineHashes[$path] -or (Get-FileHash $current).Hash -cne (Get-FileHash $before).Hash){throw "Retry unchanged dependency differs:$path"}}
}
Assert-RrDependencies
function Convert-RrIncludes([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-RrBuild($build){
    $e=$build.extraction;$v2=Join-Path $script:RepoRoot 'api/v2';$test=Join-Path $script:RepoRoot 'tests/model/retire_retry32_tests.c';$place=Split-Path $build.path
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms')){throw 'Retry shipping mode differs'}
    if($build.path -cne (Join-Path $place 'retire-retry32.exe') -or $build.fixture -cne (Join-Path $place 'retire-retry32.dll')){throw 'Retry binary path differs'}
    foreach($name in @('session','entry','fixture_entry')){$leaf=switch($name){'session'{'session.c'}'entry'{'session-entry.h'}'fixture_entry'{'fixture.c'}};if($e.$name -cne (Join-Path $place $leaf)){throw 'Retry generated source path differs'}}
    $source=Join-Path $v2 'session.c'
    if($e.source -cne $source -or $e.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($e.session) -cne (Convert-RrIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Retry complete session body differs'}
    $lines=@('#undef AcquireSRWLockExclusive','#define PbValidateTokens RetryValidate','#define AcquireSRWLockExclusive RetryExclusive','#define SetEvent RetrySignal',('#include "'+$e.session.Replace('\','/')+'"'),'#undef PbValidateTokens','#undef AcquireSRWLockExclusive','#undef SetEvent')
    if([IO.File]::ReadAllText($e.entry) -cne (($lines -join "`n")+"`n")){throw 'Retry SDK observer entry differs'}
    $lines=@('#define RR_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Retry whole fixture entry differs'}
    $fixture=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if((Get-FileHash $fixture).Hash.ToLowerInvariant() -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'){throw 'Retry frozen native-policy ledger changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$place\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Retry exact compiler flags differ'}
    $expected=@($source,$e.session,$e.entry,$e.fixture_entry,$test,$fixture,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),(Join-Path $script:RepoRoot 'include/packetbridge_version.h'),
        (Join-Path $script:RepoRoot 'common/v2_core.c'),(Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    if(@($build.sources).Count -ne 12){throw 'Retry source count must be12'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($s in $build.sources){if(!$paths.Add([string]$s.path)){throw 'Retry duplicate source metadata'}}
    $expectedSet=[Collections.Generic.HashSet[string]]::new([string[]]$expected,[StringComparer]::Ordinal)
    if($expectedSet.Count -ne 12 -or !$paths.SetEquals($expectedSet)){throw 'Retry source path set differs'}
    foreach($s in $build.sources){if((Get-FileHash -LiteralPath $s.path).Hash.ToLowerInvariant() -cne $s.sha256){throw 'Retry source raw hash differs'}}
    foreach($key in @('path','fixture')){$hash=if($key-eq'path'){$build.sha256}else{$build.fixture_sha256};if((Get-FileHash -LiteralPath $build.$key).Hash.ToLowerInvariant() -cne $hash){throw 'Retry binary hash differs'}}
    foreach($key in @('session','entry','fixture_entry')){if((Get-FileHash $e.$key).Hash.ToLowerInvariant() -cne $e.($key+'_sha256')){throw 'Retry extraction raw hash differs'}}
}
function Test-RrMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;Assert-RrBuild ($json|ConvertFrom-Json);$results=@()
    foreach($kind in @('deleted-source','duplicate-source','compiler-flags','generated-path')){
        $candidate=$json|ConvertFrom-Json
        $expected=switch($kind){
            'deleted-source'{$candidate.sources=@($candidate.sources)[0..10];'Retry source count must be12'}
            'duplicate-source'{$candidate.sources[11]=$candidate.sources[0];'Retry duplicate source metadata'}
            'compiler-flags'{$candidate.flags=@($candidate.flags)+'/DPB_API_TESTS=1';'Retry exact compiler flags differ'}
            'generated-path'{$candidate.extraction.entry=$candidate.extraction.session;'Retry generated source path differs'}
        }
        $reason=$null;try{Assert-RrBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Retry metadata accepted/wrong rejection:$kind ($reason)"};$results+=@{name=$kind;rejected=$true;reason=$reason}
    };return $results
}
function Get-RrRows([string]$text,[int]$family){
    if([regex]::Matches($text,'(?m)^FAMILY: ').Count -ne 1 -or ![regex]::IsMatch($text,"(?m)^FAMILY: IPv$family`r?$")){throw 'Retry family witness differs'}
    $rows=[regex]::Matches($text,'(?m)^CASE ip=([46]) protocol=(6|17) cancel=([01]) duplicate=([01]) measured=([01]) loans32 invalid_last=1 rollback32 accepted=(0|none) progress0 stopped_retry32 progress1 data_blocked=1 native_barrier=1 owner_join=1 prepared=(\d+) final=1 callbacks=(\d+) owned=0 unload=1\r?$')
    if($rows.Count-ne16 -or [regex]::Matches($text,'(?m)^CASE ').Count-ne16){throw 'Retry case count/format differs'};$callbacks=0
    for($i=0;$i-lt16;++$i){$shape=$i%8;$g=$rows[$i].Groups;$cancel=[int][Math]::Floor(($shape%4)/2);$prepared=[int]$g[7].Value;$joined=[int]$g[8].Value
        if([int]$g[1].Value-ne$family -or [int]$g[2].Value-ne$(if($shape-ge4){17}else{6}) -or [int]$g[3].Value-ne$cancel -or [int]$g[4].Value-ne($shape%2) -or [int]$g[5].Value-ne[int]($i-ge8) -or $g[6].Value-cne$(if($cancel){'none'}else{'0'}) -or $prepared-lt1 -or $prepared-ge8 -or $joined-ne($prepared+1)){throw "Retry family/shape/ownership row differs:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: retire-retry32 cases=(\d+) warm=(\d+) measured=(\d+) owner_joins=(\d+) callback_joins=(\d+) rejected_batches=(\d+) tokens_retried=(\d+) owned=0 true_unload=1\r?$')
    $handles=[regex]::Matches($text,'(?m)^HANDLES before=(\d+) after=(\d+)\r?$')
    if($summary.Count-ne1 -or [regex]::Matches($text,'(?m)^PASS:').Count-ne1 -or $handles.Count-ne1){throw 'Retry summary missing/extra'};$g=$summary[0].Groups
    if([int]$g[1].Value-ne16 -or [int]$g[2].Value-ne8 -or [int]$g[3].Value-ne8 -or [int]$g[4].Value-ne16 -or [int]$g[5].Value-ne$callbacks -or [int]$g[6].Value-ne16 -or [int]$g[7].Value-ne512 -or $handles[0].Groups[1].Value-cne$handles[0].Groups[2].Value){throw 'Retry exact sums/handles differ'}
    return [ordered]@{cases=16;warmup_schedules=8;measured_schedules=8;owner_os_thread_joins=16;callback_os_thread_joins=$callbacks;rejected_batches=16;tokens_retried=512;process_handles_before=[int]$handles[0].Groups[1].Value;process_handles_after=[int]$handles[0].Groups[2].Value}
}
Start-TestRecording -Directory $directory -Name 'operation-results.json';$builds=@();$firstLog=$null
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $build=Build-RetireRetry32Tests -Directory (Join-Path $directory "ipv$family-$mode-$guarded") -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-RrBuild $build;$builds+=,$build
    $psi=[Diagnostics.ProcessStartInfo]::new($build.path);$psi.ArgumentList.Add($build.fixture);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(150000)){$process.Kill($true);$process.WaitForExit();throw 'Retry process watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$family,$mode,$guarded)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));if($process.ExitCode){throw "Retry process exit$($process.ExitCode):$log"}
        $metrics=Get-RrRows $output $family
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$family;mode=$mode;guarded=$guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')});if(!$firstLog){$firstLog=$log}
    } finally { $process.Dispose() }
}}}
$metadata=Test-RrMetadata $builds[0];$original=[IO.File]::ReadAllText($firstLog);$parsers=@()
foreach($kind in @('wrong-family','missing-row','duplicate-row','wrong-callbacks')){
    $candidate=switch($kind){
        'wrong-family'{$original.Replace('CASE ip=4 ','CASE ip=6 ')}
        'missing-row'{[regex]::Replace($original,'(?m)^CASE ip=4 protocol=6 cancel=0 duplicate=0 measured=0 [^\r\n]*\r?\n','')}
        'duplicate-row'{$original+"`n"+[regex]::Match($original,'(?m)^CASE [^\r\n]*').Value+"`n"}
        'wrong-callbacks'{[regex]::Replace($original,'(?m)^(CASE [^\r\n]* prepared=)\d+','${1}0')}
    }
    $expected=if($kind-in@('missing-row','duplicate-row')){'Retry case count/format differs'}else{'Retry family/shape/ownership row differs:0'}
    $reason=$null;try{Get-RrRows $candidate 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason-cne$expected){throw "Retry parser accepted/wrong rejection:$kind ($reason)"};$parsers+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();foreach($kind in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $out=Join-Path $directory "compile-contracts/$kind";New-Item -ItemType Directory -Path $out -Force|Out-Null;$flags=@($builds[0].flags|Where-Object {$_ -notlike '/Fo*'})
    $diagnostic=switch($kind){
        'pb-api-tests'{$flags+='/DPB_API_TESTS=1';'Shipping retire retry required'}
        'missing-ndebug'{$flags=@($flags|Where-Object {$_-cne'/DNDEBUG'});'Shipping NDEBUG required'}
        'missing-fingerprint'{$flags=@($flags|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'});'shipping fingerprint required'}
        'invalid-family'{$flags=@($flags|Where-Object {$_-notlike'/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0';'Select one IP family'}
    }
    $lines=@(& cl.exe @flags /c (Join-Path $script:RepoRoot 'tests/model/retire_retry32_tests.c') "/Fo$out/contract.obj" 2>&1);$exit=$LASTEXITCODE;$log=Join-Path $out 'compiler.log';$lines|Set-Content -LiteralPath $log -Encoding utf8
    if(!$exit -or ![regex]::IsMatch(($lines-join"`n"),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($diagnostic))){throw "Retry compiler contract rejected wrong condition:$kind"}
    $contracts+=@{contract=$kind;expected_failure=$true;exit_code=$exit;expected_diagnostic=$diagnostic;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-RrDependencies
if((Get-SourceFingerprint)-cne$script:SourceFingerprint){throw 'Sources changed during retry ownership audit'}
Write-TestRecording 'passed'
$families=@(foreach($family in @(4,6)){$rows=@($script:TestExecutions|Where-Object {$_.ip_version-eq$family});[ordered]@{ip_version=$family;executed_processes=$rows.Count;cases=($rows|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;owner_os_thread_joins=($rows|ForEach-Object {$_.metrics.owner_os_thread_joins}|Measure-Object -Sum).Sum;callback_os_thread_joins=($rows|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{schema_version=1;version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$contracts;parser_checks=$parsers;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only; no confirmed production defect or before variant. Whole unchanged native-policy6508 ledger executes actual shipping DLL Open/queue/Release32/Cancel32/lifecycle. Fixed hostedNDIS6.20,32slots/ring64, valid1500B familyTCP/UDP payloads. Unique owner actually Receives32 TX loans or Reserves32 RX loans, preserves original tokens and constructs only a private last-token badEpoch or duplicate. Three lexical observers forward actual ValidateTokens, inherited ObservedAcquire/SRW lock and SDK SetEvent. Only after the real validation returns STALE or INVALID does the hard gate allow BeginStop; actual lifecycle first EX Try fails and forwards real acquisition while the original caller still holds DataGate SH. Inspector adds no SH reader behind that pending writer, relying on both existing hard gates to freeze possible writers. No pool/slot/free-index or any of four ring views/shared records/64descriptor entries changes. After invalid call returns, errors remain STALE/INVALID, Release accepted output0 while Cancel has no such output, loan32 and payloads remain intact, Fatal0 and owner Progress0; lifecycle blocks at LEASES with Query0. Only after that returned call does the original owner restore its own last input token and retry the actual stopped Release32/Cancel32. Slots/freebits restore and exactly one owner Progress signal follows success, with no private state or native event repair. Owner joins before finalClose. Hosted NativeActive1 still independently blocks reclamation after user loans0; trusted STOPPED/Active0 is required for Prepared. Real WorkCallbacks establish dynamic prep count, one final Close adds its final callback; ordinary loader covers every DLL call, then host SDK gates and every captured OS-thread join prove actual detach/unload and original owned-ledger0. Static test snapshots and raw incoming producer are hosted boundaries, not actual SYS/NDIS/IRQL/Windows7 execution or permission to edit a transferred packet. Guard covers inherited native core/slot/map allocations only. No fake FULL, manual Retain, private counter repair, SubmitWork failure or finalClose with live public users. Operations and byte snapshots are not CPU/throughput/latency or sustained kernel-leak evidence. Default current raw snapshot is standalone; optional764-filea0b750 typedOrdinal baseline authenticates every raw file, then all51 production dependencies remain unchanged. Version2.0.0 preserved.';
    audit_sources=@('tests/model/retire_retry32_tests.c','tools/build/retire-retry32-tests.ps1','tools/audit/verify-retire-retry32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
