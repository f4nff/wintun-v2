param([string]$BaselineDirectory='', [string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/tun32-worker-pause-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Worker pause audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baselineIdentity=$null
if($BaselineDirectory){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 678 -or @($names|Select-Object -Unique).Count -ne 678 -or ($names -join "`n") -cne ($sorted -join "`n") -or @($manifest.files.PSObject.Properties).Count -ne 678){throw 'Worker baseline file set/order differs'}
    $entries=@(foreach($name in $names){$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $name)).Hash.ToLowerInvariant();if($manifest.files.$name -cne $hash){throw "Worker baseline raw differs: $name"};$name+':'+$hash})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne 'dc6f9aef0663074ccdccf4413f0d1f6cdeae993b0daac5fa96900ecf54f598c1' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Worker baseline identity differs'}
    foreach($name in @(rg --files driver common include)){
        if((Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $name)).Hash.ToLowerInvariant() -cne $manifest.files.($name.Replace('\','/'))){throw "Unreviewed worker production change: $name"}
    }
}
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/tun32-worker-pause/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$utf8=[Text.UTF8Encoding]::new($false)
function Require-WpText([string]$Path,[string]$Text){
    if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw "Worker complete projection differs: $Path"}
}
function Require-WpHash([string]$Path,[string]$Hash){
    if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Worker recorded raw hash differs'}
}
function Assert-WpBuild($build){
    $e=$build.extractions
    $original=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$hash=(Get-FileHash $original).Hash.ToLowerInvariant()
    if($hash -cne 'c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3' -or (Get-FileHash $e.host_original).Hash.ToLowerInvariant() -cne $hash){throw 'Worker complete NBL host differs'}
    if($e.host_source -cne $original -or $e.host_source_sha256 -cne $hash -or $e.host_original_sha256 -cne $hash){throw 'Worker original host metadata differs'}
    Require-WpHash $e.host $e.host_sha256
    $body=[IO.File]::ReadAllText($original);$anchor='        EX_RUNDOWN_REF ReturnRundown;';$fields='        KSPIN_LOCK IndicationLock; KEVENT IndicationIdle[2]; ULONG IndicationBank; LONG IndicationCount[2];'
    if([regex]::Matches($body,[regex]::Escape($anchor)).Count -ne 1){throw 'Worker host anchor not unique'}
    $body=$body.Replace($anchor,$anchor+"`r`n"+$fields)
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $original) $m.Groups[1].Value)).Replace('\','/')+'"'})
    Require-WpText $e.host $body
    if(@($e.parts).Count -ne 5 -or $e.variant -cne 'current'){throw 'Worker requires five original current parts'}
    $expected=[ordered]@{
        outer=@('driver/wintun.c','static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;','#define TUN_FORCE_UNREGISTRATION')
        tunpause=@('driver/wintun.c','static MINIPORT_PAUSE TunPause;','static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;')
        receive=@('driver/v2/session_receive.h','static BOOLEAN PbV2ReceiveBatch(','[end]')
        gate=@('driver/v2/indication_gate.h','[whole]','[end]')
        pause=@('driver/v2/session_pause.h','[whole]','[end]')
    }
    $seen=[Collections.Generic.HashSet[string]]::new()
    foreach($part in $e.parts){
        if(!$seen.Add($part.name) -or !$expected.Contains($part.name)){throw 'Worker duplicate/unknown extraction'}
        $spec=$expected[$part.name];$source=Join-Path $script:RepoRoot $spec[0];$body=[IO.File]::ReadAllText($source)
        $start=if($spec[1] -eq '[whole]'){0}else{$body.IndexOf($spec[1])};$end=if($spec[2] -eq '[end]'){$body.Length}else{$body.IndexOf($spec[2],$start)}
        if($start -lt 0 -or $end -le $start -or $part.source -cne $source -or $part.offset -ne $start -or $part.length -ne $end-$start){throw 'Worker actual source span differs'}
        Require-WpHash $source $part.source_sha256;Require-WpHash $part.path $part.sha256
        Require-WpText $part.path $body.Substring($start,$end-$start)
    }
    $body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver/wintun.c'));$start=$body.IndexOf('static MINIPORT_RESTART TunRestart;');$end=$body.IndexOf('static MINIPORT_PAUSE TunPause;',$start)
    if($build.restart.offset -ne $start -or $build.restart.length -ne $end-$start -or $build.restart.source -cne (Join-Path $script:RepoRoot 'driver/wintun.c') -or $build.restart.start_marker -cne 'static MINIPORT_RESTART TunRestart;' -or $build.restart.end_marker -cne 'static MINIPORT_PAUSE TunPause;'){throw 'Worker Restart span differs'}
    Require-WpHash $build.restart.source $build.restart.source_sha256;Require-WpHash $build.restart.path $build.restart.sha256
    Require-WpText $build.restart.path $body.Substring($start,$end-$start)
    $dependencies=@(rg --files driver common include)
    if(@($build.production).Count -ne $dependencies.Count){throw 'Worker production closure count differs'}
    $dependencySet=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($p in $build.production){
        if(!$dependencySet.Add($p.relative)){throw 'Worker production closure set differs'}
        $source=Join-Path $script:RepoRoot $p.relative
        if($p.source -cne $source -or (Get-FileHash $source).Hash.ToLowerInvariant() -cne $p.source_sha256 -or (Get-FileHash $p.path).Hash.ToLowerInvariant() -cne $p.source_sha256){throw 'Worker raw production binding differs'}
    }
    if(!$dependencySet.SetEquals([string[]]$dependencies)){throw 'Worker production closure set differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$([IO.Path]::GetDirectoryName($build.path))\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($flags -join "`n") -cne ($build.arguments -join "`n")){throw 'Worker compile arguments differ'}
    $entry=@();if($build.guarded){$entry+='#define PB_GUARD_TESTS 1'}
    $entry+='#define PB_TEST_IP_VERSION '+$build.ip_version;$entry+='#define PB_WP_HOST "'+$e.host.Replace('\','/')+'"';$entry+='#define PB_WP_RESTART "'+$build.restart.path.Replace('\','/')+'"'
    $entry+='#define PB_REFERENCE_SOURCE "'+(Join-Path $script:RepoRoot 'driver/v2/session_references.h').Replace('\','/')+'"';$entry+='#define PB_NBL_SOURCE "'+(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h').Replace('\','/')+'"'
    foreach($part in $e.parts){$entry+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'}
    $entry+='#include "'+(Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c').Replace('\','/')+'"'
    Require-WpText $build.entry (($entry -join "`n")+"`n")
}
function Read-WpMetrics([string[]]$Lines,[int]$IpVersion){
    $text=$Lines -join "`n";$pattern='(?m)^WORKER_PAUSE32 ip(?<ip>[46]) protocol(?<protocol>6|17) ndis(?<minor>20|30|89) manual(?<manual>[01]) wrap(?<wrap>[01]) measured(?<measured>[01]) old32 new32 pausedAdmission0 kernelWake1 restartWake2 oldPins2 joins6 owners0 repairs0$'
    $rows=[regex]::Matches($text,$pattern);if($rows.Count -ne 32 -or @($Lines|Where-Object {$_ -like 'WORKER_PAUSE32 *'}).Count -ne 32){throw 'Worker case count/format differs'}
    $actual=[Collections.Generic.HashSet[string]]::new()
    foreach($row in $rows){$g=$row.Groups;if([int]$g['ip'].Value -ne $IpVersion -or !$actual.Add((@('protocol','minor','manual','wrap','measured')|ForEach-Object {$g[$_].Value}) -join ':')){throw 'Worker family/duplicate case differs'}}
    foreach($shape in @(@(6,20),@(17,20),@(6,30),@(17,89))){foreach($manual in 0,1){foreach($wrap in 0,1){foreach($measured in 0,1){if(!$actual.Contains("$($shape[0]):$($shape[1]):${manual}:${wrap}:$measured")){throw 'Worker case matrix differs'}}}}}
    $summary=[regex]::Matches($text,'(?m)^PASS: family(?<ip>[46]) actual worker Pause32/Restart32 cases32 warm16 measured16 joins192 workerjoins32 handles(?<before>\d+)/(?<after>\d+)$')
    if($summary.Count -ne 1 -or [int]$summary[0].Groups['ip'].Value -ne $IpVersion -or $summary[0].Groups['before'].Value -cne $summary[0].Groups['after'].Value){throw 'Worker summary/resources differ'}
    return @{cases=$rows.Count;warm=16;measured=16;joins=192;worker_joins=32;process_handles_before=[int]$summary[0].Groups['before'].Value;process_handles_after=[int]$summary[0].Groups['after'].Value}
}
Start-TestRecording $directory 'operations.json'
$builds=@();$first=$null
foreach($ip in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $path=Join-Path $directory "ipv$ip-$mode-guard$guard";$build=Build-Tun32WorkerPauseTests -Directory $path -IpVersion $ip -Mode $mode -Guarded:$guard
    Assert-WpBuild $build;$builds+=$build;if(!$first){$first=$build}
    Invoke-TestTool $build.path @();$row=$script:TestExecutions[$script:TestExecutions.Count-1]
    $row['ip_version']=$ip;$row['mode']=$mode;$row['guarded']=$guard;$row['metrics']=Read-WpMetrics (Get-Content $row.log) $ip
}}}
$parser=@();$original=Get-Content $script:TestExecutions[0].log
foreach($kind in 'wrong-family','missing-row','duplicate-row'){
    $changed=switch($kind){'wrong-family'{@($original|ForEach-Object {$_ -replace 'ip4 protocol','ip6 protocol'})}'missing-row'{@($original|Where-Object {$_ -notmatch 'protocol6 ndis20 manual0 wrap0 measured0'})}'duplicate-row'{@($original)+@($original|Where-Object {$_ -match '^WORKER_PAUSE32 '}|Select-Object -First 1)}}
    $rejected=$false;try{Read-WpMetrics $changed 4|Out-Null}catch{$rejected=$true};if(!$rejected){throw "Worker parser accepted $kind"};$parser+=@{name=$kind;rejected=$true}
}
$metadataChecks=@()
Assert-WpBuild ($first|ConvertTo-Json -Depth 16|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','part-source-hash','restart-source','restart-marker'){
    $changed=$first|ConvertTo-Json -Depth 16|ConvertFrom-Json
    switch($kind){
        'duplicate-dependency'{$changed.production[1]=$changed.production[0]}
        'part-source-hash'{$changed.extractions.parts[0].source_sha256='0'*64}
        'restart-source'{$changed.restart.source=Join-Path $script:RepoRoot 'common/v2_core.c'}
        'restart-marker'{$changed.restart.start_marker='unreviewed marker'}
    }
    $expected=if($kind -eq 'duplicate-dependency'){'Worker production closure set differs'}elseif($kind -eq 'part-source-hash'){'Worker recorded raw hash differs'}else{'Worker Restart span differs'}
    $rejected=$false;try{Assert-WpBuild $changed}catch{if($_.Exception.Message -cne $expected){throw "Worker metadata rejected for an unrelated reason: $_"};$rejected=$true}
    if(!$rejected){throw "Worker metadata accepted $kind"};$metadataChecks+=@{name=$kind;rejected=$true}
}
$contracts=@();$baseText=[IO.File]::ReadAllText($first.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null
    $text=$baseText;$flags=@($first.arguments|Where-Object {$_ -notlike '/Fo*'});$message=switch($kind){
        'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping worker paths required'}
        'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping worker pause configuration required'}
        'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'}
        'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+'/DPB_SOURCE_FINGERPRINT=0';'worker pause requires shipping fingerprint'}
    }
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log'
    & cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or !([IO.File]::ReadAllText($log).Contains($message))){throw "Worker compiler guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Worker source changed during audit'}
Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($BaselineDirectory){'reviewed-baseline'}else{'current-source'});baseline_source_tree_sha256=$baselineIdentity;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;builds=$builds;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadataChecks;
    method='Actual worker consumes queue notifications while paused. Only actual Restart RunningEvent produces SDK wait index2; old callbacks retain two rundown tails across generation2 reuse, then cannot change new Active or coalesce buffers. Actual Unregister/Free joins worker; all six SDK thread objects per case join, ledgers and process handles converge. Hosted DDIs and core peer only; no DLL lifecycle, native NDIS/IRQL, hardware performance or sustained leak claim.'}|
    ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
