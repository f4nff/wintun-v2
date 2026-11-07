param([string]$BaselineDirectory='out/return-wake-baseline/4fd68e95547d',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/rejected-reuse32-tests.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Rejected reuse requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='4fd68e95547d0066742c1014148caa05e7a3fdddaa9b9130ac63a4c602d89a54'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.status -cne 'accepted' -or $manifest.source_tree_sha256 -cne $accepted){throw 'Unexpected accepted rejected-reuse baseline'}
$paths=[string[]]@($manifest.source_paths);$names=[string[]]@($manifest.files.PSObject.Properties.Name)
$set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
if($paths.Count -ne 663 -or $names.Count -ne 663){throw 'Restored baseline must contain exactly663 paths'}
foreach($path in $paths){if(!$set.Add($path)){throw 'Restored baseline source order contains duplicates'}}
foreach($path in $names){if(!$set.Contains($path)){throw 'Restored baseline source order/file map differs'}}
$baselineHashes=@{};foreach($path in $paths){$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Restored baseline raw bytes differ:$path"};$baselineHashes[$path]=$hash}
function Get-RrBaselineDigest([string[]]$order){$entries=@($order|ForEach-Object {$_+':'+$baselineHashes[$_]});return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant()}
$legacyDigest=Get-RrBaselineDigest $paths;$ordinalPaths=[string[]]$paths.Clone();[Array]::Sort($ordinalPaths,[StringComparer]::Ordinal);$ordinalDigest=Get-RrBaselineDigest $ordinalPaths
if($legacyDigest -cne $accepted -or $manifest.source_fingerprint_order -cne 'legacy PowerShell object-array/native-command order, explicitly preserved' -or $ordinalDigest -cne 'a44c68a654378dfcee14ca6c00b971dbedded5c4793a7d558bb2d08b4ad07da6' -or $ordinalDigest -cne $manifest.ordinal_content_sha256){throw 'Restored baseline legacy/Ordinal dual fingerprint differs'}
$dependencies=@(rg --files driver common api include);if($LASTEXITCODE){throw 'Cannot enumerate rejected reuse dependencies'}
$dependencies+=@('wintun.props','tests/model/native_reclaim_tests.c','tests/model/late_commit_tests.c','tests/model/kernel_nbl_fixture.h','tests/safety/guard_memory.h')
function Assert-RrDependencies {
    foreach($relative in $dependencies){$before=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $before).Hash.ToLowerInvariant()
        if($manifest.files.($relative.Replace('\','/')) -cne $hash){throw "Frozen rejected reuse manifest differs:$relative"}
        Assert-NetworkDependencyFilePair -Before $before -Current $current -Relative $relative
    }
}
Assert-RrDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out/rejected-reuse32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
function Get-RrPart($build,[string]$name){$parts=@($build.parts|Where-Object {$_.name -ceq $name});if($parts.Count -ne 1){throw "Nonunique rejected reuse part:$name"};return $parts[0]}
function Convert-RrIncludes([string]$body,[string]$source,[string]$select,[string]$replacement){
    return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq $select){$replacement}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $source) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
}
function Assert-RrBuild($build){
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or $build.variant -notin @('current','without-partial-success') -or @($build.parts).Count -ne 7 -or @($build.parts.name|Select-Object -Unique).Count -ne 7){throw 'Rejected reuse source/mode shape differs'}
    $sources=[ordered]@{queue=(Join-Path $script:RepoRoot 'api/v2/session_queue.h');reserve=(Join-Path $script:RepoRoot 'api/v2/session_queue.h');session=(Join-Path $script:RepoRoot 'api/v2/session.c');host=(Join-Path $script:RepoRoot 'tests/model/native_reclaim_tests.c');'open-close'=(Join-Path $script:RepoRoot 'tests/model/late_commit_tests.c');'nt-resource'=(Join-Path $script:SdkRoot "Include/$script:KitVersion/shared/ntstatus.h");'ndis-resource'=(Join-Path $script:SdkRoot "Include/$script:KitVersion/shared/ndis/status.h")}
    foreach($name in $sources.Keys){$part=Get-RrPart $build $name;$source=$sources[$name];$body=[IO.File]::ReadAllText($source);$offset=0;$length=$body.Length
        if($name -ceq 'reserve'){$offset=$body.IndexOf('static PB_RESULT WINAPI ReserveBatch(');$end=$body.IndexOf('static PB_RESULT WINAPI CancelBatch(',$offset);$length=$end-$offset}
        elseif($name -ceq 'open-close'){$offset=$body.IndexOf('static VOID OpenPeer(');$end=$body.IndexOf('static VOID RunLate(',$offset);$length=$end-$offset}
        elseif($name -in @('nt-resource','ndis-resource')){$pattern=if($name -ceq 'nt-resource'){'(?m)^#define STATUS_INSUFFICIENT_RESOURCES[^\r\n]*\r?\n'}else{'(?m)^#define NDIS_STATUS_RESOURCES[^\r\n]*\r?\n'};$matches=[regex]::Matches($body,$pattern);if($matches.Count -ne 1){throw 'Unique SDK resource status required'};$offset=$matches[0].Index;$length=$matches[0].Length}
        if($offset -lt 0 -or $length -le 0 -or $part.source -cne $source -or $part.offset -ne $offset -or $part.length -ne $length -or $part.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($part.path) -cne $body.Substring($offset,$length) -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw "Complete original source/span differs:$name"}
    }
    $queue=Get-RrPart $build 'queue';$reserve=Get-RrPart $build 'reserve';$queueBody=[IO.File]::ReadAllText($queue.path)
    $original='    return QueueLeave(q, reserved ? PB_OK : result);';$changed='    return QueueLeave(q, result);'
    if([regex]::Matches($queueBody,[regex]::Escape($original)).Count -ne 1 -or [regex]::Matches([IO.File]::ReadAllText($reserve.path),[regex]::Escape($original)).Count -ne 1 -or $build.mutation.original -cne $original -or $build.mutation.replacement -cne $changed -or $build.mutation.offset -ne $queueBody.IndexOf($original) -or $build.mutation.length -ne $original.Length){throw 'Only unique ReserveBatch prefix-success return may mutate'}
    $expected=if($build.variant -ceq 'current'){$queueBody}else{$queueBody.Replace($original,$changed)}
    if([IO.File]::ReadAllText($build.selected) -cne $expected -or (Get-FileHash $build.selected).Hash.ToLowerInvariant() -cne $build.selected_sha256){throw 'Selected queue exceeds the one return-expression mutation'}
    $queueEntry='#define WaitForMultipleObjects ReuseWait'+"`n"+'#include "'+$build.selected.Replace('\','/')+'"'+"`n"+'#undef WaitForMultipleObjects'+"`n"
    if([IO.File]::ReadAllText($build.queue_entry) -cne $queueEntry){throw 'Only actual queue Wait may bind its SDK observer'}
    $session=Get-RrPart $build 'session';$expected=Convert-RrIncludes ([IO.File]::ReadAllText($session.path)) $session.source 'session_queue.h' $build.queue_entry
    if([IO.File]::ReadAllText($build.session) -cne $expected){throw 'Actual complete session exceeded finite include binding'}
    $hostPart=Get-RrPart $build 'host';$hostBody=[IO.File]::ReadAllText($hostPart.path);$before='adapter&&lists&&count==2&&!flags';$after='adapter&&lists&&count>0&&count<=32&&!flags'
    if([regex]::Matches($hostBody,[regex]::Escape($before)).Count -ne 1){throw 'Original indication count guard not unique'}
    $expected=Convert-RrIncludes ($hostBody.Replace($before,$after)) $hostPart.source '../../api/v2/session.c' $build.session
    if([IO.File]::ReadAllText($build.host) -cne $expected){throw 'Original native host exceeded count guard and absolute includes'}
    $open=Get-RrPart $build 'open-close';$body=[IO.File]::ReadAllText($open.path);$before='PB_CONFIG config={sizeof(config),queues,64,128,0,0};';$after='PB_CONFIG config={sizeof(config),queues,32,32,0,0};'
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1 -or [IO.File]::ReadAllText($build.open_close) -cne $body.Replace($before,$after)){throw 'Original Open/Finish changed beyond legal32/32 geometry'}
    $nt=Get-RrPart $build 'nt-resource';$status=Get-RrPart $build 'ndis-resource'
    if([IO.File]::ReadAllText($build.resource_status) -cne ('typedef ULONG NDIS_STATUS;'+"`n"+[IO.File]::ReadAllText($nt.path)+[IO.File]::ReadAllText($status.path))){throw 'Actual SDK resource status aggregate differs'}
    $test=Join-Path $script:RepoRoot 'tests/model/rejected_reuse32_tests.c';$lines=@(('#define PB_NATIVE_RECLAIM_CLEANUP_SOURCE "'+(Join-Path $script:RepoRoot 'driver/v2/session_cleanup.h').Replace('\','/')+'"'),('#define PB_REUSE_HOST "'+$build.host.Replace('\','/')+'"'),('#define PB_REUSE_OPEN_CLOSE "'+$build.open_close.Replace('\','/')+'"'),('#define PB_REUSE_RESOURCE_STATUS "'+$build.resource_status.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    if($build.test.path -cne $test -or $build.test.sha256 -cne (Get-FileHash $test).Hash.ToLowerInvariant() -or [IO.File]::ReadAllText($build.entry) -cne (($lines -join "`n")+"`n") -or $build.entry_sha256 -cne (Get-FileHash $build.entry).Hash.ToLowerInvariant() -or $build.sha256 -cne (Get-FileHash $build.path).Hash.ToLowerInvariant()){throw 'Complete entry/test/executable binding differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$(Split-Path $build.path)\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'};if($build.variant -ceq 'without-partial-success'){$flags+='/DPB_REJECTED_WITHOUT_PARTIAL_SUCCESS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Exact shipping compiler configuration differs'}
}
function Assert-RrRows([string]$text,[int]$family,[bool]$control,[bool]$negative){
    $witness=[regex]::Matches($text,'(?m)^FAMILY: IPv([46]) only\r?$');if($witness.Count -ne 1 -or [int]$witness[0].Groups[1].Value -ne $family -or [regex]::Matches($text,'(?m)^FAMILY:').Count -ne 1){throw 'Wrong/missing independent family witness'}
    $rows=[regex]::Matches($text,'(?m)^REUSE32 phase=(warm|measure) ip([46]) protocol(6|17) ndis(60014|60059) failed(0|6|30) wrap([01]) poll=1/32 reserve=1/32 partialResult=(0|2) gen=2 wait=1/1 statuses=2/4 nonfatal cancel=1/32 publicJoins=2 returnJoins=2 finalReturn=31 callbackJoins=([1-9][0-9]*) owners=0\r?$')
    $warm=if($control){2}else{24};$count=2*$warm;$callbacks=0;$partial=if($negative){2}else{0}
    if($rows.Count -ne $count -or [regex]::Matches($text,'(?m)^REUSE32 ').Count -ne $count){throw 'Missing/extra rejected reuse case rows'}
    for($i=0;$i -lt $count;++$i){$shape=$i%$warm;$phase=if($i -lt $warm){'warm'}else{'measure'}
        if($control){$protocol=if($shape){17}else{6};$version=if($shape){'60059'}else{'60014'};$slot=if($shape){30}else{6};$wrap=$shape}
        else{$version=if($shape -ge 12){'60059'}else{'60014'};$protocol=if([int][Math]::Floor($shape/6)%2){17}else{6};$slot=@(0,6,30)[[int][Math]::Floor($shape/2)%3];$wrap=$shape%2}
        $row=$rows[$i];$joined=[int]$row.Groups[8].Value
        if($row.Groups[1].Value -cne $phase -or [int]$row.Groups[2].Value -ne $family -or [int]$row.Groups[3].Value -ne $protocol -or $row.Groups[4].Value -cne $version -or [int]$row.Groups[5].Value -ne $slot -or [int]$row.Groups[6].Value -ne $wrap -or [int]$row.Groups[7].Value -ne $partial -or $joined -gt 32){throw 'Rejected reuse exact per-case family/protocol/ownership/stage mismatch'}
        $callbacks+=$joined
    }
    $summary=[regex]::Match($text,'(?m)^PASS: REUSE32 ip([46]) cases(\d+) warm(\d+) measured(\d+) partialFailures(\d+) publicJoins(\d+) returnJoins(\d+) callbackJoins(\d+) handles(\d+)/(\d+) owners=0\r?$')
    if(!$summary.Success -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1 -or [int]$summary.Groups[1].Value -ne $family -or [int]$summary.Groups[2].Value -ne $count -or [int]$summary.Groups[3].Value -ne $warm -or [int]$summary.Groups[4].Value -ne $warm -or [int]$summary.Groups[5].Value -ne $(if($negative){$count}else{0}) -or [int]$summary.Groups[6].Value -ne 2*$count -or [int]$summary.Groups[7].Value -ne 2*$count -or [int]$summary.Groups[8].Value -ne $callbacks -or $summary.Groups[9].Value -cne $summary.Groups[10].Value){throw 'Rejected reuse summary/join sum/strict handle equality differs'}
    $markers=[regex]::Matches($text,'(?m)^PARTIAL_RESERVE_FAILURE_WITH_OWNED_PREFIX: actual PB_FULL/count1 preserved, same owner cancelled real loan after stop; all owners callbacks native workers and resources joined/reclaimed\r?$')
    if($markers.Count -ne [int]$negative -or [regex]::Matches($text,'(?m)^PARTIAL_RESERVE_FAILURE').Count -ne [int]$negative){throw 'Safe142 post-cleanup witness differs'}
    return @{cases=$count;checked_warmup_schedules=$warm;measured_schedules=$warm;public_owner_joins=2*$count;native_return_joins=2*$count;callback_os_thread_joins=$callbacks;partial_failures=$(if($negative){$count}else{0});process_handles_before=[int]$summary.Groups[9].Value;process_handles_after=[int]$summary.Groups[10].Value}
}
function Invoke-RrProcess($build,[bool]$control){
    $negative=$build.variant -ceq 'without-partial-success';$expected=if($negative){142}else{0};$arguments=if($control){@('142')}else{@()}
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start rejected reuse test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Rejected reuse30s watchdog'}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path (Split-Path $build.path) $(if($control){'control.log'}else{'full.log'});[IO.File]::WriteAllText($log,$text,$utf8)
        $metrics=$null;if($code -eq $expected){$metrics=Assert-RrRows $text $build.ip_version $control $negative}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=$arguments;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;variant=$build.variant;control=$control;exit_code=$code;expected_exit_code=$expected;log=$log;watchdog_ms=30000;metrics=$metrics})
        Write-TestRecording $(if($metrics){'running'}else{'failed'});if(!$metrics){throw "Rejected reuse unexpected exit:$code"};Write-Output "IPv$($build.ip_version)/$($build.mode)/$($build.variant) control=$control cases$($metrics.cases) handles$($metrics.process_handles_before)/$($metrics.process_handles_after)"
    }finally{$process.Dispose()}
}
function Test-RrCompileContracts($build){
    $work=Join-Path $directory 'compile-contracts';New-Item -ItemType Directory -Path $work -Force|Out-Null;$results=@()
    foreach($name in @('no-ndebug','pb-api-tests','invalid-family','no-fingerprint')){
        $flags=@($build.flags|Where-Object {$_ -notlike '/Fo*'});$pattern=''
        switch($name){
            'no-ndebug' {$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'});$pattern='C1189.*Rejected reuse requires NDEBUG'}
            'pb-api-tests' {$flags+='/DPB_API_TESTS=1';$pattern='C1189.*Rejected reuse requires real shipping API paths'}
            'invalid-family' {$flags=@($flags|Where-Object {$_ -notlike '/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=7';$pattern='C1189.*Select independent IPv4 or IPv6'}
            'no-fingerprint' {$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});$pattern='C2338.*Shipping source fingerprint required'}
        }
        $flags+=('/Fo'+(Join-Path $work "$name.obj"));$log=Join-Path $work "$name.log";& cl.exe @flags /c $build.entry *> $log;$code=$LASTEXITCODE
        if(!$code -or [IO.File]::ReadAllText($log) -notmatch $pattern){throw "Rejected reuse compiler guard missed:$name"};$results+=@{name=$name;exit_code=$code;required_error=$pattern;expected_rejection=$true;log=$log;arguments=$flags}
    };$global:LASTEXITCODE=0;return $results
}
Start-TestRecording $directory 'operation-results.json'
$builds=@();$compiler=@()
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-partial-success')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "ipv$family/$mode-$memory/$variant"
    $build=Build-RejectedReuse32Tests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded -Variant $variant
    Assert-RrDependencies;Assert-RrBuild $build
    if($variant -ceq 'current'){Invoke-RrProcess $build $false};Invoke-RrProcess $build $true;$builds+=,$build
    if($family -eq 4 -and $mode -ceq 'iso' -and !$guarded -and $variant -ceq 'current'){$compiler=@(Test-RrCompileContracts $build)}
}}}}
$families=@();foreach($family in @(4,6)){$runs=@($script:TestExecutions|Where-Object ip_version -eq $family);$cases=($runs.metrics.cases|Measure-Object -Sum).Sum;$warm=($runs.metrics.checked_warmup_schedules|Measure-Object -Sum).Sum;$measured=($runs.metrics.measured_schedules|Measure-Object -Sum).Sum
    if($runs.Count -ne 12 -or $cases -ne 224 -or $warm -ne 112 -or $measured -ne 112){throw 'Rejected reuse family matrix incomplete'};$families+=@{ip_version=$family;processes=$runs.Count;cases=$cases;checked_warmup_schedules=$warm;measured_schedules=$measured}}
