param([string]$BaselineDirectory='out\queue-stop-baseline\a4e6a58f27c1')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\indication-pause-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Indication/pause audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate kernel dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=Join-Path $script:RepoRoot "out\indication-pause\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-IndicationPauseProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start indication/pause test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Indication/pause external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-indication-pause.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $correct=$code -eq $Expected -and ($Expected -ne 114 -or $output.Contains('INDICATION_PAUSE_SKIPPED_BANK_WAIT: early Pause observed with all receive objects returned; upcalls and every test thread joined before native session/Ctx free; zero owned resources'))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_mutation_failure=($Expected -eq 114 -and $correct);watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Indication/pause test failed: $code expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-indication-wait')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-IndicationPauseTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $extract=$built.extractions
    foreach($part in $extract.parts){if([IO.File]::ReadAllText($part.path) -cne [IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length)){throw "Production extraction changed: $($part.name)"}}
    if($extract.host_source_sha256 -ne $extract.host_original_sha256){throw 'Original NBL host snapshot is not byte identical'}
    $expectedHost=[IO.File]::ReadAllText($extract.host_original).Replace($extract.field_marker,$extract.field_marker+"`r`n"+$extract.field_inserted)
    foreach($change in $extract.include_changes){$expectedHost=$expectedHost.Replace($change.original,$change.adapted)}
    if($expectedHost -cne [IO.File]::ReadAllText($extract.host)){throw 'NBL host adaptation has additional changes'}
    $pause=$extract.parts|Where-Object name -eq 'pause';$expectedPause=[IO.File]::ReadAllText($pause.path)
    if($variant -eq 'without-indication-wait'){$expectedPause=$expectedPause.Replace($extract.mutation_original,$extract.mutation_replacement)}
    if($expectedPause -cne [IO.File]::ReadAllText($extract.pause)){throw 'Pause selection has additional changes'}
    if($variant -eq 'current'){Invoke-IndicationPauseProcess $built.path @('114');Invoke-IndicationPauseProcess $built.path @()}
    else{Invoke-IndicationPauseProcess $built.path @('114') 114}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during indication/pause audit'}
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
    audit_sources=@('tests\model\indication_pause_tests.c','tools\build\indication-pause-tests.ps1','tools\audit\verify-indication-pause.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual current ReceiveBatch, outer Return, TunPause, PbV2Pause and indication-gate functions are extracted verbatim on each build. Actual native session references, completion/return, allocation and Free are included; the hash-locked existing NBL host has only one indication-bank field insertion and absolute include paths in ignored output, with original/adapted bytes and all offsets/markers/hashes recorded. Ready sessions submit one/two ordinary IPv4 NBLs per queue; ReceiveBatch waits inside hosted indication upcalls with no transition/queue lock. An independent actual outer Return OS thread joins before Pause, proving Active/AllReturned/session Returns/outer ReturnRundown are drained while the upcalls remain in flight. Three shapes select bank0, bank1 after a completed serialized rotation, or bank0 plus bank1 while the actual serialized rotation waits old bank0. Actual TunPause clears Running under exclusive TransitionLock; actual inner Pause holds its session pin after dropping RegistrationLock and waits both bank events. A selected nonsignaled bank wait is observed before releasing the upcalls; this is a wait-entry observation, not a claim that the OS thread is already parked. Twelve matrix cases combine those shapes with one/two NBLs and auto/manual queue events; CLI114 selects the dual-bank case. The unique negative omits only both Pause bank waits, safely observes premature Pause completion while all objects remain owned, releases upcalls and joins every Receive/Return/Pause/rotation OS thread before any actual Free, then reports114 with zero owned pool/MDL/NBL/event/region/thread resources and equal process handles. No Halt is started, so no Halt-before-Pause schedule is used. Receive threads invoke actual ReceiveBatch once, not the worker loop; registration and serialized policy rotation caller are hosted, not actual OID/NDIS/IRQL or device/driver execution. PB_API_TESTS is rejected; the builder binds the 64-character current shipping source fingerprint, statically required at 65 bytes. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
