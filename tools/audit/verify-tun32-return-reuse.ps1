param([string]$BaselineDirectory='out\return-wake-baseline\4fd68e95547d',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-return-reuse-tests.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
trap{$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'T32 return reuse requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='4fd68e95547d0066742c1014148caa05e7a3fdddaa9b9130ac63a4c602d89a54'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted' -or $manifest.version -cne '2.0.0'){throw 'Unexpected accepted T32 return reuse baseline'}
$oldPaths=@($manifest.source_paths);$propertyPaths=@($manifest.files.PSObject.Properties.Name)
if($oldPaths.Count -ne 663 -or $propertyPaths.Count -ne 663){throw 'Accepted baseline requires663 explicit original-order paths'}
$seen=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
$entries=@();foreach($relative in $oldPaths){
    if(!$seen.Add($relative) -or $propertyPaths[$entries.Count] -cne $relative){throw 'Baseline original path order/set is not exact'}
    $hash=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$relative){throw "Frozen baseline raw file differs:$relative"};$entries+=$relative+':'+$hash
}
$hash=[Security.Cryptography.SHA256]::Create();try{$canonical=[Convert]::ToHexString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant()}finally{$hash.Dispose()}
if($canonical -cne $accepted){throw '663 raw baseline files in the recorded legacy order do not reproduce accepted fingerprint'}
[string[]]$ordinalPaths=$oldPaths.Clone();[Array]::Sort($ordinalPaths,[StringComparer]::Ordinal)
$ordinalEntries=@($ordinalPaths|ForEach-Object {$_+':'+$manifest.files.$_});$hash=[Security.Cryptography.SHA256]::Create()
try{$ordinal=[Convert]::ToHexString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($ordinalEntries -join "`n")))).ToLowerInvariant()}finally{$hash.Dispose()}
$acceptedOrdinal='a44c68a654378dfcee14ca6c00b971dbedded5c4793a7d558bb2d08b4ad07da6'
if($ordinal -cne $acceptedOrdinal -or $manifest.ordinal_content_sha256 -cne $acceptedOrdinal){throw 'Independent typed-Ordinal baseline content hash differs'}
$dependencies=@(rg --files driver common api include);if($LASTEXITCODE){throw 'Cannot enumerate production closure'}
$dependencies+=@('tests\model\return_reclaim_tests.c','tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h','wintun.props')
function Assert-TrrDependencies {
    $changed=@();foreach($relative in $dependencies){$old=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant();if($old -cne $manifest.files.($relative.Replace('\','/'))){throw "Dependency baseline not authenticated:$relative"};Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative;if($old -cne (Get-FileHash (Join-Path $script:RepoRoot $relative)).Hash.ToLowerInvariant()){$changed+=$relative.Replace('\','/')}}
    if(@($changed).Count -ne 1 -or $changed[0] -cne 'driver/v2/session_describe.h'){throw 'Only the separately reviewed complete-file checksum-family repair may differ'}
}
Assert-TrrDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-return-reuse\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Require-TrrText([string]$path,[string]$expected){$sha=[Security.Cryptography.SHA256]::Create();try{$hash=[Convert]::ToHexString($sha.ComputeHash([Text.Encoding]::UTF8.GetBytes($expected))).ToLowerInvariant()}finally{$sha.Dispose()};if((Get-FileHash $path).Hash.ToLowerInvariant() -cne $hash){throw "Exact generated text/encoding differs:$path"}}
function Assert-TrrBuild($b){
    if($b.ip_version -notin @(4,6) -or $b.mode -notin @('iso','ms')){throw 'Unexpected T32 return mode/family'}
    $e=$b.extractions;$hosts=[ordered]@{'tests\model\return_reclaim_tests.c'='513c0bda45ef4b7ca59cb5e70ebf8e4b2dfcd93a54be3f791b38edfabb3f1653';'tests\model\kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'}
    if(@($e.hosts).Count -ne 2){throw 'Two original complete hosts required'}
    foreach($record in $e.hosts){if(!$hosts.Contains($record.relative) -or $record.source -cne (Join-Path $script:RepoRoot $record.relative) -or $record.sha256 -cne $hosts[$record.relative] -or (Get-FileHash $record.source).Hash.ToLowerInvariant() -cne $record.sha256 -or (Get-FileHash $record.path).Hash.ToLowerInvariant() -cne $record.sha256 -or $record.snapshot_sha256 -cne $record.sha256){throw 'Original hosted NBL ownership source differs'}}
    $hostSource=Join-Path $baseline 'tests\model\return_reclaim_tests.c';$body=[IO.File]::ReadAllText($hostSource)
    $before='CHECK(adapter && nbls && count==1 && !flags && !Indicated);';$after='CHECK(adapter && nbls && count && count<=PB_MAX_BATCH && !flags && !Indicated);'
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1 -or $e.observer_original -cne $before -or $e.observer_adapted -cne $after){throw 'Bounded indication observer contract differs'}
    $body=$body.Replace($before,$after);$includeChanges=[Collections.Generic.List[object]]::new();$base=Join-Path $script:RepoRoot 'tests\model'
    $body=[regex]::Replace($body,'"([^"\r\n]*\.[ch])"',{param($m);$value='"'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"';$includeChanges.Add(@{original=$m.Value;adapted=$value});$value})
    Require-TrrText $e.host $body
    if((Get-FileHash $e.host).Hash.ToLowerInvariant() -cne $e.host_sha256 -or @($e.include_changes).Count -ne $includeChanges.Count){throw 'Finite host projection hash/includes differ'}
    for($i=0;$i -lt $includeChanges.Count;++$i){if($e.include_changes[$i].original -cne $includeChanges[$i].original -or $e.include_changes[$i].adapted -cne $includeChanges[$i].adapted){throw 'Original include change is not exact'}}
    $driver=Join-Path $script:RepoRoot 'driver\wintun.c';$text=[IO.File]::ReadAllText($driver);$first='static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;';$last='#define TUN_FORCE_UNREGISTRATION';$a=$text.IndexOf($first);$z=$text.IndexOf($last,$a)
    if($a -lt 0 -or $z -le $a -or $e.outer.source -cne $driver -or $e.outer.source_sha256 -cne (Get-FileHash $driver).Hash.ToLowerInvariant() -or $e.outer.offset -ne $a -or $e.outer.length -ne $z-$a -or $e.outer.start_marker -cne $first -or $e.outer.end_marker -cne $last){throw 'Actual outer Return extraction source/bounds differ'}
    Require-TrrText $e.outer.path $text.Substring($a,$z-$a);if((Get-FileHash $e.outer.path).Hash.ToLowerInvariant() -cne $e.outer.sha256){throw 'Actual outer Return fragment hash differs'}
    $parts=@('driver\v2\session_state.h','driver\v2\session_progress.h','driver\v2\platform_nbl.h','driver\v2\session_fault.h','driver\v2\session_references.h','driver\v2\session_completion.h','driver\v2\session_return.h','driver\v2\session_nbl.h','driver\v2\session_coalesce.h','driver\v2\session_rx_stats.h','driver\v2\session_receive.h','driver\v2\session_drain.h','driver\v2\session_fault_drain.h','driver\v2\session_recycle.h','driver\v2\session_query.h','driver\v2\session_cleanup.h','api\v2\packetbridge.h','api\v2\session_state.h','api\v2\session_queue.h','common\v2_core.h','common\v2_core.c','common\platform.h','common\ethernet.c','common\ethernet.h','common\coalesce.c','common\coalesce.h','common\packet.c','common\packet.h','common\flow.h','common\return_diagnostics.h','include\packetbridge_version.h','include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_v2_abi.h','include\packetbridge_pool_pressure.h','tests\safety\guard_memory.h','wintun.props')
    if(@($e.parts).Count -ne $parts.Count -or @($e.parts.relative|Select-Object -Unique).Count -ne $parts.Count -or @(Compare-Object ($e.parts.relative|Sort-Object) ($parts|Sort-Object)).Count){throw 'Exact source input set differs'}
    foreach($part in $e.parts){$source=Join-Path $script:RepoRoot $part.relative;$sha=(Get-FileHash $source).Hash.ToLowerInvariant();if($part.source -cne $source -or $part.compiled_path -cne $source -or $part.source_sha256 -cne $sha -or $part.sha256 -cne $sha -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $sha){throw 'Whole original compiled source snapshot differs'}}
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_return_reuse_tests.c';$lines=@();if($b.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$b.ip_version;$lines+='#define PB_T32_RETURN_HOST "'+$e.host.Replace('\','/')+'"';$lines+='#define PB_T32_OUTER_RETURN "'+$e.outer.path.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    Require-TrrText $b.entry (($lines -join "`n")+"`n")
    if($b.test -cne $test -or (Get-FileHash $test).Hash.ToLowerInvariant() -cne $b.test_sha256 -or (Get-FileHash $b.entry).Hash.ToLowerInvariant() -cne $b.entry_sha256 -or (Get-FileHash $b.path).Hash.ToLowerInvariant() -cne $b.sha256){throw 'Complete entry/test/executable binding differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($b.mode)",('/Fo'+(Split-Path $b.path)+'\'))+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($b.arguments -join "`n") -cne ($flags -join "`n")){throw 'Exact T32 return compiler flags differ'}
}
function Assert-TrrRows([string]$text,[int]$family){
    if([regex]::Matches($text,'(?m)^FAMILY ip').Count -ne 1 -or [regex]::Match($text,'(?m)^FAMILY ip([46])\r?$').Groups[1].Value -cne [string]$family){throw 'T32 return selected-family witness differs'}
    $rows=@($text -split '\r?\n'|Where-Object {$_.StartsWith('RETURN32 ',[StringComparison]::Ordinal)});if($rows.Count -ne 96){throw 'T32 return requires96 checked case rows'}
    $at=0;foreach($round in 0,1){foreach($protocol in 6,17){foreach($minor in 20,89){foreach($manual in 0,1){foreach($wrap in 0,1){foreach($terminal in 0,1,2){
        $wake=if($terminal){3}else{2};$expected="RETURN32 ip$family proto$protocol ndis$minor manual$manual wrap$wrap terminal$terminal old32 prefix24 held8 reused24 total56 callbacks4 joins5 naturalFull1 repairs0 wakes$wake owners0"
        if($rows[$at++] -cne $expected){throw 'T32 return executed shape/order/metrics differ'}
    }}}}}}
    $s=[regex]::Match($text,'(?m)^RETURN32_SUMMARY ip([46]) cases96 warm48 measured48 joins480 callbacks384 naturalFull96 tokens5376 distinctNbls5376 metadataChecks6144 signals96 wakes256 handles(\d+)/(\d+) owners0 repairs0\r?$')
    if(!$s.Success -or [int]$s.Groups[1].Value -ne $family -or $s.Groups[2].Value -cne $s.Groups[3].Value -or [regex]::Matches($text,'(?m)^RETURN32_SUMMARY ').Count -ne 1 -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1 -or !$text.Contains('PASS: actual32 disordered returns naturally reuse24 generation2 slots while8 original owners remain; drain/fault witnesses and callback tails preserve resources; all56 tokens retire through actual DLL polling and every OS thread joins')){throw 'T32 return summary/resource/handle evidence differs'}
    return @{cases=96;warm=48;measured=48;os_thread_joins=480;return_callbacks=384;native_tokens_returned=5376;distinct_nbls=5376;nbl_metadata_checks=6144;natural_full_observations=96;all_returned_signals=96;native_kernel_wakes=256;process_handles_before=[int]$s.Groups[2].Value;process_handles_after=[int]$s.Groups[3].Value;repairs=0}
}
function Invoke-TrrProcess($build,[string]$work){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start actual T32 return regression'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'T32 return external60s watchdog'}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $work 'results.log';[IO.File]::WriteAllText($log,$text,$encoding);$metrics=$null;if(!$code){$metrics=Assert-TrrRows $text $build.ip_version}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;exit_code=$code;expected_exit_code=0;watchdog_ms=60000;log=$log;metrics=$metrics});Write-TestRecording $(if(!$code -and $metrics){'running'}else{'failed'});if($code -or !$metrics){throw "T32 return process failed:$code"};return @{log=$log;metrics=$metrics}
    }finally{$process.Dispose()}
}
function Test-TrrCompileContracts($build){
    $work=Join-Path $directory 'compile-contracts';New-Item -ItemType Directory -Path $work -Force|Out-Null;$results=@()
    foreach($name in @('no-ndebug','pb-api-tests','no-fingerprint','invalid-family')){
        $flags=@($build.arguments|Where-Object {$_ -notlike '/Fo*'});$entry=$build.entry
        switch($name){
            'no-ndebug' {$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'});$marker='C1189.*T32 return reuse requires NDEBUG'}
            'pb-api-tests' {$flags+='/DPB_API_TESTS=1';$marker='C1189.*T32 return reuse requires actual production paths'}
            'no-fingerprint' {$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});$marker='C2338.*T32 return reuse requires shipping fingerprint'}
            'invalid-family' {$body=[IO.File]::ReadAllText($entry);$old='#define PB_TEST_IP_VERSION 4';if([regex]::Matches($body,[regex]::Escape($old)).Count -ne 1){throw 'Invalid family marker is not unique'};$entry=Join-Path $work 'invalid-family.c';[IO.File]::WriteAllText($entry,$body.Replace($old,'#define PB_TEST_IP_VERSION 7'),$encoding);$marker='C1189.*T32 return reuse requires independent IPv4 or IPv6'}
        }
        $flags+='/Fo'+(Join-Path $work ($name+'.obj'));$log=Join-Path $work ($name+'.log');& cl.exe @flags /c $entry *> $log;$code=$LASTEXITCODE
        if(!$code -or [IO.File]::ReadAllText($log) -notmatch $marker){throw "Compiler did not reject precise T32 contract:$name"};$results+=@{name=$name;exit_code=$code;expected_rejection=$true;required_error=$marker;arguments=$flags;source=$entry;source_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log}
    }
    $global:LASTEXITCODE=0;return $results
}
Start-TestRecording $directory 'operation-results.json';$executables=@();$observations=@();$contracts=@()
foreach($family in 4,6){foreach($mode in 'iso','ms'){foreach($guarded in $false,$true){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "ipv$family\$mode-$memory";$built=Build-Tun32ReturnReuseTests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-TrrBuild $built;$record=Invoke-TrrProcess $built $work;$executables+=,$built;$observations+=@{ip_version=$family;mode=$mode;guarded=$guarded;log=$record.log;metrics=$record.metrics}
    if($family -eq 4 -and $mode -ceq 'iso' -and !$guarded){$contracts=@(Test-TrrCompileContracts $built)}
}}}
$wrongFamilyRejected=$false;try{$null=Assert-TrrRows ([IO.File]::ReadAllText($observations[0].log)) 6}catch{if($_.ToString() -notmatch 'T32 return selected-family witness differs'){throw};$wrongFamilyRejected=$true}
if(!$wrongFamilyRejected -or $contracts.Count -ne 4){throw 'T32 return guard/parser contracts incomplete'}
Assert-TrrDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Source changed during T32 return reuse audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_fingerprint_order='typed string array, Ordinal paths, unquoted full Git names';baseline_source_tree_sha256=$accepted;baseline_source_fingerprint_order=$manifest.source_fingerprint_order;baseline_ordinal_content_sha256=$acceptedOrdinal;baseline=$baseline;authenticated_baseline_files=663;production_changed=$true;reviewed_production_repairs=@('driver/v2/session_describe.h');unchanged_dependencies=$dependencies.Count-1;executables=$executables;observations=$observations;operations=$script:TestResultFile;compiler_contracts=$contracts;wrong_family_rejected=$wrongFamilyRejected;kernel_runtime_tested=$false;production_ready=$false;
    dependency_ledgers=@('network-dependency-ledger.ps1','checksum-family-ledger.ps1','lso-checksum-ledger.ps1'|ForEach-Object{@{path=(Join-Path $PSScriptRoot $_);sha256=(Get-FileHash (Join-Path $PSScriptRoot $_)).Hash.ToLowerInvariant()}});
    audit_sources=@('tests\model\tun32_return_reuse_tests.c','tools\build\tun32-return-reuse-tests.ps1','tools\audit\verify-tun32-return-reuse.ps1'|ForEach-Object{@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='New independent IPv4/IPv6 actual32 receive/return/reuse coverage, not a new production defect. The recovered accepted663-file4fd snapshot is authenticated from every raw byte hash and its canonical fingerprint; no deleted historical out evidence is claimed or needed. All driver/common/API/include closure bytes remain unchanged except the separately reviewed complete-file checksum-family Send Describe repair, admitted only by the strict network ledger; that Send path is not executed here. Exact two complete frozen source hosts are reused; only the old indication observer count==1 becomes bounded1..64 and quoted source includes become absolute. Actual whole Receive/NBL/Return/reference/completion/drain/query/quiesce/cleanup and actual DLL queue functions are compiled from original paths; real outer Return callback is extracted verbatim with offsets and raw hashes. No lock, reference, ownership transition or packet algorithm is replaced. Every case actual Reserve/Commit32 valid1500-byte TCP or UDP packets produces32 ordinary partial-MDL NBLs. Three concurrent permuted8-object return chains pause after releasing RxLock before record/reference retirement. Original queue owner actually Polls24, Reserve32 naturally returns24 generation2 loans, then Reserve1 naturally returnsPB_FULL with8 genuine old Published owners plus24 genuine loans. Actual Commit/Receive24 reuses exactly those freed slots while8 generation1 NBLs remain intact; all three old callback tails still hold independent pins and Active ownership. After those callbacks join, the remaining8 old and24 fresh NBLs form one exact reordered mixed-generation32 chain. Actual DLL Poll retires every token through checked PoolTransition before any fault input or Free. In Drain-only and both finalActive/fault-witness orders, valid completed NBLs have gone but Active still32; actual Fault and Quiesce helpers model a terminal transition between completed worker transactions, not fabricated shared-cursor corruption. QUERY must observe both the global lastActive release and private FaultQuiesced witness. When QUERY reportsSTOPPED, real session Free still waits on Returns before any resource release; the callback tail is released and its OS thread joins before cleanup join. No Halt-before-Pause, arbitrary NDIS reentrancy, fakeFULL, private-state corruption or reference/counter repair. Each case joins4 return callers plus1 Free caller, retires56 actual originals through DLL Poll, validates64 exact NBL payload/metadata observations and requires zero native/DLL queue owners, MDLs/NBLs/pools/objects/mappings. Only main thread uses public queue APIs and every such call has ended before freeing its test-owned user storage and starting native Free; no full DLL Open/Close/lifecycle claim. Full48 checked warm plus48 measured shapes independently cross TCP/UDP, runtime NDIS6.20/6.89, auto/manual kernel events, true uint64 wrap and three terminal orders. Every process enforces strict process-handle equality after checked warmup. Runtime PB_NBL_VERSION and untouched future-field sentinels test bounded old-format accessor behavior in full hosted NBL arrays, not real old-kernel object sizes or Windows7 execution. Coalescing is disabled, no NDIS worker loop/IRQL/device/driver/network/registry or CPU-time proof; native kernel notification counts2/3 are operation counts only. Guarded mode retains the frozen fixture guards for native pools/MDLs/NBLs/region; original test-owned user shell/slot allocations stay ordinary. Shipping NDEBUG/nonPB/65-byte fingerprint and separate family compile guards, four precise compiler rejections and wrong-family parser rejection remain enforced. Original NT_ASSERT=CHECK stays stronger. Version2.0.0.'}|
    ConvertTo-Json -Depth 22|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
