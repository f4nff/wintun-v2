param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/completion-arm32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Completion arm requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
function Require-CaHash([string]$Path,[string]$Hash){if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Completion arm raw hash differs'}}
function Require-CaText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Completion arm complete projection differs'}}
function Assert-CaSet([string[]]$Actual,[string[]]$Expected,[string]$Message){
    $set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}}
    if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}
}
[string[]]$production=@(rg --files api driver common include);if($LASTEXITCODE){throw 'Cannot enumerate completion arm production closure'}
$frozen=[ordered]@{
    'tests/model/native_reclaim_tests.c'='f1b676903eb875cda76863981e0837593b36c51837fceaa9cecdc60a7258c765'
    'tests/model/late_commit_tests.c'='3ae8a974a8d121f47d28ba19bfb88d80f4e7498f70c83a104787fa216e81c17f'
    'tests/model/kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
    'tests/safety/guard_memory.h'='ca169cf1a4b11506dc45572b60d61d8b05cf8a73bf7ba424b04dca228637906b'
}
[string[]]$dependencies=@($production|ForEach-Object {$_.Replace('\','/')})+@($frozen.Keys)+@('wintun.props')
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/completion-arm32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
$baselineIdentity=$null;$hashes=@{};$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory)
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 698 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0' -or $manifest.order -cne 'typed string[] Ordinal'){throw 'Completion arm baseline count/order/version differs'}
    Assert-CaSet $names @($manifest.files.PSObject.Properties.Name) 'Completion arm baseline file set differs'
    $entries=@(foreach($name in $names){Require-CaHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne 'e2f0ffac4ea816c69bd1054e92c6cb1228244808cd73fb4442c6be124e4f5b21' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Completion arm baseline identity differs'}
    Assert-CaSet @($production|ForEach-Object {$_.Replace('\','/')}) @($names|Where-Object {$_ -cmatch '^(api|driver|common|include)/'}) 'Completion arm baseline production closure differs'
    foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
    $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){
        $source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null
        [IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()
    }
    $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
function Assert-CaDependencies {foreach($relative in $dependencies){Require-CaHash (Join-Path $baseline $relative) $hashes[$relative];Require-CaHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}}
Assert-CaDependencies
function Assert-CaBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Completion arm build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path)
    $paths=[ordered]@{path='completion-arm32.exe';entry='entry.c';host='native-host.h';session='session-current.c';queue_entry='queue-entry.h'}
    foreach($name in $paths.Keys){if($build.$name -cne (Join-Path $place $paths[$name])){throw 'Completion arm output binding differs'}}
    Require-CaHash $build.path $build.sha256;Require-CaHash $build.entry $build.entry_sha256;Require-CaHash $build.host $build.host_sha256;Require-CaHash $build.session $build.session_sha256;Require-CaHash $build.queue_entry $build.queue_entry_sha256
    $test=Join-Path $script:RepoRoot 'tests/model/completion_arm32_tests.c';if($build.test -cne $test){throw 'Completion arm test source differs'};Require-CaHash $test $build.test_sha256
    Assert-CaSet @($build.inputs|ForEach-Object {$_.relative}) @($frozen.Keys) 'Completion arm frozen input set differs'
    foreach($inputRecord in $build.inputs){
        $source=Join-Path $script:RepoRoot $inputRecord.relative;$copy=Join-Path $place ('original-'+[IO.Path]::GetFileName($source));$hash=$frozen[$inputRecord.relative]
        if($inputRecord.source -cne $source -or $inputRecord.path -cne $copy -or $inputRecord.source_sha256 -cne $hash -or $inputRecord.sha256 -cne $hash){throw 'Completion arm frozen input binding differs'}
        Require-CaHash $source $hash;Require-CaHash $copy $hash
    }
    Assert-CaSet @($build.parts|ForEach-Object {$_.name}) @('session','queue','open-close') 'Completion arm actual span set differs'
    foreach($part in $build.parts){
        $relative=switch($part.name){'session'{'api/v2/session.c'}'queue'{'api/v2/session_queue.h'}'open-close'{'tests/model/late_commit_tests.c'}}
        $source=Join-Path $script:RepoRoot $relative;$body=[IO.File]::ReadAllText($source);$start=0;$end=$body.Length
        if($part.name -ceq 'open-close'){$start=$body.IndexOf('static VOID OpenPeer(');$end=$body.IndexOf('static VOID RunLate(',$start)}
        if($start -lt 0 -or $end -le $start -or $part.source -cne $source -or $part.offset -ne $start -or $part.length -ne $end-$start -or $part.path -cne (Join-Path $place "$($part.name)-original.h")){throw 'Completion arm actual span binding differs'}
        Require-CaHash $source $part.source_sha256;Require-CaHash $part.path $part.sha256;Require-CaText $part.path $body.Substring($start,$end-$start)
    }
    $queue=$build.parts|Where-Object name -ceq 'queue';$lines=@('#define PbQueueArm ArmObserved','#define WaitForMultipleObjects WaitObserved',('#include "'+$queue.path.Replace('\','/')+'"'),'#undef WaitForMultipleObjects','#undef PbQueueArm')
    Require-CaText $build.queue_entry (($lines -join "`n")+"`n")
    $source=Join-Path $script:RepoRoot 'api/v2/session.c';$body=[IO.File]::ReadAllText($source)
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq 'session_queue.h'){$build.queue_entry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $source) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
    Require-CaText $build.session $body
    $source=Join-Path $script:RepoRoot 'tests/model/native_reclaim_tests.c';$body=[IO.File]::ReadAllText($source);$guard='adapter&&lists&&count==2&&!flags'
    if([regex]::Matches($body,[regex]::Escape($guard)).Count -ne 1){throw 'Completion arm indication guard not unique'};$body=$body.Replace($guard,'adapter&&lists&&count==32&&!flags')
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq '../../api/v2/session.c'){$build.session}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $source) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
    Require-CaText $build.host $body
    Assert-CaSet @($build.production|ForEach-Object {$_.relative}) $production 'Completion arm production closure set differs'
    foreach($p in $build.production){$source=Join-Path $script:RepoRoot $p.relative;$copy=Join-Path $place ('source-'+$p.relative.Replace('\','-').Replace('/','-'))
        if($p.source -cne $source -or $p.path -cne $copy -or $p.source_sha256 -cne $p.sha256){throw 'Completion arm production binding differs'}
        Require-CaHash $source $p.source_sha256;Require-CaHash $copy $p.sha256
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($flags -join "`n") -cne ($build.arguments -join "`n")){throw 'Completion arm compile arguments differ'}
    $open=$build.parts|Where-Object name -ceq 'open-close';$bindings=[ordered]@{PB_NATIVE_RECLAIM_CLEANUP_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_cleanup.h');PB_ARM_HOST=$build.host;PB_ARM_OPEN_CLOSE=$open.path}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"'
    Require-CaText $build.entry (($lines -join "`n")+"`n")
}
function Read-CaMetrics([string[]]$Lines,[int]$Family){
    $text=$Lines -join "`n";$rows=[regex]::Matches($text,'(?m)^COMPLETION_ARM32 ip([46]) protocol(6|17) ndis(20|89) beforeArm([01]) wrap([01]) measured([01]) originals32 returns16/16 polls16/16 armReady0/([01]) sdkWaits([12]) publicJoins1 returnJoins2 callbackJoins(\d+) owners0 repairs0$')
    if($rows.Count -ne 32 -or @($Lines|Where-Object {$_ -like 'COMPLETION_ARM32 *'}).Count -ne 32){throw 'Completion arm case count/format differs'}
    $callbacks=0;$waits=0
    for($i=0;$i -lt 32;++$i){$shape=$i%16;$g=$rows[$i].Groups;$arm=$shape%2;$minor=if([int][Math]::Floor($shape/4)%2){89}else{20};$protocol=if($shape -ge 8){17}else{6};$joined=[int]$g[9].Value
        if([int]$g[1].Value -ne $Family -or [int]$g[2].Value -ne $protocol -or [int]$g[3].Value -ne $minor -or [int]$g[4].Value -ne $arm -or [int]$g[5].Value -ne ([int][Math]::Floor($shape/2)%2) -or [int]$g[6].Value -ne [int]($i -ge 16) -or [int]$g[7].Value -ne $arm -or [int]$g[8].Value -ne (2-$arm) -or $joined -lt 1 -or $joined -gt 32){throw 'Completion arm family/matrix/ownership differs'}
        $callbacks+=$joined;$waits+=[int]$g[8].Value
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: COMPLETION_ARM32 family([46]) cases32 warm16 measured16 publicJoins32 returnJoins64 callbackJoins(\d+) handles(\d+)/(\d+) owners0 repairs0$')
    if($summary.Count -ne 1 -or @($Lines|Where-Object {$_ -like 'PASS:*'}).Count -ne 1 -or [int]$summary[0].Groups[1].Value -ne $Family -or [int]$summary[0].Groups[2].Value -ne $callbacks -or $summary[0].Groups[3].Value -cne $summary[0].Groups[4].Value){throw 'Completion arm summary/resources differ'}
    return @{cases=32;warm=16;measured=16;public_joins=32;return_joins=64;callback_os_thread_joins=$callbacks;sdk_waits=$waits;originals=1024;completions=1024;process_handles_before=[int]$summary[0].Groups[3].Value;process_handles_after=[int]$summary[0].Groups[4].Value}
}
Start-TestRecording $directory 'operations.json'
function Invoke-CaProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start completion arm test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'Completion arm external watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        $row=[ordered]@{program=$build.path;arguments=@();exit_code=$process.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})}
        $script:TestExecutions.Add($row);Write-TestRecording $(if($process.ExitCode){'failed'}else{'running'})
        if($process.ExitCode){throw "Completion arm exit$($process.ExitCode): $log"}
        $row['metrics']=Read-CaMetrics @($output -split '\r?\n') $build.ip_version;Write-Output $row.reported_pass_messages
    }finally{$process.Dispose()}
}
$builds=@();$firstBuild=$null
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $build=Build-CompletionArm32Tests -Directory (Join-Path $directory "ipv$family-$mode-guard$guard") -IpVersion $family -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $family -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Completion arm requested build differs'}
    Assert-CaBuild $build;$builds+=$build;if(!$firstBuild){$firstBuild=$build};Invoke-CaProcess $build
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log;Read-CaMetrics $original 4|Out-Null
foreach($kind in 'wrong-family','missing-row','duplicate-row','wrong-ready'){
    $changed=switch($kind){
        'wrong-family'{@($original|ForEach-Object {$_ -replace '^COMPLETION_ARM32 ip4 ','COMPLETION_ARM32 ip6 '})}
        'missing-row'{@($original|Where-Object {$_ -notmatch '^COMPLETION_ARM32 ip4 protocol6 ndis20 beforeArm0 wrap0 measured0 '})}
        'duplicate-row'{@($original)+@($original|Where-Object {$_ -like 'COMPLETION_ARM32 *'}|Select-Object -First 1)}
        'wrong-ready'{@($original|ForEach-Object {$_ -replace 'armReady0/1 sdkWaits1','armReady0/0 sdkWaits1'})}
    }
    $expected=if($kind -in 'wrong-family','wrong-ready'){'Completion arm family/matrix/ownership differs'}else{'Completion arm case count/format differs'}
    $reason=$null;try{Read-CaMetrics $changed 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason -cne $expected){throw "Completion arm parser wrong rejection: $kind ($reason)"};$parser+=@{name=$kind;rejected=$true;reason=$reason}
}
$metadata=@();Assert-CaBuild ($firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','duplicate-input','part-source-hash','compile-flags'){
    $changed=$firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json
    $expected=switch($kind){
        'duplicate-dependency'{$changed.production[1]=$changed.production[0];'Completion arm production closure set differs'}
        'duplicate-input'{$changed.inputs[1]=$changed.inputs[0];'Completion arm frozen input set differs'}
        'part-source-hash'{$changed.parts[0].source_sha256='0'*64;'Completion arm raw hash differs'}
        'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Completion arm compile arguments differ'}
    }
    $reason=$null;try{Assert-CaBuild $changed}catch{$reason=$_.Exception.Message}
    if($reason -cne $expected){throw "Completion arm metadata wrong rejection: $kind ($reason)"};$metadata+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($firstBuild.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$text=$originalEntry;$flags=@($firstBuild.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){
        'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping completion arm paths required'}
        'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping completion arm configuration required'}
        'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'}
        'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'completion arm requires shipping fingerprint'}
    }
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log'
    & cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or !([regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message)))){throw "Completion arm exact compiler guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;arguments=$flags;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-CaDependencies
$cases=0;$public=0;$returns=0;$callbacks=0;foreach($execution in $script:TestExecutions){$cases+=$execution.metrics.cases;$public+=$execution.metrics.public_joins;$returns+=$execution.metrics.return_joins;$callbacks+=$execution.metrics.callback_os_thread_joins}
if($script:TestExecutions.Count -ne 8 -or $cases -ne 256 -or $public -ne 256 -or $returns -ne 512){throw 'Completion arm executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Completion arm source changed during audit'}
Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;variant='current';source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();
    operations=$script:TestResultFile;builds=$builds;cases=$cases;public_joins=$public;return_joins=$returns;callback_os_thread_joins=$callbacks;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;
    reviewed_dependencies=$dependencies.Count;production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests/model/completion_arm32_tests.c','tools/build/completion-arm32-tests.ps1','tools/audit/verify-completion-arm32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Current-only coverage, no production defect or before variant. Complete native_reclaim host retains original allocation/region/MDL/event/module/private-pool/SDK callback-thread ledger, with the unique indication guard2-to32 change and finite include projection. Original OpenPeer/FinishPeer span is unchanged. One actual shipping DLL queue owner reserves/commits32 unique legal1500B packets and calls Wait/Poll16 twice. Actual SYS Receive32 and two actual Return16 windows publish normal completions; no fake FULL, counter/refcount repair or generation reuse. Second16 publishes before the actual RX_COMPLETE arm (ready1, no second SDKwait) or after arm observes empty (ready0, real auto-reset UserEvent selected SDKwait returns). SDK entry observation is not OS park proof. Auto UserEvent is never reset by the test; 6.20/6.89 capability variables control actual NBL field access, not old-kernel memory layouts/runtime. Payload oracle reads NDIS-owned partial MDLs before return; no released user loan is read afterward. All public owner/Return threads join before normal Close, and original Free joins idle-only native workers; dynamically captured callback OS handles join after private-pool closure. Hosted ready registration/indication/IOCTL/NDIS/IRQL, EXE module lifetime (no unloadable fixture DLL), and original NT_ASSERT-to-CHECK remain explicit. No actual workerloop/budget4, driver/device/registry, native CPU/throughput or sustained leak claim. Default current dependency snapshot is self-contained; optional698-file typed-Ordinal e2f0 baseline is strictly authenticated and all production bytes preserved.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
