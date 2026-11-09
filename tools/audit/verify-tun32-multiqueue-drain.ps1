param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/tun32-multiqueue-drain-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Multiqueue drain requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$utf8=[Text.UTF8Encoding]::new($false)
function Require-NdrHash([string]$Path,[string]$Hash){
    if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Multiqueue recorded raw hash differs'}
}
function Require-NdrText([string]$Path,[string]$Text){
    if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Multiqueue complete projection differs'}
}
function Assert-NdrSet([string[]]$Actual,[string[]]$Expected,[string]$Message){
    $set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($name in $Actual){if(!$set.Add($name)){throw $Message}}
    if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}
}
$baselineIdentity=$null;$baselineRecord=$null
if($BaselineDirectory){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 686 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0'){throw 'Multiqueue baseline file count/order/version differs'}
    Assert-NdrSet $names @($manifest.files.PSObject.Properties.Name) 'Multiqueue baseline file set differs'
    $entries=@(foreach($name in $names){Require-NdrHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne '56d92e483bf1974cd14eab0e68de13ef47690de416c4ab8f6e0f27bb33a8cada' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Multiqueue baseline identity differs'}
    [string[]]$current=@(rg --files driver common include|ForEach-Object {$_.Replace('\','/')});if($LASTEXITCODE){throw 'Cannot enumerate multiqueue production closure'}
    [string[]]$old=@($names|Where-Object {$_ -cmatch '^(driver|common|include)/'})
    Assert-NdrSet $current $old 'Multiqueue baseline production closure differs'
    foreach($name in $current){Require-NdrHash (Join-Path $script:RepoRoot $name) $manifest.files.$name}
    $baselineRecord=@{path=$baseline;manifest=$manifestPath;manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();files=$names.Count;order='StringComparer.Ordinal';production_files=$current.Count}
}
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/tun32-multiqueue-drain/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
function Assert-NdrBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Multiqueue build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path)
    if($build.path -cne (Join-Path $place 'tun32-multiqueue-drain.exe') -or $build.entry -cne (Join-Path $place 'entry.c') -or $build.host -cne (Join-Path $place 'drain-host.h') -or $build.observer -cne (Join-Path $place 'worker-observe.h')){throw 'Multiqueue output binding differs'}
    Require-NdrHash $build.path $build.sha256;Require-NdrHash $build.entry $build.entry_sha256
    Require-NdrHash $build.host $build.host_sha256;Require-NdrHash $build.observer $build.observer_sha256
    $test=Join-Path $script:RepoRoot 'tests/model/tun32_multiqueue_drain_tests.c'
    if($build.test -cne $test){throw 'Multiqueue test source differs'};Require-NdrHash $test $build.test_sha256
    $hosts=[ordered]@{'tests/model/drain_boundary_tests.c'='50753c442cbeaaaa3956cc27d9884cb93fbe316a42953f387514f530466bc4a8';'tests/model/kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'}
    Assert-NdrSet @($build.inputs|ForEach-Object {$_.relative}) @($hosts.Keys) 'Multiqueue frozen input set differs'
    foreach($frozen in $build.inputs){
        $source=Join-Path $script:RepoRoot $frozen.relative;$copy=Join-Path $place ('original-'+[IO.Path]::GetFileName($source))
        if($frozen.source -cne $source -or $frozen.path -cne $copy -or $frozen.source_sha256 -cne $hosts[$frozen.relative] -or $frozen.sha256 -cne $hosts[$frozen.relative]){throw 'Multiqueue frozen input binding differs'}
        Require-NdrHash $source $hosts[$frozen.relative];Require-NdrHash $copy $hosts[$frozen.relative]
    }
    $source=Join-Path $script:RepoRoot 'tests/model/drain_boundary_tests.c';$body=[IO.File]::ReadAllText($source)
    $body=[regex]::Replace($body,'"([^"\r\n]*\.[ch])"',{param($m);'"'+[IO.Path]::GetFullPath((Join-Path (Split-Path $source) $m.Groups[1].Value)).Replace('\','/')+'"'})
    Require-NdrText $build.host $body
    # This literal projection is independent of the builder's observer text.
    $observer=@'
static BOOLEAN NdrReceive(PB_KERNEL_QUEUE *);
static VOID NdrWait(ULONG,VOID **,int,int,int,BOOLEAN,VOID *,VOID *);
static VOID NdrExit(LONG);
#define PbV2ReceiveBatch NdrReceive
#define KeWaitForMultipleObjects NdrWait
#define PsTerminateSystemThread NdrExit
#include PB_NDR_WORKER
#undef PsTerminateSystemThread
#undef KeWaitForMultipleObjects
#undef PbV2ReceiveBatch
'@
    $observer=$observer.Replace("`r`n","`n").Replace("`n","`r`n")
    Require-NdrText $build.observer $observer
    [string[]]$dependencies=@(rg --files driver common include);if($LASTEXITCODE){throw 'Cannot enumerate multiqueue build closure'}
    Assert-NdrSet @($build.production|ForEach-Object {$_.relative}) $dependencies 'Multiqueue production closure set differs'
    foreach($p in $build.production){
        $source=Join-Path $script:RepoRoot $p.relative;$copy=Join-Path $place ('source-'+$p.relative.Replace('\','-').Replace('/','-'))
        if($p.source -cne $source -or $p.path -cne $copy -or $p.source_sha256 -cne $p.sha256){throw 'Multiqueue production binding differs'}
        Require-NdrHash $source $p.source_sha256;Require-NdrHash $copy $p.sha256
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($flags -join "`n") -cne ($build.arguments -join "`n")){throw 'Multiqueue compile arguments differ'}
    $entry=@();if($build.guarded){$entry+='#define PB_GUARD_TESTS 1'};$entry+='#define PB_TEST_IP_VERSION '+$build.ip_version
    $bindings=[ordered]@{PB_NDR_HOST=$build.host;PB_WORKER_SOURCE=$build.observer;PB_NDR_WORKER=(Join-Path $script:RepoRoot 'driver/v2/session_worker.h')}
    foreach($name in $bindings.Keys){$entry+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $entry+='#include "'+$test.Replace('\','/')+'"';Require-NdrText $build.entry (($entry -join "`n")+"`n")
}
function Read-NdrMetrics([string[]]$Lines,[int]$IpVersion){
    $text=$Lines -join "`n"
    $pattern='(?m)^MULTIQUEUE_DRAIN32 ip(?<ip>[46]) protocol(?<protocol>6|17) ndis(?<minor>20|89) wrap(?<wrap>[01]) returnFirst(?<first>[01]) measured(?<measured>[01]) originals32 nativeNbl16 stopped16 complete32 joins2 waitEntries2 owners0$'
    $rows=[regex]::Matches($text,$pattern)
    if($rows.Count -ne 32 -or @($Lines|Where-Object {$_ -like 'MULTIQUEUE_DRAIN32 *'}).Count -ne 32){throw 'Multiqueue case count/format differs'}
    $actual=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($row in $rows){
        $g=$row.Groups
        if([int]$g['ip'].Value -ne $IpVersion -or !$actual.Add((@('protocol','minor','wrap','first','measured')|ForEach-Object {$g[$_].Value}) -join ':')){throw 'Multiqueue family/duplicate case differs'}
    }
    foreach($protocol in 6,17){foreach($minor in 20,89){foreach($wrap in 0,1){foreach($first in 0,1){foreach($measured in 0,1){
        if(!$actual.Contains("${protocol}:${minor}:${wrap}:${first}:$measured")){throw 'Multiqueue case matrix differs'}
    }}}}}
    $summary=[regex]::Matches($text,'(?m)^PASS: family(?<ip>[46]) multiqueue normal drain32 cases32 warm16 measured16 joins64 handles(?<before>\d+)/(?<after>\d+)$')
    if($summary.Count -ne 1 -or [int]$summary[0].Groups['ip'].Value -ne $IpVersion -or $summary[0].Groups['before'].Value -cne $summary[0].Groups['after'].Value){throw 'Multiqueue summary/resources differ'}
    return @{cases=$rows.Count;warm=16;measured=16;worker_joins=64;originals=32*$rows.Count;native_nbls=16*$rows.Count;stopped_completions=16*$rows.Count;completions=32*$rows.Count;wait_entries=2*$rows.Count;process_handles_before=[int]$summary[0].Groups['before'].Value;process_handles_after=[int]$summary[0].Groups['after'].Value}
}
Start-TestRecording $directory 'operations.json'
$builds=@();$firstBuild=$null
foreach($ip in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $place=Join-Path $directory "ipv$ip-$mode-guard$guard"
    $build=Build-Tun32MultiqueueDrainTests -Directory $place -IpVersion $ip -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $ip -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Multiqueue requested build differs'}
    Assert-NdrBuild $build;$builds+=$build;if(!$firstBuild){$firstBuild=$build}
    Invoke-TestTool $build.path @()|Out-Host
    $row=$script:TestExecutions[$script:TestExecutions.Count-1]
    $row['ip_version']=$ip;$row['mode']=$mode;$row['guarded']=$guard;$row['metrics']=Read-NdrMetrics (Get-Content -LiteralPath $row.log) $ip
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log
Read-NdrMetrics $original 4|Out-Null
foreach($kind in 'wrong-family','missing-row','duplicate-row','completion-count'){
    $changed=switch($kind){
        'wrong-family'{@($original|ForEach-Object {$_ -replace '^MULTIQUEUE_DRAIN32 ip4 ','MULTIQUEUE_DRAIN32 ip6 '})}
        'missing-row'{@($original|Where-Object {$_ -notmatch '^MULTIQUEUE_DRAIN32 ip4 protocol6 ndis20 wrap0 returnFirst0 measured0 '})}
        'duplicate-row'{@($original)+@($original|Where-Object {$_ -like 'MULTIQUEUE_DRAIN32 *'}|Select-Object -First 1)}
        'completion-count'{@($original|ForEach-Object {$_ -replace 'complete32 joins2','complete31 joins2'})}
    }
    $expected=if($kind -eq 'wrong-family'){'Multiqueue family/duplicate case differs'}else{'Multiqueue case count/format differs'}
    $rejected=$false;try{Read-NdrMetrics $changed 4|Out-Null}catch{if($_.Exception.Message -cne $expected){throw "Multiqueue parser rejected for unrelated reason: $_"};$rejected=$true}
    if(!$rejected){throw "Multiqueue parser accepted $kind"};$parser+=@{name=$kind;rejected=$true;reason=$expected}
}
$metadata=@();Assert-NdrBuild ($firstBuild|ConvertTo-Json -Depth 16|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','duplicate-input','production-hash','entry-hash','compile-flags'){
    $changed=$firstBuild|ConvertTo-Json -Depth 16|ConvertFrom-Json
    $expected=switch($kind){
        'duplicate-dependency'{$changed.production[1]=$changed.production[0];'Multiqueue production closure set differs'}
        'duplicate-input'{$changed.inputs[1]=$changed.inputs[0];'Multiqueue frozen input set differs'}
        'production-hash'{$changed.production[0].source_sha256='0'*64;$changed.production[0].sha256='0'*64;'Multiqueue recorded raw hash differs'}
        'entry-hash'{$changed.entry_sha256='0'*64;'Multiqueue recorded raw hash differs'}
        'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Multiqueue compile arguments differ'}
    }
    $rejected=$false;try{Assert-NdrBuild $changed}catch{if($_.Exception.Message -cne $expected){throw "Multiqueue metadata rejected for unrelated reason: $_"};$rejected=$true}
    if(!$rejected){throw "Multiqueue metadata accepted $kind"};$metadata+=@{name=$kind;rejected=$true;reason=$expected}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($firstBuild.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null
    $text=$originalEntry;$flags=@($firstBuild.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){
        'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping multiqueue drain paths required'}
        'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping multiqueue drain configuration required'}
        'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'}
        'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'multiqueue drain requires shipping fingerprint'}
    }
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log'
    & cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or !([regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message)))){throw "Multiqueue exact compile guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;arguments=$flags;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
$cases=0;$joins=0;foreach($execution in $script:TestExecutions){$cases+=$execution.metrics.cases;$joins+=$execution.metrics.worker_joins}
if($script:TestExecutions.Count -ne 8 -or $builds.Count -ne 8 -or $cases -ne 256 -or $joins -ne 512){throw 'Multiqueue executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Multiqueue source changed during audit'}
Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;variant='current';source_mode=$(if($BaselineDirectory){'reviewed-baseline'}else{'current-source'});
    baseline_source_tree_sha256=$baselineIdentity;baseline=$baselineRecord;production_changed=$false;production_baseline_verified=[bool]$BaselineDirectory;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;builds=$builds;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;
    cases=$cases;worker_joins=$joins;audit=@{path=$PSCommandPath;sha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash.ToLowerInvariant()};
    builder=@{path=(Join-Path $PSScriptRoot '../build/tun32-multiqueue-drain-tests.ps1');sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot '../build/tun32-multiqueue-drain-tests.ps1')).Hash.ToLowerInvariant()};
    method='Current-only actual BeginDrain/Receive/Return/worker/QUERY with two queues and 32 unique original descriptors: q0 indicates16/returns15 and q1 cancels16 at its private boundary. Both barrier orders reject STOPPED until every queue boundary/credit and global Active permit it; 16 OK plus16 STOPPED completions remain unconsumed until after acknowledgment. Two real SDK worker threads per case select wait entries then exit on StopEvent and join before original Destroy. BeginDrain precedes worker startup; no claim to execute full Control/bank/report/wake or native Free. Hosted DDIs, ready session and full-size NBL layout; 6.20/6.89 capability variables do not represent old-system runtime execution. NDEBUG is required but the frozen host NT_ASSERT maps to live CHECK. Guard mode covers the existing fixture objects; no native NDIS/IRQL, kernel CPU or sustained leak claim.'}|
    ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
