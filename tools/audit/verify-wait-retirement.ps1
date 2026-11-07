param([string]$BaselineDirectory='out\wait-retirement-baseline\9d75adca3c0b',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\wait-retirement-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Wait retirement audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='9d06e10637c4291c0479735b8ab0e54d41706dbfc0b1e923b4c3e42e00c8b459'
 'api\v2\session_lifecycle.h'='33806814b12535da7304b63d544aa0b31c9862a8c7b465eb004d374a9455392d'
 'tests\safety\native_policy_fixture.c'='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
}
foreach($path in $frozen.Keys){if((Get-FileHash (Join-Path $baseline $path)).Hash.ToLowerInvariant() -ne $frozen[$path]){throw "Unexpected frozen source:$path"}}
$dependencies=@(rg --files api/v2 common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
 'tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){
    if($path.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $path}
    else{if((Get-FileHash (Join-Path $baseline $path)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $path)).Hash){throw "Unchanged dependency changed:$path"}}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\wait-retirement\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-WaitRetirementProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start wait retirement test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Wait retirement external watchdog expired20s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-wait-retirement.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $marker='RETIREMENT_SNAPSHOT_GATE_MISSING: lifecycle QUERY reached while last owner held shared DataGate after all writes; nativeActive1 prevented preparation, every owner joined before native0 and finalClose, all resources/threads/unload converged'
        $correct=$code -eq $Expected -and ($Expected -ne 121 -or $output.Contains($marker))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_mutation_failure=($Expected -eq 121 -and $correct);
            watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Wait retirement test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-snapshot-gate')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-WaitRetirementTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    if($variant -eq 'current') {
        if($built.extraction.source_sha256 -ne $built.extraction.selected_sha256){throw 'Current lifecycle selection is not byte-identical'}
        Invoke-WaitRetirementProcess $built.path @($built.fixture);Invoke-WaitRetirementProcess $built.path @($built.fixture,'121')
    }else{Invoke-WaitRetirementProcess $built.path @($built.fixture,'121') 121}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during wait retirement audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-1);
    kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    production_changed=$true;
    reviewed_dependency_repairs=@('common/offload_policy.c');
    method='Actual full non-PB_API_TESTS session/lifecycle in a real unloadable DLL reuses the unchanged native-policy host/ledger. One/two/eight real queue-owner threads import a single hosted native publication and reserve batches1/2/3/7/8/31/63/64, then actually Wait(INFINITE). The wait wrapper marks the selected native call site, not proof of kernel scheduler parking. Explicit BeginStop broadcasts cancellation; a separate defensive malformed peer completion lets queue0 actual Poll cause Fatal/ClosingEvent wake of other waiting queues. This is not a normal valid API publication. Each same owner returns TX loans and cancels RX reservations in both orders; running geometry64slots/128ring is respected, never fake FULL/partial acceptance. All other owners join before the last one enters a hard gate after every private write/output count but before releasing DataGate/Lifetime shared. The real lifecycle writer TryEX fails while zero loan counts are visible. PrepareClose remains non-consuming; nativeActive1 independently prevents PREPARED after all owners join, and only native0 permits preparation. FinalClose is invoked strictly after public owners join; actual private-pool callback OS-thread handles join and actual deferred module unload/exact resource/handle convergence are checked. Full processes warm12 then measure12 schedules. The negative omits only the first lifecycle ownership-snapshot DataGate acquire/release pair. Other owners have joined and all last-owner writes are completed, so its early QUERY observer requires no unsafe concurrent private writes or UAF; nativeActive1 blocks preparation. Last owner is released/joined before native0 and normal finalClose, with every resource/thread/unload complete before121. No repair refs or finalClose beside live Wait is used. This is snapshot-serialization coverage, not a demonstrated production defect or a claim that PrepareClose must wait every API tail. Native producer/IOCTL/reference state are hosted, not real Register/driver/device/NDIS/IRQL or Win7 runtime execution. PB_API_TESTS is rejected and shipping source fingerprint statically required; version remains2.0.0. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.';
    audit_sources=@('tests\model\wait_retirement_fixture.h','tests\safety\wait_retirement_fixture.c','tests\model\wait_retirement_tests.c','tools\build\wait-retirement-tests.ps1','tools\audit\verify-wait-retirement.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
