param([string]$BaselineDirectory='out\interface-guard-baseline\b208f76ac8e3', [switch]$FocusedOnly)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Kernel object guard audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$oldFixture=Join-Path $baseline 'tests\model\kernel_nbl_fixture.h'
if((Get-FileHash -LiteralPath $oldFixture).Hash.ToLowerInvariant() -ne '13d6001df654250cd297599527877576ba47cc82e6586b6810753e88ddb1c384'){throw 'Unexpected kernel fixture baseline'}
$production=@(rg --files driver common)
if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
foreach($path in $production){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$reviewedDependencyRepairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h')
$unchangedProduction=$production.Count-$reviewedDependencyRepairs.Count
$directory=Join-Path $script:RepoRoot "out\interface-guard-investigation\sys-guards\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('before','after')){
    $memory=if($guarded){'guarded'}else{'ordinary'}
    $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($variant -eq 'before'){$lines+='#define PB_KERNEL_NBL_FIXTURE_SOURCE "'+$oldFixture.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\kernel_object_guard_tests.c').Replace('\','/')+'"'
    $source=Join-Path $objects 'entry.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $objects 'objects.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source,'common\v2_core.c','common\ethernet.c')+$script:VersionArguments+@('/link',"/out:$exe"))
    if($variant -eq 'before' -and $guarded){
        $output=@(& $exe 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 95 -and ($output -join "`n").Contains('KERNEL_MDL_NBL_OBJECTS_LACK_GUARD_COVERAGE')
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=95;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw 'Old object fixture did not expose the coverage gap'}
    }else{Invoke-TestTool $exe @('1');Invoke-TestTool $exe @()}
    $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
if(!$FocusedOnly){
    $tests=@('receive_nbl_tests','return_batch_tests','return_coalesce_tests','drain_boundary_tests','fault_drain_tests','pause_unregister_tests','return_reclaim_tests')
    foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($test in $tests){
        $memory=if($guarded){'guarded'}else{'ordinary'}
        $objects=Join-Path $directory "$test-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
        $lines+='#include "'+(Join-Path $script:RepoRoot "tests\model\$test.c").Replace('\','/')+'"'
        $source=Join-Path $objects 'entry.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects "$test.exe"
        Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')+$script:VersionArguments+@('/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$test;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }}}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source files changed during kernel object audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=$reviewedDependencyRepairs;unchanged_dependencies=$unchangedProduction;production_changed=$true;
    kernel_runtime_tested=$false;production_ready=$false;focused_only=[bool]$FocusedOnly;unchanged_production_dependencies=$unchangedProduction;
    fixture=@{before_sha256=(Get-FileHash -LiteralPath $oldFixture).Hash.ToLowerInvariant();after_sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot 'tests\model\kernel_nbl_fixture.h')).Hash.ToLowerInvariant()};
    operations=$script:TestResultFile;executables=$executables;
    method='Hosted MDL/NBL allocation is zeroed, aligned to its actual fixture type and ends at a reserved guard page; VirtualFree removes the object mapping. Each current guarded control/matrix catches exactly eight exact-address read/write access violations at object ends and immediately after free. Ordinary memory is never probed outside object lifetime. Frozen guarded fixture is rejected with exit 95 after safe layout inspection and complete cleanup, without probing heap overruns or freed heap objects. Before/after ordinary matrices retain all four production allocation failure ordinals, hooks, counts, status capture, completion ownership and balanced handles. No native DDI, NDIS, IRQL, CPU or driver runtime claim. Guarding catches overruns and immediate post-free accesses; it does not add an address-reuse quarantine or an exact object underflow guard. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 10|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
