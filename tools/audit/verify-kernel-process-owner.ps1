param([string]$BaselineDirectory='out\process-owner-baseline\89081d142dbf')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\kernel-process-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Kernel process owner audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$production=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
foreach($path in $production){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$reviewedDependencyRepairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h')
$unchangedProduction=$production.Count-$reviewedDependencyRepairs.Count
$directory=Join-Path $script:RepoRoot "out\process-owner-investigation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-pid-check','without-file-check','without-control-wait')){
    $memory=if($guarded){'guarded'}else{'ordinary'}
    $objects=Join-Path $directory "$variant-$mode-$memory"
    $built=Build-KernelProcessTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $driver=[IO.File]::ReadAllText($built.extraction.driver)
    foreach($part in $built.extraction.extractions){if([IO.File]::ReadAllText($part.path) -cne $driver.Substring($part.offset,$part.length)){throw 'Common callback extraction is not verbatim'}}
    if($variant -eq 'current'){
        if([IO.File]::ReadAllText($built.extraction.context_header) -cne [IO.File]::ReadAllText($built.extraction.context_source)){throw 'Actual context gate changed during extraction'}
        foreach($argument in @('1','2','3')){Invoke-TestTool $built.path @($argument)}
        Invoke-TestTool $built.path @()
    }else{
        $argument=if($variant -eq 'without-pid-check'){'1'}elseif($variant -eq 'without-file-check'){'2'}else{'3'}
        $expectedCode=103+[int]$argument
        $marker=if($argument -eq '1'){'PROCESS_NOTIFY_REMOVED_REPLACEMENT_OWNER'}elseif($argument -eq '2'){'OLD_FILE_CLOSE_REMOVED_NEW_OWNER'}else{'HALT_SKIPPED_CONTROL_RUNDOWN'}
        $output=@(& $built.path $argument 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq $expectedCode -and ($output -join "`n").Contains($marker)
        $script:TestExecutions.Add([ordered]@{program=$built.path;arguments=@($argument);exit_code=$code;expected_exit_code=$expectedCode;expected_mutation_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Common owner mutation was not rejected: $variant/$mode/$memory"}
    }
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during process owner audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=$reviewedDependencyRepairs;unchanged_dependencies=$unchangedProduction;production_changed=$true;
    kernel_runtime_tested=$false;production_ready=$false;production_dependencies_unchanged=$unchangedProduction;
    operations=$script:TestResultFile;executables=$executables;
    method='Each build extracts ForOwner/Unregister/ProcessNotification, IRP_MJ_CLOSE dispatch and Halt verbatim from current wintun.c, with actual context_gate and inner Unregister/Free. Shipping source fingerprint is passed and compile-time length must equal 65. Current ISO/MS ordinary/guarded each run three CLI controls and 180 measured schedules plus one checked native-lock warmup. Real Windows CS/SRW locks/events, opaque FILE identities, joined callback/worker threads and guarded hosted Ctx/session/MDL/pool/region objects model ready sessions. Before every actual Halt, including fixture teardown, a helper establishes a simulated completed-Pause boundary under TransitionLock: Running false, RunningEvent clear, no ReturnRundown references and any remaining session Active/Indications zero. No NBL or indication is constructed in this fixture. Actual Register/Pause, kernel process termination, I/O manager dispatch, native DDIs and IRQL are not executed. Process deletion starts at simulated critical-depth one and permits nesting. Candidate gates run only after actual code releases both global resources and leaves its nested region. One stale PID_A notification or two distinct stale PID_A/PID_C notifications retain pins during A -> B or A -> C -> B reassignment, requiring PID recheck. There are no duplicate deletion callbacks for one process or repeated CLOSE of one FILE object. Later FILE_A CLOSE must preserve B; the late CLOSE after Halt uses a separate control FILE. Actual process/CLOSE callback tail can retain ControlRundown after session is physically freed and ReturnRundown zero; actual Halt revokes Reserved and must still wait for Control. Lookup during revoke rejects immediately and global locks remain independently acquirable. A revoked candidate is skipped without hiding another live adapter of the same PID; that scenario uses one explicitly held control reader to keep Halt at the revoke boundary. Create=true/no-match, one/two/three matching adapters plus another PID, competing different-process notifications/CLOSE, 1/2/8 queues and auto/manual events are covered. FILE matching does not depend on caller-process identity. No IRP_MJ_CLEANUP handler is invented. Generated-header PID/FILE/control-wait mutations must report 104/105/106 only after joining every thread and reclaiming every hosted resource. A pool-free observer parks the control-wait mutation before any unsafe free. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
