param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/control-batch32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Control batch requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
function Require-CbHash([string]$Path,[string]$Hash){if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Control batch raw hash differs'}}
function Require-CbText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Control batch complete projection differs'}}
function Assert-CbSet([string[]]$Actual,[string[]]$Expected,[string]$Message){$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}};if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}}
[string[]]$production=@(rg --files driver common include);if($LASTEXITCODE){throw 'Cannot enumerate control production closure'}
[string[]]$dependencies=@($production|ForEach-Object {$_.Replace('\','/')})+@('wintun.props','tests/model/tun32_worker_pause_tests.c','tests/model/kernel_nbl_fixture.h','tests/model/control_reply_tests.c','tests/safety/guard_memory.h','tools/build/indication-pause-tests.ps1')
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/control-batch32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineIdentity=$null;$hashes=@{}
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 711 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0' -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal'){throw 'Control batch baseline count/order/version differs'}
    Assert-CbSet $names @($manifest.files.PSObject.Properties.Name) 'Control batch baseline file set differs'
    $entries=@(foreach($name in $names){Require-CbHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne '46866e17acd228547b41b890a6a0708e2568d527cbe8bcea618349cf6780b6d6' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Control batch baseline identity differs'}
    Assert-CbSet @($production|ForEach-Object {$_.Replace('\','/')}) @($names|Where-Object {$_ -cmatch '^(driver|common|include)/'}) 'Control batch baseline production set differs'
    foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
    $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()}
    $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
function Assert-CbDependencies {foreach($relative in $dependencies){Require-CbHash (Join-Path $baseline $relative) $hashes[$relative];Require-CbHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}}
Assert-CbDependencies
function Assert-CbBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Control batch build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path);$test=Join-Path $script:RepoRoot 'tests/model/control_batch32_tests.c'
    if($build.path -cne (Join-Path $place 'control-batch32.exe') -or $build.entry -cne (Join-Path $place 'entry.c') -or $build.test -cne $test){throw 'Control batch output/test binding differs'}
    Require-CbHash $build.path $build.sha256;Require-CbHash $build.entry $build.entry_sha256;Require-CbHash $test $build.test_sha256
    $original=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$hash='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3';$e=$build.extractions
    if($e.host_source -cne $original -or $e.host_source_sha256 -cne $hash -or $e.host_original_sha256 -cne $hash){throw 'Control batch original kernel metadata differs'}
    Require-CbHash $original $hash;Require-CbHash $e.host_original $hash;Require-CbHash $e.host $e.host_sha256
    $body=[IO.File]::ReadAllText($original);$marker='        EX_RUNDOWN_REF ReturnRundown;';$fields='        KSPIN_LOCK IndicationLock; KEVENT IndicationIdle[2]; ULONG IndicationBank; LONG IndicationCount[2];'
    if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Control batch bank fields anchor not unique'};$body=$body.Replace($marker,$marker+"`r`n"+$fields)
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $original) $m.Groups[1].Value)).Replace('\','/')+'"'})
    Require-CbText $e.host $body
    $expected=[ordered]@{outer=@('driver/wintun.c','static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;','#define TUN_FORCE_UNREGISTRATION');tunpause=@('driver/wintun.c','static MINIPORT_PAUSE TunPause;','static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;');receive=@('driver/v2/session_receive.h','static BOOLEAN PbV2ReceiveBatch(','[end]');gate=@('driver/v2/indication_gate.h','[whole]','[end]');pause=@('driver/v2/session_pause.h','[whole]','[end]')}
    if($e.variant -cne 'current'){throw 'Control batch requires current production bodies'};Assert-CbSet @($e.parts|ForEach-Object {$_.name}) @($expected.Keys) 'Control batch actual part set differs'
    foreach($part in $e.parts){$spec=$expected[$part.name];$source=Join-Path $script:RepoRoot $spec[0];$body=[IO.File]::ReadAllText($source);$start=if($spec[1] -ceq '[whole]'){0}else{$body.IndexOf($spec[1])};$end=if($spec[2] -ceq '[end]'){$body.Length}else{$body.IndexOf($spec[2],$start)}
        if($start -lt 0 -or $end -le $start -or $part.source -cne $source -or $part.offset -ne $start -or $part.length -ne $end-$start){throw 'Control batch actual span binding differs'}
        Require-CbHash $source $part.source_sha256;Require-CbHash $part.path $part.sha256;Require-CbText $part.path $body.Substring($start,$end-$start)
    }
    $specs=[ordered]@{restart=@('driver/wintun.c','static MINIPORT_RESTART TunRestart;','static MINIPORT_PAUSE TunPause;');reply=@('driver/v2/session.h','static NDIS_STATUS PbV2ControlReply(','#include "session_describe.h"');frame=@('tests/model/control_reply_tests.c','static VOID Put16(','static VOID RunControl(')}
    Assert-CbSet @($build.spans|ForEach-Object {$_.name}) @($specs.Keys) 'Control batch extra span set differs'
    foreach($part in $build.spans){$spec=$specs[$part.name];$source=Join-Path $script:RepoRoot $spec[0];$body=[IO.File]::ReadAllText($source);foreach($marker in $spec[1..2]){if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Control batch markers not unique'}};$start=$body.IndexOf($spec[1]);$end=$body.IndexOf($spec[2],$start)
        if($part.source -cne $source -or $part.path -cne (Join-Path $place "$($part.name)-original.h") -or $part.snapshot -cne (Join-Path $place "$($part.name)-source-original") -or $part.offset -ne $start -or $part.length -ne $end-$start -or $part.start_marker -cne $spec[1] -or $part.end_marker -cne $spec[2] -or $part.source_sha256 -cne $part.snapshot_sha256){throw 'Control batch extra span binding differs'}
        if($part.name -ceq 'frame' -and $part.source_sha256 -cne '70492226145283c68ab9036053a65bef96827a2b49d850da41087ba0ba013d7e'){throw 'Control batch frozen frame source differs'}
        Require-CbHash $source $part.source_sha256;Require-CbHash $part.snapshot $part.snapshot_sha256;Require-CbHash $part.path $part.sha256;Require-CbText $part.path $body.Substring($start,$end-$start)
    }
    Assert-CbSet @($build.types|ForEach-Object {$_.name}) @('nblchecksum','nbllso','nbluso') 'Control batch WDK type set differs';$typeBodies=@()
    foreach($name in 'nblchecksum','nbllso','nbluso'){$part=@($build.types|Where-Object name -ceq $name)[0];$source=Join-Path $script:WdkRoot "Include/$script:KitVersion/km/ndis/$name.h";$body=[IO.File]::ReadAllText($source);$matches=[regex]::Matches($body,'(?s)typedef struct _NDIS_.*?\n\} NDIS_[^;]+;');if($matches.Count -ne 1){throw 'Control batch WDK type not unique'};$match=$matches[0];$typeBodies+=$match.Value
        if($part.source -cne $source -or $part.snapshot -cne (Join-Path $place "wdk-$name-original.h") -or $part.source_sha256 -cne $part.snapshot_sha256 -or $part.offset -ne $match.Index -or $part.length -ne $match.Length -or $part.span_sha256 -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes($match.Value))).ToLowerInvariant()){throw 'Control batch WDK type binding differs'};Require-CbHash $source $part.source_sha256;Require-CbHash $part.snapshot $part.snapshot_sha256
    }
    if($build.type_path -cne (Join-Path $place 'types.h')){throw 'Control batch aggregate type path differs'};Require-CbHash $build.type_path $build.type_sha256;Require-CbText $build.type_path ($typeBodies -join "`n")
    $hostSource=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$hash='a0a20ebbe48fdcfbf64a0f210dcafcc107e0e83cf060c4727df6301653808771'
    if($build.host_source -cne $hostSource -or $build.host_source_sha256 -cne $hash -or $build.host_original_sha256 -cne $hash -or $build.host_original -cne (Join-Path $place 'worker-host-original.c') -or $build.host -cne (Join-Path $place 'worker-host.h') -or $build.cleanup_observer -cne (Join-Path $place 'cleanup-observe.h')){throw 'Control batch frozen worker host binding differs'}
    Require-CbHash $hostSource $hash;Require-CbHash $build.host_original $hash;Require-CbHash $build.host $build.host_sha256;Require-CbHash $build.cleanup_observer $build.cleanup_observer_sha256
    $lines=@('static VOID ObservedBatchRundown(EX_RUNDOWN_REF *);','#define ExWaitForRundownProtectionRelease ObservedBatchRundown',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_cleanup.h').Replace('\','/')+'"'),'#undef ExWaitForRundownProtectionRelease');Require-CbText $build.cleanup_observer (($lines -join "`n")+"`n")
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq '../../driver/v2/session_cleanup.h'){$build.cleanup_observer}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'});Require-CbText $build.host $body
    $helper=Join-Path $script:RepoRoot 'tools/build/indication-pause-tests.ps1';if([IO.Path]::GetFullPath($build.helper.path) -cne [IO.Path]::GetFullPath($helper)){throw 'Control batch helper binding differs'};Require-CbHash $helper $build.helper.sha256
    Assert-CbSet @($build.production|ForEach-Object {$_.relative}) $production 'Control batch production closure set differs'
    foreach($p in $build.production){$source=Join-Path $script:RepoRoot $p.relative;$copy=Join-Path $place ('source-'+$p.relative.Replace('\','-').Replace('/','-'));if($p.source -cne $source -or $p.path -cne $copy -or $p.source_sha256 -cne $p.sha256){throw 'Control batch raw production binding differs'};Require-CbHash $source $p.source_sha256;Require-CbHash $copy $p.sha256}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($flags -join "`n") -cne ($build.arguments -join "`n")){throw 'Control batch compile arguments differ'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version;$bindings=[ordered]@{PB_BATCH_HOST=$build.host;PB_BATCH_TYPES=$build.type_path;PB_BATCH_REPLY=($build.spans|Where-Object name -ceq 'reply').path;PB_BATCH_FRAME=($build.spans|Where-Object name -ceq 'frame').path;PB_WP_HOST=$e.host;PB_WP_RESTART=($build.spans|Where-Object name -ceq 'restart').path;PB_REFERENCE_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_references.h');PB_NBL_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};foreach($part in $e.parts){$lines+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"';Require-CbText $build.entry (($lines -join "`n")+"`n")
}
function Read-CbMetrics([string[]]$Lines,[int]$Family){
    $text=$Lines -join "`n";$rows=[regex]::Matches($text,'(?m)^CONTROL_BATCH32 ip([46]) proto(6|17) ndis(20|89) mode([012]) wrap([01]) measured([01]) outer32 control1 nested32 published(48|64) push([23]) payload(72000|96000) prefix(2832|3776) controlBytes(60|86) replyCopies1 replyBytes(60|86) joins([45]) owners0 repairs0$')
    if($rows.Count -ne 48 -or @($Lines|Where-Object {$_ -like 'CONTROL_BATCH32 *'}).Count -ne 48){throw 'Control batch case count/format differs'}
    for($i=0;$i -lt 48;++$i){$mode=$i%3;$shape=$i%24;$protocol=if($shape -lt 12){6}else{17};$minor=if(([int][Math]::Floor($shape/6)%2) -eq 0){20}else{89};$published=if($mode){48}else{64};$g=$rows[$i].Groups
        if([int]$g[1].Value -ne $Family -or [int]$g[2].Value -ne $protocol -or [int]$g[3].Value -ne $minor -or [int]$g[4].Value -ne $mode -or [int]$g[5].Value -ne ([int][Math]::Floor($shape/3)%2) -or [int]$g[6].Value -ne [int]($i -ge 24) -or [int]$g[7].Value -ne $published -or [int]$g[8].Value -ne $(if($mode){2}else{3}) -or [int]$g[9].Value -ne 1500*$published -or [int]$g[10].Value -ne 59*$published -or [int]$g[11].Value -ne $(if($Family -eq 4){60}else{86}) -or [int]$g[12].Value -ne $(if($Family -eq 4){60}else{86}) -or [int]$g[13].Value -ne $(if($mode -eq 2){5}else{4})){throw 'Control batch family/matrix differs'}
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: CONTROL_BATCH32 family([46]) cases48 warm24 measured24 joins208 workerJoins48 handles(\d+)/(\d+) owners0 repairs0$')
    if($summary.Count -ne 1 -or @($Lines|Where-Object {$_ -like 'PASS:*'}).Count -ne 1 -or [int]$summary[0].Groups[1].Value -ne $Family -or $summary[0].Groups[2].Value -cne $summary[0].Groups[3].Value){throw 'Control batch summary/resources differ'}
    return @{cases=48;warm=24;measured=24;joined_threads=208;idle_native_worker_joins=48;outer_data_originals=48*32;control_originals=48;nested_originals=48*32;published_descriptors=16*64+32*48;process_handles_before=[int]$summary[0].Groups[2].Value;process_handles_after=[int]$summary[0].Groups[3].Value}
}
Start-TestRecording $directory 'operations.json'
function Invoke-CbProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$p=[Diagnostics.Process]::new();$p.StartInfo=$info
    try{if(!$p.Start()){throw 'Cannot start control batch test'};$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(60000)){$p.Kill($true);$p.WaitForExit();throw 'Control batch external watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        $row=[ordered]@{program=$build.path;arguments=@();exit_code=$p.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})};$script:TestExecutions.Add($row);Write-TestRecording $(if($p.ExitCode){'failed'}else{'running'})
        if($p.ExitCode){throw "Control batch exit$($p.ExitCode): $log"};$row['metrics']=Read-CbMetrics @($output -split '\r?\n') $build.ip_version;Write-Output $row.reported_pass_messages
    }finally{$p.Dispose()}
}
$builds=@();$firstBuild=$null
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $build=Build-ControlBatch32Tests -Directory (Join-Path $directory "ipv$family-$mode-guard$guard") -IpVersion $family -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $family -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Control batch requested build differs'};Assert-CbBuild $build;$builds+=$build;if(!$firstBuild){$firstBuild=$build};Invoke-CbProcess $build
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log;Read-CbMetrics $original 4|Out-Null
foreach($kind in 'wrong-family','missing-row','duplicate-row','wrong-push'){
    $changed=switch($kind){'wrong-family'{@($original|ForEach-Object {$_ -replace '^CONTROL_BATCH32 ip4 ','CONTROL_BATCH32 ip6 '})}'missing-row'{@($original|Where-Object {$_ -notmatch '^CONTROL_BATCH32 ip4 proto6 ndis20 mode0 wrap0 measured0 '})}'duplicate-row'{@($original)+@($original|Where-Object {$_ -like 'CONTROL_BATCH32 *'}|Select-Object -First 1)}'wrong-push'{@($original|ForEach-Object {$_ -replace 'published64 push3','published64 push2'})}}
    $expected=if($kind -in 'wrong-family','wrong-push'){'Control batch family/matrix differs'}else{'Control batch case count/format differs'};$reason=$null;try{Read-CbMetrics $changed 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason -cne $expected){throw "Control batch parser wrong rejection: $kind ($reason)"};$parser+=@{name=$kind;rejected=$true;reason=$reason}
}
$metadata=@();Assert-CbBuild ($firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','duplicate-part','part-source-hash','host-source','compile-flags'){
    $changed=$firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json
    $expected=switch($kind){'duplicate-dependency'{$changed.production[1]=$changed.production[0];'Control batch production closure set differs'}'duplicate-part'{$changed.extractions.parts[1]=$changed.extractions.parts[0];'Control batch actual part set differs'}'part-source-hash'{$changed.extractions.parts[0].source_sha256='0'*64;'Control batch raw hash differs'}'host-source'{$changed.host_source=Join-Path $script:RepoRoot 'tests/model/return_coalesce_tests.c';'Control batch frozen worker host binding differs'}'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Control batch compile arguments differ'}}
    $reason=$null;try{Assert-CbBuild $changed}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Control batch metadata wrong rejection: $kind ($reason)"};$metadata+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($firstBuild.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$text=$originalEntry;$flags=@($firstBuild.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping control batch paths required'}'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping control batch configuration required'}'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'}'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'control batch requires shipping fingerprint'}}
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log';& cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or !([regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message)))){throw "Control batch exact compiler guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;arguments=$flags;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-CbDependencies
$cases=0;$joins=0;$workers=0;foreach($e in $script:TestExecutions){$cases+=$e.metrics.cases;$joins+=$e.metrics.joined_threads;$workers+=$e.metrics.idle_native_worker_joins}
if($script:TestExecutions.Count -ne 8 -or $cases -ne 384 -or $joins -ne 1664 -or $workers -ne 384){throw 'Control batch executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Control batch source changed during audit'};Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;variant='current';source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();operations=$script:TestResultFile;builds=$builds;cases=$cases;joined_threads=$joins;idle_native_worker_joins=$workers;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests/model/control_batch32_tests.c','tools/build/control-batch32-tests.ps1','tools/audit/verify-control-batch32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage only, no production defect/before or repair. Independent IPv4/IPv6 TCP/UDP runs use ready registered session,64 TX slots and128-entry rings. Each original call contains32 ordinary1500-byte L3 packets plus one ARP/NS between prefix16/suffix16. Actual Read/Describe/Send publishes prefix16 before actual ControlReply RESOURCES upcall; that upcall proves no Transition/Tx lock held and synchronously sends32 distinct original NBLs in a separate call-local window. Another SDK thread optionally runs actual Pause, or actual BeginDrain+bank rotate/wait (not the full IOCTL), while the reply callback is selected and blocked. Controller releases that callback without waiting synchronously for control operations from it. Current normal case publishes16+32+16; Pause/Drain cases publish16+32 and reject suffix16 at the real second admission, with original status and zero suffix packet reads. Real callouts raise modeled IRQL and use SDK callback boundaries; the RESOURCES reply is freed synchronously without ReturnNBL. Complete frees every original NBL/MDL/input before returning, checking input packet bytes unchanged. Exact Push vectors, separate59-byte prefix and full1500-byte snapshot reads plus one60/86-byte control reply construction copy, metadata runtime gates, descriptor order/payload/generation and stats are checked. Independent core peer consumes/recycles every accepted token and crosses unsigned cursors naturally. Actual Pause joins after Active0, but original send still holds its Returns tail; actual Unregister/Free joins the idle native worker and waits on the real rundown lock before resource release. Every SDK sender/control/cleanup/idle thread joins, all pools/MDLs/NBLs/regions/objects/events/core slots converge and process handles match after a full checked warm matrix. Full frozen worker/kernel/control-frame hosts are raw-bound with only absolute includes, existing indication-bank fields and transparent Free observer; checksum fixture type is renamed only in the new wrapper before actual WDK transmit types. Plain6.20/6.89 capability variable is a hosted metadata boundary, not old-kernel structure layout/runtime proof. NDEBUG is required and original NT_ASSERT remains live CHECK. Guard pages cover host pool/input/MDL/NBL/region allocations, not stack/SDK/WDK layouts. No offload/GSO or allocation fault, fakeFULL, physical driver, native NDIS/IRQL/registration/workerloop/Halt/DLL unload or CPU throughput claim.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
