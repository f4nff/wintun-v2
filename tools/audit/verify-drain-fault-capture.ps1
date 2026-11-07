param([string]$BaselineDirectory='out\retire-edge-baseline\d1dc19684d05',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\drain-fault-capture-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Drain fault capture requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$accepted='d1dc19684d050d0acece95c8d29204dc0c9350fca6c8af7f314c5d75331c461f'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected accepted capture baseline'}
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate SYS dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h')
function Assert-DrainFaultCaptureDependencies {
    foreach($relative in $dependencies){
        $hash=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant()
        if($manifest.files.($relative.Replace('\','/')) -cne $hash){throw "Accepted baseline dependency changed:$relative"}
        if($relative.Replace('\','/') -cin @('common/offload_policy.c','driver/v2/session_describe.h')){Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative}
        elseif((Get-FileHash (Join-Path $script:RepoRoot $relative)).Hash.ToLowerInvariant() -cne $hash){throw "Unchanged capture dependency changed:$relative"}
    }
}
Assert-DrainFaultCaptureDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\drain-fault-capture\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Assert-DrainFaultCaptureProjection($build){
    $e=$build.extractions;$names=@('drain','return','completion','fault','fault_drain','progress','references','query','worker','nbl','receive')
    if(@($e.parts).Count -ne 11 -or @($e.parts.name|Select-Object -Unique).Count -ne 11 -or @(Compare-Object ($names|Sort-Object) ($e.parts.name|Sort-Object)).Count){throw 'Actual header evidence set changed'}
    foreach($part in $e.parts){
        $source=Join-Path $script:RepoRoot "driver\v2\session_$($part.name).h";$frozen=Join-Path $baseline "driver\v2\session_$($part.name).h"
        if($part.source -cne $source -or $part.compiled_path -cne $source -or $part.offset -ne 0 -or $part.length -ne [IO.File]::ReadAllText($frozen).Length -or
            (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne (Get-FileHash $frozen).Hash.ToLowerInvariant() -or $part.sha256 -cne (Get-FileHash $part.path).Hash.ToLowerInvariant() -or
            $part.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant()){throw "Whole original production header changed:$($part.name)"}
    }
    $hostHashes=@{'tests\model\drain_boundary_tests.c'='4fd8afe2b998c6ca628ff05fa266b689e4602be4e68109a477cc5f3001fa7df5';
        'tests\model\kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'}
    if(@($e.hosts).Count -ne 2 -or @($e.hosts.relative|Select-Object -Unique).Count -ne 2){throw 'Whole host evidence set changed'}
    foreach($record in $e.hosts){
        if(!$hostHashes.ContainsKey($record.relative) -or $record.source -cne (Join-Path $script:RepoRoot $record.relative) -or $record.sha256 -cne $hostHashes[$record.relative] -or
            $record.original_sha256 -cne $hostHashes[$record.relative] -or (Get-FileHash $record.original).Hash.ToLowerInvariant() -cne $hostHashes[$record.relative]){throw 'Complete frozen fixture evidence changed'}
    }
    $hostSource=Join-Path $script:RepoRoot 'tests\model\drain_boundary_tests.c';$body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\drain_boundary_tests.c'))
    $before='PB_RX_SLOT Slots[2][PB_MAX_SLOTS];PB_DESCRIPTOR Pending[2][512];KEVENT Kernel[2],User[2];'
    $after='PB_RX_SLOT Slots[PB_MAX_QUEUES][PB_MAX_SLOTS];PB_DESCRIPTOR Pending[PB_MAX_QUEUES][512];KEVENT Kernel[PB_MAX_QUEUES],User[PB_MAX_QUEUES];'
    if($e.typedef_original -cne $before -or $e.typedef_adapted -cne $after -or [regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'Bounded queue-array adaptation changed'}
    $body=$body.Replace($before,$after);$includes=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'"([^"\r\n]*\.h)"',{
        param($m);$path=$m.Groups[1].Value
        if(![IO.Path]::IsPathFullyQualified($path)){$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $path))}
        $value='"'+$path.Replace('\','/')+'"';$includes.Add(@{original=$m.Value;adapted=$value});$value
    })
    if([IO.File]::ReadAllText($e.host) -cne $body -or (Get-FileHash $e.host).Hash.ToLowerInvariant() -cne $e.host_sha256 -or
        (@($includes.ToArray())|ConvertTo-Json -Depth 4 -Compress) -cne ($e.include_changes|ConvertTo-Json -Depth 4 -Compress)){throw 'Host changed beyond unique typedef/header-path projection'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_DRAIN_FAULT_CAPTURE_HOST "'+$e.host.Replace('\','/')+'"';$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\drain_fault_capture_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`n")+"`n") -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256){throw 'Actual compilation entry changed'}
    if(!$build.ndebug -or $build.pb_api_tests -or '/DNDEBUG' -notin $build.arguments -or "/volatile:$($build.mode)" -notin $build.arguments -or "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint" -notin $build.arguments -or
        @($build.arguments|Where-Object {$_ -match 'PB_API_TESTS'}).Count){throw 'Shipping compile contracts changed'}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-DrainFaultCaptureProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start drain capture test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Drain capture external watchdog30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-drain-fault-capture.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding)
        $summary=[regex]::Match($output,'PASS: (\d+) actual drain Return fault capture cases warm4 measure4; joined(\d+) drain(\d+) query(\d+) returns(\d+) workers(\d+) handles(\d+)/(\d+); all NBL MDL pool references credits and harness owners zero')
        $rows=[regex]::Matches($output,'(?m)^CASE: drain fault capture queues(2|8) activeFirst([01]) joins(\d+) workers(\d+) kernelWakes(\d+) userWakes(\d+); snapshot alone rejected, native owners zero\r?$')
        $valid=$code -eq 0 -and $summary.Success -and $rows.Count -eq 8 -and @($output -split '\r?\n'|Where-Object {$_.StartsWith('CASE:')}).Count -eq 8
        for($i=0;$i -lt $rows.Count;++$i){$row=$rows[$i];$q=if($i%4 -lt 2){2}else{8};$last=$i%2
            $valid=$valid -and [int]$row.Groups[1].Value -eq $q -and [int]$row.Groups[2].Value -eq $last -and [int]$row.Groups[3].Value -eq $q+4 -and
                [int]$row.Groups[4].Value -eq $q -and [int]$row.Groups[5].Value -eq $q -and [int]$row.Groups[6].Value -eq $q
        }
        if($summary.Success){$valid=$valid -and [int]$summary.Groups[1].Value -eq 8 -and [int]$summary.Groups[2].Value -eq 72 -and [int]$summary.Groups[3].Value -eq 8 -and
            [int]$summary.Groups[4].Value -eq 8 -and [int]$summary.Groups[5].Value -eq 16 -and [int]$summary.Groups[6].Value -eq 40 -and $summary.Groups[7].Value -eq $summary.Groups[8].Value}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();exit_code=$code;expected_exit_code=0;log=$log;watchdog_ms=30000;schedules=$rows.Count;checked_warmup_schedules=[Math]::Min(4,$rows.Count);measured_schedules=[Math]::Max(0,$rows.Count-4);
            threads_joined=$(if($summary.Success){[int]$summary.Groups[2].Value}else{0});drain_joins=$(if($summary.Success){[int]$summary.Groups[3].Value}else{0});query_joins=$(if($summary.Success){[int]$summary.Groups[4].Value}else{0});
            return_joins=$(if($summary.Success){[int]$summary.Groups[5].Value}else{0});worker_joins=$(if($summary.Success){[int]$summary.Groups[6].Value}else{0});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($valid){'running'}else{'failed'});if(!$valid){throw "Drain fault capture failed:$code"};Write-Output $summary.Value
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-DrainFaultCaptureTests -Directory (Join-Path $directory "$mode-$memory") -Mode $mode -Guarded:$guarded
    Assert-DrainFaultCaptureDependencies;Assert-DrainFaultCaptureProjection $build;Invoke-DrainFaultCaptureProcess $build;$executables+=,$build
}}
Assert-DrainFaultCaptureDependencies
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during drain capture audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;unchanged_dependencies=($dependencies.Count-2);
    production_changed=$true;kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\drain_fault_capture_tests.c','tools\build\drain-fault-capture-tests.ps1','tools\audit\verify-drain-fault-capture.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    method='Coverage-only actual multi-queue BeginDrain and native Return/Flush/Fault/Query/worker-quiescence combination. Original production headers compile directly from unchanged paths; eleven complete byte-identical archived snapshots are evidence, not alternate implementations. Both complete frozen drain-boundary and kernel-NBL hosts are raw-hash locked. The only ignored-output host changes are one complete typedef expanding its four per-queue arrays from2 to PB_MAX_QUEUES and absolute quoted header paths; old main is token-renamed by the new test. NDEBUG/non-PB and65-byte shipping fingerprint are enforced; original NT_ASSERT=CHECK intentionally retains stronger model assertions, so this does not claim shipping assertions are absent. The new C statically checks every expanded array and runs only2/8 queues,4 slots and8 ring entries. A ready-session/NDIS-owner factory calls actual NBL allocation and retain helpers, modeling two native-owned ordinary NBLs on queue0 and the last queue; it does not execute full registration or a native NDIS indication. Each public submit ring contains a legal distinct slot1 descriptor. Actual BeginDrain holds real hosted Transition exclusive and captures queue0 Producer1, then an existing queue-unlock observer gates before queue1 capture (still target0). This host-only scheduling gate models another CPU interleaving; native code does not sleep on it while holding a kernel spin lock. A separate actual Query starts while that lock is held and cannot finish until capture resumes; its selected-call entry marker is not a claim of opaque scheduler parking. The only corrupted state is queue0 public completion Consumer1 ahead of its actual Producer0. A real returned NBL invokes actual PushRing validation, Fault, private pending-credit retirement and reference release while BeginDrain is gated, rather than directly setting Fault/credit/reference flags. Terminal/sequence and genuine Progress/User/Kernel SDK events are observed, with exactly one terminal User/Kernel broadcast per queue. BeginDrain then finishes every snapshot and publishes SnapshotReady1 despite Fault. Actual Query chooses fault witnesses and stays DRAINING. Real joined OS worker callers enter the unchanged production worker already Faulted, execute actual Quiesce, and exit before any packet/recycle/arm body. Two orders prove both barriers: all worker witnesses with one native-owned NBL still require its final Active/credit release, or final native Return with Active0 still requires the last worker witness. Only after both are complete does Query return STOPPED; legal post-fault shared submissions remain unconsumed without authorizing native reuse. All original NBL/MDL/pool/Active/Returns/credit/pending owners reach zero, and every4+queue drain/query/return/worker OS thread has its captured ID, exit code, wait and handle close checked before the original fixture Destroy closes original events and frees its region/embedded storage. Five additional harness events and all thread handles are independently counted to zero. Guarded mode uses unchanged original kernel NBL/pool/MDL guards; the old ready factory and original region remain ordinary storage, not falsely claimed guard allocations. Full processes run4 checked warm plus4 measured cases with strict process-handle equality. There is no unsafe mutation or forced negative, no manual Fault/private counter/generation/cache repair, no actual Free/Halt/ControlRundown/outer ReturnRundown combination claim, no driver/device/registry/network or native Win7/kernel/DDI/IRQL/NDIS/CPU-performance result. Version remains2.0.0. Historical network dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair and checksum-family Describe selection repair; all other source bindings and test paths retain their existing contracts.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
