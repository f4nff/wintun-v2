param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/cross-queue-prepare32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Cross-queue audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$hashes=@{}
$accepted=$null
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    $accepted=$manifest.source_tree_sha256
    if($accepted -ceq '56d92e483bf1974cd14eab0e68de13ef47690de416c4ab8f6e0f27bb33a8cada'){$expectedSources=686;$order=$manifest.order}
    elseif($accepted -ceq '114acbbb18b0b63fcb4b738d6eb1525547661c633ed56ada614ae175572147c8'){$expectedSources=771;$order=$manifest.source_fingerprint_order}
    else{throw 'Unexpected cross-queue historical fingerprint'}
    if($manifest.version -cne '2.0.0' -or $order -cne 'typed string[] Ordinal' -or $paths.Count -ne $expectedSources -or @($manifest.files.PSObject.Properties).Count -ne $expectedSources -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected cross-queue historical manifest'}
    $set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$set.Add($path)){throw 'Duplicate cross-queue baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Baseline raw bytes changed:$path"};$hashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($name in $manifest.files.PSObject.Properties.Name){if(!$set.Contains($name)){throw 'Historical source order/file map differ'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Historical cross-queue fingerprint differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate cross-queue dependencies'}
$dependencies+=@('wintun.props','tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/cross-queue-prepare32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($relative in $dependencies){$current=Join-Path $script:RepoRoot $relative;$snapshot=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $snapshot) -Force|Out-Null;$bytes=[IO.File]::ReadAllBytes($current);[IO.File]::WriteAllBytes($snapshot,$bytes);$hashes[$relative]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-CqDependencies {
    foreach($relative in $dependencies){$before=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $hashes[$relative] -or (Get-FileHash $before).Hash -cne (Get-FileHash $current).Hash){throw "Cross-queue raw dependency differs:$relative"}}
}
Assert-CqDependencies
function Assert-CqBuild($build){
    $e=$build.extraction;$api=Join-Path $script:RepoRoot 'api/v2';$sessionSource=Join-Path $api 'session.c';$hostSource=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c';$test=Join-Path $script:RepoRoot 'tests/model/cross_queue_prepare32_tests.c'
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or !$build.ndebug -or $build.pb_api_tests){throw 'Cross-queue shipping mode differs'}
    $body=[regex]::Replace([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c')),'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value)).Replace('\','/')+'"'})
    if($e.session_source -cne $sessionSource -or [IO.File]::ReadAllText($e.session) -cne $body){throw 'Complete session exceeded exact quoted-include projection'}
    if($e.host -cne $hostSource -or $e.host_sha256 -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83' -or (Get-FileHash $e.host_original).Hash.ToLowerInvariant() -cne $e.host_sha256 -or (Get-FileHash $hostSource).Hash.ToLowerInvariant() -cne $e.host_sha256){throw 'Complete frozen native-policy fixture differs'}
    $entry=@'
#define WaitForMultipleObjects CrossWait
#undef WaitForSingleObject
#define WaitForSingleObject CrossProgress
#define SleepConditionVariableSRW CrossSleep
#include PB_CROSSQUEUE_SESSION_SOURCE
#undef WaitForMultipleObjects
#undef SleepConditionVariableSRW
'@
    if([IO.File]::ReadAllText($e.entry) -cne $entry){throw 'Actual SDK wait/CV observer binding differs'}
    $lines=@('#define CT_FIXTURE 1');if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_CROSSQUEUE_SESSION_SOURCE "'+$e.session.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    if($e.test -cne $test -or [IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Complete cross-queue entry differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$($build.mode)",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Cross-queue exact compiler flags differ'}
    if((Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256 -or (Get-FileHash $build.fixture).Hash.ToLowerInvariant() -cne $build.fixture_sha256){throw 'Cross-queue binary hash differs'}
    $expectedPaths=@($sessionSource,$e.session,$hostSource,$e.host_original,$e.entry,$e.fixture_entry,$test,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'))
    if(@($build.sources).Count -ne 8){throw 'Cross-queue source count must be8'}
    $sourceSet=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($source in $build.sources){if(!$sourceSet.Add($source.path)){throw 'Cross-queue duplicate source metadata'};if((Get-FileHash $source.path).Hash.ToLowerInvariant() -cne $source.sha256){throw 'Cross-queue input/generated source hash differs'}}
    if(!$sourceSet.SetEquals([string[]]$expectedPaths)){throw 'Cross-queue exact source set differs'}
}
function Get-CqRows([string]$text,[int]$family){
    $rows=[regex]::Matches($text,'(?m)^CASE family=([46]) protocol=(tcp|udp) wrap=([01]) pin=([01]) measured=([01]) receive=32 release=32 reserve=32 stopped=0 cancel=32 progress=0/1/1/0/1 waits=3 timeout=0 prepared_pinned=1 public_joins=3 callback_joins=(\d+)\r?$')
    if($rows.Count -ne 32 -or [regex]::Matches($text,'(?m)^CASE ').Count -ne 32){throw 'Cross-queue case rows missing/extra'};$callbacks=0
    for($i=0;$i -lt 32;++$i){$shape=$i%8;$protocol=if($shape -ge 4){'udp'}else{'tcp'};$wrap=[int][Math]::Floor($shape/2)%2;$pin=$shape%2;$g=$rows[$i].Groups;$joined=[int]$g[6].Value
        if([int]$g[1].Value -ne $family -or $g[2].Value -cne $protocol -or [int]$g[3].Value -ne $wrap -or [int]$g[4].Value -ne $pin -or [int]$g[5].Value -ne [int]($i -ge 16) -or $joined -lt 2 -or $joined -gt 8){throw "Cross-queue exact shape/ownership row differs:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: CROSSQUEUE32 family([46]) cases(\d+) warm16 measured16 publicJoins(\d+) callbackJoins(\d+) handles(\d+)/(\d+) owners=0\r?$')
    if($summary.Count -ne 1 -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1){throw 'Nonunique cross-queue summary'};$g=$summary[0].Groups
    if([int]$g[1].Value -ne $family -or [int]$g[2].Value -ne 32 -or [int]$g[3].Value -ne 96 -or [int]$g[4].Value -ne $callbacks -or $g[5].Value -cne $g[6].Value){throw 'Cross-queue summary/actual join sums/handle equality differ'}
    return @{cases=32;checked_warmup_schedules=16;measured_schedules=16;public_owner_joins=64;prepare_caller_joins=32;callback_os_thread_joins=$callbacks;actual_lifecycle_waits=96;actual_lifecycle_timeouts=0;process_handles_before=[int]$g[5].Value;process_handles_after=[int]$g[6].Value}
}
function Get-CwRows([string]$text,[int]$family){
    $rows=[regex]::Matches($text,'(?m)^CLOSEWAIT32 family=([46]) protocol=(tcp|udp) measured=([01]) reserve=32 commit=32 wait_failures=1 wait_calls=1 error=995 published_retained=32 public_retained=1 poll=32 native_barrier=1 owner_joins=1 caller_joins=1 prepared=(\d+) final=1 callbacks=(\d+) owned=0 unload=1\r?$')
    if($rows.Count -ne 8 -or [regex]::Matches($text,'(?m)^CLOSEWAIT32 ').Count -ne 8){throw 'Close-wait case rows missing/extra'};$callbacks=0
    for($i=0;$i -lt 8;++$i){$g=$rows[$i].Groups;$protocol=if($i%2){'udp'}else{'tcp'};$prepared=[int]$g[4].Value;$joined=[int]$g[5].Value
        if([int]$g[1].Value -ne $family -or $g[2].Value -cne $protocol -or [int]$g[3].Value -ne [int]($i -ge 4) -or $prepared -lt 1 -or $prepared -ge 8 -or $joined -ne $prepared+1){throw "Close-wait exact shape/callback stages differ:$i"};$callbacks+=$joined
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: CLOSEWAIT32 family([46]) cases8 warm4 measured4 ownerJoins8 callerJoins8 callbackJoins(\d+) handles(\d+)/(\d+) owners=0\r?$')
    if($summary.Count -ne 1 -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1){throw 'Nonunique close-wait summary'};$g=$summary[0].Groups
    if([int]$g[1].Value -ne $family -or [int]$g[2].Value -ne $callbacks -or $g[3].Value -cne $g[4].Value){throw 'Close-wait summary/actual joins/handle equality differ'}
    return @{cases=8;checked_warmup_schedules=4;measured_schedules=4;public_owner_joins=8;prepare_caller_joins=8;callback_os_thread_joins=$callbacks;defensive_wait_failures=8;defensive_wait_calls=8;published_packets_preserved=256;public_handles_preserved=8;completion_packets_polled=256;process_handles_before=[int]$g[3].Value;process_handles_after=[int]$g[4].Value}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-CqProcess($build,[switch]$CloseWaitFailure){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.ArgumentList.Add($build.fixture);$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    $arguments=@($build.fixture);if($CloseWaitFailure){$info.ArgumentList.Add('--close-wait-failure32');$arguments+='--close-wait-failure32'}
    try{if(!$process.Start()){throw 'Cannot start cross-queue process'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'Cross-queue watchdog expired60s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ip{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        if($process.ExitCode){throw "Cross-queue exit$($process.ExitCode):$log"};$metrics=if($CloseWaitFailure){Get-CwRows $output $build.ip_version}else{Get-CqRows $output $build.ip_version};$script:TestExecutions.Add([ordered]@{program=$build.path;arguments=$arguments;case_group=$(if($CloseWaitFailure){'close-wait-failure32'}else{'cross-queue-prepare32'});exit_code=$process.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;metrics=$metrics;log=$log;log_sha256=(Get-FileHash -LiteralPath $log).Hash.ToLowerInvariant();reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})});Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})
    }finally{$process.Dispose()}
}
$builds=@();foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){$memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-CrossQueuePrepare32Tests -Directory (Join-Path $directory "ipv$family/$mode-$memory") -IpVersion $family -Mode $mode -Guarded:$guarded;Assert-CqBuild $build;$builds+=,$build;Invoke-CqProcess $build;Invoke-CqProcess $build -CloseWaitFailure}}}
$wrongFamilyRejected=$false;try{Get-CqRows ([IO.File]::ReadAllText($script:TestExecutions[0].log)) 6|Out-Null}catch{$wrongFamilyRejected=$true};if(!$wrongFamilyRejected){throw 'Cross-queue wrong-family log accepted'}
function Assert-CqRejected([scriptblock]$action,[string]$reason){
    $caught=$false;try{& $action|Out-Null}catch{if($_.Exception.Message -cne $reason){throw "Unexpected contract rejection:$($_.Exception.Message)"};$caught=$true}
    if(!$caught){throw "Accepted invalid cross-queue contract:$reason"}
}
$parserChecks=@();$closeLog=[IO.File]::ReadAllText($script:TestExecutions[1].log)
foreach($kind in @('wrong-family','missing-row','wait-retry','callback-stage')){
    $text=$closeLog;$family=4
    switch($kind){
        'wrong-family'{$family=6;$reason='Close-wait exact shape/callback stages differ:0'}
        'missing-row'{$text=[regex]::new('(?m)^CLOSEWAIT32 [^\r\n]*\r?\n').Replace($text,'',1);$reason='Close-wait case rows missing/extra'}
        'wait-retry'{$text=$text.Replace('wait_calls=1','wait_calls=2');$reason='Close-wait case rows missing/extra'}
        'callback-stage'{$text=[regex]::Replace($text,'prepared=(\d+) final=1 callbacks=(\d+)','prepared=1 final=1 callbacks=8');$reason='Close-wait exact shape/callback stages differ:0'}
    }
    Assert-CqRejected {Get-CwRows $text $family} $reason;$parserChecks+=@{name=$kind;rejected=$true;reason=$reason}
}
$clone=$builds[0]|ConvertTo-Json -Depth 18|ConvertFrom-Json;Assert-CqBuild $clone;$validClone=$true;$metadataChecks=@()
foreach($kind in @('deleted-source','duplicate-source','compile-flags','generated-test')){
    $bad=$builds[0]|ConvertTo-Json -Depth 18|ConvertFrom-Json
    switch($kind){
        'deleted-source'{$bad.sources=@($bad.sources|Select-Object -Skip 1);$reason='Cross-queue source count must be8'}
        'duplicate-source'{$bad.sources[1]=$bad.sources[0];$reason='Cross-queue duplicate source metadata'}
        'compile-flags'{$bad.flags=@($bad.flags|Where-Object {$_ -cne '/DNDEBUG'});$reason='Cross-queue exact compiler flags differ'}
        'generated-test'{$bad.extraction.test='unexpected-test.c';$reason='Complete cross-queue entry differs'}
    }
    Assert-CqRejected {Assert-CqBuild $bad} $reason;$metadataChecks+=@{name=$kind;rejected=$true;reason=$reason}
}
$guards=@();$reference=$builds[0];foreach($contract in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $guardDir=Join-Path $directory "compile-contracts/$contract";New-Item -ItemType Directory -Path $guardDir -Force|Out-Null;$flags=@($reference.flags)
    if($contract -ceq 'pb-api-tests'){$flags+='/DPB_API_TESTS=1'}elseif($contract -ceq 'missing-ndebug'){$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'})}elseif($contract -ceq 'missing-fingerprint'){$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})}else{$flags=@($flags|Where-Object {$_ -notlike '/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0'}
    $output=@(& cl.exe @flags /c $reference.extraction.test "/Fo$guardDir/contract.obj" 2>&1);$code=$LASTEXITCODE;$output|Set-Content (Join-Path $guardDir 'compiler.log') -Encoding utf8
    $diagnostic=switch($contract){'pb-api-tests'{'Actual queue/lifecycle paths required'}'missing-ndebug'{'Shipping queue/lifecycle branch required'}'missing-fingerprint'{'shipping fingerprint required'}'invalid-family'{'Select one IP family'}}
    if($code -ne 2 -or ($output -join "`n").IndexOf($diagnostic,[StringComparison]::Ordinal) -lt 0 -or ($output -join "`n") -notmatch 'C(?:1189|2338)'){throw "Wrong cross-queue compile rejection:$contract"};$guards+=@{contract=$contract;exit_code=$code;expected_failure=$true;expected_diagnostic=$diagnostic;log=(Join-Path $guardDir 'compiler.log');log_sha256=(Get-FileHash -LiteralPath (Join-Path $guardDir 'compiler.log')).Hash.ToLowerInvariant()}
}
Assert-CqDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during cross-queue audit'}
Write-TestRecording 'passed'
$caseGroups=@(foreach($name in @('cross-queue-prepare32','close-wait-failure32')){$runs=@($script:TestExecutions|Where-Object {$_.case_group -ceq $name});@{name=$name;executed_processes=$runs.Count;cases=($runs|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;public_owner_joins=($runs|ForEach-Object {$_.metrics.public_owner_joins}|Measure-Object -Sum).Sum;prepare_caller_joins=($runs|ForEach-Object {$_.metrics.prepare_caller_joins}|Measure-Object -Sum).Sum;callback_os_thread_joins=($runs|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
$families=@(foreach($family in @(4,6)){$runs=@($script:TestExecutions|Where-Object {$_.ip_version -eq $family});@{ip_version=$family;executed_processes=$runs.Count;cases=($runs|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;measured_schedules=($runs|ForEach-Object {$_.metrics.measured_schedules}|Measure-Object -Sum).Sum;callback_os_thread_joins=($runs|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{schema_version=1;version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;case_groups=$caseGroups;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$guards;parser_checks=$parserChecks;metadata_checks=$metadataChecks;valid_build_json_clone=$validClone;wrong_family_log_rejected=$wrongFamilyRejected;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only, no production bug/fix or before variant. The same8 executables now also run --close-wait-failure32 as8 separate processes: TCP/UDP each2warm/2measured gives64new cases. Actual Reserve/Commit32 establishes Published32; the designated non-final PrepareClose caller receives one defensive SDK CV FALSE/ERROR_OPERATION_ABORTED while still owning State. It immediately returns PB_CORRUPT with original995, no retry/Fatal/public-handle consumption. Published32/private slots/mapping/module pins/native1 stay retained. The hosted peer genuinely pops RX_SUBMIT32, validates full payload and publishes completions, then the original owner Poll32 and PollEMPTY before joining. A second real lifecycle wait still sees independent NativeActive1; only actual SetNativeActive0/trustedQUERY permits Prepared. All public callers join before successful finalClose, and every captured callback OS handle joins before actual module unload. No naturally occurring Windows CV failure is claimed; synchronous Drain/SetPolicy tests remain separate.  Real unloadable DLL executes unchanged shipping session/queue/lifecycle and complete native-policy owner ledger. Two actual queue owners exchange32 valid1500B packets per direction/family: pin queue Receive32/Release32 then Wait; other queue Reserve32 then Wait. BeginStop broadcasts the actual ClosingEvent. Pin owner is held after real SDK Wait returns but before DLL QueueLeave releases Lifetime; this is not an OS park claim. Lifecycle first consumes the old BeginStop hint and reaches another real SDK-entry gate with Progress unsignaled. Other owner CommitStopped accepts0, confirmed no new progress; same owner Cancel32 then joins, producing a real success hint0-to1. Concurrent non-final PrepareClose5000 repeats BeginStop and coalesces the auto-reset hint at1, then actual SDK condition-variable wait releases State. Lifecycle consumes the combined hint, passes zero-loan bookkeeping, and reaches independent hosted NativeActive1 barrier with Progress0. NativeActive0 alone signals the final1 and preparation completes while the first owner still holds Lifetime shared. Real WorkCallbacks barrier proves Prepared with pub+registry refs2/pins2, native Region1, resources not closed; TryLifetimeEX fails until pin owner exits and joins. Only after both queue owners and preparation caller join does unique successful finalClose consume the public handle. Final callback count equals actual stable preparation callbacks plus1; every captured OS thread handle joins/closes, ordinary loader ownership ends with real module detach and all inherited resource/quota ledgers zero. No manual Retain/refcount/event/counter repair or unknown SDK handle closure. Ring cursor wrap starts empty and slots keep legal generation1; incoming token recycle and reserved-token cancellation are genuine batch32 calls. IPv4/IPv6 TCP/UDP payloads carry direction/sequence and opaque64 markers, not a timestamp API. No CPU/throughput, native SYS/NDIS/IRQL, actual driver/device/registry, full runtime dependencies or Win7 runtime proof. SDK-entry markers do not claim kernel parking. Guarded mode protects inherited native allocations/map; caller/config/stack storage remains ordinary live memory. Default audit captures complete current declared dependencies without historical out; explicit baseline authenticates either all686 typed-Ordinal56d9 bytes or all771 typed-Ordinal114acbbb bytes and the accepted manifest-order field, before exact dependency comparisons. Version2.0.0 preserved.';
    audit_sources=@('tests/model/cross_queue_prepare32_tests.c','tools/build/cross-queue-prepare32-tests.ps1','tools/audit/verify-cross-queue-prepare32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
