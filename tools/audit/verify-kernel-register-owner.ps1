param([string]$BaselineDirectory='out\registration-owner-baseline\74362d336bb1')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\kernel-register-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Kernel register owner audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$production=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
foreach($path in $production){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$reviewedDependencyRepairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h')
$unchangedProduction=$production.Count-$reviewedDependencyRepairs.Count
$directory=Join-Path $script:RepoRoot "out\registration-owner-investigation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-alias-check','start-before-publish')){
    $memory=if($guarded){'guarded'}else{'ordinary'}
    $objects=Join-Path $directory "$variant-$mode-$memory"
    $built=Build-KernelRegisterTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $source=[IO.File]::ReadAllText($built.extraction.source)
    if([IO.File]::ReadAllText($built.extraction.extracted) -cne $source.Substring($built.extraction.offset,$built.extraction.length)){throw 'Register extraction differs from production source'}
    if($variant -eq 'current'){Invoke-TestTool $built.path @('1');Invoke-TestTool $built.path @('2');Invoke-TestTool $built.path @()}
    else{
        $alias=$variant -eq 'without-alias-check';$argument=if($alias){'1'}else{'2'}
        $expectedCode=if($alias){100}else{101};$marker=if($alias){'REGISTER_ACCEPTED_EVENT_OBJECT_ALIAS'}else{'REGISTER_SIGNALED_START_BEFORE_PUBLICATION'}
        $output=@(& $built.path $argument 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq $expectedCode -and ($output -join "`n").Contains($marker)
        $script:TestExecutions.Add([ordered]@{program=$built.path;arguments=@($argument);exit_code=$code;expected_exit_code=$expectedCode;expected_mutation_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Register mutation not rejected: $variant/$mode/$memory"}
    }
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during register owner audit'}
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
    method='Each build extracts current PbV2Register verbatim; driver/common dependencies differ from the accepted baseline only by the reviewed Send diagnostic cursor reset, separately reviewed complete-file LSO/checksum dependency repair and checksum-family Describe selection repair. ISO/MS ordinary/guarded current each run both CLI controls and 1045 actual Register/Free/Unregister scenarios. Every pair among Progress plus two events per 1/2/8 queues is tested with identical handles and real DuplicateHandle handles mapped to one hosted event object, for auto/manual events. Real input gates, ordered pool/MDL/Probe/Map/ObRef/thread failure sites and optional allocation success paths execute. 88 plain core shared-queue/pool/admission/completion checks retain plain operation after optional allocation failure and success; they are not full NBL/NDIS sends or indications. Starts are observed at the actual KeSetEvent call and require owner/PID/list/V2/offload state and disconnection event publication first. Failure publishes no owner/list/V2/start/connected state; all partial fake workers first wait on Start, then Stop, are joined, and all driver event references and mappings are balanced. The caller retains real event handles until after driver dereference; distinct unused event objects are also reclaimed. Current Register-only tests call actual inner Unregister after modeling the common owner/list removal; common process/FILE callbacks and ControlRundown are not executed. Generated-header mutants remove only alias validation or move Start before publication; four each must report 100/101 after complete cleanup. Fake workers never perform data work, so the startup mutation is observed without unsafe access. Probe exceptions and DDI failures are hosted, not native DDI/NDIS/IRQL or real page-lock tests. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
