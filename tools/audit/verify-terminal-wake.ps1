param([string]$BaselineDirectory='out\terminal-wake-baseline\e16627eaddcc')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\terminal-wake-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Terminal wake audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$production=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
$production+=@('api\v2\packetbridge.h','api\v2\lifecycle.h','api\v2\session_state.h','api\v2\session_queue.h',
    'include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h')
foreach($path in $production){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$reviewedDependencyRepairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h')
$unchangedProduction=$production.Count-$reviewedDependencyRepairs.Count
$directory=Join-Path $script:RepoRoot "out\terminal-wake-investigation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
function Invoke-TerminalWakeProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program)
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start terminal wake test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(15000)){$process.Kill($true);$process.WaitForExit();throw 'Terminal wake external process watchdog expired (15s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-terminal-wake.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $correct=$code -eq $Expected -and ($Expected -ne 107 -or $output.Contains('TERMINAL_USER_EVENT_BROADCAST_MISSING'))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_mutation_failure=($Expected -eq 107 -and $correct);watchdog_ms=15000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log})
        Write-TestRecording $(if($correct){'running'}else{'failed'})
        if(!$correct){throw "Terminal wake process failed: exit $code, expected $Expected"}
    }finally{$process.Dispose()}
}
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-terminal-broadcast')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$variant-$mode-$memory"
    $built=Build-TerminalWakeTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    if($variant -eq 'current'){
        foreach($copy in $built.extraction.copies){if($copy.source_sha256 -ne $copy.header_sha256){throw 'Current SYS header is not byte-identical'}}
        Invoke-TerminalWakeProcess $built.path @('1');Invoke-TerminalWakeProcess $built.path @()
    }else{
        Invoke-TerminalWakeProcess $built.path @('1') 107
    }
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during terminal wake audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=$reviewedDependencyRepairs;unchanged_dependencies=$unchangedProduction;production_changed=$true;
    production_dependencies_unchanged=$unchangedProduction;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\terminal_wake_tests.c','tools\build\terminal-wake-tests.ps1','tools\audit\verify-terminal-wake.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual SYS Progress/Fault/References/Drain/Recycle headers copied verbatim for current builds and actual DLL session_state/session_queue (PB_API_TESTS disabled) use a shared mapping and real Windows events, locks and joined owner threads. Ready construction, an explicit held native reference, and final destruction are hosted; native NDIS, IRQL, system workers and full DLL Open/Close/lifecycle are not executed. Owners themselves borrow TX/RX, wait, stop admission, cancel and return; the coordinator never calls their queue APIs. Gates cover lifetime entry, both arms before/after, native multiwait entry, and selection before terminal. Selection means the native wait wrapper reached its call site, not proof of kernel scheduler parking. Terminal after a final observation is not required to retroactively stop a call. Progress-only does not signal user events or stop waiters. A publisher held after first user broadcast proves ClosingEvent cascades to other queues before their SYS broadcast. Terminal cannot retire loans, clear the independent native reference or release mapping. Mutation 107 removes only terminal user broadcasts, observes the missing wake of an infinite wait, explicitly rescues it, then joins/frees everything before reporting. Shipping fingerprint is statically required. External process watchdog remains the bound for an unexpected infinite wait. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
