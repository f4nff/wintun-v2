param([string]$BaselineDirectory='out\tun32-edge-baseline\f8745a1b269f',[string]$OutputDirectory='',
    [ValidateSet(0,4,6)][int]$IpVersion=0,[switch]$SplitIpVersions)
$ErrorActionPreference='Stop'
if($SplitIpVersions -and $IpVersion){throw 'Select either a single IP version or split IPv4/IPv6 runs'}
$ipVersions=if($SplitIpVersions){@(4,6)}else{@($IpVersion)}
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-coalesce-drain-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'TUN32 coalesce drain requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='f8745a1b269f1f6ad5097b5acc9c7806f27d720725a45baf2fd217dca0c5bd89'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected accepted coalesce drain baseline'}
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate SYS dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h','include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h','tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h')
function Assert-TcdDependencies {
    foreach($relative in $dependencies){$old=(Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant();if($manifest.files.($relative.Replace('\','/')) -cne $old){throw "Accepted baseline dependency changed:$relative"}
        if($relative.Replace('\','/') -cin @('common/offload_policy.c','driver/v2/session_describe.h')){Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative}
        elseif((Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $relative)).Hash.ToLowerInvariant() -cne $old){throw "Immutable coalesce drain dependency changed:$relative"}}
}
Assert-TcdDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-coalesce-drain\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
function Part-Tcd($build,[string]$name){$found=@($build.extractions.generated|Where-Object {$_.name -ceq $name});if($found.Count -ne 1){throw "Nonunique generated coalesce drain part:$name"};return $found[0]}
function Assert-TcdProjection($build){
    $e=$build.extractions;$hostNames=@('tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h')
    if(@($e.inputs).Count -ne 2 -or @($e.inputs.relative|Select-Object -Unique).Count -ne 2 -or @($e.generated).Count -ne 3){throw 'Coalesce drain host evidence shape changed'}
    foreach($relative in $hostNames){$records=@($e.inputs|Where-Object {$_.relative -ceq $relative});if($records.Count -ne 1){throw 'Missing frozen coalesce drain host'};$record=$records[0];$source=Join-Path $script:RepoRoot $relative;$old=Join-Path $baseline $relative
        if($record.path -cne $source -or $record.sha256 -cne (Get-FileHash $old).Hash.ToLowerInvariant() -or $record.snapshot_sha256 -cne $record.sha256 -or (Get-FileHash $record.snapshot).Hash.ToLowerInvariant() -cne $record.sha256){throw 'Whole frozen coalesce drain host changed'}}
    $production=@('receive','nbl','coalesce','return','completion','references','recycle','rx_stats','drain','query','worker','fault_drain')|ForEach-Object {"driver\v2\session_$_.h"}
    $production+=@('driver\v2\platform_nbl.h','common\v2_core.c','common\coalesce.c','common\packet.c','common\ethernet.c','common\flow.c')
    if(@($e.parts).Count -ne 18 -or @($e.parts.relative|Select-Object -Unique).Count -ne 18){throw 'Coalesce drain actual production evidence shape changed'}
    foreach($relative in $production){$records=@($e.parts|Where-Object {$_.relative -ceq $relative});if($records.Count -ne 1){throw 'Missing complete production part'};$record=$records[0];$source=Join-Path $script:RepoRoot $relative;$old=Join-Path $baseline $relative
        if($record.source -cne $source -or $record.compiled_path -cne $source -or $record.offset -ne 0 -or $record.length -ne [IO.File]::ReadAllText($old).Length -or $record.source_sha256 -cne (Get-FileHash $old).Hash.ToLowerInvariant() -or $record.sha256 -cne $record.source_sha256 -or (Get-FileHash $record.path).Hash.ToLowerInvariant() -cne $record.sha256){throw "Whole actual production part changed:$relative"}}
    foreach($record in $e.generated){if((Get-FileHash $record.path).Hash.ToLowerInvariant() -cne $record.sha256){throw 'Generated coalesce drain artifact changed'}}
    $hostPart=Part-Tcd $build 'drain';$relative='tests\model\drain_boundary_tests.c';$base=Split-Path (Join-Path $script:RepoRoot $relative);$expected=[IO.File]::ReadAllText((Join-Path $baseline $relative))
    $expected=[regex]::Replace($expected,'"([^"\r\n]*\.[ch])"',{param($m);$path=$m.Groups[1].Value;if(![IO.Path]::IsPathFullyQualified($path)){$path=[IO.Path]::GetFullPath((Join-Path $base $path))};'"'+$path.Replace('\','/')+'"'})
    if([IO.File]::ReadAllText($hostPart.path) -cne $expected){throw 'Frozen drain host projection exceeded absolute includes'}
    $nbl=Part-Tcd $build 'nbl-observer';$expected=@'
static VOID ObserveNblCopy(VOID *,const VOID *,SIZE_T);
static VOID **ObserveNblInfo(NET_BUFFER_LIST *,ULONG);
#pragma push_macro("RtlCopyMemory")
#pragma push_macro("NET_BUFFER_LIST_INFO")
#undef RtlCopyMemory
#undef NET_BUFFER_LIST_INFO
#define RtlCopyMemory ObserveNblCopy
#define NET_BUFFER_LIST_INFO(n,f) (*ObserveNblInfo(n,f))
#include PB_TUN32_COALESCE_NBL
#pragma pop_macro("NET_BUFFER_LIST_INFO")
#pragma pop_macro("RtlCopyMemory")
'@
    if([IO.File]::ReadAllText($nbl.path) -cne $expected){throw 'NBL observer forwarding changed'}
    $receive=Part-Tcd $build 'receive-observer';$expected=@'
static VOID ObserveSnapshot(VOID *,const VOID *,SIZE_T);
static PB_PACKET_STATUS ObserveBegin(PB_COALESCE *,uint8_t *,uint32_t,const uint8_t *,uint32_t);
static PB_PACKET_STATUS ObserveAppend(PB_COALESCE *,const uint8_t *,uint32_t);
static PB_PACKET_STATUS ObserveFinish(PB_COALESCE *);
#pragma push_macro("RtlCopyMemory")
#undef RtlCopyMemory
#define RtlCopyMemory ObserveSnapshot
#define PbCoalesceBegin ObserveBegin
#define PbCoalesceAppend ObserveAppend
#define PbCoalesceFinish ObserveFinish
#include PB_TUN32_COALESCE_CORE
#undef PbCoalesceBegin
#undef PbCoalesceAppend
#undef PbCoalesceFinish
#pragma pop_macro("RtlCopyMemory")
static PB_RESULT ObservePop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t,uint32_t *);
static VOID ObserveIndicate(VOID *,NET_BUFFER_LIST *,ULONG,ULONG);
#define PbQueuePopBudget ObservePop
#define PbIndicateReceive ObserveIndicate
#include PB_TUN32_COALESCE_RECEIVE
#undef PbQueuePopBudget
#undef PbIndicateReceive
'@
    if([IO.File]::ReadAllText($receive.path) -cne $expected){throw 'Receive/coalesce observer forwarding changed'}
    $bindings=[ordered]@{PB_NBL_SOURCE=$nbl.path;PB_RECEIVE_SOURCE=$receive.path;PB_TUN32_COALESCE_DRAIN_HOST=$hostPart.path;PB_TUN32_COALESCE_NBL=(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h');PB_TUN32_COALESCE_CORE=(Join-Path $script:RepoRoot 'driver\v2\session_coalesce.h');PB_TUN32_COALESCE_RECEIVE=(Join-Path $script:RepoRoot 'driver\v2\session_receive.h')}
    if($build.ip_version -notin @(0,4,6)){throw 'Invalid coalesce drain family selector'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};if($build.ip_version){$lines+='#define PB_TEST_IP_VERSION '+$build.ip_version};foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_coalesce_drain_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`n")+"`n") -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256 -or $build.test -cne $test -or (Get-FileHash $test).Hash.ToLowerInvariant() -cne $build.test_sha256 -or (Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256){throw 'Complete compile entry/test/executable changed'}
    if(-not $build.ndebug -or $build.pb_api_tests -or @($build.arguments|Where-Object {$_ -eq '/DNDEBUG'}).Count -ne 1 -or @($build.arguments|Where-Object {$_ -like '/DPB_API_TESTS*'}).Count -or @($build.arguments|Where-Object {$_ -ceq "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"}).Count -ne 1 -or @($build.arguments|Where-Object {$_ -ceq "/volatile:$($build.mode)"}).Count -ne 1){throw 'Coalesce drain shipping flags changed'}
}
function Assert-TcdRows([string]$log,[ValidateSet(0,4,6)][int]$Version){
    $text=[IO.File]::ReadAllText($log);$rows=[regex]::Matches($text,'(?m)^CASE: COALESCE32 ip(\d+) proto(\d+) mode(\d+) ndis([0-9a-f]+) pops(\d+) nbls(\d+) partial(\d+) snapshotMdls(\d+) prefix(\d+) record(\d+) snapshot(\d+) output(\d+) workerJoins(\d+)\r?$')
    $warm=if($Version){12}else{24};$expectedCases=2*$warm;$expectedJoins=if($Version){8}else{16}
    if($rows.Count -ne $expectedCases -or ([regex]::Matches($text,'(?m)^CASE:')).Count -ne $expectedCases){throw 'Coalesce drain executed case rows missing'}
    $familyRows=[regex]::Matches($text,'(?m)^FAMILY: IPv([46]) only\r?$');$familyLines=[regex]::Matches($text,'(?m)^FAMILY:')
    if($Version){if($familyRows.Count -ne 1 -or $familyLines.Count -ne 1 -or [int]$familyRows[0].Groups[1].Value -ne $Version){throw 'Coalesce drain selected family witness missing'}}elseif($familyLines.Count){throw 'Legacy mixed coalesce drain gained a family witness'}
    $totals=[ordered]@{cases=$rows.Count;worker_joins=0;private_snapshot_bytes=0L;coalesced_output_bytes=0L;indicated_nbls=0;partial_mdls=0;private_snapshot_mdls=0;classification_prefix_bytes=0;record_bytes=0}
    for($i=0;$i -lt $rows.Count;++$i){$shape=$i%$warm;$ip=if($Version){$Version}else{4+2*[int][Math]::Floor($shape/12)};$protocol=@(6,17)[[int][Math]::Floor($shape/6)%2];$scenario=$shape%6;$early=if($scenario -lt 3){$scenario}else{0};$head=$(if($ip -eq 4){20}else{40})+$(if($protocol -eq 6){20}else{8})
        $expected=@($ip,$protocol,$scenario,$(if($protocol -eq 6){'6001e'}else{'60059'}),$(if($scenario -lt 4){2}else{1}),$(if($scenario -lt 3){30-$early}else{14}),$(if($scenario -lt 3){28-2*$early}else{12}),(2+$early),(40*$(if($scenario -lt 3){30-$early}else{14})),$(if($scenario -lt 3){2048}else{1024}),((6+3*$early)*32768),((2+$early)*($head+2*(32768-$head))),$(if($scenario -ge 4){1}else{0}))
        for($field=0;$field -lt $expected.Count;++$field){if($rows[$i].Groups[$field+1].Value -cne [string]$expected[$field]){throw "Coalesce drain case$i field$field actual=$($rows[$i].Groups[$field+1].Value) expected=$($expected[$field])"}}
        $totals.worker_joins+=[int]$expected[12];$totals.private_snapshot_bytes+=[long]$expected[10];$totals.coalesced_output_bytes+=[long]$expected[11];$totals.indicated_nbls+=[int]$expected[5];$totals.partial_mdls+=[int]$expected[6];$totals.private_snapshot_mdls+=[int]$expected[7];$totals.classification_prefix_bytes+=[int]$expected[8];$totals.record_bytes+=[int]$expected[9]
    }
    $summary=[regex]::Match($text,'(?m)^PASS: TUN32 coalesce drain cases(\d+) warm(\d+) measure(\d+) workerJoins(\d+) handles(\d+)/(\d+); all NBL MDL slots credits references buffers and worker threads reclaimed\r?$')
    if(-not $summary.Success -or [int]$summary.Groups[1].Value -ne $expectedCases -or [int]$summary.Groups[2].Value -ne $warm -or [int]$summary.Groups[3].Value -ne $warm -or [int]$summary.Groups[4].Value -ne $expectedJoins -or $summary.Groups[5].Value -ne $summary.Groups[6].Value -or $totals.worker_joins -ne $expectedJoins){throw 'Coalesce drain summary/join/handle ledger changed'}
    $totals.process_handles_before=[int]$summary.Groups[5].Value;$totals.process_handles_after=[int]$summary.Groups[6].Value;return $totals
}
Start-TestRecording $directory 'operation-results.json';$executables=@();$metrics=@()
foreach($selectedIpVersion in $ipVersions){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$prefix=if($selectedIpVersion){"ipv$selectedIpVersion-"}else{''};$work=Join-Path $directory "$prefix$mode-$memory";$build=Build-Tun32CoalesceDrainTests -Directory $work -Mode $mode -Guarded:$guarded -IpVersion $selectedIpVersion;Assert-TcdProjection $build
    $log=Join-Path $work 'results.log';$errors=Join-Path $work 'stderr.log';$process=Start-Process -FilePath $build.path -WindowStyle Hidden -PassThru -RedirectStandardOutput $log -RedirectStandardError $errors
    if(-not $process.WaitForExit(30000)){Stop-Process -Id $process.Id -Force;$process.WaitForExit();throw 'Coalesce drain executable exceeded30s watchdog'};$code=$process.ExitCode;$process.Dispose()
    $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();ip_version=$build.ip_version;exit_code=$code;expected_exit_code=0;log=$log;stderr=$errors});Write-TestRecording $(if($code -eq 0){'running'}else{'failed'})
    if($code -ne 0 -or (Get-Item -LiteralPath $errors).Length){throw "Coalesce drain process failed:$mode/$memory exit$code"};$counts=Assert-TcdRows $log $selectedIpVersion;$metrics+=@{ip_version=$build.ip_version;mode=$mode;guarded=$guarded;metrics=$counts};$executables+=$build;Write-Output ((Get-Content -LiteralPath $log|Where-Object {$_ -like 'PASS:*'}) -join "`n")
}}}
Assert-TcdDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during coalesce drain checks'}
Write-TestRecording 'passed'
$audit=Join-Path $script:RepoRoot 'tools\audit\verify-tun32-coalesce-drain.ps1';$builder=Join-Path $script:RepoRoot 'tools\build\tun32-coalesce-drain-tests.ps1'
[ordered]@{result='passed';version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;baseline_source_tree_sha256=$accepted;ip_versions=$ipVersions;split_ip_versions=[bool]$SplitIpVersions;family_contract='IpVersion0 preserves legacy48cases24warm24measured16workerjoins. Split has separate IPv4-only/IPv6-only entries/executables/processes/logs, each24cases12warm12measured8actual workerOSjoins. Every selected family witness and case row is checked. Eight ISO/MS ordinary/guarded split processes total192cases64workerjoins; no process executes the other family.';production_changed=$true;unchanged_dependencies=($dependencies.Count-2);kernel_runtime_tested=$false;
    audit=@{path=$audit;sha256=(Get-FileHash $audit).Hash.ToLowerInvariant()};builder=@{path=$builder;sha256=(Get-FileHash $builder).Hash.ToLowerInvariant()};executables=$executables;executions=$metrics;operation_record=$script:TestResultFile;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    method='Actual ready-session receive pops32 valid TCP/UDP IPv4/IPv6 packets of32768 bytes in two16-item/512-KiB turns. Two real coalesce-buffer leases remain busy until original MDL/NBL return; returning zero/one/two coalesced objects selects30/29/28 total indicated NBLs without fake FULL, manual Busy reset, counter repair or retries. Count metadata-prefix and descriptor copies separately from private snapshots and successful core output copy spans; the whole original coalesce core binds those begin/append deltas to actual memcpy operations, not a universal memory profiler. Two groups each inspect a third packet before the65535-byte append-capacity rejection: this bounded lookahead is not reported as a production defect. Check original shared payload unchanged, direct partial-MDL alias versus private output MDLs, coalesced payload/OOB, exact statuses and all unique completion tokens. BeginDrain after turn1 cancels the remaining16 without new copy/NBL; fault cases leave those16 unadmitted and require actual private worker quiescence and last native Active/credit in both orders before QuerySTOPPED. Each worker runs on an actual joined OS thread; ready factory, NDIS/DDI, interrupt clock and termination remain hosted. No actual registration, ControlRundown, Halt/Free, network, IRQL, native kernel layout, CPU/latency or throughput claims. RSC runtime6.30 and URO6.89 macro-access gates are observed over a full hosted info array; this is not old-Windows execution. NDEBUG/nonPB/FP65 required, original NT_ASSERT=CHECK remains stronger in the host. Ordinary pools/MDLs/NBLs use original allocators; guarded mode guards those objects. Optional buffers are guarded in both modes; ready factory/region follow the original host. Legacy IpVersion0 has24 checked warmups then24 measured cases and16 joined workers per process; each selected IPv4/IPv6 family has12warm12measured8joinedworkers in its own process/log. Both modes retain strict process-handle equality and reclaim all slots/credits/references/MDLs/NBLs/buffers/threads before successful summary. Historical network dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair and checksum-family Describe selection repair; all other source bindings and test paths retain their existing contracts.'}|ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
