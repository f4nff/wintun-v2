param([string]$BaselineDirectory='out\dispatch-register-baseline\7b8cd9b2cae5',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\status-lifetime-tests.ps1')
. (Join-Path $PSScriptRoot 'native-start-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Status lifetime audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'
 'api\v2\session_status.h'='8879fdc9a562d998efaa3376be11b0d0ec50adcb098fb7a208e26bc5013a8584'
 'tests\safety\native_policy_fixture.c'='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
}
foreach($relative in $frozen.Keys){if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant() -ne $frozen[$relative]){throw "Unexpected baseline: $relative"}}
$dependencies=@(rg --files api/v2 common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate core dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{Assert-NativeStartDependency -BaselineDirectory $baseline -Relative $relative}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\status-lifetime\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-StatusLifetimeProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program)
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start status lifetime test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Status lifetime external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-status-lifetime.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $marker='STATUS_LIFETIME_PIN_MISSING: real worker held Lifetime exclusive while status reader remained in I/O; reader released/joined before physical close, normal cleanup and DLL unload completed with zero owned residue'
        $correct=$code -eq $Expected -and ($Expected -ne 113 -or $output.Contains($marker))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_mutation_failure=($Expected -eq 113 -and $correct);
            watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;reported_pass_messages=@($output -split "\r?\n"|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'})
        if(!$correct){throw "Status lifetime test failed: exit $code, expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-status-pin')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-StatusLifetimeTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    if($variant -eq 'current') {
        if($built.extraction.source_sha256 -ne $built.extraction.header_sha256){throw 'Current status header is not byte-identical'}
        Invoke-StatusLifetimeProcess $built.path @($built.fixture);Invoke-StatusLifetimeProcess $built.path @($built.fixture,'113')
    } else {Invoke-StatusLifetimeProcess $built.path @($built.fixture,'113') 113}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during status lifetime audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-2);reviewed_dependencies=$dependencies.Count;
    native_start_ledger=@{path='tools/audit/native-start-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'native-start-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    production_changed=$true;
    reviewed_dependency_repairs=@('common/offload_policy.c');
    historical_dependency_repairs=@('api/v2/session.c');
    method='Actual non-PB_API_TESTS session/status/lifecycle in a real unloadable DLL includes the unchanged native-policy driver boundary and exact resource ledger. The first Close(0) returns PB_QUARANTINED with public ownership retained before a new reader enters GetStatus or GetCapabilities. Hosted native-active1 parks lifecycle until that reader is inside actual IOCTL with Lifetime shared. After native-active0, the worker wrapper first tries a real exclusive SRW acquisition: current must fail immediately while the reader remains in I/O, then uses actual blocking Acquire. No timed absence is used as the primary witness. After reader success/access-denied failure and OS-thread join, the worker owns real EX and is gated before ReleaseResources; both current Try readers return BUSY without I/O. Physical close and registry/work release leave a Closed shell with sole public ownership; GetStatus synthesizes STOPPED/zero references, capabilities/policy/wake return STOPPED without more I/O. Final successful Close consumes public only after readers join. Every callback OS-thread handle is joined after pool close, ordinary LoadLibrary remains through all joins, actual DLL detach and independent ledgers prove zero heap/map/event/device/work/pool/module/runtime residue. Each current full process warms32 schedules then measures32 with exact equal process handle count. The paired GetStatus TryAcquire/Release mutation lets the real worker hold EX while the reader is still inside I/O; before any physical release the reader is released/joined, then normal cleanup/unload completes before113. This is a coverage observer mutation, not a demonstrated historical production defect. IOCTLs/native-active are hosted; no real device, driver, Win7 runtime or kernel execution is claimed. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\status_lifetime_fixture.h','tests\safety\status_lifetime_fixture.c','tests\model\status_lifetime_tests.c','tools\build\status-lifetime-tests.ps1','tools\audit\verify-status-lifetime.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
