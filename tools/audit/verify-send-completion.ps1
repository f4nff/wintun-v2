param([string]$BaselineDirectory='out\ownership-handoff-baseline\9334ef9d25d1')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\send-completion-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Send completion audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate kernel dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h','tests\model\indication_pause_tests.c','tools\build\indication-pause-tests.ps1')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=Join-Path $script:RepoRoot "out\send-completion\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-SendCompletionProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start send completion test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Send completion external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-send-completion.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $correct=$code -eq $Expected -and ($Expected -ne 117 -or $output.Contains('SEND_COMPLETION_SKIPPED_RETURNS_WAIT: first resource release parked with send tail pinned; actual Send and Pause joined before native Free resumes; zero owned resources'))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_mutation_failure=($Expected -eq 117 -and $correct);watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Send completion test failed: $code expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-returns-wait')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-SendCompletionTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $extract=$built.extractions
    foreach($part in @($extract.parts)+@($extract.base.parts)){if([IO.File]::ReadAllText($part.path) -cne [IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length)){throw "Production extraction changed: $($part.name)"}}
    $base=$extract.base
    if($base.host_source_sha256 -ne $base.host_original_sha256 -or $extract.reused_source_sha256 -ne $extract.reused_original_sha256){throw 'Original reused host snapshot is not byte identical'}
    $expectedHost=[IO.File]::ReadAllText($base.host_original).Replace($base.field_marker,$base.field_marker+"`r`n"+$base.field_inserted)
    foreach($change in $base.include_changes){$expectedHost=$expectedHost.Replace($change.original,$change.adapted)}
    if($expectedHost -cne [IO.File]::ReadAllText($base.host)){throw 'Base NBL host adaptation has additional changes'}
    if($expectedHost.Replace($extract.device_original,$extract.device_adapted) -cne [IO.File]::ReadAllText($extract.host)){throw 'Completion NBL host has additional changes'}
    $expectedReuse=[IO.File]::ReadAllText($extract.reused_original)
    foreach($change in @($extract.include_changes)+@($extract.observer_changes)){$expectedReuse=$expectedReuse.Replace($change.original,$change.adapted)}
    if($expectedReuse -cne [IO.File]::ReadAllText($extract.reused)){throw 'Reused indication host has additional changes'}
    $cleanup=$extract.parts|Where-Object name -eq 'cleanup';$expectedCleanup=[IO.File]::ReadAllText($cleanup.path)
    if($variant -eq 'without-returns-wait'){$expectedCleanup=$expectedCleanup.Replace($extract.mutation_original,$extract.mutation_replacement)}
    if($expectedCleanup -cne [IO.File]::ReadAllText($extract.cleanup)){throw 'Free selection has additional changes'}
    if($variant -eq 'current'){Invoke-SendCompletionProcess $built.path @('117');Invoke-SendCompletionProcess $built.path @()}
    else{Invoke-SendCompletionProcess $built.path @('117') 117}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during send completion audit'}
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
    audit_sources=@('tests\model\send_completion_tests.c','tools\build\send-completion-tests.ps1','tools\audit\verify-send-completion.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual Send and complete Free headers, common FILE-owner unregister functions and existing actual TunPause/PbV2Pause/bank functions are extracted verbatim per build. Actual references, fault, inner Unregister, completion and allocation paths remain production source. The existing hash-locked NBL/indication fixture is reused through finite ignored-output adaptations: extra Device owner/list fields, absolute source includes, exactly three observer calls before a native wait/rundown wait/first object release, and token-renamed unused prior main. Original/adapted source and exact extraction offsets/markers/hashes are recorded; existing fixture source files are unchanged. The ready kernel session factory models registration and idle-only workers: real OS worker threads own their ready queues and invoke actual fault-quiescence before exit, but do not run the complete worker loop. A nonempty modeled NBL/NB/MDL owns a64-byte Ethernet/IPv4 frame; the actual first Send lookup decides admission before Fault, and the second admission rejects after Fault. Read/Describe/control-reply boundaries are forbidden in this selected schedule. Actual completion frees the complete guarded or ordinary original NBL/NB/MDL/payload before its upcall returns. Actual TunPause stops Running and waits Active1 while a separate actual common FILE unregister detaches under RegistrationLock and enters full actual Free. After completion returns, Active0 releases Pause; Pause is joined while the send Returns tail is still pinned. Current Free waits real SRW rundown before releasing any objects or mapping ownership. Six cases cover1/2/8 queues and auto/manual queue events; CLI117 selects two queues/manual. The unique negative omits only Free Returns wait and parks at the first ObDereference before any unsafe physical release; actual Send/Pause are joined before rescue allows remaining actual Free to run, then reports117 after zero owned pool/MDL/NBL/event/region/thread counters and equal process handles. Every idle worker, send, pause and unregister OS thread is joined before final Ctx free; the separately owned mapped region is released after SYS Free merely unlocks its MDL. No Halt, true process exit/NDIS/IRQL, driver/device/network/registry or native Win7 runtime execution is claimed. PB_API_TESTS is rejected and the builder binds the current64-character shipping fingerprint, statically required at65 bytes. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
