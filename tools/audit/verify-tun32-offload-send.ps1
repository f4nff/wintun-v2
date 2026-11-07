param([string]$BaselineDirectory='out\packet-edge-baseline\6fb17007a923',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-offload-send-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-projection.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'TUN32 offload send requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='6fb17007a9234543f89dea26d3c705639e73b01c2fd2762943862149368a4826'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected accepted offload send baseline'}
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate SYS offload dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h','include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\send_window_tests.c','tests\model\send_describe_tests.c','tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h','tests\model\send_prefix_cases.h','tests\model\tun32_sys_tests.c','tests\safety\guard_memory.h','tools\build\tun32-sys-tests.ps1','tools\audit\tun32-projection.ps1')
function Assert-O32Dependencies {
    foreach($relative in $dependencies){$old=(Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant();if($manifest.files.($relative.Replace('\','/')) -cne $old){throw "Frozen offload send manifest changed:$relative"};if($relative.Replace('\','/') -ceq 'tools/audit/tun32-projection.ps1'){Assert-ChecksumFamilyProjectionFilePair -Before (Join-Path $baseline $relative) -Current (Join-Path $script:RepoRoot $relative) -Relative $relative}
        else{Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative}}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-offload-send\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Replace-O32Projection([string]$text,[string]$before,[string]$after){if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw "Nonunique exact offload projection:$before"};return $text.Replace($before,$after)}
function Assert-O32Build($build){
    Assert-T32Projection $build.base
    if($build.ip_version -notin @(4,6) -or $build.base.ip_version -ne $build.ip_version -or $build.base.direction -cne 'send'){throw 'Offload send family/base binding changed'}
    $lsoSource=Join-Path $script:RepoRoot 'driver\v2\offload_probe.h';$old=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\offload_probe.h'))
    $lso=[regex]::Match($old,'(?ms)^static VOID PbSetLsoCompletion\(NET_BUFFER_LIST \*nbls\)\r?\n\{.*?^\}')
    $part=$build.lso_completion
    if(!$lso.Success -or $part.source -cne $lsoSource -or $part.source_sha256 -cne (Get-FileHash $lsoSource).Hash.ToLowerInvariant() -or $part.offset -ne $lso.Index -or $part.length -ne $lso.Length -or [IO.File]::ReadAllText($part.path) -cne $lso.Value -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw 'Actual LSO completion original span changed'}
    $hostRecord=Get-T32Generated $build.base 'send-window';$hostProjection=$build.host_projection
    if($hostProjection.before_path -cne $hostRecord.path -or $hostProjection.before_sha256 -cne $hostRecord.sha256){throw 'Offload host original projection binding changed'}
    $expected=[IO.File]::ReadAllText($hostRecord.path)
    $expected=Replace-O32Projection $expected 'static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbls) { (void)nbls; AssertUnlocked(); }' '#include PB_TUN32_LSO_COMPLETION'
    $mask=[regex]::Match($expected,'(?ms)^static ULONG PbEffectiveMask\(TUN_CTX \*ctx\)\r?\n\{.*?^\}')
    if(!$mask.Success -or $hostProjection.mask_offset -ne $mask.Index -or $hostProjection.mask_length -ne $mask.Length){throw 'Original effective-mask stub span changed'}
    $policyHeader=(Join-Path $script:RepoRoot 'common\offload_policy.h').Replace('\','/')
    $replacement='#include "'+$policyHeader+'"'+"`n"+'static ULONG PbEffectiveMask(TUN_CTX *ctx){CHECK(ctx==Current);return PbResolveOffloads(O32Allowed,O32Requested,PB_PROBE_SUPPORTED,TRUE);}'
    $expected=Replace-O32Projection $expected $mask.Value $replacement
    $expected=Replace-O32Projection $expected "static VOID OnWindowEnd(VOID)`r`n{" "static VOID OnWindowEnd(VOID)`r`n{`r`n    O32Boundary();"
    if([IO.File]::ReadAllText($hostProjection.path) -cne $expected -or (Get-FileHash $hostProjection.path).Hash.ToLowerInvariant() -cne $hostProjection.sha256){throw 'Offload host exceeded three allowed finite projections'}
    $expected=[IO.File]::ReadAllText($build.base.entry)
    $expected=Replace-O32Projection $expected ('#define PB_SEND_DESCRIBE_HOST "'+$hostRecord.path.Replace('\','/')+'"') ('#define PB_SEND_DESCRIBE_HOST "'+$hostProjection.path.Replace('\','/')+'"')
    $original=Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c';$test=Join-Path $script:RepoRoot 'tests\model\tun32_offload_send_tests.c'
    $expected=Replace-O32Projection $expected ('#include "'+$original.Replace('\','/')+'"') ('#include "'+$test.Replace('\','/')+'"')
    $expected='#define PB_TUN32_LSO_COMPLETION "'+$part.path.Replace('\','/')+'"'+"`n"+$expected
    if([IO.File]::ReadAllText($build.entry) -cne $expected -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256 -or $build.test -cne $test -or (Get-FileHash $test).Hash.ToLowerInvariant() -cne $build.test_sha256 -or (Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256){throw 'Complete offload test entry/source/executable changed'}
    if($build.variant -notin @('current','checksum-gated-lso') -or $build.policy.variant -cne $build.variant){throw 'Offload policy variant changed'}
    $policy=$build.policy;$current=Join-Path $script:RepoRoot 'common\offload_policy.c';$policySource=if($build.variant -ceq 'current'){$current}else{Join-Path $script:RepoRoot 'out\packet-edge-baseline\6fb17007a923\common\offload_policy.c'}
    $policyPath=if($build.variant -ceq 'current'){$current}else{Join-Path (Split-Path $build.path) 'offload-policy-before.c'}
    if($policy.source -cne $policySource -or $policy.compiled_path -cne $policyPath -or $policy.source_sha256 -cne (Get-FileHash $policySource).Hash.ToLowerInvariant() -or $policy.sha256 -cne $policy.source_sha256 -or (Get-FileHash $policyPath).Hash.ToLowerInvariant() -cne $policy.sha256 -or $policy.shipping -ne ($build.variant -ceq 'current')){throw 'Raw policy source/compiled-path binding changed'}
    Assert-LsoChecksumFilePair -Before (Join-Path $baseline 'common\offload_policy.c') -Current $current -Relative 'common/offload_policy.c'
    if($build.variant -ceq 'checksum-gated-lso' -and $policy.sha256 -cne 'a3a61042c8dc48ea02af9201fccf57c1da88a3cc52abf5de7c7fc56ffbd39374'){throw 'Negative policy body was modified'}
    $flags=@($build.base.arguments|Where-Object {$_ -notlike '/Fo*'})+('/I'+(Join-Path $script:RepoRoot 'common'))+('/Fo'+(Split-Path $build.path)+'\')
    if(($build.arguments -join "`n") -cne ($flags -join "`n")){throw 'Offload send exact compilation flags changed'}
}
function Assert-O32Rows([string]$text,[int]$family,[string]$kind,[string]$variant){
    $witness=[regex]::Matches($text,'(?m)^FAMILY: IPv([46]) only\r?$')
    if($witness.Count -ne 1 -or [int]$witness[0].Groups[1].Value -ne $family -or [regex]::Matches($text,'(?m)^FAMILY:').Count -ne 1){throw 'Offload selected family witness missing'}
    $rows=[regex]::Matches($text,'(?m)^CASE: OFFLOAD32 ip(\d+) kind(\d+) mode(\d+) ndis([0-9a-f]+) length(\d+) published(\d+) pushes(\d+) prefix(\d+) payload(\d+) complete(\d+) lsoComplete(\d+) policyChanges(\d+) wirePackets(\d+)\r?$')
    $independent=$kind -ceq 'independent';$count=if($independent){3}else{24}
    if($kind -notin @('normal','independent') -or $variant -notin @('current','checksum-gated-lso') -or $rows.Count -ne $count -or [regex]::Matches($text,'(?m)^CASE:').Count -ne $count){throw 'Offload32 original case rows missing'}
    $metrics=[ordered]@{cases=$rows.Count;checked_warmup_schedules=$(if($independent){2}else{12});measured_schedules=$(if($independent){1}else{12});published_descriptors=0;producer_push_calls=0;classification_copy_bytes=0L;full_l3_copy_bytes=0L;send_complete_calls=0;lsov2_completion_unions=0;modeled_completed_policy_changes=0;logical_segment_packets=0}
    for($i=0;$i -lt $rows.Count;++$i){$shape=$i%12;$request=if($independent){2}else{[int][Math]::Floor($shape/3)};$scenario=if($independent){3}else{$shape%3};$length=if($scenario -eq 2){4083}else{2500}
        $published=if($independent){if($variant -ceq 'current'){32}else{0}}elseif($scenario -eq 1){0}elseif($scenario -eq 2){31}else{32}
        $offset=if($family -eq 4){24}elseif($request -eq 3){40}else{48};$header=$offset+$(if($request -in @(0,2)){32}else{8});$segments=if($request -lt 2){1}else{[int][Math]::Ceiling(($length-$header)/1000)}
        $expected=@($family,$request,$scenario,$(if($request -eq 3){'60054'}else{'60014'}),$length,$published,$(if($published){1}else{0}),1888,(32*$length),1,$(if($request -eq 2){32}else{0}),$(if($scenario -eq 2){1}else{0}),($published*$segments))
        for($field=0;$field -lt $expected.Count;++$field){if($rows[$i].Groups[$field+1].Value -cne [string]$expected[$field]){throw "Offload32 row$i field$field mismatch"}}
        $metrics.published_descriptors+=[int]$rows[$i].Groups[6].Value;$metrics.producer_push_calls+=[int]$rows[$i].Groups[7].Value;$metrics.classification_copy_bytes+=[long]$rows[$i].Groups[8].Value;$metrics.full_l3_copy_bytes+=[long]$rows[$i].Groups[9].Value;$metrics.send_complete_calls+=[int]$rows[$i].Groups[10].Value;$metrics.lsov2_completion_unions+=[int]$rows[$i].Groups[11].Value;$metrics.modeled_completed_policy_changes+=[int]$rows[$i].Groups[12].Value;$metrics.logical_segment_packets+=[int]$rows[$i].Groups[13].Value
    }
    if($independent){
        $summary=[regex]::Match($text,'(?m)^INDEPENDENT_LSO: ip([46]) contractPublish32 observed(0|32) requested([0-9a-f]+) failures(0|3) cases3 warm2 measure1 handles(\d+)/(\d+) sourceMappings288/288 owners0 repairs0\r?$')
        $before=$variant -ceq 'checksum-gated-lso';$requested=if($family -eq 4){'1c1c3'}else{'17173'}
        if(!$summary.Success -or [int]$summary.Groups[1].Value -ne $family -or [int]$summary.Groups[2].Value -ne $(if($before){0}else{32}) -or $summary.Groups[3].Value -cne $requested -or [int]$summary.Groups[4].Value -ne $(if($before){3}else{0}) -or $summary.Groups[5].Value -cne $summary.Groups[6].Value -or [regex]::Matches($text,'(?m)^LSO_DEPENDS_ON_ENABLED_CHECKSUM\r?$').Count -ne [int]$before -or [regex]::Matches($text,'(?m)^PASS:').Count){throw 'Independent LSO contract/cleanup witness changed'}
        $metrics.process_handles_before=[int]$summary.Groups[5].Value;$metrics.process_handles_after=[int]$summary.Groups[6].Value;$metrics.source_mappings_allocated=$metrics.source_mappings_freed=288;$metrics.contract_violations=if($before){3}else{0};$metrics.repairs=0
    }else{
        $summary=[regex]::Match($text,'(?m)^PASS: TUN32 offload send cases24 warm12 measure12 handles(\d+)/(\d+) sourceMappings2304/2304; all packets slots references and source owners reclaimed, no test threads\r?$')
        if(!$summary.Success -or $summary.Groups[1].Value -cne $summary.Groups[2].Value -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1 -or [regex]::Matches($text,'(?m)^INDEPENDENT_LSO:').Count){throw 'Offload32 resource/source mapping/handle summary changed'}
        $metrics.process_handles_before=[int]$summary.Groups[1].Value;$metrics.process_handles_after=[int]$summary.Groups[2].Value;$metrics.source_mappings_allocated=$metrics.source_mappings_freed=2304
    }
    return $metrics
}
Assert-O32Dependencies
$ledger=Test-LsoChecksumLedger -Original ([IO.File]::ReadAllText((Join-Path $baseline 'common\offload_policy.c'))) -Repaired ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'common\offload_policy.c'))) -Directory (Join-Path $directory 'ledger-contracts')
if($ledger.checks.Count -ne 16 -or $ledger.files.Count -ne 6){throw 'LSO checksum ledger contract set changed'}
Start-TestRecording $directory 'operation-results.json'
function Invoke-O32Process($build,[string]$work,[string]$kind){
    $arguments=if($kind -ceq 'independent'){@('--checksum-independent-lso')}else{@()};$expected=if($build.variant -ceq 'checksum-gated-lso'){139}else{0}
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start offload32 test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Offload32 external watchdog30s'};$text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $work "$kind.log";[IO.File]::WriteAllText($log,$text,$encoding)
        $metrics=$null;if($code -eq $expected){$metrics=Assert-O32Rows $text $build.ip_version $kind $build.variant}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=$arguments;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;kind=$kind;variant=$build.variant;exit_code=$code;expected_exit_code=$expected;log=$log;watchdog_ms=30000;metrics=$metrics;reported_pass_messages=@($text -split '\r?\n'|Where-Object {$_ -like 'PASS:*' -or $_ -like 'INDEPENDENT_LSO:*'})});Write-TestRecording $(if($code -eq $expected -and $metrics){'running'}else{'failed'})
        if($code -ne $expected -or !$metrics){throw "Offload32 process failed:$code expected$expected"};Write-Output "IPv$($build.ip_version)/$($build.mode)/guarded$($build.guarded)/$($build.variant)/${kind}:$($metrics.cases) cases exit$code handles$($metrics.process_handles_before)/$($metrics.process_handles_after)"
    }finally{$process.Dispose()}
}
$executables=@()
foreach($variant in @('current','checksum-gated-lso')){foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "$variant\ipv$family\$mode-$memory";$build=Build-Tun32OffloadSendTests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded -Variant $variant
    Assert-O32Dependencies;Assert-O32Build $build;if($variant -ceq 'current'){Invoke-O32Process $build $work 'normal'};Invoke-O32Process $build $work 'independent';$executables+=,$build
}}}}
Assert-O32Dependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during offload32 audit'}
$families=@();foreach($family in @(4,6)){$runs=@($script:TestExecutions|Where-Object ip_version -eq $family);$cases=($runs|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;$warm=($runs|ForEach-Object {$_.metrics.checked_warmup_schedules}|Measure-Object -Sum).Sum;$measured=($runs|ForEach-Object {$_.metrics.measured_schedules}|Measure-Object -Sum).Sum
    if($runs.Count -ne 12 -or $cases -ne 120 -or $warm -ne 64 -or $measured -ne 56){throw 'Offload32 independent family aggregate incomplete'};$families+=@{ip_version=$family;processes=$runs.Count;cases=$cases;checked_warmup_schedules=$warm;measured_schedules=$measured}}
Write-TestRecording 'passed'
$ledgerPath=Join-Path $script:RepoRoot 'tools\audit\lso-checksum-ledger.ps1'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_source_tree_sha256=$accepted;baseline=$baseline;production_changed=$true;permitted_production_change='common/offload_policy.c: exact reviewed LSO/checksum dependency repair; driver/v2/session_describe.h: exact reviewed checksum-family selection repair';kernel_runtime_tested=$false;production_ready=$false;unchanged_dependencies=$dependencies.Count-3;operations=$script:TestResultFile;executables=$executables;families=$families;
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_audit_repairs=@('tools/audit/tun32-projection.ps1');
    ledger=@{path=$ledgerPath;sha256=(Get-FileHash $ledgerPath).Hash.ToLowerInvariant();contracts=$ledger};audit_sources=@('tests\model\tun32_offload_send_tests.c','tools\build\tun32-offload-send-tests.ps1','tools\audit\verify-tun32-offload-send.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Independent IPv4/IPv6 actualRead/Describe/Send over32 original NBL requests, one NB per NBL, using real WDK checksum/LSO/USO unions and original runtime gates. TCP/UDP checksum and LSOv2 use6.20, USO6.84 (the first modeled short-final-capable revision); full hosted info arrays only, not old-kernel memory layout/runtime. First160 MDL bytes hold every current Ethernet/IP/options/IPv6-extension/TCP Timestamp header; payload spans independent read-only guarded MDLs and a zero-length link. Normal2500-byte IP requests produce onePush32; LSOv2 total-length fields are zero and partial pseudoheader checksums are supplied. Original source remains unchanged; actual checksum finalization writes private slots. Validate GSO descriptor fields with actualcore; checked segment output preserves payload/options/timestamps and final checksums. These are32 original GSO requests with multiple logical segments, not32 physical wire packets. Tx statistics count logical segments and repeated IP/transport header bytes. Modeled effective state calls actual policy/platform pure functions: policy0-start rejects all32, and policy0 at the original unlocked31-packet byte-budget boundary preserves the31 prefix then rejects the last original. This models a completed policy update, not actual OID/IOCTL/report/NDIS concurrency. Describe performs its one full input copy even before a policy rejection;1888 classification bytes are separate from80000 or130656 full-copy bytes, with no replay. Actual PbSetLsoCompletion is extracted verbatim with source/span/hash; all LSOV2 completion unions have Reserved0/TypeV2 before the single completion upcall. That upcall actually guard-frees original NBL/NB storage; the Send tail may use only its session/rundown. Source frames/MDLs remain owned until all checks; accepted slots recycle through actual functions and rejected reservations free with actual round-robin generations. Whole frozen hosts/WDK types retain the full base projection; only completion/policy stand-ins and unlocked observer change in the new hosted adapter. Base executable is a compile control, not executed. Independent CLI applies actual checksum-disable parameter1 while retaining requested/allowed/encapLSO: current mustPublish32; the whole raw baseline policy variant uniquely reproducesPublish0 and three violations, then exits139 only after source288/288, strict handles and all-owner cleanup, no repair. Normal current8processes24cases plus independent current8processes3cases and baseline8processes3cases give24processes240cases,120perfamily. Only the reviewed policy2branch repair and checksum-family Describe selection repair are allowed by their separate complete-file text/raw ledgers; other production/old-host files stay byte-identical. The frozen shared projection tool admits only its separate exact full-file header-authentication repair, preserving original mixed newlines and all other tool statements. Historical policy compilation uses its original bytes plus the current unchanged common header IncludePath and is explicitly nonshipping. No actual kernel/NDIS/IRQL/network/device/driver/registry/CPU/latency/throughput claims, no public concurrent callers or test threads. NDEBUG/nonPB/FP65/version2.0.0 required; original NT_ASSERT=CHECK is stronger in the host. Native old allocators remain, guard mode covers tracked heap/region, caller NBL/NB/frame and source MDLs are guarded in both modes. Dirty/conflicting OOB is excluded from this positive/policy matrix.'}|
    ConvertTo-Json -Depth 22|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
