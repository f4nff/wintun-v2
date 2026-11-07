param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/reference-handoff32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Reference handoff requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
function Require-HoHash([string]$Path,[string]$Hash){if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Reference handoff raw hash differs'}}
function Require-HoText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Reference handoff complete projection differs'}}
function Assert-HoSet([string[]]$Actual,[string[]]$Expected,[string]$Message){$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}};if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}}
[string[]]$production=@(rg --files driver common include);if($LASTEXITCODE){throw 'Cannot enumerate handoff production closure'}
[string[]]$dependencies=@($production|ForEach-Object {$_.Replace('\','/')})+@('wintun.props','tests/model/tun32_worker_pause_tests.c','tests/model/kernel_nbl_fixture.h','tests/safety/guard_memory.h','tools/build/indication-pause-tests.ps1')
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/reference-handoff32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineIdentity=$null;$hashes=@{}
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 735 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0' -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal'){throw 'Reference handoff baseline count/order/version differs'}
    Assert-HoSet $names @($manifest.files.PSObject.Properties.Name) 'Reference handoff baseline file set differs'
    $entries=@(foreach($name in $names){Require-HoHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne '64be5e2b5802e3057b044c89eb6440d0cbbb432addbf3c4be2ce18a6f730ca02' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Reference handoff baseline identity differs'}
    Assert-HoSet @($production|ForEach-Object {$_.Replace('\','/')}) @($names|Where-Object {$_ -cmatch '^(driver|common|include)/'}) 'Reference handoff baseline production set differs'
    foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
    $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()}
    $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
function Assert-HoDependencies {foreach($relative in $dependencies){Require-HoHash (Join-Path $baseline $relative) $hashes[$relative];Require-HoHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}}
Assert-HoDependencies
function Assert-HoBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Reference handoff build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path);$test=Join-Path $script:RepoRoot 'tests/model/reference_handoff32_tests.c'
    if($build.path -cne (Join-Path $place 'reference-handoff32.exe') -or $build.entry -cne (Join-Path $place 'entry.c') -or $build.test -cne $test){throw 'Reference handoff output/test binding differs'}
    Require-HoHash $build.path $build.sha256;Require-HoHash $build.entry $build.entry_sha256;Require-HoHash $test $build.test_sha256
    $original=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$hash='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3';$e=$build.extractions
    if($e.host_source -cne $original -or $e.host_source_sha256 -cne $hash -or $e.host_original_sha256 -cne $hash){throw 'Reference handoff original kernel metadata differs'}
    Require-HoHash $original $hash;Require-HoHash $e.host_original $hash;Require-HoHash $e.host $e.host_sha256
    $body=[IO.File]::ReadAllText($original);$marker='        EX_RUNDOWN_REF ReturnRundown;';$fields='        KSPIN_LOCK IndicationLock; KEVENT IndicationIdle[2]; ULONG IndicationBank; LONG IndicationCount[2];'
    if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Reference handoff bank fields anchor not unique'};$body=$body.Replace($marker,$marker+"`r`n"+$fields)
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $original) $m.Groups[1].Value)).Replace('\','/')+'"'})
    Require-HoText $e.host $body
    $expected=[ordered]@{outer=@('driver/wintun.c','static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;','#define TUN_FORCE_UNREGISTRATION');tunpause=@('driver/wintun.c','static MINIPORT_PAUSE TunPause;','static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;');receive=@('driver/v2/session_receive.h','static BOOLEAN PbV2ReceiveBatch(','[end]');gate=@('driver/v2/indication_gate.h','[whole]','[end]');pause=@('driver/v2/session_pause.h','[whole]','[end]')}
    if($e.variant -cne 'current'){throw 'Reference handoff requires current production bodies'};Assert-HoSet @($e.parts|ForEach-Object {$_.name}) @($expected.Keys) 'Reference handoff actual part set differs'
    foreach($part in $e.parts){$spec=$expected[$part.name];$source=Join-Path $script:RepoRoot $spec[0];$body=[IO.File]::ReadAllText($source);$start=if($spec[1] -ceq '[whole]'){0}else{$body.IndexOf($spec[1])};$end=if($spec[2] -ceq '[end]'){$body.Length}else{$body.IndexOf($spec[2],$start)}
        if($start -lt 0 -or $end -le $start -or $part.source -cne $source -or $part.offset -ne $start -or $part.length -ne $end-$start){throw 'Reference handoff actual span binding differs'}
        Require-HoHash $source $part.source_sha256;Require-HoHash $part.path $part.sha256;Require-HoText $part.path $body.Substring($start,$end-$start)
    }
    $source=Join-Path $script:RepoRoot 'driver/wintun.c';$body=[IO.File]::ReadAllText($source);$start=$body.IndexOf('static MINIPORT_RESTART TunRestart;');$end=$body.IndexOf('static MINIPORT_PAUSE TunPause;',$start);$r=$build.restart
    if($r.source -cne $source -or $r.offset -ne $start -or $r.length -ne $end-$start){throw 'Reference handoff Restart span differs'};Require-HoHash $source $r.source_sha256;Require-HoHash $r.path $r.sha256;Require-HoText $r.path $body.Substring($start,$end-$start)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$hash='a0a20ebbe48fdcfbf64a0f210dcafcc107e0e83cf060c4727df6301653808771'
    if($build.host_source -cne $hostSource -or $build.host_source_sha256 -cne $hash -or $build.host_original_sha256 -cne $hash -or $build.host_original -cne (Join-Path $place 'worker-host-original.c') -or $build.host -cne (Join-Path $place 'worker-host.h')){throw 'Reference handoff frozen worker host binding differs'}
    Require-HoHash $hostSource $hash;Require-HoHash $build.host_original $hash;Require-HoHash $build.host $build.host_sha256
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value)).Replace('\','/')+'"'});Require-HoText $build.host $body
    foreach($name in 'reference','receive','pause'){$path=$build.($name+'_observer');if($path -cne (Join-Path $place "$name-observe.h")){throw 'Reference handoff observer path differs'};Require-HoHash $path $build.($name+'_observer_sha256')}
    $lines=@('static VOID HandoffClear(KEVENT *);','static VOID HandoffReferenceAcquire(KSPIN_LOCK *,KLOCK_QUEUE_HANDLE *);','#define KeClearEvent HandoffClear','#define KeAcquireInStackQueuedSpinLock HandoffReferenceAcquire',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_references.h').Replace('\','/')+'"'),'#undef KeClearEvent','#undef KeAcquireInStackQueuedSpinLock');Require-HoText $build.reference_observer (($lines -join "`n")+"`n")
    $lines=@('static VOID HandoffRetain(PB_KERNEL_SESSION *,ULONG,ULONG);','#define PbV2RetainReceives HandoffRetain',('#include "'+($e.parts|Where-Object name -ceq 'receive').path.Replace('\','/')+'"'),'#undef PbV2RetainReceives');Require-HoText $build.receive_observer (($lines -join "`n")+"`n")
    $lines=@('static KIRQL HandoffPauseAcquire(SRWLOCK *);','#define ExAcquireSpinLockExclusive HandoffPauseAcquire',('#include "'+($e.parts|Where-Object name -ceq 'tunpause').path.Replace('\','/')+'"'),'#undef ExAcquireSpinLockExclusive');Require-HoText $build.pause_observer (($lines -join "`n")+"`n")
    $helper=Join-Path $script:RepoRoot 'tools/build/indication-pause-tests.ps1';if([IO.Path]::GetFullPath($build.helper.path) -cne [IO.Path]::GetFullPath($helper)){throw 'Reference handoff helper binding differs'};Require-HoHash $helper $build.helper.sha256
    Assert-HoSet @($build.production|ForEach-Object {$_.relative}) $production 'Reference handoff production closure set differs'
    foreach($p in $build.production){$source=Join-Path $script:RepoRoot $p.relative;$copy=Join-Path $place ('source-'+$p.relative.Replace('\','-').Replace('/','-'));if($p.source -cne $source -or $p.path -cne $copy -or $p.source_sha256 -cne $p.sha256){throw 'Reference handoff raw production binding differs'};Require-HoHash $source $p.source_sha256;Require-HoHash $copy $p.sha256}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($flags -join "`n") -cne ($build.arguments -join "`n")){throw 'Reference handoff compile arguments differ'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version;$bindings=[ordered]@{PB_HO_HOST=$build.host;PB_WP_HOST=$e.host;PB_WP_RESTART=$r.path;PB_REFERENCE_SOURCE=$build.reference_observer;PB_NBL_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};foreach($part in $e.parts){$path=if($part.name -ceq 'receive'){$build.receive_observer}elseif($part.name -ceq 'tunpause'){$build.pause_observer}else{$part.path};$lines+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$path.Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"';Require-HoText $build.entry (($lines -join "`n")+"`n")
}
function Read-HoMetrics([string[]]$Lines,[int]$Family){
    $text=$Lines -join "`n";$rows=[regex]::Matches($text,'(?m)^HANDOFF32 ip([46]) proto(6|17) ndis(20|89) wrap([01]) measured([01]) q0_32 q1_32 distinct64 sets2 clears2 pauseBlocked1 joins6 owners0 repairs0$')
    if($rows.Count -ne 16 -or @($Lines|Where-Object {$_ -like 'HANDOFF32 *'}).Count -ne 16){throw 'Reference handoff case count/format differs'}
    for($i=0;$i -lt 16;++$i){$shape=$i%8;$protocol=if($shape -lt 4){6}else{17};$minor=if(([int][Math]::Floor($shape/2)%2) -eq 0){20}else{89};$g=$rows[$i].Groups
        if([int]$g[1].Value -ne $Family -or [int]$g[2].Value -ne $protocol -or [int]$g[3].Value -ne $minor -or [int]$g[4].Value -ne $shape%2 -or [int]$g[5].Value -ne [int]($i -ge 8)){throw 'Reference handoff family/matrix differs'}
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: HANDOFF32 family([46]) cases16 warm8 measured8 joins96 workerJoins32 handles(\d+)/(\d+) owners0 repairs0$')
    if($summary.Count -ne 1 -or @($Lines|Where-Object {$_ -like 'PASS:*'}).Count -ne 1 -or [int]$summary[0].Groups[1].Value -ne $Family -or $summary[0].Groups[2].Value -cne $summary[0].Groups[3].Value){throw 'Reference handoff summary/resources differ'}
    return @{cases=16;warm=8;measured=8;joined_threads=96;idle_native_worker_joins=32;receive_os_thread_joins=16;return_os_thread_joins=32;pause_os_thread_joins=16;distinct_tokens=16*64;packet_bytes=16*96000;all_returned_sets=32;all_returned_clears=32;process_handles_before=[int]$summary[0].Groups[2].Value;process_handles_after=[int]$summary[0].Groups[3].Value}
}
Start-TestRecording $directory 'operations.json'
function Invoke-HoProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$p=[Diagnostics.Process]::new();$p.StartInfo=$info
    try{if(!$p.Start()){throw 'Cannot start reference handoff test'};$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(60000)){$p.Kill($true);$p.WaitForExit();throw 'Reference handoff external watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        $row=[ordered]@{program=$build.path;arguments=@();exit_code=$p.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})};$script:TestExecutions.Add($row);Write-TestRecording $(if($p.ExitCode){'failed'}else{'running'})
        if($p.ExitCode){throw "Reference handoff exit$($p.ExitCode): $log"};$row['metrics']=Read-HoMetrics @($output -split '\r?\n') $build.ip_version;Write-Output $row.reported_pass_messages
    }finally{$p.Dispose()}
}
$builds=@();$firstBuild=$null
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $build=Build-ReferenceHandoff32Tests -Directory (Join-Path $directory "ipv$family-$mode-guard$guard") -IpVersion $family -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $family -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Reference handoff requested build differs'};Assert-HoBuild $build;$builds+=$build;if(!$firstBuild){$firstBuild=$build};Invoke-HoProcess $build
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log;Read-HoMetrics $original 4|Out-Null
foreach($kind in 'wrong-family','missing-row','duplicate-row','missing-reset'){
    $changed=switch($kind){'wrong-family'{@($original|ForEach-Object {$_ -replace '^HANDOFF32 ip4 ','HANDOFF32 ip6 '})}'missing-row'{@($original|Where-Object {$_ -notmatch '^HANDOFF32 ip4 proto6 ndis20 wrap0 measured0 '})}'duplicate-row'{@($original)+@($original|Where-Object {$_ -like 'HANDOFF32 *'}|Select-Object -First 1)}'missing-reset'{@($original|ForEach-Object {$_ -replace 'sets2 clears2','sets2 clears1'})}}
    $expected=if($kind -eq 'wrong-family'){'Reference handoff family/matrix differs'}else{'Reference handoff case count/format differs'};$reason=$null;try{Read-HoMetrics $changed 4|Out-Null}catch{$reason=$_.Exception.Message}
    if($reason -cne $expected){throw "Handoff parser wrong rejection: $kind ($reason)"};$parser+=@{name=$kind;rejected=$true;reason=$reason}
}
$metadata=@();Assert-HoBuild ($firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json)
foreach($kind in 'duplicate-dependency','duplicate-part','part-source-hash','host-source','compile-flags'){
    $changed=$firstBuild|ConvertTo-Json -Depth 20|ConvertFrom-Json
    $expected=switch($kind){'duplicate-dependency'{$changed.production[1]=$changed.production[0];'Reference handoff production closure set differs'}'duplicate-part'{$changed.extractions.parts[1]=$changed.extractions.parts[0];'Reference handoff actual part set differs'}'part-source-hash'{$changed.extractions.parts[0].source_sha256='0'*64;'Reference handoff raw hash differs'}'host-source'{$changed.host_source=Join-Path $script:RepoRoot 'tests/model/return_coalesce_tests.c';'Reference handoff frozen worker host binding differs'}'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Reference handoff compile arguments differ'}}
    $reason=$null;try{Assert-HoBuild $changed}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Handoff metadata wrong rejection: $kind ($reason)"};$metadata+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($firstBuild.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$text=$originalEntry;$flags=@($firstBuild.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping reference handoff paths required'}'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping reference handoff configuration required'}'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'}'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'reference handoff requires shipping fingerprint'}}
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log';& cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or !([regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message)))){throw "Handoff exact compiler guard failed: $kind"}
    $contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;arguments=$flags;entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-HoDependencies
$cases=0;$joins=0;$workers=0;foreach($e in $script:TestExecutions){$cases+=$e.metrics.cases;$joins+=$e.metrics.joined_threads;$workers+=$e.metrics.idle_native_worker_joins}
if($script:TestExecutions.Count -ne 8 -or $cases -ne 128 -or $joins -ne 768 -or $workers -ne 256){throw 'Handoff executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Reference handoff source changed during audit'};Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;variant='current';source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();operations=$script:TestResultFile;builds=$builds;cases=$cases;joined_threads=$joins;idle_native_worker_joins=$workers;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests/model/reference_handoff32_tests.c','tools/build/reference-handoff32-tests.ps1','tools/audit/verify-reference-handoff32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage only; no new production defect, repair or before variant. Each case uses two queues with32 slots/capacity32 and actual64 distinct scoped gen1 tokens; q0 Receive32 first owns32 NDIS NBLs. A second SDK thread executes q1 actual ReceiveBatch through constructing32 private NBLs, then a transparent Retain wrapper selects its pre-retain boundary while Transition shared remains held. These32 private objects are not yet indicated/retained. q0 actual outerReturn32 publishes its completions and reaches actual ReferenceLock-protected Active32-to0. Hosted OnSetEvent freezes immediately before SDK AllReturned Set while the real modeled ReferenceLock remains held and event still reset. q1 enters the unchanged RetainReferences function: a lexical lock-acquisition observer proves TryExclusive fails before delegating to the real SRW-backed primitive. Actual TunPause starts on another thread, and its lexical Transition EX observer proves TryExclusive fails before delegating. These are controlled hosted scheduling/lock-contention witnesses, not OS parked-thread or native kernel spinlock blocking/IRQL permission. Releasing q0 permits its real Set under ReferenceLock, then q1 actual0-to32 retain; the lexical clear observer verifies the old SDK event is signaled and delegates the real reset under that same lock. Pause can acquire Transition EX only after q1 receive transaction publishes its references; after both receive/old-return callers join, actual Pause remains at its AllReturned wait with Active32/event reset. Only actual q1 outerReturn32 resets Active0 and supplies the second Set; Pause and return caller join before Free. Both queue completion rings deliver exactly32 unique OK descriptors and actual core pool transitions retire64 owners; packet1500-byte data/partial MDL identity, plain old/modern OOB accessors, exact64 references/returns/stats, true uint64 cursor wrap, exactly2 AllReturned Sets/Clears and zero residual credits/pending are checked. Actual Unregister/Free joins two idle-only native threads and releases native pools, MDLs/NBLs/regions/object events; every one of6 created SDK threads joins per case, strict process handles match after a full checked warm matrix. All gates/counters are test-owned and do not repair private owners or fakeFULL. Full frozen worker/kernel hosts retain raw snapshots; only absolute includes, existing bank fields and three literal lexical observer headers bind current production bodies. Reference Store/NT_ASSERT/loop bodies remain intact, original NT_ASSERT maps to live CHECK with NDEBUG required. Offload/coalescing disabled; NDIS6.20/6.89 runtime macro and full hosted NBL future-field sentinels are not old-kernel-layout/Win7-runtime proof. Ready registration/idle workers/NDIS upcalls/DDIs/IRQL are hosted; this EXE does not claim real driver/network/device/registry, native workerloop/Halt/DLL unload, CPU/latency/throughput or sustained native leak verification. Guard scope is native pools/MDLs/NBLs/region, not stack/core pool arrays or SDK allocations. Version2.0.0 and production APIs unchanged.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
