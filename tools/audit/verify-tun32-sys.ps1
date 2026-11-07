param([string]$BaselineDirectory='out\tun32-baseline\a465479905de',[string]$OutputDirectory='',
    [ValidateSet(0,4,6)][int]$IpVersion=0,[switch]$SplitIpVersions)
$ErrorActionPreference='Stop'
if($SplitIpVersions -and $IpVersion){throw 'Select either a single IP version or split IPv4/IPv6 runs'}
$ipVersions=if($SplitIpVersions){@(4,6)}else{@($IpVersion)}
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-sys-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'TUN32 SYS requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='a465479905de63cdc165cffe6f368cc0693e2e5da5ac90e635938c6f1f01ef66'
$manifest=Get-Content (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected accepted TUN32 baseline'}
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate SYS dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h','include\packetbridge_return_diagnostics.h',
    'include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h','tests\model\send_window_tests.c','tests\model\send_describe_tests.c',
    'tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h')
function Assert-T32Dependencies {
    foreach($relative in $dependencies){$old=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant()
        if($manifest.files.($relative.Replace('\','/')) -cne $old){throw "Accepted baseline dependency changed:$relative"}
        if($relative.Replace('\','/') -cin @('common/offload_policy.c','driver/v2/session_describe.h')){Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative}
        elseif((Get-FileHash (Join-Path $script:RepoRoot $relative)).Hash.ToLowerInvariant() -cne $old){throw "TUN32 immutable dependency changed:$relative"}
    }
}
Assert-T32Dependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-sys\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
. (Join-Path $PSScriptRoot 'tun32-projection.ps1')
Start-TestRecording $directory 'operation-results.json'
function Invoke-T32Process($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start TUN32 SYS regression'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'TUN32 external watchdog30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-tun32-{1}.log' -f $script:TestExecutions.Count,$build.direction)
        [IO.File]::WriteAllText($log,$output,$encoding);$send=$build.direction -ceq 'send';$metrics=@{};$valid=$code -eq 0
        $familyRows=[regex]::Matches($output,'(?m)^FAMILY: IPv([46]) only\r?$');$familyLines=[regex]::Matches($output,'(?m)^FAMILY:')
        $valid=$valid -and $(if($build.ip_version){$familyRows.Count -eq 1 -and $familyLines.Count -eq 1 -and [int]$familyRows[0].Groups[1].Value -eq $build.ip_version}else{$familyLines.Count -eq 0})
        $warm=if($send){if($build.ip_version){32}else{64}}else{if($build.ip_version){2}else{4}};$expectedCases=2*$warm
        if($send){
            $rows=[regex]::Matches($output,'(?m)^CASE: SEND32 ip([46]) protocol(6|17) multi([01]) queues(1|2|4|8) ndis(60014|60059) pushCalls(1|32) descriptors32 prefixBytes1888 payloadBytes48000 completeCalls1 notify(1|2|4|8)\r?$')
            $summary=[regex]::Match($output,'PASS: TUN32 SYS send cases(\d+) warm(\d+) measure(\d+) handles(\d+)/(\d+); copied source mappings(\d+)/(\d+), all tokens references and owners reclaimed; no test threads')
            $valid=$valid -and $rows.Count -eq $expectedCases -and $summary.Success;$pushes=0;$notify=0
            for($i=0;$i -lt $rows.Count;++$i){$row=$rows[$i];$shape=$i%$warm;$ip=if($build.ip_version){$build.ip_version}else{4+2*([int][Math]::Floor($shape/32))};$protocol=if([int][Math]::Floor($shape/16)%2){17}else{6};$multi=[int][Math]::Floor($shape/8)%2;$q=@(1,2,4,8)[[int][Math]::Floor($shape/2)%4];$version=if($shape%2){'60059'}else{'60014'};$push=if($q -eq 1){1}else{32}
                $valid=$valid -and [int]$row.Groups[1].Value -eq $ip -and [int]$row.Groups[2].Value -eq $protocol -and [int]$row.Groups[3].Value -eq $multi -and [int]$row.Groups[4].Value -eq $q -and $row.Groups[5].Value -ceq $version -and [int]$row.Groups[6].Value -eq $push -and [int]$row.Groups[7].Value -eq $q
                $pushes+=[int]$row.Groups[6].Value;$notify+=[int]$row.Groups[7].Value
            }
            if($summary.Success){$valid=$valid -and [int]$summary.Groups[1].Value -eq $expectedCases -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $warm -and $summary.Groups[4].Value -eq $summary.Groups[5].Value -and [int]$summary.Groups[6].Value -eq 96*$expectedCases -and $summary.Groups[6].Value -eq $summary.Groups[7].Value}
            $metrics=@{producer_push_calls=$pushes;accepted_descriptors=32*$rows.Count;classification_copy_bytes=1888*$rows.Count;l3_copy_bytes=48000*$rows.Count;send_completion_calls=$rows.Count;notify_results=$notify;source_mapping_allocations=$(if($summary.Success){[int]$summary.Groups[6].Value}else{0})}
        }else{
            $rows=[regex]::Matches($output,'(?m)^CASE: RECEIVE32 ip([46]) protocol(6|17) popCalls1 descriptors32 indications1 nbls32 prefixBytes1280 recordBytes2048 fullPayloadCopies0 ethHeaders32 completionPush1\r?$')
            $summary=[regex]::Match($output,'PASS: TUN32 SYS receive cases(\d+) warm(\d+) measure(\d+) handles(\d+)/(\d+); all NBL MDL pool reference credit owners reclaimed; no test threads')
            $valid=$valid -and $rows.Count -eq $expectedCases -and $summary.Success
            for($i=0;$i -lt $rows.Count;++$i){$row=$rows[$i];$ip=if($build.ip_version){$build.ip_version}elseif($i%4 -lt 2){4}else{6};$protocol=if($i%2){17}else{6};$valid=$valid -and [int]$row.Groups[1].Value -eq $ip -and [int]$row.Groups[2].Value -eq $protocol}
            if($summary.Success){$valid=$valid -and [int]$summary.Groups[1].Value -eq $expectedCases -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $warm -and $summary.Groups[4].Value -eq $summary.Groups[5].Value}
            $metrics=@{submission_pop_calls=$rows.Count;accepted_descriptors=32*$rows.Count;indication_calls=$rows.Count;indicated_nbls=32*$rows.Count;metadata_prefix_copy_bytes=1280*$rows.Count;record_copy_bytes=2048*$rows.Count;other_nbl_copy_calls=0;ethernet_header_builds=32*$rows.Count;completion_push_calls=$rows.Count}
        }
        $valid=$valid -and @($output -split '\r?\n'|Where-Object {$_.StartsWith('CASE:')}).Count -eq $rows.Count
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();direction=$build.direction;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;exit_code=$code;expected_exit_code=0;log=$log;watchdog_ms=30000;schedules=$rows.Count;
            checked_warmup_schedules=$(if($summary.Success){[int]$summary.Groups[2].Value}else{0});measured_schedules=$(if($summary.Success){[int]$summary.Groups[3].Value}else{0});metrics=$metrics;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($valid){'running'}else{'failed'});if(!$valid){throw "TUN32 SYS test failed:$code"};Write-Output $summary.Value
    }finally{$process.Dispose()}
}
$executables=@()
foreach($selectedIpVersion in $ipVersions){foreach($direction in @('send','receive')){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$prefix=if($selectedIpVersion){"ipv$selectedIpVersion-"}else{''};$build=Build-Tun32SysTests -Directory (Join-Path $directory "$prefix$direction-$mode-$memory") -Direction $direction -Mode $mode -Guarded:$guarded -IpVersion $selectedIpVersion
    Assert-T32Dependencies;Assert-T32Projection $build;Invoke-T32Process $build;$executables+=,$build
}}}}
Assert-T32Dependencies
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during TUN32 SYS audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;ip_versions=$ipVersions;split_ip_versions=[bool]$SplitIpVersions;family_contract='IpVersion0 preserves the legacy mixed matrix and summaries. Split selects distinct IPv4-only and IPv6-only compile entries/executables/processes/logs; exact family witness, every case row and warm/measure counts are independently checked. Per family each send process has64 cases32warm32measured, receive4 cases2warm2measured. Split ISO/MS ordinary/guarded totals16 processes544cases; no process runs the other family.';unchanged_dependencies=($dependencies.Count-2);production_changed=$true;kernel_runtime_tested=$false;production_ready=$false;
    operations=$script:TestResultFile;executables=$executables;audit_sources=@('tests\model\tun32_sys_tests.c','tools\build\tun32-sys-tests.ps1','tools\audit\verify-tun32-sys.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    method='Coverage-only operation/byte accounting for32 ordinary valid TCP/UDP IPv4/IPv6 packets of1500 L3 bytes, Ethernet1514, offload metadata zero. Four complete frozen hosted sources and eleven complete production headers are raw-hash locked (Describe admits only the reviewed checksum-family selection repair); the original headers compile directly, archived copies are evidence not alternatives. Independent projection reconstructs only the existing actual-Read/Describe send stand-in replacement, runtime OOB/heap/region observers, outer unused main rename and absolute C/header includes. Receive selects explicit forwarding include wrappers and otherwise unchanged drain/NBL hosts. NDEBUG/FP65/non-PB contracts hold; original NT_ASSERT=CHECK remains a stronger hosted assertion, not absent shipping assertions. Packet creation and test oracles use actual checksum finalization outside transfer counters; valid unicast addresses search the actual address hash, TCP headers/sequence/ACK and UDP lengths/checksums are valid. Send tests both one NBL with32 NB and32 NBL withone NB. Same queue produces one actual Push32 and one send-completion upcall. Two/four/eight alternating address-selected queues intentionally produce32 legitimate Push1 and one completion upcall; this records the current lock/ordering/stack-budget tradeoff, not a proven batching defect. Every packet has one actual full L3 Read/Describe copy, measured48000 bytes per32, separate from1888 classification bytes (32x59) and64 prefix/96 payload MDL mappings. Exact batch counts, per-queue descriptor order/generation/length/data and actual recycle are checked. NDIS6.20/6.89 are runtime capability variables only;6.20 cannot evaluate unavailable USO fields through the original OOB observer. Send fixture lock/event substitutes are counter models, including the original shared user-event stand-in; notify results are recorded separately from producer publications and are not native scheduler/event-cost measurements. Plain Receive performs one actual Pop32, builds32 ordinary NBLs and indicates their chain once. Actual NBL creation copy hooks distinguish32x40-byte metadata prefixes and32x64-byte descriptor records; Ethernet header construction is counted32 times (14 output bytes each). There is no other-size NBL copy in this1500-byte plain case, coalescing is disabled and ScratchBytes stays zero. This does not claim interception of every copy in common/core/coalesce or all packet formats. Each data MDL is partial and points directly at its original shared1500-byte slot, whose bytes are checked against a separately regenerated test oracle; oracle/setup copies are not transfer copies. One actual return window emits a single32-descriptor completion publication and retires every NBL/MDL/pool/Active/Returns/credit owner before original cleanup. Send source pages are independent read-only guards; guarded mode uses unchanged send heap/region and kernel NBL/MDL/pool guards, while the old receive ready factory/region stays ordinary storage. No test-created threads/async callers exist; callbacks are synchronous hosted boundaries, not native NDIS execution. Every source mapping/frame/heap/region or native object/event belongs to its original cleanup and process handles must match strictly after one checked warm matrix. Legacy IpVersion0 has Send64 warm+64 measured and Receive4 warm+4 measured; a selected IPv4/IPv6 family has Send32+32 and Receive2+2 in a separate executable/process/log per ISO/MS ordinary/guarded mode; no timing/QPC/CPU/latency/throughput result, no driver/device/network/registry or native Win7/kernel/IRQL guarantee, only the separately reviewed full-file LSO/checksum and checksum-family Describe selection repairs differ from the historical production closure. Version remains2.0.0. Historical network dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair and checksum-family Describe selection repair; all other source bindings and test paths retain their existing contracts.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
