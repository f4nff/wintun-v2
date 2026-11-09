param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/coalesce-prefilter32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Coalesce prefilter version differs'}
$script:SourceFingerprint=Get-SourceFingerprint;$utf8=[Text.UTF8Encoding]::new($false)
$beforeHeader='5e4f762a8dfa3c1968b699b3e7249efda6a8355050bf77a3356b2d347e0cb426';$afterHeader='8ac09e92bf917755f62474dc8bfd021e2cc6f54a68e1163b83de571ad8eb0e04'
[string[]]$dependencies=@('driver/v2/session_coalesce.h','driver/v2/session_state.h','common/v2_core.c','common/v2_core.h','common/packet.c','common/packet.h','common/coalesce.c','common/coalesce.h','common/flow.h','include/packetbridge_probe.h','include/packetbridge_v2_abi.h','include/packetbridge_types.h','include/packetbridge_version.h','tests/safety/guard_memory.h','wintun.props')
function Require-CfHash([string]$Path,[string]$Hash){if($Hash -cnotmatch '^[a-f0-9]{64}$' -or (Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -cne $Hash){throw 'Prefilter raw hash differs'}}
function Require-CfText([string]$Path,[string]$Text){if(![Linq.Enumerable]::SequenceEqual([byte[]][IO.File]::ReadAllBytes($Path),[byte[]]$utf8.GetBytes($Text))){throw 'Prefilter complete entry differs'}}
function Assert-CfSet([string[]]$Actual,[string[]]$Expected,[string]$Message){$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);foreach($name in $Actual){if(!$set.Add($name)){throw $Message}};if($Actual.Count -ne $Expected.Count -or !$set.SetEquals($Expected)){throw $Message}}
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out/coalesce-prefilter32/$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$historical=![string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineIdentity=$null;$hashes=@{}
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifestPath=Join-Path $baseline 'baseline.json';$manifest=Get-Content -LiteralPath $manifestPath -Raw|ConvertFrom-Json
    [string[]]$names=@($manifest.source_paths);[string[]]$sorted=$names.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($names.Count -ne 743 -or ($names -join "`n") -cne ($sorted -join "`n") -or $manifest.version -cne '2.0.0' -or $manifest.source_fingerprint_order -cne 'typed string[] Ordinal'){throw 'Prefilter baseline count/order/version differs'}
    Assert-CfSet $names @($manifest.files.PSObject.Properties.Name) 'Prefilter baseline file set differs'
    $entries=@(foreach($name in $names){Require-CfHash (Join-Path $baseline $name) $manifest.files.$name;$name+':'+$manifest.files.$name})
    $baselineIdentity=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    if($baselineIdentity -cne '1695a4be1d9b2ea619261b1d8ee6ee7f2372bb0b845884cb8d65f100c243bc18' -or $manifest.source_tree_sha256 -cne $baselineIdentity){throw 'Prefilter baseline identity differs'}
    foreach($relative in $dependencies){$hashes[$relative]=$manifest.files.$relative}
}else{
    $baseline=Join-Path $directory 'current-source';foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$hashes[$relative]=(Get-FileHash $source).Hash.ToLowerInvariant()}
    $manifestPath=Join-Path $baseline 'snapshot.json';@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 5|Set-Content -LiteralPath $manifestPath -Encoding utf8
}
function Assert-CfDependencies {
    foreach($relative in $dependencies){Require-CfHash (Join-Path $baseline $relative) $hashes[$relative]
        if($historical -and $relative -ceq 'driver/v2/session_coalesce.h'){if($hashes[$relative] -cne $beforeHeader){throw 'Prefilter old helper differs'};Require-CfHash (Join-Path $script:RepoRoot $relative) $afterHeader}
        else{Require-CfHash (Join-Path $script:RepoRoot $relative) $hashes[$relative]}
    };Require-CfHash (Join-Path $script:RepoRoot 'driver/v2/session_coalesce.h') $afterHeader
}
Assert-CfDependencies
function Assert-CfBuild($build){
    if($build.ip_version -notin 4,6 -or $build.mode -cnotin 'iso','ms' -or $build.guarded -isnot [bool]){throw 'Prefilter build selection differs'}
    $place=[IO.Path]::GetDirectoryName($build.path);$test=Join-Path $script:RepoRoot 'tests/model/coalesce_prefilter32_tests.c'
    if($build.path -cne (Join-Path $place 'coalesce-prefilter32.exe') -or $build.entry -cne (Join-Path $place 'entry.c') -or $build.test -cne $test){throw 'Prefilter output/test binding differs'}
    Require-CfHash $build.path $build.sha256;Require-CfHash $build.entry $build.entry_sha256;Require-CfHash $test $build.test_sha256
    Assert-CfSet @($build.sources|ForEach-Object {$_.relative}) $dependencies 'Prefilter exact dependency set differs'
    foreach($part in $build.sources){$source=Join-Path $script:RepoRoot $part.relative;$copy=Join-Path $place ('source/'+$part.relative)
        if($part.source -cne $source -or $part.path -cne $copy -or $part.source_sha256 -cne $part.sha256){throw 'Prefilter source copy binding differs'}
        Require-CfHash $source $part.source_sha256;Require-CfHash $copy $part.sha256
        $expected=if($part.relative -ceq 'driver/v2/session_coalesce.h'){$afterHeader}else{$hashes[$part.relative]};if($part.sha256 -cne $expected){throw 'Prefilter source differs from baseline contract'}
    }
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version
    $lines+='#define PB_PREFILTER_HEADER "'+(Join-Path $script:RepoRoot 'driver/v2/session_coalesce.h').Replace('\','/')+'"';$lines+='#define PB_PREFILTER_PACKET "'+(Join-Path $script:RepoRoot 'common/packet.c').Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    Require-CfText $build.entry (($lines -join "`n")+"`n")
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)","/Fo$place\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.arguments -join "`n") -cne ($flags -join "`n")){throw 'Prefilter compile arguments differ'}
}
function Read-CfMetrics([string[]]$Lines,[int]$Family){
    $rows=@($Lines|Where-Object {$_ -cmatch '^PREFILTER32 '});$summary=@($Lines|Where-Object {$_ -cmatch '^PASS: PREFILTER32 '})
    if($rows.Count -ne 62 -or $summary.Count -ne 1){throw 'Prefilter case count/format differs'}
    $keys=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$snapshots=0L;$outputBytes=0L;$scanBytes=0L;$injected=0;$disabled=0L
    foreach($row in $rows){
        if($row -cnotmatch '^PREFILTER32 ip([46]) protocol(6|17) shape([0-9]) length([0-9]+) measured([01]) groups([0-9]+) begin([0-9]+) append([0-9]+) finish([0-9]+) snapshot([0-9]+) output([0-9]+) checksum([0-9]+) copies([0-9]+) injection([01]) owners0$'){throw 'Prefilter case count/format differs'}
        $m=$Matches.Clone();$ip=[int]$m[1];$protocol=[int]$m[2];$shape=[int]$m[3];$length=[int]$m[4];$phase=[int]$m[5];$key=($protocol,$shape,$length,$phase)-join '/'
        $header=$(if($Family -eq 4){20}else{40})+$(if($protocol -eq 6){20}else{8});$good=$shape -in 0,4,6,7
        if($ip -ne $Family -or !$keys.Add($key) -or ($shape -eq 8 -and $protocol -ne 6) -or ($shape -eq 9 -and $length -notin 0,1,19,20,39,40,41) -or ($shape -ne 9 -and $length -ne $(if($shape -eq 4){$header+1}else{1500}))){throw 'Prefilter family/matrix differs'}
        $groups=if($good){if($shape -eq 6){8}else{1}}else{32};$begin=if($good){$groups}elseif($shape -eq 5){31}elseif($shape -eq 9 -and $length -ge $(if($Family -eq 4){20}else{40})){31}else{0}
        $append=if($good){31}else{0};$finish=if($good){$groups}else{0}
        $snapshot=switch($shape){0{32L*$length};1{31L*40};2{0L};3{0L};4{32L*$length};5{31L*$length};6{39L*$length};7{32L*$length};8{31L*40};9{31L*$length}}
        $out=if($good){[long]$groups*$header+32L*($length-$header)}else{0L}
        $scan=if($good -or $shape -eq 5){[long]$(if($shape -eq 6){39}elseif($shape -eq 5){31}else{32})*$(if($Family -eq 4){$length+8}else{$length-8})}elseif($shape -eq 9 -and $Family -eq 4 -and (($protocol -eq 6 -and $length -ge 41) -or ($protocol -eq 17 -and $length -ge 28))){620L}else{0L}
        $copies=switch($shape){0{32+[int]($length -gt 40)};1{31};2{0};3{0};4{32+[int]($length -gt 40)};5{62};6{47};7{33};8{31};9{31*(1+[int]($length -gt 40))}}
        $inject=[int]($shape -eq 7)
        if([int]$m[6] -ne $groups -or [int]$m[7] -ne $begin -or [int]$m[8] -ne $append -or [int]$m[9] -ne $finish -or [long]$m[10] -ne $snapshot -or [long]$m[11] -ne $out -or [long]$m[12] -ne $scan -or [int]$m[13] -ne $copies -or [int]$m[14] -ne $inject){throw 'Prefilter byte/call/ownership differs'}
        $snapshots+=[long]$m[10];$outputBytes+=[long]$m[11];$scanBytes+=[long]$m[12];$injected+=$inject;if($shape -in 1,8){$disabled+=[long]$m[10]}
    }
    # Exact key coverage, rather than trusting the program's case count alone.
    foreach($phase in 0,1){foreach($protocol in 6,17){foreach($shape in 0..7){$length=if($shape -eq 4){$(if($Family -eq 4){20}else{40})+$(if($protocol -eq 6){20}else{8})+1}else{1500};if(!$keys.Contains((($protocol,$shape,$length,$phase)-join '/'))){throw 'Prefilter family/matrix differs'}};if($protocol -eq 6 -and !$keys.Contains((($protocol,8,1500,$phase)-join '/'))){throw 'Prefilter family/matrix differs'};foreach($length in 0,1,19,20,39,40,41){if(!$keys.Contains((($protocol,9,$length,$phase)-join '/'))){throw 'Prefilter family/matrix differs'}}}}
    if($summary[0] -cnotmatch '^PASS: PREFILTER32 family([46]) cases62 warm31 measured31 handles([0-9]+)/([0-9]+) owners0$' -or [int]$Matches[1] -ne $Family -or $Matches[2] -cne $Matches[3]){throw 'Prefilter summary/resources differ'}
    return @{cases=62;warm_cases=31;measured_cases=31;snapshot_bytes=$snapshots;coalesced_output_bytes=$outputBytes;eligibility_checksum_bytes=$scanBytes;hostile_prefix_cases=$injected;wrong_mode_snapshot_bytes=$disabled;process_handles_before=[int]$Matches[2];process_handles_after=[int]$Matches[3];caller_joins=0}
}
function Invoke-CfProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$p=[Diagnostics.Process]::Start($info)
    try{$stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync();if(!$p.WaitForExit(60000)){$p.Kill($true);$p.WaitForExit();throw 'Prefilter watchdog'};$text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$p.ExitCode}finally{$p.Dispose()}
    $log=Join-Path (Split-Path $build.path) 'result.log';[IO.File]::WriteAllText($log,$text,$utf8);if($code){throw "Prefilter unit failed$code :$log"};$metrics=Read-CfMetrics @($text -split '\r?\n') $build.ip_version
    $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;exit_code=$code;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant();metrics=$metrics});Write-TestRecording 'running'
}
Start-TestRecording -Directory $directory -Name 'operation-results.json'
$builds=@();$first=$null
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guard in $false,$true){
    $build=Build-CoalescePrefilter32Tests -Directory (Join-Path $directory "ipv$family-$mode-$guard") -IpVersion $family -Mode $mode -Guarded:$guard
    if($build.ip_version -ne $family -or $build.mode -cne $mode -or $build.guarded -ne $guard){throw 'Prefilter requested build differs'};Assert-CfBuild $build;$builds+=$build;if(!$first){$first=$build};Invoke-CfProcess $build
}}}
$parser=@();[string[]]$original=Get-Content -LiteralPath $script:TestExecutions[0].log
foreach($kind in 'wrong-family','missing-row','duplicate-row','old-full-copy'){
    $changed=switch($kind){'wrong-family'{@($original|ForEach-Object {$_ -replace '^PREFILTER32 ip4 ','PREFILTER32 ip6 '})};'missing-row'{@($original|Where-Object {$_ -cnotmatch '^PREFILTER32 ip4 protocol6 shape0 length1500 measured0 '})};'duplicate-row'{@($original)+@($original|Where-Object {$_ -clike 'PREFILTER32 *'}|Select-Object -First 1)};'old-full-copy'{@($original|ForEach-Object {$_ -replace 'snapshot1240 output0','snapshot46500 output0'})}}
    $expected=switch($kind){'wrong-family'{'Prefilter family/matrix differs'};'old-full-copy'{'Prefilter byte/call/ownership differs'};default{'Prefilter case count/format differs'}};$reason=$null;try{Read-CfMetrics $changed 4|Out-Null}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Prefilter parser wrong rejection:$kind ($reason)"};$parser+=@{name=$kind;rejected=$true;reason=$reason}
}
$metadata=@();Assert-CfBuild ($first|ConvertTo-Json -Depth 15|ConvertFrom-Json)
foreach($kind in 'duplicate-source','source-hash','entry-path','compile-flags'){
    $changed=$first|ConvertTo-Json -Depth 15|ConvertFrom-Json;$expected=switch($kind){'duplicate-source'{$changed.sources[1]=$changed.sources[0];'Prefilter exact dependency set differs'};'source-hash'{$changed.sources[0].source_sha256='0'*64;$changed.sources[0].sha256='0'*64;'Prefilter raw hash differs'};'entry-path'{$changed.entry=$changed.test;'Prefilter output/test binding differs'};'compile-flags'{$changed.arguments=@($changed.arguments)+'/DPB_API_TESTS=1';'Prefilter compile arguments differ'}}
    $reason=$null;try{Assert-CfBuild $changed}catch{$reason=$_.Exception.Message};if($reason -cne $expected){throw "Prefilter metadata wrong rejection:$kind ($reason)"};$metadata+=@{name=$kind;rejected=$true;reason=$reason}
}
$contracts=@();$originalEntry=[IO.File]::ReadAllText($first.entry)
foreach($kind in 'api-test','assertions','mixed-family','fingerprint'){
    $place=Join-Path $directory "compiler-$kind";New-Item -ItemType Directory -Path $place -Force|Out-Null;$text=$originalEntry;$flags=@($first.arguments|Where-Object {$_ -notlike '/Fo*'})
    $message=switch($kind){'api-test'{$flags+='/DPB_API_TESTS=1';'Shipping coalesce prefilter paths required'};'assertions'{$flags=@($flags|Where-Object {$_ -ne '/DNDEBUG'});'Shipping coalesce prefilter configuration required'};'mixed-family'{$text=$text.Replace('#define PB_TEST_IP_VERSION 4','#define PB_TEST_IP_VERSION 0');'Select one IP family'};'fingerprint'{$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'coalesce prefilter requires shipping fingerprint'}}
    $entry=Join-Path $place 'entry.c';[IO.File]::WriteAllText($entry,$text,$utf8);$log=Join-Path $place 'compile.log';& cl.exe @flags /c $entry "/Fo$place\entry.obj" *> $log;$code=$LASTEXITCODE
    if(!$code -or ![regex]::IsMatch([IO.File]::ReadAllText($log),'(?m)error C(?:1189|2338):[^\r\n]*'+[regex]::Escape($message))){throw "Prefilter exact compiler guard failed:$kind"};$contracts+=@{name=$kind;exit_code=$code;expected_failure=$true;guard_message=$message;log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
Assert-CfDependencies
$cases=0;foreach($execution in $script:TestExecutions){$cases+=$execution.metrics.cases};if($script:TestExecutions.Count -ne 8 -or $cases -ne 496){throw 'Prefilter executed totals differ'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Prefilter source changed during audit'};Write-TestRecording 'passed'
@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$baselineIdentity;baseline_manifest=$manifestPath;baseline_manifest_sha256=(Get-FileHash $manifestPath).Hash.ToLowerInvariant();operations=$script:TestResultFile;builds=$builds;cases=$cases;compile_contracts=$contracts;parser_checks=$parser;metadata_checks=$metadata;valid_build_json_clone=$true;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=($dependencies.Count-[int]$historical);production_closure_changed=$historical;reviewed_delta=@($(if($historical){@{path='driver/v2/session_coalesce.h';before_sha256=$beforeHeader;after_sha256=$afterHeader}}));approved_repair_contract=@{path='driver/v2/session_coalesce.h';before_sha256=$beforeHeader;after_sha256=$afterHeader};kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests/model/coalesce_prefilter32_tests.c','tools/build/coalesce-prefilter32-tests.ps1','tools/audit/verify-coalesce-prefilter32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual complete kernel prefilter header and actual complete packet.c (only checksum symbol binding), plus unchanged linked core/coalesce. Fifteen exact raw transitive project dependencies and one generated entry are independently bound; no fixture source projection or historical out is needed in current mode. Ordinary policy-mismatched1500-byte32 inputs snapshot31*40 instead of31*1500; no core eligibility/output work. Selected full groups still snapshot exactly32*L, including one prefix plus disjoint suffix for the first input; flow4 boundaries preserve the original seven bounded lookahead snapshots, total39*L. Per-slot/per-byte tags reject any reread within one helper attempt while allowing separately bounded boundary lookahead. Core Begin/Append/Finish observers forward actual calls. Eligibility checksum bytes count coalesce calls into packet checksum helper, separately from packet-internal finalization and independent oracle checks. Full output bytes/state, first IPv4ID/SEQ/PSH/minhop, URO checksum-success zero-field representation, source/descriptor spans, source guards/private buffer guards and busy ownership are checked. Zero mask/both busy skip snapshots; small valid packets, invalid checksum and defensive direct helper spans0/1/19/20/39/40/41 are explicit. These short malformed spans are not claimed as successful native admission. One hostile shared-source protocol-byte injection occurs only after slot0 prefix40 was really copied; core still sees original valid private wire while the source oracle expects only that one changed shared byte with no checksum repair. This is fault injection, not permission to mutate a native/Rust loan concurrently. Each case closes every owned buffer/region; no events, threads, actual Register/NDIS/IRQL/old-Windows execution, timestamp scheduling, Submit/Commit or native CPU/performance claim. Optional guard mode adds pages to declared private buffers/region; ordinary mode retains byte guards. CPU A/B evidence is a separate uninstrumented host benchmark. Optional743-file1695 baseline is wholly authenticated; only the one exact reviewed header raw SHA pair may differ, all fourteen other declared dependencies remain byte-identical. Version2.0.0 unchanged.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
