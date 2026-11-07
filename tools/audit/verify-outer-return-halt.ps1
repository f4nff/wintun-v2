param([string]$BaselineDirectory='out\outer-return-baseline\180d7cbe5ff4')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\kernel-return-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Outer callback audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$production=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
foreach($path in $production){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$reviewedDependencyRepairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h')
$unchangedProduction=$production.Count-$reviewedDependencyRepairs.Count
$directory=Join-Path $script:RepoRoot "out\outer-return-investigation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-outer-rundown')){
    $memory=if($guarded){'guarded'}else{'ordinary'}
    $objects=Join-Path $directory "$variant-$mode-$memory"
    $built=Build-KernelReturnTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $exe=$built.path
    # Re-read exact production substrings: generated declarations/bodies are not
    # a hand-maintained copy. The only mutation is the unique outer wait line.
    $driverText=[IO.File]::ReadAllText($built.extraction.driver)
    if([IO.File]::ReadAllText($built.extraction.return_extraction) -cne $driverText.Substring($built.extraction.return_offset,$built.extraction.return_length) -or
       [IO.File]::ReadAllText($built.extraction.halt_extraction) -cne $driverText.Substring($built.extraction.halt_offset,$built.extraction.halt_length)){throw 'Callback extraction is not verbatim'}
    if($variant -eq 'current'){Invoke-TestTool $exe @('1');Invoke-TestTool $exe @()}
    else{
        $output=@(& $exe 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 96 -and ($output -join "`n").Contains('HALT_SKIPPED_OUTER_RETURN_RUNDOWN')
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=96;expected_mutation_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw 'Outer callback barrier mutation was not rejected'}
    }
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during outer callback audit'}
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
    method='Every build extracts actual TunReturnNetBufferLists and TunHaltEx declarations/bodies verbatim from current driver/wintun.c. Current ISO/MS ordinary/guarded each execute CLI1 plus 560 schedules; four isolated generated-header mutations omit only the outer Halt rundown wait and require exit 96 after all callers and objects are reclaimed. Tests execute actual session Return/Unregister/Free, guarded MDL/NBL/pool objects and Windows threads/events/locks. Fixture establishes returned NBL ownership and a completed-pause boundary (Running false, Active zero) before invoking Halt; it does not execute real Register, Pause, ControlRundown, NDIS or IRQL. The actual fake workers only wait for Stop and are joined. The pool-free observer detects a bad Halt before releasing objects, parks until all outer callbacks leave, then permits complete fixture cleanup rather than intentionally causing an unsafe free. One/two/eight concurrent callback jobs cover every last-reference and session-pin pair, matching/different last-outer-pin rotations, both event reset types and fault/normal stop. Separate 65-NBL chain crosses the 64-original window boundary, alternating-queue chain forces three windows, and controlled same-thread nested callback checks reentrancy. Actual production worker/event reset-arm paths remain covered by existing tests, not this harness. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
