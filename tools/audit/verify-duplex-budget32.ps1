param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/duplex-budget32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Duplex audit requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted=if($historical){'9631f540027d403d704c7288d06d8472eb9631c3f5ac5299e9f9845fb13e934b'}else{$script:SourceFingerprint}
if($historical){
    # Explicit historical mode fails closed before creating any audit output.
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    $set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal' -or $paths.Count -ne 703 -or @($manifest.files.PSObject.Properties).Count -ne 703 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected duplex accepted baseline'}
    foreach($path in $paths){if(!$set.Add($path)){throw 'Duplicate duplex baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Baseline raw bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($name in $manifest.files.PSObject.Properties.Name){if(!$set.Contains($name)){throw 'Duplex baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Duplex baseline whole fingerprint differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate duplex source dependencies'}
$dependencies+=@('api/session.c','api/compat_worker.h','api/compat_pump.h','api/termination_wait.h','api/adapter.h','api/logger.h','api/main.h','api/wintun.h','api/operation.h','wintun.props',
    'tests/safety/facade_stop_fixture.c','tests/safety/native_policy_fixture.c','tests/model/facade_stop_fixture.h','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/duplex-budget32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
if(!$historical){
    # The default needs no old out artifact. Authenticate complete current raw
    # dependencies before compilation, retain their byte snapshots, and bind
    # the unchanged declared closure and complete tree fingerprint at the end.
    $baseline=Join-Path $directory 'current-source'
    foreach($relative in $dependencies){$current=Join-Path $script:RepoRoot $relative;$snapshot=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $snapshot) -Force|Out-Null
        $bytes=[IO.File]::ReadAllBytes($current);[IO.File]::WriteAllBytes($snapshot,$bytes);$baselineHashes[$relative]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    }
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-BdDependencies {
    foreach($relative in $dependencies){$before=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $baselineHashes[$relative] -or (Get-FileHash $current).Hash -cne (Get-FileHash $before).Hash){throw "Duplex unchanged dependency differs:$relative"}}
}
Assert-BdDependencies
function Convert-BdExpected([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-BdBuild($build){
    $e=$build.extraction;$api=Join-Path $script:RepoRoot 'api';$v2=Join-Path $api 'v2';$test=Join-Path $script:RepoRoot 'tests/model/duplex_budget32_tests.c'
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or !$build.ndebug -or $build.pb_api_tests){throw 'Duplex shipping mode differs'}
    $place=Split-Path $build.path
    $outputs=@{path='duplex-budget32.exe';fixture='duplex-budget32.dll'}
    foreach($name in $outputs.Keys){if($build.$name -cne (Join-Path $place $outputs[$name])){throw 'Budget binary path differs'}}
    $generated=@{legacy='legacy-session.c';native='native-session.c';base_original='facade-original.c';base='facade-host.h';entry='session-entry.h';fixture_entry='fixture.c'}
    foreach($name in $generated.Keys){if($e.$name -cne (Join-Path $place $generated[$name])){throw 'Budget generated source path differs'}}
    $expected="#define memcpy BudgetCopy`n"+(Convert-BdExpected ([IO.File]::ReadAllText((Join-Path $baseline 'api/session.c'))) $api)+"`n#undef memcpy`n"
    if($e.legacy_source -cne (Join-Path $api 'session.c') -or [IO.File]::ReadAllText($e.legacy) -cne $expected){throw 'Duplex legacy body exceeded include/copy observer binding'}
    if($e.native_source -cne (Join-Path $v2 'session.c') -or [IO.File]::ReadAllText($e.native) -cne (Convert-BdExpected ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Duplex complete native body differs'}
    $baseSource=Join-Path $script:RepoRoot 'tests/safety/facade_stop_fixture.c';$nativeHost=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if($e.base_source -cne $baseSource -or $e.native_host -cne $nativeHost -or $e.base_source_sha256 -cne '42ed2b2b099bca8b6d5be8c8d6fb843ee529ad8be5549b0643899d813bc9fcc3' -or $e.native_host_sha256 -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83' -or (Get-FileHash $e.base_original).Hash -cne (Get-FileHash $baseSource).Hash -or [IO.File]::ReadAllText($e.base) -cne (Convert-BdExpected ([IO.File]::ReadAllText((Join-Path $baseline 'tests/safety/facade_stop_fixture.c'))) (Split-Path $baseSource))){throw 'Frozen duplex ownership host exceeded include projection'}
    $entry=@'
#undef DeviceIoControl
#define DeviceIoControl FacadeIo
#define WaitForMultipleObjects BudgetWait
#include PB_FACADE_NATIVE_SOURCE
#undef WaitForMultipleObjects
'@
    if([IO.File]::ReadAllText($e.entry) -cne $entry){throw 'Duplex native SDK entry binding differs'}
    $lines=@('#define B_FIXTURE 1');if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_FACADE_NATIVE_SOURCE "'+$e.native.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'
    $lines+='#define PB_FACADE_LEGACY_SOURCE "'+$e.legacy.Replace('\','/')+'"';$lines+='#define PB_BUDGET_BASE "'+$e.base.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    if($e.test -cne $test -or [IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Duplex final source entry differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$($build.mode)",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/I$api")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Exact duplex shipping compiler flags differ'}
    if((Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256 -or (Get-FileHash $build.fixture).Hash.ToLowerInvariant() -cne $build.fixture_sha256){throw 'Duplex binary binding changed'}
    $expectedSources=@($e.legacy_source,$e.legacy,$e.native_source,$e.native,$baseSource,$e.base_original,$e.base,$nativeHost,$e.entry,$e.fixture_entry,$test,
        (Join-Path $api 'compat_worker.h'),(Join-Path $api 'compat_pump.h'),(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),(Join-Path $script:RepoRoot 'tests/model/facade_stop_fixture.h'))
    if(@($build.sources).Count -ne 15){throw 'Budget source metadata count must be15'}
    $sourcePaths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($source in $build.sources){if(!$sourcePaths.Add([string]$source.path)){throw 'Budget source metadata duplicate path'}}
    $expectedPaths=[Collections.Generic.HashSet[string]]::new([string[]]$expectedSources,[StringComparer]::Ordinal)
    if($expectedPaths.Count -ne 15 -or !$sourcePaths.SetEquals($expectedPaths)){throw 'Budget source metadata path set differs'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -cne $source.sha256){throw 'Duplex source/artifact binding changed'}}
}
function Test-BdMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;Assert-BdBuild ($json|ConvertFrom-Json);$contracts=@()
    foreach($kind in @('deleted-source','duplicate-source','compile-flags','generated-path')){
        $candidate=$json|ConvertFrom-Json
        $expected=switch($kind){
            'deleted-source'{$candidate.sources=@($candidate.sources)[0..13];'Budget source metadata count must be15'}
            'duplicate-source'{$candidate.sources[14]=$candidate.sources[0];'Budget source metadata duplicate path'}
            'compile-flags'{$candidate.flags=@($candidate.flags)+'/DPB_API_TESTS=1';'Exact duplex shipping compiler flags differ'}
            'generated-path'{$candidate.extraction.fixture_entry=$candidate.extraction.base;'Budget generated source path differs'}
        }
        $reason=$null;try{Assert-BdBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Budget metadata accepted/wrong rejection:$kind ($reason)"}
        $contracts+=@{name=$kind;rejected=$true;reason=$reason}
    }
    return $contracts
}
function Get-BdRows([string]$text,[int]$family){
    $rows=[regex]::Matches($text,'(?m)^CASE family=([46]) protocol=(tcp|udp) wrap=([01]) stopTurn=(0|1|3) measured=([01]) turns=([134]) in=(8|24|32) out=(8|24|32) reserveCommit=8x([134]) receive=32 release=(8/24|8/8/8/8) stoppedReserve=([01]) uncopiedRetired=(0|8|24) copyBytes=(\d+) endJoins=1 callbackJoins=(\d+)\r?$')
    if($rows.Count -ne 24 -or [regex]::Matches($text,'(?m)^CASE ').Count -ne 24){throw 'Budget case count/format differs'};$callbacks=0;$copiedBytes=0L;$retired=0
    for($i=0;$i -lt 24;++$i){$shape=$i%12;$protocol=if($shape-ge6){'udp'}else{'tcp'};$wrap=[int][Math]::Floor($shape/3)%2;$stop=@(0,1,3)[$shape%3];$turns=if($stop){$stop}else{4};$packets=8*$turns;$g=$rows[$i].Groups;$joins=[int]$g[14].Value
        if([int]$g[1].Value-ne$family -or $g[2].Value-cne$protocol -or [int]$g[3].Value-ne$wrap -or [int]$g[4].Value-ne$stop -or [int]$g[5].Value-ne[int]($i-ge12) -or [int]$g[6].Value-ne$turns -or [int]$g[7].Value-ne$packets -or [int]$g[8].Value-ne$packets -or [int]$g[9].Value-ne$turns -or $g[10].Value-cne$(if($stop-eq1){'8/24'}else{'8/8/8/8'}) -or [int]$g[11].Value-ne[int]($stop-ne0) -or [int]$g[12].Value-ne(32-$packets) -or [int64]$g[13].Value-ne(2*$packets*65535) -or $joins-lt1 -or $joins-gt8){throw "Budget family/shape/copy/ownership row differs:$i"}
        $callbacks+=$joins;$copiedBytes+=[int64]$g[13].Value;$retired+=[int]$g[12].Value
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: BUDGET32 family([46]) cases(\d+) warm12 measured12 endJoins(\d+) callbackJoins(\d+) handles(\d+)/(\d+) owners0 repairs0\r?$')
    if($summary.Count-ne1 -or [regex]::Matches($text,'(?m)^PASS:').Count-ne1){throw 'Budget summary missing/extra'};$g=$summary[0].Groups
    if([int]$g[1].Value-ne$family -or [int]$g[2].Value-ne24 -or [int]$g[3].Value-ne24 -or [int]$g[4].Value-ne$callbacks -or $g[5].Value-cne$g[6].Value){throw 'Budget exact callback sums/handles differ'}
    return @{cases=24;checked_warmup_schedules=12;measured_schedules=12;end_caller_joins=24;callback_os_thread_joins=$callbacks;explicit_payload_copy_bytes=$copiedBytes;uncopied_native_loans_retired=$retired;process_handles_before=[int]$g[5].Value;process_handles_after=[int]$g[6].Value}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-BdProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.ArgumentList.Add($build.fixture)
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start duplex process'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'Duplex process watchdog expired60s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ip{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        if($process.ExitCode){throw "Duplex process exit$($process.ExitCode):$log"};$metrics=Get-BdRows $output $build.ip_version
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})});Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})
    }finally{$process.Dispose()}
}
$builds=@();foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){$memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-DuplexBudget32Tests -Directory (Join-Path $directory "ipv$family/$mode-$memory") -IpVersion $family -Mode $mode -Guarded:$guarded;Assert-BdBuild $build;$builds+=,$build;Invoke-BdProcess $build}}}
$metadataChecks=Test-BdMetadata $builds[0]
$parserChecks=@();$original=[IO.File]::ReadAllText($script:TestExecutions[0].log);Get-BdRows $original 4|Out-Null
foreach($kind in @('wrong-family','missing-row','duplicate-row','wrong-progress')){
    $candidate=switch($kind){
        'wrong-family'{$original.Replace('CASE family=4 ','CASE family=6 ')}
        'missing-row'{[regex]::Replace($original,'(?m)^CASE family=4 protocol=tcp wrap=0 stopTurn=0 measured=0 [^\r\n]*\r?\n','')}
        'duplicate-row'{$original+"`n"+[regex]::Match($original,'(?m)^CASE [^\r\n]*').Value+"`n"}
        'wrong-progress'{$original.Replace('in=32 out=32','in=24 out=32')}
    }
    $expected=if($kind -in @('missing-row','duplicate-row')){'Budget case count/format differs'}else{'Budget family/shape/copy/ownership row differs:0'}
    $reason=$null;try{Get-BdRows $candidate 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason -cne $expected){throw "Budget parser accepted/wrong rejection:$kind ($reason)"};$parserChecks+=@{name=$kind;rejected=$true;reason=$reason}
}
$guards=@();$reference=$builds[0];foreach($contract in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $guardDir=Join-Path $directory "compile-contracts/$contract";New-Item -ItemType Directory -Path $guardDir -Force|Out-Null;$flags=@($reference.flags)
    if($contract -ceq 'pb-api-tests'){$flags+='/DPB_API_TESTS=1'}elseif($contract -ceq 'missing-ndebug'){$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'})}elseif($contract -ceq 'missing-fingerprint'){$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})}else{$flags=@($flags|Where-Object {$_ -notlike '/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0'}
    $compilerOutput=@(& cl.exe @flags /c $reference.extraction.test "/Fo$guardDir/contract.obj" 2>&1);$exitCode=$LASTEXITCODE;$compilerOutput|Set-Content (Join-Path $guardDir 'compiler.log') -Encoding utf8
    $diagnostic=switch($contract){'pb-api-tests'{'Shipping facade budget required'}'missing-ndebug'{'Shipping facade budget configuration required'}'missing-fingerprint'{'facade stop tests require shipping fingerprint layout'}'invalid-family'{'Select one IP family'}}
    if(!$exitCode -or ![regex]::IsMatch(($compilerOutput -join "`n"),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($diagnostic))){throw "Duplex compile contract did not reject the expected condition:$contract"};$guards+=@{contract=$contract;exit_code=$exitCode;expected_failure=$true;expected_diagnostic=$diagnostic;log=(Join-Path $guardDir 'compiler.log');log_sha256=(Get-FileHash (Join-Path $guardDir 'compiler.log')).Hash.ToLowerInvariant()}
}
Assert-BdDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during duplex audit'}
Write-TestRecording 'passed'
$familySummaries=@(foreach($family in @(4,6)){$executions=@($script:TestExecutions|Where-Object {$_.ip_version -eq $family});@{ip_version=$family;executed_processes=$executions.Count;cases=($executions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;measured_schedules=($executions|ForEach-Object {$_.metrics.measured_schedules}|Measure-Object -Sum).Sum;callback_os_thread_joins=($executions|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$familySummaries;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$guards;parser_checks=$parserChecks;metadata_checks=$metadataChecks;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;
    kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only, no production defect/fix/before variant. Whole unchanged facade-stop/native-policy ledgers execute actual shipping legacy worker, default PB_COMPAT_OPEN and non-PB_API_TESTS native session/queue/lifecycle with NDEBUG. Actual WintunStartSession requests4MiB private rings; native geometry remains64slots/128ring. Both directions publish32 valid65535B IPv4/IPv6 TCP/UDP packets at one initial empty-SDK-entry gate. The real512KiB payload budget permits8 packets per turn, so simultaneous backlogs receive bounded service in both directions: each normal turn has one actual Reserve8/Commit8 and Release8, while native Receive32 occurs once. Four turns exhaust32 each direction. No fake FULL/partial results, per-packet commit, NativeCounter/COMPAT_PENDING/Stopping repair or manual reset of user wake events. Explicit legacy-only memcpy scope counts direction/sequence hits exactly once and excludes native setup/peer/oracle/metadata copies, not CPU/throughput. At turn1 or3, the application genuinely receives/returns every copied8 or24 old pointer before actual Life.BeginStop(native) returns; only then release the already-returned Raw Release observation gate. The worker itself encounters one real ReserveSTOPPED0 without additional copy/Commit, breaks and calls original ReturnPending, retiring uncopied24 or8 without delivery. Actual release vectors are8/24 or8/8/8/8. Normal completion instead polls real hosted outgoing32 completions, reaches an actual idle SDK entry, and actual LegacyEnd publishes Stopping/Wake. All public packet/control calls finish before the worker final Close5000, and End joins the actual legacy worker; captured native callback OS handles join individually before real DLL unload and exact original resource/quota ledgers become0. NativeActive1 is an independent hosted close barrier, released only after real Close entry and callback park; ordinary loader ownership remains through DLL calls/thread joins. Normal/final callback counts are dynamic, never fixed to one per case. Guards protect inherited native allocations/map; legacy rings retain VirtualAlloc and ordinary caller/config storage is not guarded. SDK hard gates mark selected call or completed Raw Release, not OS kernel park. No actual SYS/NDIS/IRQL/register/driver/device/registry, MainRuntime lease, old-system runtime, or sustained leak/latency proof is claimed. Default current snapshots declared raw dependencies and source fingerprint without old out; explicit703-file9631 typed-Ordinal baseline is fully authenticated before exact byte comparisons. Version2.0.0 preserved.';
    audit_sources=@('tests/model/duplex_budget32_tests.c','tools/build/duplex-budget32-tests.ps1','tools/audit/verify-duplex-budget32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
