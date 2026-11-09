param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/cross-queue-fault32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version-cne'2.0.0'){throw 'Cross queue fault requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
function Require-CqHash([string]$Path,[string]$Hash){if($Hash-cnotmatch'^[a-f0-9]{64}$'-or(Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant()-cne$Hash){throw 'Cross queue raw hash differs'}}
function Require-CqText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Cross queue complete projection differs'}}
function Assert-CqSet([string[]]$Actual,[string[]]$Expected,[string]$Message){$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}};if($Actual.Count-ne$Expected.Count-or!$set.SetEquals($Expected)){throw $Message}}
[string[]]$production=@(rg --files driver common include|ForEach-Object {$_.Replace('\','/')});if($LASTEXITCODE){throw 'Cannot enumerate cross queue production closure'}
[string[]]$dependencies=@($production)+@('wintun.props','tests/model/drain_boundary_tests.c','tests/model/kernel_nbl_fixture.h','tests/safety/guard_memory.h')
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/cross-queue-fault32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineIdentity=$null;$hashes=@{}
if($historical){
 $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
 [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
 if($manifest.version-cne'2.0.0'-or$manifest.source_fingerprint_order-cne'typed string[] Ordinal'-or$names.Count-ne749-or($names-join"`n")-cne($sorted-join"`n")){throw 'Cross queue baseline count/order/version differs'}
 Assert-CqSet $names @($manifest.files.PSObject.Properties.Name) 'Cross queue baseline file set differs'
 $entries=@(foreach($name in $names){Require-CqHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
 $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries-join"`n")))).ToLowerInvariant()
 if($baselineIdentity-cne'9ddcb5506fe9d6a05d45e9a5ba6949b60116e5bca2dd4d5d0d9798c7f69afc94'-or$manifest.source_tree_sha256-cne$baselineIdentity){throw 'Cross queue baseline identity differs'}
 Assert-CqSet $production @($names|Where-Object {$_-cmatch'^(driver|common|include)/'}) 'Cross queue baseline production set differs'
 foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
 $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()}
 $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
function Assert-CqDependencies {foreach($relative in $dependencies){Require-CqHash (Join-Path $baseline $relative) $hashes[$relative];Require-CqHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}}
Assert-CqDependencies
function Convert-CqAbsolute([string]$Text,[string]$Source){return [regex]::Replace($Text,'"([^"\r\n]*\.[ch])"',{param($m)$p=$m.Groups[1].Value;if(![IO.Path]::IsPathFullyQualified($p)){$p=[IO.Path]::GetFullPath((Join-Path (Split-Path $Source) $p))};'"'+$p.Replace('\','/')+'"'})}
function Assert-CqBuild($build){
 if($build.ip_version-notin4,6-or$build.mode-cnotin'iso','ms'-or$build.guarded-isnot[bool]-or!$build.ndebug-or$build.pb_api_tests){throw 'Cross queue build selection differs'}
 $place=[IO.Path]::GetDirectoryName($build.path);$test=Join-Path $script:RepoRoot 'tests/model/cross_queue_fault32_tests.c'
 if($build.path-cne(Join-Path $place 'cross-queue-fault32.exe')-or$build.entry-cne(Join-Path $place 'entry.c')-or$build.test-cne$test){throw 'Cross queue output binding differs'}
 Require-CqHash $build.path $build.sha256;Require-CqHash $build.entry $build.entry_sha256;Require-CqHash $test $build.test_sha256
 $hosts=[ordered]@{'tests/model/drain_boundary_tests.c'='50753c442cbeaaaa3956cc27d9884cb93fbe316a42953f387514f530466bc4a8';'tests/model/kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'}
 Assert-CqSet @($build.inputs|ForEach-Object {$_.relative}) @($hosts.Keys) 'Cross queue input set differs'
 foreach($r in $build.inputs){$source=Join-Path $script:RepoRoot $r.relative;$copy=Join-Path $place ('original-'+[IO.Path]::GetFileName($r.relative));if($r.source-cne$source-or$r.path-cne$copy-or$r.source_sha256-cne$hosts[$r.relative]-or$r.sha256-cne$hosts[$r.relative]){throw 'Cross queue frozen host binding differs'};Require-CqHash $source $r.source_sha256;Require-CqHash $copy $r.sha256}
 $paths=[ordered]@{'completion-observer'=(Join-Path $place 'completion-observe.h');'query-observer'=(Join-Path $place 'query-observe.h');kernel=(Join-Path $place 'kernel-host.h');drain=(Join-Path $place 'drain-host.h')}
 Assert-CqSet @($build.generated|ForEach-Object {$_.name}) @($paths.Keys) 'Cross queue generated set differs'
 foreach($r in $build.generated){if($r.path-cne$paths[$r.name]){throw 'Cross queue generated path differs'};Require-CqHash $r.path $r.sha256}
 $completion=$paths['completion-observer'];$query=$paths['query-observer']
 $lines=@('static PB_RESULT CqPush(PB_QUEUE_VIEW *,const PB_DESCRIPTOR *,uint32_t,uint32_t,uint32_t,uint32_t *,uint32_t *);','#define PbQueuePushRing CqPush',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_completion.h').Replace('\','/')+'"'),'#undef PbQueuePushRing');Require-CqText $completion (($lines-join"`n")+"`n")
 $lines=@('static VOID CqQueryAcquire(KSPIN_LOCK *,KLOCK_QUEUE_HANDLE *);','#define KeAcquireInStackQueuedSpinLock CqQueryAcquire',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_query.h').Replace('\','/')+'"'),'#undef KeAcquireInStackQueuedSpinLock');Require-CqText $query (($lines-join"`n")+"`n")
 $source=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$body=[IO.File]::ReadAllText($source);$marker='#include "../../driver/v2/session_completion.h"'
 if([regex]::Matches($body,[regex]::Escape($marker)).Count-ne1){throw 'Cross queue completion marker differs'}
 $body=$body.Replace($marker,('#include "'+$completion.Replace('\','/')+'"'));Require-CqText $paths.kernel (Convert-CqAbsolute $body $source)
 $source=Join-Path $script:RepoRoot 'tests/model/drain_boundary_tests.c';$body=[IO.File]::ReadAllText($source);$marker='#include "kernel_nbl_fixture.h"'
 if([regex]::Matches($body,[regex]::Escape($marker)).Count-ne1){throw 'Cross queue fixture marker differs'}
 $body=$body.Replace($marker,('#include "'+$paths.kernel.Replace('\','/')+'"'));Require-CqText $paths.drain (Convert-CqAbsolute $body $source)
 Assert-CqSet @($build.production|ForEach-Object {$_.relative}) $production 'Cross queue production set differs'
 foreach($r in $build.production){$source=Join-Path $script:RepoRoot $r.relative;$copy=Join-Path $place ('source-'+$r.relative.Replace('/','-'));if($r.source-cne$source-or$r.path-cne$copy-or$r.source_sha256-cne$r.sha256){throw 'Cross queue production binding differs'};Require-CqHash $source $r.source_sha256;Require-CqHash $copy $r.sha256}
 $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
 if(($flags-join"`n")-cne($build.arguments-join"`n")){throw 'Cross queue compile arguments differ'}
 $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version;$lines+='#define PB_CQ_HOST "'+$paths.drain.Replace('\','/')+'"';$lines+='#define PB_QUERY_SOURCE "'+$query.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"';Require-CqText $build.entry (($lines-join"`n")+"`n")
}
function Read-CqMetrics([string[]]$Lines,[int]$Family){
 $text=$Lines-join"`n";$rows=[regex]::Matches($text,'(?m)^CROSSFAULT32 ip([46]) proto(6|17) ndis(20|89) wrap([01]) publicationFirst([01]) measured([01]) packets32 publish(0|16) privateRetired(16|32) badPush1 queryBlocked([01]) joins5 workerJoins2 owners0 repairs0$')
 if($rows.Count-ne32-or@($Lines|Where-Object {$_}).Count-ne33){throw 'Cross queue case count/format differs'}
 for($i=0;$i-lt32;++$i){$shape=$i%16;$protocol=if($shape-lt8){6}else{17};$minor=if(([int][Math]::Floor($shape/4)%2)-eq0){20}else{89};$wrap=[int][Math]::Floor($shape/2)%2;$order=$shape%2;$g=$rows[$i].Groups
  if([int]$g[1].Value-ne$Family-or[int]$g[2].Value-ne$protocol-or[int]$g[3].Value-ne$minor-or[int]$g[4].Value-ne$wrap-or[int]$g[5].Value-ne$order-or[int]$g[6].Value-ne[int]($i-ge16)-or[int]$g[7].Value-ne$(if($order){16}else{0})-or[int]$g[8].Value-ne$(if($order){16}else{32})-or[int]$g[9].Value-ne$order){throw 'Cross queue family/matrix differs'}
 }
 $summary=[regex]::Matches($text,'(?m)^PASS: CROSSFAULT32 family([46]) cases32 warm16 measured16 joins160 workerJoins64 handles(\d+)/(\d+) owners0 repairs0$')
 if($summary.Count-ne1-or[int]$summary[0].Groups[1].Value-ne$Family-or$summary[0].Groups[2].Value-cne$summary[0].Groups[3].Value){throw 'Cross queue summary/resources differ'}
 return @{cases=32;warm=16;measured=16;packets=1024;packet_bytes=1536000;published_completions=256;privately_retired=768;bad_consumer_faults=32;query_lock_conflicts=16;joined_threads=160;return_os_thread_joins=64;query_os_thread_joins=32;fault_worker_os_thread_joins=64;process_handles_before=[int]$summary[0].Groups[2].Value;process_handles_after=[int]$summary[0].Groups[3].Value}
}
function Invoke-CqProcess($build){
 $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$p=[Diagnostics.Process]::new();$p.StartInfo=$info
 try{if(!$p.Start()){throw 'Cross queue process failed to start'};$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(120000)){$p.Kill($true);$p.WaitForExit();throw 'Cross queue external watchdog expired'};$text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$lines=@($text-split'\r?\n'|Where-Object {$_});$log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log'-f$script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$text,$utf8);if($p.ExitCode){throw "Cross queue process exit$($p.ExitCode):$log"};$metrics=Read-CqMetrics $lines $build.ip_version
  $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();exit_code=$p.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics});Write-TestRecording 'running';$lines|Out-Host
 }finally{$p.Dispose()}
}
# Begin execution only after authenticating the requested baseline and inputs.
Start-TestRecording $directory 'operations.json';$builds=@()
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in @($false,$true)){$place=Join-Path $directory "ipv$family-$mode-$guard";$b=Build-CrossQueueFault32Tests -Directory $place -IpVersion $family -Mode $mode -Guarded:$guard;Assert-CqDependencies;Assert-CqBuild $b;Invoke-CqProcess $b;$builds+=,$b}}}
$first=$builds[0];$clone=$first|ConvertTo-Json -Depth 20|ConvertFrom-Json;Assert-CqBuild $clone;$metadata=@()
foreach($kind in 'duplicate-production','host-hash','observer-hash','compile-flags','duplicate-generated'){
 $changed=$first|ConvertTo-Json -Depth 20|ConvertFrom-Json
 $expected=switch($kind){'duplicate-production'{$changed.production[1]=$changed.production[0];'Cross queue production set differs'};'host-hash'{$changed.inputs[0].source_sha256='0'*64;'Cross queue frozen host binding differs'};'observer-hash'{$changed.generated[0].sha256='0'*64;'Cross queue raw hash differs'};'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Cross queue compile arguments differ'};'duplicate-generated'{$changed.generated[1]=$changed.generated[0];'Cross queue generated set differs'}}
 $why='';try{Assert-CqBuild $changed}catch{$why=$_.Exception.Message};if($why-cne$expected){throw "Cross queue metadata rejection differs:$kind/$why"};$metadata+=@{kind=$kind;rejected=$true;reason=$why}
}
$operation=$script:TestExecutions[0];$originalLines=@(Get-Content -LiteralPath $operation.log);$parser=@()
foreach($kind in 'wrong-family','missing-row','changed-order','unequal-handles'){
 $lines=@($originalLines);$family=$first.ip_version;$expected='Cross queue family/matrix differs'
 switch($kind){'wrong-family'{$family=6};'missing-row'{$lines=@($lines|Select-Object -Skip 1);$expected='Cross queue case count/format differs'};'changed-order'{$lines[0]=$lines[0].Replace('publicationFirst0','publicationFirst1')};'unequal-handles'{$lines[$lines.Count-1]=$lines[-1]-replace'handles\d+/\d+','handles1/2';$expected='Cross queue summary/resources differ'}}
 $why='';try{Read-CqMetrics $lines $family|Out-Null}catch{$why=$_.Exception.Message};if($why-cne$expected){throw "Cross queue parser rejection differs:$kind/$why"};$parser+=@{kind=$kind;rejected=$true;reason=$why}
}
$contracts=@();foreach($kind in 'no-fingerprint','pb-api-tests','no-ndebug','invalid-family'){
 $place=Join-Path $directory "contracts/$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$flags=@($first.arguments|ForEach-Object {if($_-like'/Fo*'){"/Fo$place\"}else{$_}});$entry=$first.entry
 switch($kind){'no-fingerprint'{$flags=@($flags|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'});$expected='cross queue fault requires shipping fingerprint';$code='C2338'};'pb-api-tests'{$flags+='/DPB_API_TESTS=1';$expected='Shipping cross queue fault paths required';$code='C1189'};'no-ndebug'{$flags=@($flags|Where-Object {$_-cne'/DNDEBUG'});$expected='Shipping cross queue fault configuration required';$code='C1189'};'invalid-family'{$entry=Join-Path $place 'entry.c';$text=[IO.File]::ReadAllText($first.entry);if([regex]::Matches($text,'#define PB_TEST_IP_VERSION 4').Count-ne1){throw 'Cross queue family contract marker differs'};[IO.File]::WriteAllText($entry,$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 7'),$utf8);$expected='Select one IP family';$code='C1189'}}
 $log=Join-Path $place 'compiler.log';& cl.exe @flags $entry common/v2_core.c common/ethernet.c common/coalesce.c common/packet.c /link ("/out:"+(Join-Path $place 'rejected.exe')) *> $log;$exit=$LASTEXITCODE;$text=[IO.File]::ReadAllText($log)
 if(!$exit-or$text-cnotmatch[regex]::Escape($expected)-or$text-cnotmatch[regex]::Escape($code)){throw "Cross queue compile contract missed selected guard:$kind"}
 $contracts+=@{kind=$kind;exit_code=$exit;expected_diagnostic=$expected;compiler_code=$code;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();arguments=$flags}
}
Assert-CqDependencies
if((Get-SourceFingerprint)-cne$script:SourceFingerprint){throw 'Cross queue source tree changed during audit'}
Write-TestRecording 'passed'
@{result='passed';version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();reviewed_dependencies=$dependencies.Count;production_closure_changed=$false;operations=$script:TestResultFile;builds=$builds;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;kernel_runtime_tested=$false;production_ready=$false;
 audit_sources=@('tests/model/cross_queue_fault32_tests.c','tools/build/cross-queue-fault32-tests.ps1','tools/audit/verify-cross-queue-fault32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
 method='Actual ReceiveBatch produces32 plain IPv4-only or IPv6-only TCP/UDP packets as16 distinct NBLs on each of two queues. Runtime6.20/6.89 gates operate over a full hosted OOB array with future-field sentinels, not a native old-kernel layout. Slot16/ring32 geometry needs no fabricated FULL. Both complete frozen drain-boundary and kernel-NBL hosts are raw-hash locked and archived; only quoted paths and the one kernel completion include binding change. Literal wrappers forward actual PushRing and Query queue-lock acquisition, leaving production bodies, original NT_ASSERT=CHECK and ownership rules intact. In publication-first order the actual q0 core PushRing has committed16 data/Producer entries but the real caller still holds RxLock and has not settled its private16 Pending/Credits. A transparent post-call gate selects another CPU interleaving; it does not permit native spinlock/IRQL code to sleep. A q1 Return discovers one explicitly hostile shared Consumer ahead of Producer through the actual core check, invokes Fault and privately retires16. This is shared-wire robustness input, not a normal public DLL emission. Query demonstrably fails TryAcquire on q0 RxLock before forwarding the genuine blocking SRW acquire. Releasing q0 settles its accepted16 exactly once despite the intervening Fault; repeated Fault emits no second terminal queue broadcast. The inverse order faults before q0 Flush, so all32 retire privately and no q0 completion is published. No consumer/generation/credit/Active repair occurs. Payload and token/OOB bytes are checked while NDIS still owns MDLs. Actual Query remains non-STOPPED until both real OS worker callers publish their separate FaultQuiesced witnesses; each starts already Faulted and exits without packet, recycle or wait-loop work. Per case two Return callers and one Query caller plus two sequential fault-worker callers have captured IDs/exit codes, actual waits and handle closes checked. Every caller is joined before original Destroy releases fixture events/region/storage, with all NBL/MDL/pool/slot/credit/Pending/Active/Returns owners zero. Guarded mode guards original host NBL/MDL/record objects; the ready factory and region retain ordinary original storage. No actual registration, public DLL/Close, Unregister, FreeSession, Halt, sustained CPU, native NDIS/kernel/Win7 execution, signing or device operation claim. Each of8 family/volatile/guard processes runs16 checked warm and16 measured cases,160 joins and strict handle equality. Default dependencies are self-contained current snapshots; optional749/9dd baseline authenticates every raw file and typed-Ordinal identity and permits no production delta. Version2.0.0 unchanged.'}|
 ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
