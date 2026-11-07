param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/status-ownership32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Status ownership version must remain2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted='46866e17acd228547b41b890a6a0708e2568d527cbe8bcea618349cf6780b6d6'
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal' -or $paths.Count -ne 711 -or @($manifest.files.PSObject.Properties).Count -ne 711 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected status ownership accepted baseline'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$seen.Add($path)){throw 'Duplicate status baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Status baseline bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($path in $manifest.files.PSObject.Properties.Name){if(!$seen.Contains($path)){throw 'Status baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Status baseline identity differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate status dependencies'}
$dependencies+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/status-ownership32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($path in $dependencies){$raw=[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $path));$copy=Join-Path $baseline $path;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,$raw);$baselineHashes[$path]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($raw)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-SoDependencies {
    $actual=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate current status closure'}
    $actual+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
    $actual=[Collections.Generic.HashSet[string]]::new([string[]]@($actual|ForEach-Object {$_.Replace('\','/')}),[StringComparer]::Ordinal)
    if(!$actual.SetEquals([string[]]$dependencies)){throw 'Status dependency path set changed'}
    foreach($path in $dependencies){$before=Join-Path $baseline $path;$current=Join-Path $script:RepoRoot $path;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $baselineHashes[$path] -or (Get-FileHash $current).Hash -cne (Get-FileHash $before).Hash){throw "Status unchanged dependency differs:$path"}}
}
Assert-SoDependencies
function Convert-SoIncludes([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-SoBuild($build){
    $e=$build.extraction;$v2=Join-Path $script:RepoRoot 'api/v2';$test=Join-Path $script:RepoRoot 'tests/model/status_ownership32_tests.c';$place=Split-Path $build.path
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms')){throw 'Status shipping mode differs'}
    if($build.path -cne (Join-Path $place 'status-ownership32.exe') -or $build.fixture -cne (Join-Path $place 'status-ownership32.dll')){throw 'Status binary path differs'}
    foreach($name in @('session','entry','fixture_entry')){$leaf=switch($name){'session'{'session.c'}'entry'{'session-entry.h'}'fixture_entry'{'fixture.c'}};if($e.$name -cne (Join-Path $place $leaf)){throw 'Status generated source path differs'}}
    $source=Join-Path $v2 'session.c'
    if($e.source -cne $source -or $e.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($e.session) -cne (Convert-SoIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Status complete session body differs'}
    $lines=@('#undef DeviceIoControl','#define DeviceIoControl ReplyIo',('#include "'+$e.session.Replace('\','/')+'"'))
    if([IO.File]::ReadAllText($e.entry) -cne (($lines -join "`n")+"`n")){throw 'Status SDK observer entry differs'}
    $lines=@('#define O_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Status whole fixture entry differs'}
    $fixture=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if((Get-FileHash $fixture).Hash.ToLowerInvariant() -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'){throw 'Status frozen native-policy ledger changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$place\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Status exact compiler flags differ'}
    $expected=@($source,$e.session,$e.entry,$e.fixture_entry,$test,$fixture,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),(Join-Path $script:RepoRoot 'include/packetbridge_version.h'),
        (Join-Path $script:RepoRoot 'common/v2_core.c'),(Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    if(@($build.sources).Count -ne 12){throw 'Status source count must be12'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($s in $build.sources){if(!$paths.Add([string]$s.path)){throw 'Status duplicate source metadata'}}
    $expectedSet=[Collections.Generic.HashSet[string]]::new([string[]]$expected,[StringComparer]::Ordinal)
    if($expectedSet.Count -ne 12 -or !$paths.SetEquals($expectedSet)){throw 'Status source path set differs'}
    foreach($s in $build.sources){if((Get-FileHash -LiteralPath $s.path).Hash.ToLowerInvariant() -cne $s.sha256){throw 'Status source raw hash differs'}}
    foreach($key in @('path','fixture')){$hash=if($key-eq'path'){$build.sha256}else{$build.fixture_sha256};if((Get-FileHash -LiteralPath $build.$key).Hash.ToLowerInvariant() -cne $hash){throw 'Status binary hash differs'}}
    foreach($key in @('session','entry','fixture_entry')){if((Get-FileHash $e.$key).Hash.ToLowerInvariant() -cne $e.($key+'_sha256')){throw 'Status extraction raw hash differs'}}
}
function Test-SoMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;Assert-SoBuild ($json|ConvertFrom-Json);$results=@()
    foreach($kind in @('deleted-source','duplicate-source','compiler-flags','generated-path')){
        $candidate=$json|ConvertFrom-Json
        $expected=switch($kind){
            'deleted-source'{$candidate.sources=@($candidate.sources)[0..10];'Status source count must be12'}
            'duplicate-source'{$candidate.sources[11]=$candidate.sources[0];'Status duplicate source metadata'}
            'compiler-flags'{$candidate.flags=@($candidate.flags)+'/DPB_API_TESTS=1';'Status exact compiler flags differ'}
            'generated-path'{$candidate.extraction.entry=$candidate.extraction.session;'Status generated source path differs'}
        }
        $reason=$null;try{Assert-SoBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Status metadata accepted/wrong rejection:$kind ($reason)"};$results+=@{name=$kind;rejected=$true;reason=$reason}
    };return $results
}
function Get-SoRows([string]$text,[int]$family){
    if([regex]::Matches($text,'(?m)^FAMILY: ').Count -ne 1 -or ![regex]::IsMatch($text,"(?m)^FAMILY: IPv$family`r?$")){throw 'Status family witness differs'}
    $rows=[regex]::Matches($text,'(?m)^CASE ip=([46]) protocol=(6|17) query=([1-5]) ndis=(20|89) measured=([01]) loans=32 published=32 caps_rejected=10 query_rejected=1 released=32 reconciled=32 prepared=(\d+) final=1 callbacks=(\d+) owner_join=1 owned=0 unload=1\r?$')
    if($rows.Count-ne40 -or [regex]::Matches($text,'(?m)^CASE ').Count-ne40){throw 'Status case count/format differs'};$callbacks=0
    for($i=0;$i-lt40;++$i){$shape=$i%20;$g=$rows[$i].Groups;$prepared=[int]$g[6].Value;$joined=[int]$g[7].Value
        if([int]$g[1].Value-ne$family -or [int]$g[2].Value-ne$(if($shape-ge10){17}else{6}) -or [int]$g[3].Value-ne(1+[int][Math]::Floor(($shape%10)/2)) -or [int]$g[4].Value-ne$(if($shape%2){89}else{20}) -or [int]$g[5].Value-ne[int]($i-ge20) -or $prepared-lt1 -or $prepared-ge8 -or $joined-ne($prepared+1)){throw "Status family/shape/ownership row differs:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: status-ownership32 cases=(\d+) warm=(\d+) measured=(\d+) owner_joins=(\d+) callback_joins=(\d+) caps_rejected=(\d+) query_rejected=(\d+) defensive_replies=1 owned=0 true_unload=1\r?$')
    $handles=[regex]::Matches($text,'(?m)^HANDLES before=(\d+) after=(\d+)\r?$')
    if($summary.Count-ne1 -or [regex]::Matches($text,'(?m)^PASS:').Count-ne1 -or $handles.Count-ne1){throw 'Status summary missing/extra'};$g=$summary[0].Groups
    if([int]$g[1].Value-ne40 -or [int]$g[2].Value-ne20 -or [int]$g[3].Value-ne20 -or [int]$g[4].Value-ne40 -or [int]$g[5].Value-ne$callbacks -or [int]$g[6].Value-ne400 -or [int]$g[7].Value-ne40 -or $handles[0].Groups[1].Value-cne$handles[0].Groups[2].Value){throw 'Status exact sums/handles differ'}
    return [ordered]@{cases=40;warmup_schedules=20;measured_schedules=20;owner_os_thread_joins=40;callback_os_thread_joins=$callbacks;caps_rejected=400;query_rejected=40;published_reconciled=1280;user_loans_returned=1280;process_handles_before=[int]$handles[0].Groups[1].Value;process_handles_after=[int]$handles[0].Groups[2].Value}
}
Start-TestRecording -Directory $directory -Name 'operation-results.json';$builds=@();$firstLog=$null
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $build=Build-StatusOwnership32Tests -Directory (Join-Path $directory "ipv$family-$mode-$guarded") -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-SoBuild $build;$builds+=,$build
    $psi=[Diagnostics.ProcessStartInfo]::new($build.path);$psi.ArgumentList.Add($build.fixture);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(150000)){$process.Kill($true);$process.WaitForExit();throw 'Status process watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$family,$mode,$guarded)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));if($process.ExitCode){throw "Status process exit$($process.ExitCode):$log"}
        $metrics=Get-SoRows $output $family
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$family;mode=$mode;guarded=$guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')});if(!$firstLog){$firstLog=$log}
    } finally { $process.Dispose() }
}}}
$metadata=Test-SoMetadata $builds[0];$original=[IO.File]::ReadAllText($firstLog);$parsers=@()
foreach($kind in @('wrong-family','missing-row','duplicate-row','wrong-callbacks')){
    $candidate=switch($kind){
        'wrong-family'{$original.Replace('CASE ip=4 ','CASE ip=6 ')}
        'missing-row'{[regex]::Replace($original,'(?m)^CASE ip=4 protocol=6 query=1 ndis=20 measured=0 [^\r\n]*\r?\n','')}
        'duplicate-row'{$original+"`n"+[regex]::Match($original,'(?m)^CASE [^\r\n]*').Value+"`n"}
        'wrong-callbacks'{[regex]::Replace($original,'(?m)^(CASE [^\r\n]* prepared=)\d+','${1}0')}
    }
    $expected=if($kind-in@('missing-row','duplicate-row')){'Status case count/format differs'}else{'Status family/shape/ownership row differs:0'}
    $reason=$null;try{Get-SoRows $candidate 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason-cne$expected){throw "Status parser accepted/wrong rejection:$kind ($reason)"};$parsers+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();foreach($kind in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $out=Join-Path $directory "compile-contracts/$kind";New-Item -ItemType Directory -Path $out -Force|Out-Null;$flags=@($builds[0].flags|Where-Object {$_ -notlike '/Fo*'})
    $diagnostic=switch($kind){
        'pb-api-tests'{$flags+='/DPB_API_TESTS=1';'Shipping status ownership required'}
        'missing-ndebug'{$flags=@($flags|Where-Object {$_-cne'/DNDEBUG'});'Shipping NDEBUG required'}
        'missing-fingerprint'{$flags=@($flags|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'});'shipping fingerprint required'}
        'invalid-family'{$flags=@($flags|Where-Object {$_-notlike'/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0';'Select one IP family'}
    }
    $lines=@(& cl.exe @flags /c (Join-Path $script:RepoRoot 'tests/model/status_ownership32_tests.c') "/Fo$out/contract.obj" 2>&1);$exit=$LASTEXITCODE;$log=Join-Path $out 'compiler.log';$lines|Set-Content -LiteralPath $log -Encoding utf8
    if(!$exit -or ![regex]::IsMatch(($lines-join"`n"),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($diagnostic))){throw "Status compiler contract rejected wrong condition:$kind"}
    $contracts+=@{contract=$kind;expected_failure=$true;exit_code=$exit;expected_diagnostic=$diagnostic;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-SoDependencies
if((Get-SourceFingerprint)-cne$script:SourceFingerprint){throw 'Sources changed during status ownership audit'}
Write-TestRecording 'passed'
$families=@(foreach($family in @(4,6)){$rows=@($script:TestExecutions|Where-Object {$_.ip_version-eq$family});[ordered]@{ip_version=$family;executed_processes=$rows.Count;cases=($rows|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;owner_os_thread_joins=($rows|ForEach-Object {$_.metrics.owner_os_thread_joins}|Measure-Object -Sum).Sum;callback_os_thread_joins=($rows|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{schema_version=1;version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$contracts;parser_checks=$parsers;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only; no confirmed production defect, repair or before variant. Whole unchanged native-policy fixture ledger executes actual shipping Open/queue/status/lifecycle in an unloadable DLL. Each unique queue-owner thread Reserve/Commit32 valid1500B packets (Published32), Receive32 independent hosted TX_READY packets (Loans32), and remains outside API on a host gate while public GetCapabilities rejects ten defensive reply variants: reported short/long count, Size/Magic/Schema/FileVersion/DataAbi, first fingerprint byte or terminator, native IO failure. A valid reply is recovered after each; no failure-output immutability is promised. Exact pools, ownership, payloads, CloseRequested and Fatal stay unchanged. Actual BeginStop first parks behind32 loans without lifecycle QUERY. Same original owner validates original packet data, actually ReleaseBatch32 and joins; valid QUERY with hosted NativeActive1 independently prevents preparation. After NativeActive0, five malformed QUERY shapes (short/long/Size/Epoch or IO failure) reject even an underlying STOPPED/Active0 response: Published32, mapping/device/module/registry owners remain. Only restoring exact trusted QUERY acknowledgement authorizes production reconciliation of32 abandoned Published slots. First real WaitForThreadpoolWorkCallbacks establishes recovery/Prepared before a new PrepareClose request; another real callback wait yields stable dynamic prep count, and exactly one final Close adds its final callback. All public callers join before successful final Close consumes the public reference; ordinary LoadLibrary lasts through DLL calls, then only host SDK events and every captured callback OS-thread handle are used through actual DLL detach/unload. Native reference count, capabilities and IOCTL state are hosted values, not actual SYS/NDIS/IRQL or Windows7 runtime execution. NDIS6.20/6.89 capability models preserve low/modern masks. Guard protects inherited native core/slot/map allocations only; caller config/stack and SDK allocations retain real allocators. SDK observation gates are not scheduler park evidence. Operation/ownership counts do not measure payload copy, native CPU, latency, throughput or sustained driver leaks. Default mode authenticates current raw dependency snapshots and whole source fingerprint without historical out; explicit711-file4686 baseline is fully typed-Ordinal/hash authenticated before exact raw dependency comparisons. Version2.0.0 preserved.';
    audit_sources=@('tests/model/status_ownership32_tests.c','tools/build/status-ownership32-tests.ps1','tools/audit/verify-status-ownership32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
