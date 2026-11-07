param([string]$BaselineDirectory='out\stop-progress-baseline\124a7befbedb',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\late-commit-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Late Commit audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common api/v2);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h','tests\model\native_reclaim_tests.c','tools\build\native-reclaim-tests.ps1')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\late-commit\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-LateCommitProjection($build) {
    $e=$build.extractions
    $hostSource=Join-Path $script:RepoRoot 'tests\model\native_reclaim_tests.c'
    if($e.host_source -cne $hostSource -or $e.host_source_sha256 -ne 'f1b676903eb875cda76863981e0837593b36c51837fceaa9cecdc60a7258c765' -or
        $e.host_source_sha256 -ne $e.host_original_sha256 -or (Get-FileHash (Join-Path $baseline 'tests\model\native_reclaim_tests.c')).Hash.ToLowerInvariant() -ne $e.host_original_sha256){throw 'Original host snapshot/hash changed'}
    $projectedHost=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\native_reclaim_tests.c'))
    $expectedChanges=[Collections.Generic.List[object]]::new()
    $projectedHost=[regex]::Replace($projectedHost,'(?m)^#include "([^"]+)"',{
        param($match)
        $path=if($match.Groups[1].Value -eq '../../api/v2/session.c'){$e.session_entry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $match.Groups[1].Value))}
        $adapted='#include "'+$path.Replace('\','/')+'"';$expectedChanges.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    if($projectedHost -cne [IO.File]::ReadAllText($e.host) -or ($expectedChanges|ConvertTo-Json -Depth 4 -Compress) -cne ($e.host_include_changes|ConvertTo-Json -Depth 4 -Compress)){throw 'Host adaptation changed beyond exact include projection'}
    $sessionSource=Join-Path $script:RepoRoot 'api\v2\session.c'
    if((Get-FileHash (Join-Path $baseline 'api\v2\session.c')).Hash.ToLowerInvariant() -ne $e.session_original_sha256 -or
        [IO.File]::ReadAllText($e.session_original) -cne [IO.File]::ReadAllText($sessionSource)){throw 'Full session original snapshot changed'}
    $sessionChanges=[Collections.Generic.List[object]]::new()
    $session=[regex]::Replace([IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session.c')),'(?m)^#include "([^"]+)"',{
        param($match)
        $path=if($match.Groups[1].Value -eq 'session_queue.h'){$e.queue_entry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $sessionSource) $match.Groups[1].Value))}
        $adapted='#include "'+$path.Replace('\','/')+'"';$sessionChanges.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    if($session -cne [IO.File]::ReadAllText($e.session) -or ($sessionChanges|ConvertTo-Json -Depth 4 -Compress) -cne ($e.session_include_changes|ConvertTo-Json -Depth 4 -Compress)){throw 'Full session changed beyond recorded includes'}
    $queue=@'
#define PbQueuePush LatePush
#define ReleaseSRWLockShared LateRelease
#include PB_LATE_QUEUE_SOURCE
#undef PbQueuePush
#undef ReleaseSRWLockShared
'@
    $sessionEntry=@'
#define AcquireSRWLockExclusive LateAcquire
#include PB_LATE_SESSION_SOURCE
#undef AcquireSRWLockExclusive
'@
    if([IO.File]::ReadAllText($e.queue_entry) -cne $queue -or [IO.File]::ReadAllText($e.session_entry) -cne $sessionEntry){throw 'Observer entry wrapper changed'}
    $cleanup=$e.cleanup;$actualFree=[IO.File]::ReadAllText($cleanup.source).Substring($cleanup.offset,$cleanup.length)
    if($cleanup.variant -ne 'current' -or [IO.File]::ReadAllText($cleanup.extracted) -cne $actualFree -or
        [IO.File]::ReadAllText($cleanup.header) -cne ("#pragma once`r`n"+$actualFree)){throw 'Actual full Free extraction changed'}
    $arm=$e.arm;$source=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session_drain.h'))
    $marker='static __inline PB_RESULT PbV2ArmSubmissions(';$offset=$source.IndexOf($marker);$original=$source.Substring($offset)
    if($arm.marker -cne $marker -or $arm.offset -ne $offset -or $arm.length -ne $original.Length -or [IO.File]::ReadAllText($arm.original) -cne $original){throw 'Actual ArmSubmissions extraction changed'}
    $before='    if (!ReadAcquire(&q->Session->Draining)) return PbQueueArm(&q->Rings[PB_QUEUE_RX_SUBMIT], ready);'
    $after='    if (ReadAcquire(&q->Session->DrainSnapshotReady) || !ReadAcquire(&q->Session->Draining)) return PbQueueArm(&q->Rings[PB_QUEUE_RX_SUBMIT], ready);'
    if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'Frozen arm guard not unique'}
    if($build.variant -eq 'without-drain-arm-guard'){
        if($arm.mutation.original -cne $before -or $arm.mutation.changed -cne $after -or $arm.mutation.offset -ne $original.IndexOf($before) -or $arm.mutation.length -ne $before.Length){throw 'Arm guard mutation ledger changed'}
        $original=$original.Replace($before,$after)
    }elseif($null -ne $arm.mutation){throw 'Current arm unexpectedly mutated'}
    $selected="#define PbV2ArmSubmissions SelectedArmSubmissions`r`n"+$original+"`r`n#undef PbV2ArmSubmissions`r`n"
    if([IO.File]::ReadAllText($arm.selected) -cne $selected){throw 'Selected arm changed beyond macro rename and unique negative guard'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_NATIVE_RECLAIM_CLEANUP_SOURCE "'+$cleanup.header.Replace('\','/')+'"'
    $lines+='#define PB_LATE_RECLAIM_HOST "'+$e.host.Replace('\','/')+'"'
    $lines+='#define PB_LATE_ARM_SOURCE "'+$arm.selected.Replace('\','/')+'"'
    $lines+='#define PB_LATE_QUEUE_SOURCE "'+(Join-Path $script:RepoRoot 'api\v2\session_queue.h').Replace('\','/')+'"'
    $lines+='#define PB_LATE_SESSION_SOURCE "'+$e.session.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\late_commit_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`r`n")+"`r`n")){throw 'Final compilation entry changed'}
    foreach($item in @($build.sources)+@(@{path=$cleanup.source;sha256=$cleanup.source_sha256},@{path=$cleanup.extracted;sha256=$cleanup.extracted_sha256},@{path=$cleanup.header;sha256=$cleanup.header_sha256})){
        if((Get-FileHash -LiteralPath $item.path).Hash.ToLowerInvariant() -ne $item.sha256){throw "Recorded source hash changed:$($item.path)"}
    }
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-LateCommitProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start late Commit test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Late Commit external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-late-commit.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) actual nonPB late Commit/SYS DRAIN/QUERY/lifecycle reconciliation/free schedules plus (\d+) checked warmup; post-boundary Producer untouched, real DataGate writer waits bookkeeping, STOPPED ack retires Published without generation reuse; all owners/callbacks/workers joined, resources zero; handles (\d+)/(\d+); unexpected arm ready (\d+)/(\d+)')
        $full=$Arguments.Count -eq 0;$schedules=if($full){60}else{1};$warm=if($full){60}else{2}
        $ready=if($Expected -eq 124){1}else{0};$warmReady=if($Expected -eq 124){2}else{0}
        $marker='LATE_COMMIT_DRAIN_ARM_GUARD_BYPASSED: selected actual arm reports ready for post-boundary tail despite zero finite room; bounded observation only, real owner/lifecycle/native cleanup and all thread joins precede124, zero repair'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 124 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $schedules -and [int]$summary.Groups[2].Value -eq $warm -and
            $summary.Groups[3].Value -eq $summary.Groups[4].Value -and [int]$summary.Groups[5].Value -eq $ready -and [int]$summary.Groups[6].Value -eq $warmReady}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 124 -and $correct);
            watchdog_ms=30000;log=$log;measured_schedules=$schedules;checked_warmup_schedules=$warm;schedules=$schedules+$warm;unexpected_ready=$ready;warmup_unexpected_ready=$warmReady;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Late Commit test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-drain-arm-guard')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-LateCommitTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-LateCommitProjection $built
    if($variant -eq 'current'){Invoke-LateCommitProcess $built.path @();Invoke-LateCommitProcess $built.path @('124')}
    else{Invoke-LateCommitProcess $built.path @('124') 124}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during late Commit audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');
    unchanged_dependencies=($dependencies.Count-3);
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\late_commit_tests.c','tools\build\late-commit-tests.ps1','tools\audit\verify-late-commit.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only combination with the original native-reclaim host and object/region/module/pool/thread ledger. Each build snapshots that exact full host and actual full DLL session; only quoted includes select explicit absolute production paths and recorded wrapper headers. Actual non-PB_API_TESTS CommitBatch passes its stop-admission checks, then an observer gates immediately before real PbQueuePush while that same owner holds actual DataGate shared. Actual BeginStop and lifecycle DRAIN invoke actual SYS BeginDrain under Transition exclusive, fixing every native DrainProducer at the old producer. The actual lifecycle next tries DataGate exclusive; a failed genuine TryAcquire proves a conflicting shared owner, then genuine Acquire remains blocked. Release of the first gate permits real Push and Commit private Loans-to-Published bookkeeping/output writes. A second observer holds shared DataGate until all these writes are complete; no lifecycle QUERY has occurred. Actual SYS ReceiveBatch and Query at this ready-worker boundary prove no post-boundary entry is consumed, no NBL/native slot/credit/reference is created, and native STOPPED can already be logically true. After the owner releases DataGate and joins, the actual lifecycle reads its trusted QUERY acknowledgement and reconciles Published to FREE without changing generation or admitting another loan. Full1/2/8 queue x batch1/2/7/63/64 x auto/manual event x zero/uint64-wrap cursors runs60 checked warm then60 measured cases. CLI124 runs two checked warm plus one measured case. Every process requires exact final process-handle equality after warmup, every case exact owned resource zeros and real owner/native-worker/callback OS-thread joins. Two checked selected warms intentionally include CNG staged first-use initialization also observed in independent pure SDK controls; no unknown SDK handle is closed, excluded or repaired. A separately exact extracted PbV2ArmSubmissions is macro-renamed for a bounded invocation under its required RxLock, while the production worker/header stays unchanged. Current reports ready0 for an unconsumed post-boundary tail and zero finite room. Negative only routes the completed drain snapshot through original PbQueueArm, observes ready1 in each of its three safe cases, then performs unchanged owner join, actual trusted query/reconciliation, successful single public Close, full actual SYS Unregister/Free and DLL resource teardown before returning124. No worker spin loop or CPU measurement is claimed, and no reference/generation/counter repair or unsafe free is performed. A known internal fixture ref protects work/pool observations after the one successful Close consumes the public handle; all public callers have already joined. This is an EXE integration, not an unloadable DLL test. Registration, idle-only native workers, device dispatch, NDIS and IRQL are hosted; shared Region remains DLL-owned, native MDL teardown only unlocks. No real device/driver/network/registry, native Win7 execution or Driver Verifier result is claimed. PB_API_TESTS is rejected and shipping source fingerprint must be65 bytes. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