$log=($script:TestExecutions|Where-Object {$_.ip_version -eq 4 -and !$_.control -and !$_.guarded -and $_.mode -ceq 'iso'}).log;$wrong=$false
try{$null=Assert-RrRows ([IO.File]::ReadAllText($log)) 6 $false $false}catch{if($_.ToString() -notmatch 'Wrong/missing independent family witness'){throw};$wrong=$true}
if(!$wrong -or $compiler.Count -ne 4){throw 'Rejected reuse compiler/family contracts incomplete'}
Assert-RrDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Source changed during rejected reuse audit'}
$repairs=@($dependencies|Where-Object {(Get-FileHash (Join-Path $baseline $_)).Hash -cne (Get-FileHash (Join-Path $script:RepoRoot $_)).Hash})
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;baseline_source_paths_count=$paths.Count;baseline_fingerprint_order=$manifest.source_fingerprint_order;baseline_ordinal_content_sha256=$ordinalDigest;production_changed=($repairs.Count -gt 0);reviewed_dependency_repairs=$repairs;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count-$repairs.Count;kernel_runtime_tested=$false;production_ready=$false;executables=$builds;operations=$script:TestResultFile;families=$families;compiler_contracts=$compiler;wrong_family_rejected=$wrong;
    dependency_ledgers=@('network-dependency-ledger.ps1','checksum-family-ledger.ps1','lso-checksum-ledger.ps1'|ForEach-Object {@{path=('tools/audit/'+$_);sha256=(Get-FileHash (Join-Path $PSScriptRoot $_)).Hash.ToLowerInvariant()}});
    audit_sources=@('tests/model/rejected_reuse32_tests.c','tools/build/rejected-reuse32-tests.ps1','tools/audit/verify-rejected-reuse32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Two real public queue-owner threads use actual nonPB/NDEBUG DLL Reserve/Commit/Wait/Poll/Cancel and lifecycle, sharing its mapping with actual SYS Receive/NBL/Return/Drain/Query/Unregister/Free. Native registration and idle-only kernel workers come from the complete unchanged native-reclaim ledger; exact old Open/Finish is extracted with only geometry64/128 to32/32. Full queue/session/host and SDK resource status spans are byte/hash bound. Only quoted includes and the old indication count2 guard become bounded1..32; new C checks exact31/32 chains, original-token order, partial MDL addresses, complete1500-byte valid IPv4/IPv6 TCP/UDP data and two opaque64-bit application markers. Queue0 real tracked construction fails the selected first/seventh/thirty-first NBL allocation, producing per-packet PB_FULL2 and31 indicated Active originals. Queue1 controlled owned NBL return status uses the actual SDK RESOURCES value, a defensive boundary input without claiming that a Windows protocol stack naturally writes receive-return statuses; actual Return maps it to packet rejection4, not API PB_CORRUPT. Real auto events and actual SDK Wait entry wake the owners; the entry marker is not scheduler-park evidence. Poll1/32 returns PB_OK and frees exactly1/32 slots; Reserve32 naturally accepts1/32 with generation2 while old31 remain native-owned. BeginStop then makes Commit return STOPPED/count0, actual owners cancel their1/32 real reservations and join before final Close. NativeActive31 independently blocks reclamation until the actual final Return31; trusted QUERY/normal completion retirement/physical close and all native workers/callback OS-thread handles converge. One existing internal fixture reference protects post-successful-Close work/pool observations; no consumed public API call, extra new Retain or counter/reference/generation repair. NDIS6.20/6.89 are runtime metadata-gate models with full-size hosted NBL arrays: unavailable fields keep poison, supported fields clear, not an old-kernel layout/runtime proof. Ordinary/guarded ISO/MS separate family executables run48 checked schedules each (24warm24measure), current CLI4 and negative CLI4 give24processes448cases. Safe142 changes only the actual unique ReserveBatch final prefix-success expression to raw result: preserves actual PB_FULL/count1 and its loan, records it without overriding the return, then the same owner normally cancels after stop and full cleanup precedes142. This is a regression mutation, not a found production failure. Callback joins are accumulated from each actual captured handle, never a fixed SDK callback count. Guard protects original core/pools/NBL/MDL/region allocations; static Context and packet oracle stack stay ordinary. Recreated accepted4fd is the sole historical baseline; every current production/old-host dependency is raw-identical except explicit exact complete-file network ledgers, never a reconstructed deleted artifact. Four precise compiler rejects and wrong-family output rejection remain recorded. No devices/driver installation/registry/network, native NDIS/IRQL/Verifier, true DLL unload or CPU/latency/throughput claim; version2.0.0 remains fixed.'}|
    ConvertTo-Json -Depth 17|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
