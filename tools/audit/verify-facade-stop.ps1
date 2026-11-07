param([string]$BaselineDirectory='out\queue-stop-baseline\a4e6a58f27c1',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\facade-stop-tests.ps1')
. (Join-Path $PSScriptRoot 'native-start-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Facade stop audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files api/v2 common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate core dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
 'api\session.c','api\compat_worker.h','api\compat_pump.h','api\termination_wait.h','api\wintun.h','api\logger.h','api\main.h','api\adapter.h',
 'tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{Assert-NativeStartDependency -BaselineDirectory $baseline -Relative $relative}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\facade-stop\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-FacadeStopProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start facade stop test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Facade stop external watchdog expired20s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-facade-stop.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $marker='FACADE_PENDING_RETURN_MISSING: worker Close boundary observed63 loans; same queue owner explicitly retired known63 tokens for negative cleanup before actual Close/Release, all users/workers joined and resources/unload converged'
        $correct=$code -eq $Expected -and ($Expected -ne 116 -or $output.Contains($marker))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_mutation_failure=($Expected -eq 116 -and $correct);
            watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Facade stop test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-pending-return')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-FacadeStopTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    if($variant -eq 'current'){
        if($built.extraction.worker_source_sha256 -ne $built.extraction.worker_sha256){throw 'Current worker is not byte-identical'}
        Invoke-FacadeStopProcess $built.path @($built.fixture);Invoke-FacadeStopProcess $built.path @($built.fixture,'116')
    }else{Invoke-FacadeStopProcess $built.path @($built.fixture,'116') 116}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during facade stop audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count-2;
    native_start_ledger=@{path='tools/audit/native-start-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'native-start-ledger.ps1')).Hash.ToLowerInvariant()};
    kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;
    production_changed=$true;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    method='Actual Wintun session/worker/pump with default PB_COMPAT_OPEN uses actual non-PB_API_TESTS native session. A factory boundary retains actual Open/Close and binds observation wrappers. The native-policy ledger is included once; legacy heap/events/thread/ring/path/module/runtime are separately tracked. Guard mode covers native objects/map; legacy byte rings remain real VirtualAlloc. An actual producer thread allocates8 old packets and sends in reverse order, joins before End, and Wake is pending before the selected native WaitForMultipleObjects call. Actual Reserve8 is followed by terminal at Commit, real STOPPED/accepted0 and same-owner Cancel8. Receive uses one hosted publication of64 valid max-size native descriptors with real64-slot/128-ring geometry; actual pump copies/retires1 and parks receive=false with63 native loans. Terminal stops it. A factory boundary gates only after actual BeginStop submits its callback and before the owner returns pending loans; each receive case deterministically witnesses an early native LEASES park with63 loans and no Close call. After releasing the owner, main waits an independent CloseEntered event proving loan0 (or completed negative repair), advances the early park, then observes the second park from actual QUERY before using final-close conditions. Actual CompatReturnPending retires63 in current builds. A public reader holds the copied private packet until the facade worker really exits, returns that valid issued pointer before End, and joins before ring destruction. Actual Close5000 is never shortened: normal send close waits behind native-active1; QUERY failure makes Close return CORRUPT and actual ReleaseSession transfers native ownership. Facade End can release private resources while native-active1 and registry/work preserve device/map/core; corrected QUERY/active0 completes actual cleanup. Every callback OS-thread handle is joined, ordinary LoadLibrary stays through all public/worker joins, actual unload and exact ledgers/handle convergence are checked. Full processes warm32 then measure32. Mutation omits only CompatReturnPending; its real queue owner observes63 loans at Close boundary, explicitly invokes actual ReleaseBatch with producer-known tokens solely for safe negative teardown, then normal Close/Release, joins/unload finish before116. Positive builds never repair. This is a coverage mutation, not a historical production defect. Native producer, IOCTLs and legacy MainRuntime boundary are hosted; no real device/driver/registry, Win7 runtime or kernel execution is claimed. Cold Allocate changes and the LSO/checksum dependency repair are independently hash-locked by their finite complete-file ledgers; all other listed dependencies remain unchanged. Historical policy dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair; all other source bindings and test paths retain their existing contracts.';
    audit_sources=@('tests\model\facade_stop_fixture.h','tests\safety\facade_stop_fixture.c','tests\model\facade_stop_tests.c','tools\build\facade-stop-tests.ps1','tools\audit\verify-facade-stop.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
