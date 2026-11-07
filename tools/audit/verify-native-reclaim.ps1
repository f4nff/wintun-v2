param([string]$BaselineDirectory='out\native-close-baseline\019ed63327d9')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\native-reclaim-tests.ps1')
. (Join-Path $PSScriptRoot 'native-start-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Native reclamation audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common api/v2)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native reclamation dependencies'}
$dependencies+=@('include\packetbridge_version.h','include\packetbridge_probe.h','include\packetbridge_types.h','include\packetbridge_v2_abi.h','tests\model\kernel_nbl_fixture.h')
foreach($path in $dependencies){
    if($path.Replace('\','/') -cin @('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h')){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
    else{Assert-NativeStartDependency -BaselineDirectory $baseline -Relative $path}
}
$directory=Join-Path $script:RepoRoot "out\native-reclaim-investigation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-NativeReclaimProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start native reclamation test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Native reclamation external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-native-reclaim.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $correct=$code -eq $Expected -and ($Expected -ne 109 -or $output.Contains('NATIVE_CLOSE_SKIPPED_RETURNS_BARRIER'))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_mutation_failure=($Expected -eq 109 -and $correct);watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Native reclamation process failed: $code expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-returns-wait')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$variant-$mode-$memory"
    $built=Build-NativeReclaimTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $part=$built.extraction;$source=[IO.File]::ReadAllText($part.source)
    if([IO.File]::ReadAllText($part.extracted) -cne $source.Substring($part.offset,$part.length)){throw 'SYS Free extraction is not verbatim'}
    if($variant -eq 'current'){Invoke-NativeReclaimProcess $built.path @('1');Invoke-NativeReclaimProcess $built.path @()}
    else{Invoke-NativeReclaimProcess $built.path @('1') 109}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during native reclamation audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');unchanged_dependencies=($dependencies.Count-4);production_changed=$true;historical_dependency_repairs=@('api/v2/session.c');
    dependencies_unchanged=($dependencies.Count-4);reviewed_dependencies=$dependencies.Count;
    native_start_ledger=@{path='tools/audit/native-start-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'native-start-ledger.ps1')).Hash.ToLowerInvariant()};
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\native_reclaim_tests.c','tools\build\native-reclaim-tests.ps1','tools\audit\verify-native-reclaim.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual non-PB_API_TESTS DLL session.c lifecycle and queue APIs share one real mapping with actual SYS ReceiveBatch/Return/Fault/Quiesce/Query/Unregister/Free. Public queue owner Reserve/Commit runs in a joined thread before stop and final Close; public status/progress callers do not run during final Close. QUERY IO forwards actual PbV2Query output, never synthesizes STOPPED. Ready REGISTER factory, idle-only kernel worker threads and device/NDIS/IRQL/indication boundaries are hosted. MmUnlockPages only ends SYS lock ownership; DLL VirtualFree occurs after actual SYS Free finishes. Missing queue witness and held NBL each prevent native STOPPED and DLL Published retirement; trusted STOPPED permits PREPARED while a return tail retains Returns. Actual final Close uses RegistrationLock exclusive and actual Unregister/Free. Positive waits on real SRW rundown conflict. Negative removes only Returns wait, parks in first ObDereferenceObject observer before closing/freeing any object, then actual return tail is rescued and OS thread joined before native physical reclamation resumes and 109 is reported. No CloseHandle failure is injected; successful fake-device closure is modeled after Unregister, not a claim about I/O-manager scheduling. Callback wrapper validates actual LifecycleWorker identity and duplicates its OS thread handle on every entry; after work completes and actual Destroy closes the private pool, each handle is waited/closed individually, including repeated handles for one thread. Normal and corrupt-fault checked warmups precede 60 measured schedules; exact fixture counts return zero after every run and process handles match before/after the measured matrix. DLL APIs and core use shipping fingerprint and Win7 build target. Native NDIS/IRQL, full Register/Pause/Halt, adapter create/close, process termination and production kernel CPU are not executed. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers. The existing reviewed Allocate failure-contract ledger is preserved independently.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
