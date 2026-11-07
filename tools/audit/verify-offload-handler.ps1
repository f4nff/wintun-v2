param([string]$BaselineDirectory='out\wait-retirement-baseline\9d75adca3c0b')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\offload-handler-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Offload handler audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate kernel dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\safety\guard_memory.h')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=Join-Path $script:RepoRoot "out\offload-handler\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Invoke-OffloadHandlerProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start offload handler test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(20000)){$process.Kill($true);$process.WaitForExit();throw 'Offload handler external watchdog expired (20s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-offload-handler.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,[Text.UTF8Encoding]::new($false));Write-Output $output.TrimEnd()
        $correct=$code -eq $Expected -and ($Expected -ne 120 -or $output.Contains('OFFLOAD_POLICY_SKIPPED_BANK_WAIT: actual POLICY returned while prior bank remained owned; owner released and caller joined before context/event free; zero owned resources'))
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;
            expected_mutation_failure=($Expected -eq 120 -and $correct);watchdog_ms=20000;cpu_ms=$process.TotalProcessorTime.TotalMilliseconds;log=$log;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Offload handler test failed: $code expected $Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-policy-bank-wait')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-OffloadHandlerTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $extract=$built.extractions
    foreach($part in $extract.parts){if([IO.File]::ReadAllText($part.path) -cne [IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length)){throw "Production extraction changed: $($part.name)"}}
    $handlers=$extract.parts|Where-Object name -eq 'handlers';$expected=[IO.File]::ReadAllText($handlers.path)
    if($variant -eq 'without-policy-bank-wait'){
        $offset=$extract.wait_offset
        if($expected.Substring($offset,$extract.mutation_original.Length) -cne $extract.mutation_original -or $offset -le $extract.policy_offset){throw 'Negative wait offset is outside POLICY branch'}
        $expected=$expected.Substring(0,$offset)+$extract.mutation_replacement+$expected.Substring($offset+$extract.mutation_original.Length)
    }
    if($expected -cne [IO.File]::ReadAllText($extract.selected)){throw 'Selected handler has additional changes'}
    $type=$extract.parts|Where-Object name -eq 'status-type';$code=$extract.parts|Where-Object name -eq 'status-code'
    if([IO.File]::ReadAllText($extract.types) -cne [IO.File]::ReadAllText($type.path)+[IO.File]::ReadAllText($code.path)){throw 'WDK status type has additional changes'}
    foreach($bound in $extract.sdk_wdk_types){if((Get-FileHash $bound.path).Hash.ToLowerInvariant() -ne $bound.sha256){throw 'SDK/WDK type source changed during build'}}
    if($variant -eq 'current'){Invoke-OffloadHandlerProcess $built.path @('120');Invoke-OffloadHandlerProcess $built.path @()}
    else{Invoke-OffloadHandlerProcess $built.path @('120') 120}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during offload handler audit'}
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
    audit_sources=@('tests\model\offload_handler_tests.c','tools\build\offload-handler-tests.ps1','tools\audit\verify-offload-handler.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual EffectiveMask, ReportOffload, parameter/checksum helpers, OID offload parameters/encapsulation and complete ProbeIoctl are extracted verbatim per build from current offload_probe.h. Actual production probe_state, platform policy, offload format and indication gate remain included. WDK ndis/oidrequest.h and ndis/encapsulationconfig.h plus SDK ntddndis.h provide actual OID/envelope/parameter/encapsulation layouts; the WDK status-indication type, REV1 size and status-code definition are copied verbatim from ndis.h with offsets/markers/source and output hashes. Only the unique POLICY branch prior-bank wait is omitted in the negative; both OID waits and actual Report remain unchanged. Every allocation and event/thread handle is counted; guarded mode protects Ctx and exact-sized old parameter/encapsulation spans, ordinary mode uses calloc/free. Runtime6.20/6.30/6.83/6.89 cases check revisions1..6 with exact SDK old sizes, correct BytesRead or rejection, explicit RSC/USO/URO POLICY support and actual parameter-bit enablement, V2Active nonzero rejection, zero disable, registration rejection with zero disable still allowed, and14/18-byte encapsulation plus invalid16-byte rejection. Eight serialized schedules select POLICY waiting prior bank0, then concurrent ordinary OID parameters or encapsulation waiting on actual OffloadMutex; a new bank1 owner does not extend the first barrier, and the second rotation waits bank1 while TransitionLock is released. Actual Report executes its StatusLock and transition snapshot; the hosted NdisMIndicateStatusEx validates real status/format headers and modeled DISPATCH with StatusLock held and TransitionLock released. Normal OID entry is modeled PASSIVE; direct recursive normal OID delivery at status DISPATCH is never fabricated. Ordinary OID serialization alone is not used as a proof against all reentrancy. The contract follows Microsoft MiniportOidRequest PASSIVE requirement and queued/nonblocking work from status callbacks; no real NDIS queue, IRQL, driver/device/network/registry or Win7 runtime execution is claimed. A ready presence object models session membership; no registration, worker/NBL allocation or packet path is exercised. Full current processes execute82 checks; CLI120 selects one prior-bank wait case. Negative observes actual POLICY return while the prior bank still has an owner, then explicitly releases that owner and joins the real handler OS thread before any event/context free and reports120 after zero resource counters/equal process handles. PB_API_TESTS is rejected; builder binds current64-character shipping fingerprint, statically required at65 bytes. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 14|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
