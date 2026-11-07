param([string]$BaselineDirectory='out\dispatch-register-baseline\7b8cd9b2cae5')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\dispatch-register-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Dispatch/register audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate kernel dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\register_owner_tests.c','tests\safety\guard_memory.h','tools\build\kernel-register-tests.ps1')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=Join-Path $script:RepoRoot "out\dispatch-register\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-DispatchRegisterProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start dispatch/register test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Dispatch/register external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-dispatch-register.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $correct=$code -eq $Expected -and ($Expected -ne 112 -or $output.Contains('DISPATCH_HALT_SKIPPED_CONTROL_WAIT: unsafe free parked; actual Register/Dispatch joined before native pool/Ctx free; zero owned resources'))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_mutation_failure=($Expected -eq 112 -and $correct);watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Dispatch/register test failed: $code expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-control-wait')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-DispatchRegisterTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $extract=$built.extractions
    foreach($part in $extract.parts){if([IO.File]::ReadAllText($part.path) -cne [IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length)){throw "Production extraction changed: $($part.name)"}}
    $r=$extract.register;if([IO.File]::ReadAllText($r.extracted) -cne [IO.File]::ReadAllText($r.source).Substring($r.offset,$r.length)){throw 'Register extraction changed'}
    if($variant -eq 'current' -and (Get-FileHash $extract.gate).Hash -ne (Get-FileHash $extract.gate_source).Hash){throw 'Current context gate is not byte identical'}
    if($extract.host_source_sha256 -ne $extract.host_original_sha256){throw 'Original host snapshot is not byte identical'}
    if($variant -eq 'current'){Invoke-DispatchRegisterProcess $built.path @('1');Invoke-DispatchRegisterProcess $built.path @()}
    else{Invoke-DispatchRegisterProcess $built.path @('1') 112}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during dispatch/register audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');production_changed=$true;
    unchanged_dependencies=($dependencies.Count-3);
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\dispatch_register_tests.c','tools\build\dispatch-register-tests.ps1','tools\audit\verify-dispatch-register.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual current DeviceControl, common ForOwner/Unregister, Halt, legacy IOCTL definition, Register and full v2 Control router are extracted verbatim per build; actual context_gate/query/drain/session Free/Unregister remain production sources. The existing hash-locked register host is reused, with only its private Ctx/IRP typedef region and single static Context storage declaration adapted in ignored output plus absolute include paths. Original host bytes, adapted host, typedef region offsets/markers and every source/output hash are recorded. Every case models already completed Pause: Running false, RunningEvent clear, no NBL or receive references. Control lookup gate runs only after releasing CtxGuard, leaving an actual Control pin while Halt revokes lookup and starts its real SRW rundown wait. Four critical Register failure prefixes (first pool, map, final event reference, final worker thread) and successful registration execute after revoke; partial workers are joined on rollback. A second post-revoke IRP is refused. After RegistrationLock release, the pin tail can still block Halt despite zero session resources; after the last pin is released, guarded Ctx can be physically freed before original dispatcher completes its IRP, which has no further Ctx access. Negative omits only the unique Control wait, parks at NBL-pool free before any unsafe physical reclamation, finishes and joins the actual Register/Dispatch caller before resuming safe Halt/free and reporting112. Dispatch surface checks nine recognized codes under access denial and absent Ctx, live register/short query/unsupported probe and legacy routing, unknown forwarded ownership, actual successful Register and positive-sized Query output, paired security subject release, matching NTSTATUS/Information and exactly one completion. New security/IRP/NDIS/indication boundaries are hosted; real Windows locks/events/heap/guard pages and joined OS threads implement scheduling. No native driver/device/registry/security evaluation, NDIS/IRQL, real Pause or production CPU execution is claimed. Version remains2.0.0; PB_API_TESTS is rejected and source fingerprint layout is statically required. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
