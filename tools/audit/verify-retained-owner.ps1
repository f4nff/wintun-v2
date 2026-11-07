param([string]$BaselineDirectory='out\diagnostic-owner-baseline\5fd7eb8e2353',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\retained-owner-tests.ps1')
. (Join-Path $PSScriptRoot 'native-start-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Retained owner audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'
 'api\v2\session_lifecycle.h'='33806814b12535da7304b63d544aa0b31c9862a8c7b465eb004d374a9455392d'
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
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\retained-owner\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-RetainedOwnerProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program)
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start retained owner test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Retained owner external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-retained-owner.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $marker='DIAGNOSTIC_LOOKUP_PIN_MISSING: refs=2 witnessed while public+registry live; unpinned diagnostic completed/joined before public release, normal cleanup and DLL unload finished with zero owned residue'
        $correct=$code -eq $Expected -and ($Expected -ne 111 -or $output.Contains($marker))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_mutation_failure=($Expected -eq 111 -and $correct);
            watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;reported_pass_messages=@($output -split "\r?\n"|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'})
        if(!$correct){throw "Retained owner test failed: exit $code, expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-diagnostic-pin')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-RetainedOwnerTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    if($variant -eq 'current'){Invoke-RetainedOwnerProcess $built.path @($built.fixture);Invoke-RetainedOwnerProcess $built.path @($built.fixture,'111')}
    else{Invoke-RetainedOwnerProcess $built.path @($built.fixture,'111') 111}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during retained owner audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-2);reviewed_dependencies=$dependencies.Count;
    native_start_ledger=@{path='tools/audit/native-start-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'native-start-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    production_changed=$true;
    reviewed_dependency_repairs=@('common/offload_policy.c');
    historical_dependency_repairs=@('api/v2/session.c');
    method='Actual non-PB_API_TESTS DLL session/lifecycle and GetRetained; the native-policy hosted IOCTL and resource ledger are included once rather than copied. A shared State-entry wrapper gates the diagnostic after actual RuntimeLock lookup/Retain and unlock, without any observer Retain. Public release, hosted native-active drain, physical resource close, registry removal and callback work-reference Drop leave a Closed shell owned only by that diagnostic. Empty enumeration does not expose the removed core. Releasing/joining the diagnostic proves its final Drop destroys core/work/pool/module pin on its own OS thread. All captured callback OS thread handles are joined after pool close and ordinary LoadLibrary remains through the joins, then actual DLL detach checks exact resource/quota convergence. Each current full process warms16 complete schedules then measures16 with equal process handle count. The paired GetRetained Retain/Drop mutation observes refs2 while public+registry remain live, finishes and joins the unpinned diagnostic before public release, then completes normal shutdown/thread joins/unload before exit111. No repair reference, unpaired Drop, stale public handle or intentional use-after-free is used. This is a regression for a previously uncovered valid ownership window, not a historical production defect. IOCTLs and native-active are hosted; no device, driver, Win7 runtime or kernel execution is claimed. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\retained_owner_fixture.h','tests\safety\retained_owner_fixture.c','tests\model\retained_owner_tests.c','tools\build\retained-owner-tests.ps1','tools\audit\verify-retained-owner.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
