param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/reuse-terminal32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Reuse ownership version must remain2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted='64be5e2b5802e3057b044c89eb6440d0cbbb432addbf3c4be2ce18a6f730ca02'
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal' -or $paths.Count -ne 735 -or @($manifest.files.PSObject.Properties).Count -ne 735 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected reuse ownership accepted baseline'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$seen.Add($path)){throw 'Duplicate reuse baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Reuse baseline bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($path in $manifest.files.PSObject.Properties.Name){if(!$seen.Contains($path)){throw 'Reuse baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Reuse baseline identity differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate reuse dependencies'}
$dependencies+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/reuse-terminal32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($path in $dependencies){$raw=[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $path));$copy=Join-Path $baseline $path;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,$raw);$baselineHashes[$path]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($raw)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-RtDependencies {
    $actual=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate current reuse closure'}
    $actual+=@('tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h','wintun.props')
    $actual=[Collections.Generic.HashSet[string]]::new([string[]]@($actual|ForEach-Object {$_.Replace('\','/')}),[StringComparer]::Ordinal)
    if(!$actual.SetEquals([string[]]$dependencies)){throw 'Reuse dependency path set changed'}
    foreach($path in $dependencies){$before=Join-Path $baseline $path;$current=Join-Path $script:RepoRoot $path;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $baselineHashes[$path] -or (Get-FileHash $current).Hash -cne (Get-FileHash $before).Hash){throw "Reuse unchanged dependency differs:$path"}}
}
Assert-RtDependencies
function Convert-RtIncludes([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-RtBuild($build){
    $e=$build.extraction;$v2=Join-Path $script:RepoRoot 'api/v2';$test=Join-Path $script:RepoRoot 'tests/model/reuse_terminal32_tests.c';$place=Split-Path $build.path
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms')){throw 'Reuse shipping mode differs'}
    if($build.path -cne (Join-Path $place 'reuse-terminal32.exe') -or $build.fixture -cne (Join-Path $place 'reuse-terminal32.dll')){throw 'Reuse binary path differs'}
    foreach($name in @('session','entry','fixture_entry')){$leaf=switch($name){'session'{'session.c'}'entry'{'session-entry.h'}'fixture_entry'{'fixture.c'}};if($e.$name -cne (Join-Path $place $leaf)){throw 'Reuse generated source path differs'}}
    $source=Join-Path $v2 'session.c'
    if($e.source -cne $source -or $e.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($e.session) -cne (Convert-RtIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Reuse complete session body differs'}
    $lines=@('#define PbQueuePushTokens ReusePush','#define WaitForMultipleObjects ReuseWait',('#include "'+$e.session.Replace('\','/')+'"'),'#undef PbQueuePushTokens','#undef WaitForMultipleObjects')
    if([IO.File]::ReadAllText($e.entry) -cne (($lines -join "`n")+"`n")){throw 'Reuse SDK observer entry differs'}
    $lines=@('#define R_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Reuse whole fixture entry differs'}
    $fixture=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if((Get-FileHash $fixture).Hash.ToLowerInvariant() -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'){throw 'Reuse frozen native-policy ledger changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$place\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Reuse exact compiler flags differ'}
    $expected=@($source,$e.session,$e.entry,$e.fixture_entry,$test,$fixture,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),(Join-Path $script:RepoRoot 'include/packetbridge_version.h'),
        (Join-Path $script:RepoRoot 'common/v2_core.c'),(Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    if(@($build.sources).Count -ne 12){throw 'Reuse source count must be12'}
    $paths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($s in $build.sources){if(!$paths.Add([string]$s.path)){throw 'Reuse duplicate source metadata'}}
    $expectedSet=[Collections.Generic.HashSet[string]]::new([string[]]$expected,[StringComparer]::Ordinal)
    if($expectedSet.Count -ne 12 -or !$paths.SetEquals($expectedSet)){throw 'Reuse source path set differs'}
    foreach($s in $build.sources){if((Get-FileHash -LiteralPath $s.path).Hash.ToLowerInvariant() -cne $s.sha256){throw 'Reuse source raw hash differs'}}
    foreach($key in @('path','fixture')){$hash=if($key-eq'path'){$build.sha256}else{$build.fixture_sha256};if((Get-FileHash -LiteralPath $build.$key).Hash.ToLowerInvariant() -cne $hash){throw 'Reuse binary hash differs'}}
    foreach($key in @('session','entry','fixture_entry')){if((Get-FileHash $e.$key).Hash.ToLowerInvariant() -cne $e.($key+'_sha256')){throw 'Reuse extraction raw hash differs'}}
}
function Test-RtMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;Assert-RtBuild ($json|ConvertFrom-Json);$results=@()
    foreach($kind in @('deleted-source','duplicate-source','compiler-flags','generated-path')){
        $candidate=$json|ConvertFrom-Json
        $expected=switch($kind){
            'deleted-source'{$candidate.sources=@($candidate.sources)[0..10];'Reuse source count must be12'}
            'duplicate-source'{$candidate.sources[11]=$candidate.sources[0];'Reuse duplicate source metadata'}
            'compiler-flags'{$candidate.flags=@($candidate.flags)+'/DPB_API_TESTS=1';'Reuse exact compiler flags differ'}
            'generated-path'{$candidate.extraction.entry=$candidate.extraction.session;'Reuse generated source path differs'}
        }
        $reason=$null;try{Assert-RtBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Reuse metadata accepted/wrong rejection:$kind ($reason)"};$results+=@{name=$kind;rejected=$true;reason=$reason}
    };return $results
}
function Get-RtRows([string]$text,[int]$family){
    if([regex]::Matches($text,'(?m)^FAMILY: ').Count -ne 1 -or ![regex]::IsMatch($text,"(?m)^FAMILY: IPv$family`r?$")){throw 'Reuse family witness differs'}
    $rows=[regex]::Matches($text,'(?m)^CASE ip=([46]) protocol=(6|17) wrap=([01]) early=([01]) measured=([01]) old32 alias_offset=64 reused32 generation=2 returned64 stale_hints=([01]) sdk_waits=([13]) terminal=1 native_barrier=1 owner_join=1 prepared=(\d+) final=1 callbacks=(\d+) owned=0 unload=1\r?$')
    if($rows.Count-ne16 -or [regex]::Matches($text,'(?m)^CASE ').Count-ne16){throw 'Reuse case count/format differs'};$callbacks=0
    for($i=0;$i-lt16;++$i){$shape=$i%8;$g=$rows[$i].Groups;$early=$shape%2;$prepared=[int]$g[8].Value;$joined=[int]$g[9].Value
        if([int]$g[1].Value-ne$family -or [int]$g[2].Value-ne$(if($shape-ge4){17}else{6}) -or [int]$g[3].Value-ne[int][Math]::Floor(($shape%4)/2) -or [int]$g[4].Value-ne$early -or [int]$g[5].Value-ne[int]($i-ge8) -or [int]$g[6].Value-ne(1-$early) -or [int]$g[7].Value-ne$(if($early){1}else{3}) -or $prepared-lt1 -or $prepared-ge8 -or $joined-ne($prepared+1)){throw "Reuse family/shape/ownership row differs:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: reuse-terminal32 cases=(\d+) warm=(\d+) measured=(\d+) owner_joins=(\d+) callback_joins=(\d+) gen2_loans=(\d+) returned_tokens=(\d+) owned=0 true_unload=1\r?$')
    $handles=[regex]::Matches($text,'(?m)^HANDLES before=(\d+) after=(\d+)\r?$')
    if($summary.Count-ne1 -or [regex]::Matches($text,'(?m)^PASS:').Count-ne1 -or $handles.Count-ne1){throw 'Reuse summary missing/extra'};$g=$summary[0].Groups
    if([int]$g[1].Value-ne16 -or [int]$g[2].Value-ne8 -or [int]$g[3].Value-ne8 -or [int]$g[4].Value-ne16 -or [int]$g[5].Value-ne$callbacks -or [int]$g[6].Value-ne512 -or [int]$g[7].Value-ne1024 -or $handles[0].Groups[1].Value-cne$handles[0].Groups[2].Value){throw 'Reuse exact sums/handles differ'}
    return [ordered]@{cases=16;warmup_schedules=8;measured_schedules=8;owner_os_thread_joins=16;callback_os_thread_joins=$callbacks;generation2_loans=512;returned_tokens=1024;stale_hint_no_data_wakes=8;process_handles_before=[int]$handles[0].Groups[1].Value;process_handles_after=[int]$handles[0].Groups[2].Value}
}
Start-TestRecording -Directory $directory -Name 'operation-results.json';$builds=@();$firstLog=$null
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $build=Build-ReuseTerminal32Tests -Directory (Join-Path $directory "ipv$family-$mode-$guarded") -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-RtBuild $build;$builds+=,$build
    $psi=[Diagnostics.ProcessStartInfo]::new($build.path);$psi.ArgumentList.Add($build.fixture);$psi.UseShellExecute=$false;$psi.CreateNoWindow=$true;$psi.RedirectStandardOutput=$true;$psi.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($psi)
    try {
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(150000)){$process.Kill($true);$process.WaitForExit();throw 'Reuse process watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$family,$mode,$guarded)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));if($process.ExitCode){throw "Reuse process exit$($process.ExitCode):$log"}
        $metrics=Get-RtRows $output $family
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$family;mode=$mode;guarded=$guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')});if(!$firstLog){$firstLog=$log}
    } finally { $process.Dispose() }
}}}
$metadata=Test-RtMetadata $builds[0];$original=[IO.File]::ReadAllText($firstLog);$parsers=@()
foreach($kind in @('wrong-family','missing-row','duplicate-row','wrong-callbacks')){
    $candidate=switch($kind){
        'wrong-family'{$original.Replace('CASE ip=4 ','CASE ip=6 ')}
        'missing-row'{[regex]::Replace($original,'(?m)^CASE ip=4 protocol=6 wrap=0 early=0 measured=0 [^\r\n]*\r?\n','')}
        'duplicate-row'{$original+"`n"+[regex]::Match($original,'(?m)^CASE [^\r\n]*').Value+"`n"}
        'wrong-callbacks'{[regex]::Replace($original,'(?m)^(CASE [^\r\n]* prepared=)\d+','${1}0')}
    }
    $expected=if($kind-in@('missing-row','duplicate-row')){'Reuse case count/format differs'}else{'Reuse family/shape/ownership row differs:0'}
    $reason=$null;try{Get-RtRows $candidate 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason-cne$expected){throw "Reuse parser accepted/wrong rejection:$kind ($reason)"};$parsers+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();foreach($kind in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $out=Join-Path $directory "compile-contracts/$kind";New-Item -ItemType Directory -Path $out -Force|Out-Null;$flags=@($builds[0].flags|Where-Object {$_ -notlike '/Fo*'})
    $diagnostic=switch($kind){
        'pb-api-tests'{$flags+='/DPB_API_TESTS=1';'Shipping reuse terminal required'}
        'missing-ndebug'{$flags=@($flags|Where-Object {$_-cne'/DNDEBUG'});'Shipping NDEBUG required'}
        'missing-fingerprint'{$flags=@($flags|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'});'shipping fingerprint required'}
        'invalid-family'{$flags=@($flags|Where-Object {$_-notlike'/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0';'Select one IP family'}
    }
    $lines=@(& cl.exe @flags /c (Join-Path $script:RepoRoot 'tests/model/reuse_terminal32_tests.c') "/Fo$out/contract.obj" 2>&1);$exit=$LASTEXITCODE;$log=Join-Path $out 'compiler.log';$lines|Set-Content -LiteralPath $log -Encoding utf8
    if(!$exit -or ![regex]::IsMatch(($lines-join"`n"),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($diagnostic))){throw "Reuse compiler contract rejected wrong condition:$kind"}
    $contracts+=@{contract=$kind;expected_failure=$true;exit_code=$exit;expected_diagnostic=$diagnostic;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-RtDependencies
if((Get-SourceFingerprint)-cne$script:SourceFingerprint){throw 'Sources changed during reuse ownership audit'}
Write-TestRecording 'passed'
$families=@(foreach($family in @(4,6)){$rows=@($script:TestExecutions|Where-Object {$_.ip_version-eq$family});[ordered]@{ip_version=$family;executed_processes=$rows.Count;cases=($rows|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;owner_os_thread_joins=($rows|ForEach-Object {$_.metrics.owner_os_thread_joins}|Measure-Object -Sum).Sum;callback_os_thread_joins=($rows|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{schema_version=1;version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$contracts;parser_checks=$parsers;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only; no confirmed production defect or before variant. Whole unchanged native-policy resource fixture executes actual shipping Open/queue/wait/lifecycle in an unloadable DLL, fixed hosted NDIS6.20 and valid1500B family TCP/UDP. Unique owner Receives32 generation1 packets, actual empty Wait arms and reaches real SDK entry; public Wake consumes that auto-reset hint without clearing Armed. Caller writes32 input tokens at legal offset64 in its first held payload and recomputes checksum before transfer. Actual ReleaseBatch snapshots alias input, publishes recycle32, then a transparent observer performs genuine peer PopTokens and validates/transitions its independent32-slot pool before actual reserve and republish of all same slots generation2. Peer overwrites only transferred regions; old payload pointers are never read after return. Actual ready1 Wait bypasses SDK, Receive32 borrows new gen2 data, and the earlier native UserEvent remains signaled. Early terminal follows this Receive but precedes next Wait; late terminal follows one real SDK stale-hint success plus actual EMPTY Receive while gen2 loans remain valid, then reaches next real SDK entry. SDK markers do not prove scheduler park. Terminal/sequence and real SDK hints model notification only, not quiescence. Holding32 new loans blocks QUERY; final actual Release32 and public owner join permit QUERY but hosted NativeActive1 independently retains mapping/device/module. Only NativeActive0 and trusted STOPPED allow Prepared with public shell owned. Real WorkCallbacks establishes stable Prepared and dynamic prep count, final Close adds exactly one final callback. Every captured callback OS-thread handle joins; ordinary loader covers every DLL function, then host-only SDK gates and actual detach establish true unload and zero owned resources. Peer pool, IOCTL replies and NativeActive are hosted boundaries, not actual SYS/NDIS/IRQL or Windows7 execution. Guard protects inherited native core/slot/map only; static-peer/caller/SDK storage retains its actual owner. Counters observe API and ownership behavior, not CPU, latency, throughput, copies or sustained device leakage. Default mode snapshots raw current closure independently of historical out; optional735-file64be baseline is authenticated typed-Ordinal before all51 dependency bytes compare exactly. Version2.0.0 preserved.';
    audit_sources=@('tests/model/reuse_terminal32_tests.c','tools/build/reuse-terminal32-tests.ps1','tools/audit/verify-reuse-terminal32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
