param([string]$BaselineDirectory='out\outer-return-baseline\180d7cbe5ff4')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Driver enumeration audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozenFile=Join-Path $baseline 'api\driver.c'
$frozenHash='bd910fe559444944fa8a87b17f8739fbaac694869b102b3670a7060145ed61ff'
if((Get-FileHash -LiteralPath $frozenFile).Hash.ToLowerInvariant() -ne $frozenHash){throw 'Unexpected frozen driver.c'}
foreach($name in @('logger.h','adapter.h','driver.h','main.h','registry.h','resource.h','wintun.h','operation.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne
       (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Driver enumeration dependency changed: $name"}
}
if((Get-FileHash -LiteralPath (Join-Path $baseline 'include\packetbridge_identity.h')).Hash -ne
   (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot 'include\packetbridge_identity.h')).Hash){throw 'Hardware identity changed'}
$body=[IO.File]::ReadAllText($frozenFile)
$compareStart=$body.IndexOf("static BOOL`r`nIsNewer(")
$compareEnd=$body.IndexOf('#include "driver_version.h"',$compareStart)
$scanStart=$body.IndexOf("_Use_decl_annotations_`r`nVOID`r`nDriverInstallDeferredCleanup(",$compareEnd)
$scanEnd=$body.IndexOf('static ULONG_PTR RunDeleteDriver',$scanStart)
if($compareStart -lt 0 -or $compareEnd -le $compareStart -or $scanStart -lt 0 -or $scanEnd -le $scanStart){throw 'Cannot isolate frozen installer regions'}
$functions=$body.Substring($compareStart,$compareEnd-$compareStart)+$body.Substring($scanStart,$scanEnd-$scanStart)
$expected=$body.Substring(0,$scanStart)+"#include `"driver_install.h`"`r`n`r`n"+$body.Substring($scanEnd)
$expected=$expected.Substring(0,$compareStart)+$expected.Substring($compareEnd)
$expected=$expected.Replace("`r`n","`n")
# The name-property review independently tests the two adapter toggle helpers.
# Permit only their exact extraction here, retaining the full aggregate check.
$toggleStart=$expected.IndexOf("static _Return_type_success_(return != FALSE)`nBOOL`nDisableAllOurAdapters(")
$toggleEnd=$expected.IndexOf('#include "driver_version.h"',$toggleStart)
if($toggleStart -lt 0 -or $toggleEnd -le $toggleStart){throw 'Cannot isolate frozen adapter toggle helpers'}
$expected=$expected.Substring(0,$toggleStart)+"#include `"driver_toggle.h`"`n`n"+$expected.Substring($toggleEnd)
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\driver.c')).Replace("`r`n","`n") -cne $expected){
    throw 'Driver aggregate changed beyond exact comparator, installer and toggle extractions'
}
$directory=Join-Path $script:RepoRoot "out\driver-enumeration\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$frozen=Join-Path $directory 'installer-before.h';[IO.File]::WriteAllText($frozen,$functions)
$functionHash=(Get-FileHash -LiteralPath $frozen).Hash.ToLowerInvariant()
if($functionHash -ne '1c02b7d080759ea47efbefe010e0fe23f5069cc78df2c146123386cc146698b9'){throw 'Frozen installer extraction hash mismatch'}
$controls=@(
    @(1,96,'DRIVER_INSTALL_AFTER_INCOMPLETE_ENUMERATION'),
    @(2,97,'DRIVER_DELETE_SCAN_FAILURE_ACCEPTED_AS_SUCCESS'),
    @(3,97,'DRIVER_DELETE_SCAN_FAILURE_ACCEPTED_AS_SUCCESS'),
    @(4,98,'DRIVER_STATIC_INF_SKIPPED_ON_LONG_IDS')
)
$test=Join-Path $script:RepoRoot 'tests\safety\driver_enumeration_tests.c'
$executables=@()
foreach($mode in @('default','iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=$test
    if($variant -eq 'before') {
        $source=Join-Path $objects 'entry.c'
        @(('#define PB_DRIVER_INSTALL_SOURCE "'+$frozen.Replace('\','/')+'"'),
          ('#include "'+$test.Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$objects\")
    if($mode -ne 'default'){$flags+="/volatile:$mode"}
    $exe=Join-Path $objects 'enumeration.exe'
    Invoke-BuildTool cl.exe ($flags+@($source)+$script:VersionArguments+@('/link',"/out:$exe",'shlwapi.lib'))
    if($variant -eq 'after') {
        Invoke-TestTool $exe @()
        foreach($control in $controls){Invoke-TestTool $exe @([string]$control[0])}
    } else {foreach($control in $controls) {
        $argument=[string]$control[0];$output=@(& $exe $argument 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects ("control-$argument.log");$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq $control[1] -and ($output -join "`n").Contains($control[2])
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@($argument);exit_code=$code;expected_exit_code=$control[1];expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected installer baseline control $argument : exit=$code"}
    }}
    $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during driver enumeration audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    frozen_driver_sha256=$frozenHash;frozen_functions_sha256=$functionHash;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Whole frozen driver.c and verbatim comparator/deferred-cleanup/install/delete regions hash checked; current driver.c matches those exact extractions plus the separately audited toggle-helper extraction. Default/ISO/MS Win7 targets. Historical CLI1=96 proves installation preparation after incomplete enumeration; CLI2/3=97 prove deletion success despite enum/detail failure; CLI4=98 proves skipping the guaranteed static INF when only variable hardware-ID tail space is insufficient. Every negative control checks cleanup before exit. Current matrices and repaired controls must exit zero. All SetupAPI/resource/file boundaries are hosted; native mutex/event handles, heap list nodes, PathCombine/PathFindFileName and CRT formatting remain real. Actual deferred cleanup verifies reenable/list/set ownership after partial prior work. Installer best-effort ordinary detail/uninstall warnings and cancellation error priority are retained. No actual driver/device/registry/file mutations, old-OS or kernel runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
