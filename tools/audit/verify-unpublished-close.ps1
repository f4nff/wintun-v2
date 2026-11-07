param([string]$BaselineDirectory='out\ownership-handoff-baseline\9334ef9d25d1',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\unpublished-close-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Unpublished close audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='9d06e10637c4291c0479735b8ab0e54d41706dbfc0b1e923b4c3e42e00c8b459'
 'tests\safety\native_policy_fixture.c'='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
}
foreach($path in $frozen.Keys){if((Get-FileHash (Join-Path $baseline $path)).Hash.ToLowerInvariant() -ne $frozen[$path]){throw "Unexpected frozen source:$path"}}
$dependencies=@(rg --files api/v2 common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
 'tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){
    if($path.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $path}
    else{if((Get-FileHash (Join-Path $baseline $path)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $path)).Hash){throw "Unchanged production/fixture dependency changed:$path"}}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\unpublished-close\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-UnpublishedCloseProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start unpublished close test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Unpublished close external watchdog expired20s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-unpublished-close.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $marker='UNPUBLISHED_OWNER_HANDOFF_MISSING: refs2 construction+registry with publicReleased0/scheduled0/callbacks0 observed; actual ReleaseSession used once for negative cleanup while ordinary loader remained owned; real close/joins/unload completed with zero owned residue'
        $correct=$code -eq $Expected -and ($Expected -ne 118 -or $output.Contains($marker))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_mutation_failure=($Expected -eq 118 -and $correct);
            watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unpublished close test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-owner-handoff')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-UnpublishedCloseTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    if($variant -eq 'current') {
        if($built.extraction.source_sha256 -ne $built.extraction.selected_sha256){throw 'Current session selection is not byte-identical'}
        Invoke-UnpublishedCloseProcess $built.path @($built.fixture);Invoke-UnpublishedCloseProcess $built.path @($built.fixture,'118')
    }else{Invoke-UnpublishedCloseProcess $built.path @($built.fixture,'118') 118}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during unpublished close audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-1);
    kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    production_changed=$true;
    reviewed_dependency_repairs=@('common/offload_policy.c');
    method='Actual non-PB_API_TESTS Open/Destroy/lifecycle in a real unloadable DLL reuses the unchanged native-policy resource ledger. Three hosted pre-registration failures are PROBE I/O access-denied, malformed successful PROBE magic, and transactional REGISTER allocation failure with no publication. A real private event handle protected from Close supplies the defensive failure input; this is not a claim that normal private valid device Close naturally fails. Actual Open returns NULL and preserves the original failure through destructive cleanup. Without any observer Retain or issued public session handle, current Destroy publishes the runtime owner and actual ReleaseSession schedules its callback; registry/work refs2 and two pre-acquired module pins retain core/map/device/budget. Actual global GetRetained and GetRuntimeStatus snapshots finish while ordinary LoadLibrary is still owned. Only after every DLL call has returned is that ordinary reference dropped. From then on the host only clears protection and signals borrowed Device/Progress while a hard callback park guarantees their validity, releases host-owned gates, observes detach/module enumeration and joins actual callback OS-thread handles; the shared configuration stack outlives every join. Unregistered cleanup directly retries actual Close and must never send DRAIN/QUERY. Physical close, tail-pin transfer, registry/work last Drop/Destroy and real deferred unload balance the original exact heap/map/event/device/pool/work/module/runtime ledger. Full processes warm24 then measure24 with equal process handle counts. The negative changes only failed Destroy ReleaseSession+return to return after PublishSession; refs2 construction+registry, publicReleased0/scheduled0/callbacks0 is witnessed without waiting for an absent callback. While ordinary loader and those owners still live, actual ReleaseSession is invoked once solely for negative teardown, then identical real callback cleanup/joins/unload completes before118. Current builds never repair. This is an ownership coverage mutation, not a demonstrated historical defect. Native registration/IOCTL boundaries are hosted, not real kernel/device/driver/registry or Win7 runtime execution; version remains2.0.0. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    audit_sources=@('tests\model\unpublished_close_fixture.h','tests\safety\unpublished_close_fixture.c','tests\model\unpublished_close_tests.c','tools\build\unpublished-close-tests.ps1','tools\audit\verify-unpublished-close.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
