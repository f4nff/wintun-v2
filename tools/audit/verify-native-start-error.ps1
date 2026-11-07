param([string]$BaselineDirectory='out\queue-stop-baseline\a4e6a58f27c1',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\native-start-error-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Native-start error audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\queue-stop-baseline\a4e6a58f27c1')){throw 'Builder requires the frozen a4e6 whole-source baseline'}
$beforePath=Join-Path $baseline 'api\v2\session.c';$beforeHash='5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'
if((Get-FileHash -LiteralPath $beforePath).Hash.ToLowerInvariant() -ne $beforeHash){throw 'Unexpected frozen native session.c'}
$before=[IO.File]::ReadAllText($beforePath).Replace("`r`n","`n");$after=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c')).Replace("`r`n","`n")
$startMarker='static PB_RESULT Allocate(const PB_CONFIG *config, PB_SESSION **out)';$endMarker='/* Consumes StartAdapterSession'
foreach($text in @($before,$after)){foreach($marker in @($startMarker,$endMarker)){if([regex]::Matches($text,[regex]::Escape($marker)).Count -ne 1){throw 'Allocate region markers are not unique'}}}
$beforeStart=$before.IndexOf($startMarker);$beforeEnd=$before.IndexOf($endMarker,$beforeStart)
$afterStart=$after.IndexOf($startMarker);$afterEnd=$after.IndexOf($endMarker,$afterStart)
if($beforeStart -lt 0 -or $beforeEnd -le $beforeStart -or $afterStart -lt 0 -or $afterEnd -le $afterStart){throw 'Cannot isolate complete Allocate function'}
if($before.Substring(0,$beforeStart) -cne $after.Substring(0,$afterStart) -or $before.Substring($beforeEnd) -cne $after.Substring($afterEnd)){throw 'Native session production changed outside Allocate'}
$dependencies=@(rg --files api/v2 common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native startup dependencies'}
$dependencies=@($dependencies|Where-Object {$_ -replace '/','\' -ne 'api\v2\session.c'})
$dependencies+=@('api\session.c','api\compat_worker.h','api\compat_pump.h','api\adapter.h','api\wintun.h','api\logger.h','api\main.h','api\operation.h','api\registry.h','api\termination_wait.h',
    'include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h','tests\safety\guard_memory.h')
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $relative)).Hash){throw "Native-start dependency changed: $relative"}}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\native-start-error\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false);$frozen=Join-Path $directory 'allocate-before.c';$current=Join-Path $directory 'allocate-current.c'
[IO.File]::WriteAllText($frozen,$before.Substring($beforeStart,$beforeEnd-$beforeStart),$encoding);[IO.File]::WriteAllText($current,$after.Substring($afterStart,$afterEnd-$afterStart),$encoding)
Start-TestRecording $directory 'operation-results.json'
function Invoke-NativeStartProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start native-start test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Native-start external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-native-start.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='NATIVE_START_RESOURCE_ERROR_LOST: actual non-test Api.Open returned PB_FULL after calloc failure; legacy reported unrelated/fallback error; all owners reclaimed'
        $correct=$code -eq $Expected -and ($Expected -ne 115 -or $output.Contains($marker))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_baseline_failure=($Expected -eq 115 -and $correct);watchdog_ms=20000;log=$log;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unexpected native-start test: exit=$code expected=$Expected"}
    }finally{$process.Dispose()}
}
$artifacts=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('before','current')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $build=Build-NativeStartErrorTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;$artifacts+=$build
    if($variant -eq 'before'){Invoke-NativeStartProcess $build.path @('1') 115}
    else{Invoke-NativeStartProcess $build.path @();Invoke-NativeStartProcess $build.path @('1')}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during native-start audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;frozen_session_sha256=$beforeHash;unchanged_dependencies=($dependencies.Count-1);
    allocate_before=@{path=$frozen;offset=$beforeStart;length=$beforeEnd-$beforeStart;sha256=(Get-FileHash -LiteralPath $frozen).Hash.ToLowerInvariant()};
    allocate_current=@{path=$current;offset=$afterStart;length=$afterEnd-$afterStart;sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant()};
    operations=$script:TestResultFile;artifacts=$artifacts;kernel_runtime_tested=$false;production_ready=$false;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    production_changed=$true;
    reviewed_dependency_repairs=@('common/offload_policy.c');
    method='Frozen whole non-test native session.c hash checked; native session.c differs only in complete Allocate; common/offload_policy.c separately includes the strict reviewed LSO/checksum dependency repair. Builder normalizes local include paths only. Actual legacy session.c and CompatWorker use default PB_COMPAT_OPEN; a forwarding native factory/Open observer only records and returns the actual result/LastError. Native OpenSession/Allocate and real InitializeLifecycle/Destroy execute; no fabricated Api.Open result, PB_API_TESTS shortcut or runtime count/byte override. Before CLI1 requires actual PB_FULL plus zero native error becoming legacy NOT_SUPPORTED50 after first calloc NULL/errnoENOMEM; joined worker witness, both full owner ledgers and private heap destroy precede115. Current facade tests every one-queue acquisition prefix, ambient errors, cleanup clobber, immediate first-failure short circuit, real file/PROBE/register errors and epoch0/BCrypt failure; direct actual native Open covers every2/8queue prefix. Direct Allocate config invalid/unsupported contract tested. Ten false+zero Win32 acquisitions are defensive fault models only. Quota uses real Allocate reservations before devices:16 actual constructions for count, or three large actual layouts leaving less than legacy-open bytes; all are destroyed afterward. Independent core/facade ledgers cover calloc/guard allocations, mappings, event-backed device/queue handles, real module pins, private pools/work objects, path, private legacy heap, rings/runtime budget and actual joined facade thread. Native failed construction publishes no callback/work; this test does not model a hung device-close barrier or actual DLL unload. Full current workload warms then repeats with exact process handle equality; every fixture-owned ledger returns to zero per scenario. Native data I/O/security/driver behavior is hosted; no real devices/registry/driver, old-OS runtime, NDIS/IRQL or kernel CPU execution claim. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
