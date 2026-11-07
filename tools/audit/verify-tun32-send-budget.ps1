param([string]$BaselineDirectory='out\tun32-edge-baseline\f8745a1b269f',[string]$OutputDirectory='',
    [ValidateSet(0,4,6)][int]$IpVersion=0,[switch]$SplitIpVersions)
$ErrorActionPreference='Stop'
if($SplitIpVersions -and $IpVersion){throw 'Select either a single IP version or split IPv4/IPv6 runs'}
$ipVersions=if($SplitIpVersions){@(4,6)}else{@($IpVersion)}
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-send-budget-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-projection.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
if($script:Version -ne '2.0.0'){throw 'TUN32 send budget requires2.0.0'}
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$manifest=Get-Content (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -ne 'f8745a1b269f1f6ad5097b5acc9c7806f27d720725a45baf2fd217dca0c5bd89' -or $manifest.status -ne 'accepted'){throw 'TUN32 send budget baseline identity changed'}
$dependencies=@(rg --files driver common);if($LASTEXITCODE){throw 'Cannot enumerate dependencies'}
$dependencies+=@('tests\model\send_window_tests.c','tests\model\send_describe_tests.c','tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h',
    'tests\safety\guard_memory.h','include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h')
function Assert-B32Dependencies {
    foreach($relative in $dependencies){$old=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant();if($manifest.files.($relative.Replace('\','/')) -ne $old){throw "Accepted baseline dependency changed:$relative"}
        if($relative.Replace('\','/') -cin @('common/offload_policy.c','driver/v2/session_describe.h')){Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative}
        elseif((Get-FileHash $relative).Hash.ToLowerInvariant() -ne $old){throw "TUN32 send budget dependency changed:$relative"}}
}
Assert-B32Dependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-send-budget\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-B32Test($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start TUN32 budget regression'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();$null=$stdout.GetAwaiter().GetResult();$null=$stderr.GetAwaiter().GetResult();throw 'TUN32 budget external watchdog30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-budget.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false))
        $rows=[regex]::Matches($output,'(?m)^CASE: BUDGET32 ip([46]) protocol(6|17) multi([01]) ndis(60014|60059) length(4082|4083|65535) action(0|1|2|4) published(31|32) pushes(1|2|16) prefix(1888|1829) payload(\d+)\r?$')
        $summary=[regex]::Match($output,'PASS: TUN32 send budget cases(\d+) warm(\d+) measure(\d+) handles(\d+)/(\d+) mappings(\d+)/(\d+); exact byte budgets, prefix ownership and no test threads')
        $warm=if($build.ip_version){48}else{96};$expectedCases=2*$warm
        $valid=$code -eq 0 -and $rows.Count -eq $expectedCases -and $summary.Success -and [int]$summary.Groups[1].Value -eq $expectedCases -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $warm -and $summary.Groups[4].Value -eq $summary.Groups[5].Value -and [int]$summary.Groups[6].Value -eq 96*$expectedCases -and $summary.Groups[6].Value -eq $summary.Groups[7].Value
        $familyRows=[regex]::Matches($output,'(?m)^FAMILY: IPv([46]) only\r?$');$familyLines=[regex]::Matches($output,'(?m)^FAMILY:')
        $valid=$valid -and $(if($build.ip_version){$familyRows.Count -eq 1 -and $familyLines.Count -eq 1 -and [int]$familyRows[0].Groups[1].Value -eq $build.ip_version}else{$familyLines.Count -eq 0})
        $counts=[ordered]@{published_descriptors=0;producer_push_calls=0;classification_copy_bytes=0L;l3_copy_bytes=0L}
        for($i=0;$i -lt $rows.Count;++$i){$shape=$i%$warm;$group=[int][Math]::Floor($shape/6);$scenario=$shape%6
            $ip=if($build.ip_version){$build.ip_version}else{4+2*([int][Math]::Floor($group/8))};$protocol=if([int][Math]::Floor($group/4)%2){17}else{6};$multi=[int][Math]::Floor($group/2)%2;$version=if($group%2){'60059'}else{'60014'}
            $length=@(4082,4083,65535,4083,4083,4083)[$scenario];$action=@(0,0,0,1,2,4)[$scenario];$published=if($action){31}else{32};$pushes=if($action){1}elseif($length -eq 4082){1}elseif($length -eq 4083){2}else{16}
            $expected=@("$ip","$protocol","$multi",$version,"$length","$action","$published","$pushes",(59*$published).ToString(),([uint64]$length*$published).ToString())
            for($field=0;$field -lt 10;++$field){$valid=$valid -and $rows[$i].Groups[$field+1].Value -ceq $expected[$field]}
            $counts.published_descriptors+=[int]$rows[$i].Groups[7].Value;$counts.producer_push_calls+=[int]$rows[$i].Groups[8].Value;$counts.classification_copy_bytes+=[long]$rows[$i].Groups[9].Value;$counts.l3_copy_bytes+=[long]$rows[$i].Groups[10].Value
        }
        $valid=$valid -and @($output -split '\r?\n'|Where-Object {$_.StartsWith('CASE:')}).Count -eq $expectedCases
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();exit_code=$code;log=$log;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;schedules=$rows.Count;checked_warmup_schedules=$warm;measured_schedules=$warm;watchdog_ms=30000;source_mapping_allocations=$(if($summary.Success){[int]$summary.Groups[6].Value}else{0});metrics=$counts;reported_pass_messages=@($summary.Value)})
        Write-TestRecording $(if($valid){'running'}else{'failed'});if(!$valid){throw 'TUN32 budget result shape failed'};Write-Output $summary.Value
    }finally{$process.Dispose()}
}
$executables=@()
foreach($selectedIpVersion in $ipVersions){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$prefix=if($selectedIpVersion){"ipv$selectedIpVersion-"}else{''};$build=Build-Tun32SendBudgetTests -Directory (Join-Path $directory "$prefix$mode-$memory") -Mode $mode -Guarded:$guarded -IpVersion $selectedIpVersion
    Assert-B32Dependencies;Assert-T32Projection $build.base
    $original=Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c';$test=Join-Path $script:RepoRoot 'tests\model\tun32_send_budget_tests.c'
    $before='#include "'+$original.Replace('\','/')+'"';$after='#include "'+$test.Replace('\','/')+'"';$body=[IO.File]::ReadAllText($build.base.entry)
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1 -or [IO.File]::ReadAllText($build.entry) -cne $body.Replace($before,$after) -or $build.test -cne $test -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -ne $build.entry_sha256 -or (Get-FileHash $test).Hash.ToLowerInvariant() -ne $build.test_sha256 -or $build.ip_version -ne $selectedIpVersion -or $build.base.ip_version -ne $selectedIpVersion -or (Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256){throw 'Budget source binding changed'}
    $expectedFlags=@($build.base.arguments|Where-Object {$_ -notlike '/Fo*'})+('/Fo'+(Split-Path $build.path)+'\')
    if(($build.arguments -join "`n") -cne ($expectedFlags -join "`n")){throw 'Budget compilation flags changed'}
    Invoke-B32Test $build;$executables+=,$build
}}}
Assert-B32Dependencies
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during budget audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_source_tree_sha256=$manifest.source_tree_sha256;operations=$script:TestResultFile;ip_versions=$ipVersions;split_ip_versions=[bool]$SplitIpVersions;
    family_contract='IpVersion0 keeps the legacy192-case mixed process. Split compiles separate IPv4-only/IPv6-only entries, executables, processes and logs with96cases48warm48measured per process,9216 owned source mappings each. Exact family witness plus every row and summary are checked. Eight split ISO/MS ordinary/guarded processes total768cases; no process executes the other family.';
    mutable_test_input=@{path=(Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c');sha256=(Get-FileHash (Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c')).Hash.ToLowerInvariant();reason='Family-selectable construction control and current test input, not a frozen production or old host dependency. Complete current source is hash-bound by the base projection and source fingerprint; all other production/old-host dependencies stay frozen; common/offload_policy.c and driver/v2/session_describe.h admit only their separately reviewed full-file LSO/checksum and checksum-family selection repairs.'};
    production_changed=$true;kernel_runtime_tested=$false;production_ready=$false;executables=$executables;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    method='Actual Read/Describe/Send32 valid TCP/UDP IPv4/IPv6 packets, oneNBLx32NB or32NBLxoneNB, runtime NDIS6.20/6.89 capability gates. L3=4082 at exact128KiB Ethernet charge yields onePush32;4083 yields31+1;65535 uses capped per-attempt charge65536 and16Push2. One original send completion upcall, exact full L3 copies once each plus59-byte classification prefixes, no payload replay. At the natural31-packet window release, original host actions pause/detach/fault admission;32nd packet has no read/reserve/Describe/copy and the31 published prefix is consumed/recycled before cleanup. Single-NBL failure status may coexist with a previously published prefix, which is not replayed within this Send call. Original locks/actions/events are hosted substitutes, not native IRQL/NDIS or CPU measurement. Old construction and whole production bindings are independently validated through the shared exact projection helper; base executable is a compilation control only, not an executed case. Only the separately reviewed full-file LSO/checksum and checksum-family Describe selection repairs differ from the historical production closure; all source mappings/frames/heap/region/slot references retire and strict process handle equality after96 checked warm cases in legacy IpVersion0, or48 checked warm cases in each selected family process. Four legacy mixed processes or eight separate IPv4/IPv6 processes give768 cases, half checked warmup; no test threads or asynchronous callers. Historical network dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair and checksum-family Describe selection repair; all other source bindings and test paths retain their existing contracts.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
