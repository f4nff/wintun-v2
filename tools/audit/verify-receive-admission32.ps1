param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/receive-admission32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Receive admission requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
function Require-RaHash([string]$Path,[string]$Hash){if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Receive admission raw hash differs'}}
function Require-RaText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Receive admission complete projection differs'}}
function Assert-RaSet([string[]]$Actual,[string[]]$Expected,[string]$Message){$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}};if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}}
[string[]]$production=@(rg --files driver common include|ForEach-Object {$_.Replace('\','/')});if($LASTEXITCODE){throw 'Cannot enumerate receive admission production closure'}
[string[]]$dependencies=@($production)+@('wintun.props','tests/model/drain_boundary_tests.c','tests/model/kernel_nbl_fixture.h','tests/safety/guard_memory.h')
$repairs=[ordered]@{
    'common/v2_core.c'=@('2426da12e2089d731acfd234dc7f66e5c73311da0b1734d5e7cd1cac7884e883','8cf8d1cf2475183a4e366f68f3586e82233673640660cda7797b2a3c9d3ca3ca')
    'common/v2_core.h'=@('2994f94b4b76f0e08b92474badc8f1d05b5214eb400fb2723c035093bea062a1','19ed21b8d73f8fe81f3bb0a08ebb982d68bb4d2ce6b3b889a4aef91d885073d0')
}
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/receive-admission32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineIdentity=$null;$hashes=@{}
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 719 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0' -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal'){throw 'Receive admission baseline count/order/version differs'}
    Assert-RaSet $names @($manifest.files.PSObject.Properties.Name) 'Receive admission baseline file set differs'
    $entries=@(foreach($name in $names){Require-RaHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne '0f920bb470e314c293d212d5b26f4108f7cbb2e26086b88c6e1a5f4099788b61' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Receive admission baseline identity differs'}
    Assert-RaSet $production @($names|Where-Object {$_ -cmatch '^(driver|common|include)/'}) 'Receive admission baseline production set differs'
    foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
    $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()}
    $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
function Assert-RaDependencies {
    foreach($relative in $dependencies){
        Require-RaHash (Join-Path $baseline $relative) $hashes[$relative]
        if($historical -and $repairs.Contains($relative)){
            if($hashes[$relative] -cne $repairs[$relative][0]){throw 'Receive admission historical repair source differs'}
            Require-RaHash (Join-Path $script:RepoRoot $relative) $repairs[$relative][1]
        }else{Require-RaHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}
    }
    foreach($relative in $repairs.Keys){Require-RaHash (Join-Path $script:RepoRoot $relative) $repairs[$relative][1]}
}
Assert-RaDependencies
function Assert-RaBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Receive admission build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path);$test=Join-Path $script:RepoRoot 'tests/model/receive_admission32_tests.c'
    if($build.path -cne (Join-Path $place 'receive-admission32.exe') -or $build.entry -cne (Join-Path $place 'entry.c') -or $build.test -cne $test){throw 'Receive admission output/test binding differs'}
    Require-RaHash $build.path $build.sha256;Require-RaHash $build.entry $build.entry_sha256;Require-RaHash $test $build.test_sha256
    $hostSource=Join-Path $script:RepoRoot 'tests/model/drain_boundary_tests.c';$hostHash='50753c442cbeaaaa3956cc27d9884cb93fbe316a42953f387514f530466bc4a8'
    $kernel=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$kernelHash='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
    if($build.host_source -cne $hostSource -or $build.host_source_sha256 -cne $hostHash -or $build.host_original_sha256 -cne $hostHash -or $build.host_original -cne (Join-Path $place 'drain-host-original.c') -or $build.host -cne (Join-Path $place 'drain-host.h') -or $build.observer -cne (Join-Path $place 'receive-observe.h') -or $build.kernel_source -cne $kernel -or $build.kernel_source_sha256 -cne $kernelHash){throw 'Receive admission original host binding differs'}
    Require-RaHash $hostSource $hostHash;Require-RaHash $kernel $kernelHash;Require-RaHash $build.host_original $hostHash;Require-RaHash $build.host $build.host_sha256;Require-RaHash $build.observer $build.observer_sha256
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value)).Replace('\','/')+'"'})
    Require-RaText $build.host $body
    $observer=@('static VOID ObserveAdmissionComplete(PB_KERNEL_QUEUE *,const PB_DESCRIPTOR *,ULONG);','#define PbV2CompleteLocked ObserveAdmissionComplete',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_receive.h').Replace('\','/')+'"'),'#undef PbV2CompleteLocked')
    Require-RaText $build.observer (($observer -join "`n")+"`n")
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version
    $bindings=[ordered]@{PB_ADMISSION_HOST=$build.host;PB_RECEIVE_SOURCE=$build.observer;PB_WORKER_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_worker.h');PB_QUERY_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_query.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"'
    Require-RaText $build.entry (($lines -join "`n")+"`n")
    Assert-RaSet @($build.production|ForEach-Object {$_.relative}) $production 'Receive admission production closure set differs'
    foreach($part in $build.production){$source=Join-Path $script:RepoRoot $part.relative;$copy=Join-Path $place ('source/'+$part.relative)
        if($part.source -cne $source -or $part.path -cne $copy -or $part.source_sha256 -cne $part.sha256){throw 'Receive admission production copy binding differs'}
        Require-RaHash $source $part.source_sha256;Require-RaHash $copy $part.sha256
        $expected=if($historical -and $repairs.Contains($part.relative)){$repairs[$part.relative][1]}else{$hashes[$part.relative]}
        if($part.sha256 -cne $expected){throw 'Receive admission production copy differs from accepted dependency'}
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.arguments -join "`n") -cne ($flags -join "`n")){throw 'Receive admission compile arguments differ'}
}
function Read-RaMetrics([string[]]$Lines,[int]$Family){
    $rows=@($Lines|Where-Object {$_ -cmatch '^RECEIVE_ADMISSION32 '});$summary=@($Lines|Where-Object {$_ -cmatch '^RECEIVE_ADMISSION32_SUMMARY '})
    if($rows.Count -ne 24 -or $summary.Count -ne 1){throw 'Receive admission case count/format differs'}
    $seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($row in $rows){
        if($row -cnotmatch '^RECEIVE_ADMISSION32 ip([46]) protocol(6|17) ndis(30|89) shape([012]) wrap([01]) measured([01]) oldObjects32 oldNbls17 claims31 invalid([01]) stopped(30|31) shared0 returned32 owners0 threads0$'){throw 'Receive admission case count/format differs'}
        $m=$Matches.Clone();$key=($m[2],$m[4],$m[5],$m[6])-join '/'
        if([int]$m[1] -ne $Family -or [int]$m[3] -ne $(if($m[2] -eq '6'){30}else{89}) -or [int]$m[7] -ne $(if($m[4] -eq '2'){1}else{0}) -or [int]$m[8] -ne $(if($m[4] -eq '2'){30}else{31}) -or !$seen.Add($key)){throw 'Receive admission family/matrix/ownership differs'}
    }
    if($summary[0] -cnotmatch '^RECEIVE_ADMISSION32_SUMMARY ip([46]) cases24 warm12 measured12 handles([0-9]+)/([0-9]+) threads0$' -or [int]$Matches[1] -ne $Family -or $Matches[2] -cne $Matches[3]){throw 'Receive admission summary differs'}
    return @{cases=24;warm_cases=12;measured_cases=12;handles_before=[int]$Matches[2];handles_after=[int]$Matches[3];process_handles_before=[int]$Matches[2];process_handles_after=[int]$Matches[3];caller_joins=0;logical_completion_claims=24*31;shared_completions=0;returned_originals=24*32}
}
function Invoke-RaProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::Start($info)
    try{$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'Receive admission watchdog'};$text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode}finally{$process.Dispose()}
    $log=Join-Path (Split-Path $build.path) 'result.log';[IO.File]::WriteAllText($log,$text,$utf8);$lines=$text -split '\r?\n';$metrics=Read-RaMetrics $lines $build.ip_version
    $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;exit_code=$code;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics;passed=$code -eq 0});Write-TestRecording 'running';if($code){throw 'Receive admission process failed'}
}
Start-TestRecording -Directory $directory -Name 'operation-results.json'
$builds=@();$first=$null
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $build=Build-ReceiveAdmission32Tests -Directory (Join-Path $directory "ipv$family-$mode-guard$guard") -IpVersion $family -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $family -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Receive admission requested build differs'}
    Assert-RaBuild $build;$builds+=$build;if(!$first){$first=$build};Invoke-RaProcess $build
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log
foreach($kind in 'wrong-family','missing-row','duplicate-row','wrong-claim'){
    $changed=switch($kind){'wrong-family'{@($original|ForEach-Object {$_ -replace '^RECEIVE_ADMISSION32 ip4 ','RECEIVE_ADMISSION32 ip6 '})};'missing-row'{@($original|Where-Object {$_ -cnotmatch '^RECEIVE_ADMISSION32 ip4 protocol6 ndis30 shape0 wrap0 measured0 '})};'duplicate-row'{@($original)+@($original|Where-Object {$_ -clike 'RECEIVE_ADMISSION32 *'}|Select-Object -First 1)};'wrong-claim'{@($original|ForEach-Object {$_ -replace 'claims31 invalid1 stopped30','claims31 invalid0 stopped30'})}}
    $expected=if($kind -in 'wrong-family','wrong-claim'){'Receive admission family/matrix/ownership differs'}else{'Receive admission case count/format differs'};$reason=$null
    try{Read-RaMetrics $changed 4|Out-Null}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Receive admission parser wrong rejection: $kind ($reason)"};$parser+=@{name=$kind;rejected=$true;reason=$reason}
}
$metadata=@();Assert-RaBuild ($first|ConvertTo-Json -Depth 20|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','host-hash','observer-hash','compile-flags'){
    $changed=$first|ConvertTo-Json -Depth 20|ConvertFrom-Json;$expected=switch($kind){'duplicate-dependency'{$changed.production[1]=$changed.production[0];'Receive admission production closure set differs'};'host-hash'{$changed.host_source_sha256='0'*64;'Receive admission original host binding differs'};'observer-hash'{$changed.observer_sha256='0'*64;'Receive admission raw hash differs'};'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Receive admission compile arguments differ'}}
    $reason=$null;try{Assert-RaBuild $changed}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Receive admission metadata wrong rejection: $kind ($reason)"};$metadata+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($first.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$text=$originalEntry;$flags=@($first.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping receive admission paths required'};'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping receive admission configuration required'};'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'};'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'receive admission requires shipping fingerprint'}}
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log'; & cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or ![regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message))){throw "Receive admission exact compiler guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-RaDependencies
$cases=0;foreach($execution in $script:TestExecutions){$cases+=$execution.metrics.cases};if($script:TestExecutions.Count -ne 8 -or $cases -ne 192){throw 'Receive admission executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Receive admission source changed during audit'}
Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;variant='current';source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();
    operations=$script:TestResultFile;builds=$builds;cases=$cases;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;production_closure_changed=$historical;reviewed_delta=@($repairs.Keys|Where-Object {$historical}|ForEach-Object {@{path=$_;before_sha256=$repairs[$_][0];after_sha256=$repairs[$_][1]}});approved_shared_repair_contracts=@($repairs.Keys|ForEach-Object {@{path=$_;before_sha256=$repairs[$_][0];after_sha256=$repairs[$_][1]}});historical_dependency_repairs_applied=$historical;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests/model/receive_admission32_tests.c','tools/build/receive-admission32-tests.ps1','tools/audit/verify-receive-admission32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual ReceiveBatch and ReturnNbls with whole frozen drain-boundary/kernel-NBL hosted DDI fixtures; production C/header bodies are unchanged by projection, except a transparent logical-completion observer forwarding the actual CompleteLocked call in ReceiveBatch only. Thirty-two old packets remain NDIS-owned as one16-original coalesced NBL plus16 plain NBLs. TCP6.30 or UDP6.89 feature gates are capability variables, not old-machine/NDIS-layout execution. New32 descriptors contain a malformed current/future live token, or a malformed-first inactive token followed by a same-token valid replay. The reviewed core repair terminally records only scoped inactive nonzero generations. Each current run claims31 logical completions and Fault retires all pending entries: zero shared completions is intentional, not a delivered rejection claim. Old records, payloads and active generations are checked before deterministic forward/reverse real Return; actual returns release records/MDLs/NBLs, then fixture Destroy closes SDK events/region. No new concurrent threads, actual workerloop/Query/FreeSession execution, driver/device, real kernel/IRQL, native CPU or sustained leak claim. Existing concurrent suites retain those separate scopes. Guarded mode guards original host record/MDL/NBL objects; scratch/cache buffers are guard allocations in both modes. The test uses exact source and metadata oracles, no owner-counter repair. Default current snapshot is self-contained; optional719-file typed-Ordinal0f92 baseline admits only two whole-file raw-hash-locked core repairs, with all other dependency bytes identical. Historical old143 proof is independently preserved by root and is not manufactured by this current-only audit.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
