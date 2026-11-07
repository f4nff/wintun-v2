param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/coalesce-dual-tail32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Dual coalesce requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
function Require-DtHash([string]$Path,[string]$Hash){if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Dual coalesce raw hash differs'}}
function Require-DtText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Dual coalesce complete projection differs'}}
function Assert-DtSet([string[]]$Actual,[string[]]$Expected,[string]$Message){$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}};if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}}
[string[]]$production=@(rg --files driver common include);if($LASTEXITCODE){throw 'Cannot enumerate dual production closure'}
[string[]]$dependencies=@($production|ForEach-Object {$_.Replace('\','/')})+@('wintun.props','tests/model/tun32_worker_pause_tests.c','tests/model/kernel_nbl_fixture.h','tests/safety/guard_memory.h','tools/build/indication-pause-tests.ps1')
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/coalesce-dual-tail32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineIdentity=$null;$hashes=@{}
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 703 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0' -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal'){throw 'Dual coalesce baseline count/order/version differs'}
    Assert-DtSet $names @($manifest.files.PSObject.Properties.Name) 'Dual coalesce baseline file set differs'
    $entries=@(foreach($name in $names){Require-DtHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne '9631f540027d403d704c7288d06d8472eb9631c3f5ac5299e9f9845fb13e934b' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Dual coalesce baseline identity differs'}
    Assert-DtSet @($production|ForEach-Object {$_.Replace('\','/')}) @($names|Where-Object {$_ -cmatch '^(driver|common|include)/'}) 'Dual coalesce baseline production set differs'
    foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
    $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()}
    $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
function Assert-DtDependencies {foreach($relative in $dependencies){Require-DtHash (Join-Path $baseline $relative) $hashes[$relative];Require-DtHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}}
Assert-DtDependencies
function Assert-DtBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Dual coalesce build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path);$test=Join-Path $script:RepoRoot 'tests/model/coalesce_dual_tail32_tests.c'
    if($build.path -cne (Join-Path $place 'coalesce-dual-tail32.exe') -or $build.entry -cne (Join-Path $place 'entry.c') -or $build.test -cne $test){throw 'Dual coalesce output/test binding differs'}
    Require-DtHash $build.path $build.sha256;Require-DtHash $build.entry $build.entry_sha256;Require-DtHash $test $build.test_sha256
    $original=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$hash='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3';$e=$build.extractions
    if($e.host_source -cne $original -or $e.host_source_sha256 -cne $hash -or $e.host_original_sha256 -cne $hash){throw 'Dual coalesce original kernel metadata differs'}
    Require-DtHash $original $hash;Require-DtHash $e.host_original $hash;Require-DtHash $e.host $e.host_sha256
    $body=[IO.File]::ReadAllText($original);$marker='        EX_RUNDOWN_REF ReturnRundown;';$fields='        KSPIN_LOCK IndicationLock; KEVENT IndicationIdle[2]; ULONG IndicationBank; LONG IndicationCount[2];'
    if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Dual coalesce bank fields anchor not unique'};$body=$body.Replace($marker,$marker+"`r`n"+$fields)
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $original) $m.Groups[1].Value)).Replace('\','/')+'"'})
    Require-DtText $e.host $body
    $expected=[ordered]@{outer=@('driver/wintun.c','static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;','#define TUN_FORCE_UNREGISTRATION');tunpause=@('driver/wintun.c','static MINIPORT_PAUSE TunPause;','static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;');receive=@('driver/v2/session_receive.h','static BOOLEAN PbV2ReceiveBatch(','[end]');gate=@('driver/v2/indication_gate.h','[whole]','[end]');pause=@('driver/v2/session_pause.h','[whole]','[end]')}
    if($e.variant -cne 'current'){throw 'Dual coalesce requires current production bodies'};Assert-DtSet @($e.parts|ForEach-Object {$_.name}) @($expected.Keys) 'Dual coalesce actual part set differs'
    foreach($part in $e.parts){$spec=$expected[$part.name];$source=Join-Path $script:RepoRoot $spec[0];$body=[IO.File]::ReadAllText($source);$start=if($spec[1] -ceq '[whole]'){0}else{$body.IndexOf($spec[1])};$end=if($spec[2] -ceq '[end]'){$body.Length}else{$body.IndexOf($spec[2],$start)}
        if($start -lt 0 -or $end -le $start -or $part.source -cne $source -or $part.offset -ne $start -or $part.length -ne $end-$start){throw 'Dual coalesce actual span binding differs'}
        Require-DtHash $source $part.source_sha256;Require-DtHash $part.path $part.sha256;Require-DtText $part.path $body.Substring($start,$end-$start)
    }
    $source=Join-Path $script:RepoRoot 'driver/wintun.c';$body=[IO.File]::ReadAllText($source);$start=$body.IndexOf('static MINIPORT_RESTART TunRestart;');$end=$body.IndexOf('static MINIPORT_PAUSE TunPause;',$start);$r=$build.restart
    if($r.source -cne $source -or $r.offset -ne $start -or $r.length -ne $end-$start){throw 'Dual coalesce Restart span differs'};Require-DtHash $source $r.source_sha256;Require-DtHash $r.path $r.sha256;Require-DtText $r.path $body.Substring($start,$end-$start)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$hash='a0a20ebbe48fdcfbf64a0f210dcafcc107e0e83cf060c4727df6301653808771'
    if($build.host_source -cne $hostSource -or $build.host_source_sha256 -cne $hash -or $build.host_original_sha256 -cne $hash -or $build.host_original -cne (Join-Path $place 'worker-host-original.c') -or $build.host -cne (Join-Path $place 'worker-host.h') -or $build.cleanup_observer -cne (Join-Path $place 'cleanup-observe.h')){throw 'Dual coalesce frozen worker host binding differs'}
    Require-DtHash $hostSource $hash;Require-DtHash $build.host_original $hash;Require-DtHash $build.host $build.host_sha256;Require-DtHash $build.cleanup_observer $build.cleanup_observer_sha256
    $lines=@('static VOID ObservedDualRundown(EX_RUNDOWN_REF *);','#define ExWaitForRundownProtectionRelease ObservedDualRundown',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_cleanup.h').Replace('\','/')+'"'),'#undef ExWaitForRundownProtectionRelease');Require-DtText $build.cleanup_observer (($lines -join "`n")+"`n")
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq '../../driver/v2/session_cleanup.h'){$build.cleanup_observer}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'});Require-DtText $build.host $body
    $helper=Join-Path $script:RepoRoot 'tools/build/indication-pause-tests.ps1';if([IO.Path]::GetFullPath($build.helper.path) -cne [IO.Path]::GetFullPath($helper)){throw 'Dual coalesce helper binding differs'};Require-DtHash $helper $build.helper.sha256
    Assert-DtSet @($build.production|ForEach-Object {$_.relative}) $production 'Dual coalesce production closure set differs'
    foreach($p in $build.production){$source=Join-Path $script:RepoRoot $p.relative;$copy=Join-Path $place ('source-'+$p.relative.Replace('\','-').Replace('/','-'));if($p.source -cne $source -or $p.path -cne $copy -or $p.source_sha256 -cne $p.sha256){throw 'Dual coalesce raw production binding differs'};Require-DtHash $source $p.source_sha256;Require-DtHash $copy $p.sha256}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($flags -join "`n") -cne ($build.arguments -join "`n")){throw 'Dual coalesce compile arguments differ'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version;$bindings=[ordered]@{PB_DUAL_HOST=$build.host;PB_WP_HOST=$e.host;PB_WP_RESTART=$r.path;PB_REFERENCE_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_references.h');PB_NBL_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};foreach($part in $e.parts){$lines+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"';Require-DtText $build.entry (($lines -join "`n")+"`n")
}
function Read-DtMetrics([string[]]$Lines,[int]$Family){
    $text=$Lines -join "`n";$rows=[regex]::Matches($text,'(?m)^DUALTAIL32 ip([46]) old(6|17) new(6|17) ndis(30|89) freshFirst([01]) wrap([01]) measured([01]) batches32/32 unique64 nbl2/2 joins5 owners0 repairs0$')
    if($rows.Count -ne 24 -or @($Lines|Where-Object {$_ -like 'DUALTAIL32 *'}).Count -ne 24){throw 'Dual coalesce case count/format differs'}
    for($i=0;$i -lt 24;++$i){$shape=[int][Math]::Floor(($i%12)/4);$old=if($shape -eq 2){17}else{6};$minor=if($shape -eq 0){30}else{89};$new=if($shape -eq 1){17}else{6};$g=$rows[$i].Groups
        if([int]$g[1].Value -ne $Family -or [int]$g[2].Value -ne $old -or [int]$g[3].Value -ne $new -or [int]$g[4].Value -ne $minor -or [int]$g[5].Value -ne $i%2 -or [int]$g[6].Value -ne ([int][Math]::Floor($i/2)%2) -or [int]$g[7].Value -ne [int]($i -ge 12)){throw 'Dual coalesce family/matrix differs'}
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: DUALTAIL32 family([46]) cases24 warm12 measured12 joins120 workerJoins24 handles(\d+)/(\d+) owners0 repairs0$')
    if($summary.Count -ne 1 -or @($Lines|Where-Object {$_ -like 'PASS:*'}).Count -ne 1 -or [int]$summary[0].Groups[1].Value -ne $Family -or $summary[0].Groups[2].Value -cne $summary[0].Groups[3].Value){throw 'Dual coalesce summary/resources differ'}
    return @{cases=24;warm=12;measured=12;joined_threads=120;receive_worker_joins=24;distinct_originals=24*64;old_originals=24*32;fresh_originals=24*32;hosted_nbls=24*4;process_handles_before=[int]$summary[0].Groups[2].Value;process_handles_after=[int]$summary[0].Groups[3].Value}
}
Start-TestRecording $directory 'operations.json'
function Invoke-DtProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$p=[Diagnostics.Process]::new();$p.StartInfo=$info
    try{if(!$p.Start()){throw 'Cannot start dual coalesce test'};$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(60000)){$p.Kill($true);$p.WaitForExit();throw 'Dual coalesce external watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        $row=[ordered]@{program=$build.path;arguments=@();exit_code=$p.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})};$script:TestExecutions.Add($row);Write-TestRecording $(if($p.ExitCode){'failed'}else{'running'})
        if($p.ExitCode){throw "Dual coalesce exit$($p.ExitCode): $log"};$row['metrics']=Read-DtMetrics @($output -split '\r?\n') $build.ip_version;Write-Output $row.reported_pass_messages
    }finally{$p.Dispose()}
}
$builds=@();$firstBuild=$null
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $build=Build-CoalesceDualTail32Tests -Directory (Join-Path $directory "ipv$family-$mode-guard$guard") -IpVersion $family -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $family -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Dual coalesce requested build differs'};Assert-DtBuild $build;$builds+=$build;if(!$firstBuild){$firstBuild=$build};Invoke-DtProcess $build
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log;Read-DtMetrics $original 4|Out-Null
foreach($kind in 'wrong-family','missing-row','duplicate-row','unsupported-uro'){
    $changed=switch($kind){'wrong-family'{@($original|ForEach-Object {$_ -replace '^DUALTAIL32 ip4 ','DUALTAIL32 ip6 '})}'missing-row'{@($original|Where-Object {$_ -notmatch '^DUALTAIL32 ip4 old6 new6 ndis30 freshFirst0 wrap0 measured0 '})}'duplicate-row'{@($original)+@($original|Where-Object {$_ -like 'DUALTAIL32 *'}|Select-Object -First 1)}'unsupported-uro'{@($original|ForEach-Object {$_ -replace 'old6 new6 ndis30','old6 new17 ndis30'})}}
    $expected=if($kind -in 'wrong-family','unsupported-uro'){'Dual coalesce family/matrix differs'}else{'Dual coalesce case count/format differs'};$reason=$null;try{Read-DtMetrics $changed 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason -cne $expected){throw "Dual parser wrong rejection: $kind ($reason)"};$parser+=@{name=$kind;rejected=$true;reason=$reason}
}
$metadata=@();Assert-DtBuild ($firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','duplicate-part','part-source-hash','host-source','compile-flags'){
    $changed=$firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json
    $expected=switch($kind){'duplicate-dependency'{$changed.production[1]=$changed.production[0];'Dual coalesce production closure set differs'}'duplicate-part'{$changed.extractions.parts[1]=$changed.extractions.parts[0];'Dual coalesce actual part set differs'}'part-source-hash'{$changed.extractions.parts[0].source_sha256='0'*64;'Dual coalesce raw hash differs'}'host-source'{$changed.host_source=Join-Path $script:RepoRoot 'tests/model/return_coalesce_tests.c';'Dual coalesce frozen worker host binding differs'}'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Dual coalesce compile arguments differ'}}
    $reason=$null;try{Assert-DtBuild $changed}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Dual metadata wrong rejection: $kind ($reason)"};$metadata+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($firstBuild.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$text=$originalEntry;$flags=@($firstBuild.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping dual coalesce paths required'}'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping dual coalesce configuration required'}'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'}'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'dual coalesce requires shipping fingerprint'}}
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log';& cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or !([regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message)))){throw "Dual exact compiler guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;arguments=$flags;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-DtDependencies
$cases=0;$joins=0;$workers=0;foreach($e in $script:TestExecutions){$cases+=$e.metrics.cases;$joins+=$e.metrics.joined_threads;$workers+=$e.metrics.receive_worker_joins}
if($script:TestExecutions.Count -ne 8 -or $cases -ne 192 -or $joins -ne 960 -or $workers -ne 192){throw 'Dual executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Dual coalesce source changed during audit'};Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;variant='current';source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();operations=$script:TestResultFile;builds=$builds;cases=$cases;joined_threads=$joins;receive_worker_joins=$workers;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests/model/coalesce_dual_tail32_tests.c','tools/build/coalesce-dual-tail32-tests.ps1','tools/audit/verify-coalesce-dual-tail32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage only, no production defect/before or repair. Two batches each32 originals use64 distinct gen1 slots in a64slot/capacity128 layout. Each batch forms two16-original coalesced NBLs in independent flows. Controller holds the real hosted diagnostic SRW lock; one actual outer Return window physically frees both old NBLs/MDLs and releases buffer0/1 before blocking at that lock. A sole hosted receive thread runs the unchanged actual ReceiveBatch twice and then waits for Stop; fresh slots32..63 legitimately borrow both buffers with new bytes/length and, at6.89, protocol. Two controlled old NBL failure status words are defensive hosted input, not an observed natural NDIS failure. Old diagnostic snapshots retain their original bytes/count/IP/protocol/length after private buffers are overwritten. Both fresh-first/old-first return orders preserve fresh Busy/data, exact64 unique completion statuses and stats. Actual Pause waits all old+fresh Active; after Pause joins, actual Unregister/Free joins the receive thread then conflicts on the final real Returns pin before physical release. All five created SDK threads per case join; parent MDL/map/events/pools/NBLs/records/core slots converge and process handles match. Original worker/Pause host is full raw-bound; only includes select literal production extracts/transparent Free DDI observer, with the original kernel-host bank-field insertion. No altered production bodies or fakeFULL; no private LocallyCancelled mutation. Modern TCP6.30/6.89 and UDP6.89 only;6.20 does not support coalescing and remains covered by other suites. NDEBUG required while original NT_ASSERT maps to live CHECK. Guard scope is the existing pool/MDL/NBL/region allocations, not stack job/core arrays. Hosted DDIs/IRQL/registration and two-turn receive driver; not actual native workerloop/Halt/DLL/NDIS/old-OS/runtime CPU or sustained leak proof.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
