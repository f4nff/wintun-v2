param([string]$BaselineDirectory='out\failure-owner-baseline\e1ae1acb4404')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'adapter-open-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Failure ownership audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$hashes=@{
    'adapter.c'='bf42aff318d069c78b43bdab8ebac333fe6bf88eccdfc254697605f9e1f9d35e'
    'driver.c'='76a87d136f7366e227e06dba0e553f72ebd62f0db4734190994981d70fbfd157'
}
foreach($name in $hashes.Keys){
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash.ToLowerInvariant() -ne $hashes[$name]){throw "Unexpected frozen source: $name"}
}
foreach($name in @('logger.h','main.h','adapter.h','driver.h','registry.h','ntdll.h','wintun.h')) {
    $previous=Join-Path $baseline "api\$name";$current=Join-Path $script:RepoRoot "api\$name"
    if($name -in @('adapter.h','driver.h')) {
        # A later compatibility review centralized the identical PacketBridgeV2
        # identity. Expand only that known alias before comparing all other bytes.
        $identity=Join-Path $script:RepoRoot 'include\packetbridge_identity.h'
        if((Get-FileHash -LiteralPath $identity).Hash.ToLowerInvariant() -ne '61573e1ac413c204421a1707ecab067a764b0a55e50952758de475f045dbd41f'){throw 'Unreviewed hardware identity definition'}
        $expanded=[IO.File]::ReadAllText($current).Replace("`r`n","`n").Replace('#include "../include/packetbridge_identity.h"'+"`n",'').Replace('#define WINTUN_HWID PB_HARDWARE_ID','#define WINTUN_HWID L"PacketBridgeV2"')
        if($expanded -cne [IO.File]::ReadAllText($previous).Replace("`r`n","`n")){throw "Failure dependency changed beyond identity alias: $name"}
    } elseif((Get-FileHash -LiteralPath $previous).Hash -ne (Get-FileHash -LiteralPath $current).Hash){throw "Failure dependency changed: $name"}
}
$directory=Join-Path $script:RepoRoot "out\failure-owner-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$adapterBody=[IO.File]::ReadAllText((Join-Path $baseline 'api\adapter.c')).Replace("`r`n","`n")
$start=$adapterBody.IndexOf('_Must_inspect_result_');$end=$adapterBody.IndexOf('#include "cleanup.h"',$start)
if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen PopulateAdapterData'}
$adapterFunction=$adapterBody.Substring($start,$end-$start)
if(!$adapterFunction.Contains('PopulateAdapterData(') -or $adapterFunction.Contains('CleanupRun(')){throw 'Unexpected metadata extraction'}
$adapter=Join-Path $directory 'adapter-before.h';[IO.File]::WriteAllText($adapter,$adapterFunction)
$expected=$adapterBody.Substring(0,$start)+"#include `"adapter_data.h`"`n`n"+$adapterBody.Substring($end)
# The later discovery review extracted this independent function to provide
# bounded CR_BUFFER_SMALL retries. Account only for that exact include change;
# its old/new behavior is checked by verify-adapter-discovery.ps1.
$discoverySignature=$expected.IndexOf('AdapterGetDeviceObjectFileName(LPCWSTR InstanceId)')
$discoveryStart=$expected.LastIndexOf('_Use_decl_annotations_',$discoverySignature)
$discoveryEnd=$expected.IndexOf('typedef struct _WAIT_FOR_INTERFACE_CTX',$discoverySignature)
if($discoverySignature -lt 0 -or $discoveryStart -lt 0 -or $discoveryEnd -le $discoveryStart){throw 'Cannot isolate independent frozen discovery helper'}
$expected=$expected.Substring(0,$discoveryStart)+"#include `"adapter_discovery.h`"`n`n"+$expected.Substring($discoveryEnd)
$renameStart=$expected.IndexOf("static _Return_type_success_(return != FALSE)`nBOOL`nRenameByNetGUID(")
$renameEnd=$expected.IndexOf("_Use_decl_annotations_`nVOID WINAPI`nWintunGetAdapterLUID(",$renameStart)
if($renameStart -lt 0 -or $renameEnd -le $renameStart){throw 'Cannot isolate independent frozen rename helpers'}
$expected=$expected.Substring(0,$renameStart)+"#include `"adapter_rename.h`"`n`n"+$expected.Substring($renameEnd)
$expected=Update-AdapterOpenFailureLedger $expected
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\adapter.c')).Replace("`r`n","`n") -cne $expected){throw 'Adapter changed beyond metadata/discovery/rename extractions and reviewed Open enum failure'}
$driverBody=[IO.File]::ReadAllText((Join-Path $baseline 'api\driver.c')).Replace("`r`n","`n")
$start=$driverBody.IndexOf("static _Return_type_success_(return != 0)`nDWORD`nVersionOfFile(")
$end=$driverBody.IndexOf("_Use_decl_annotations_`nVOID`nDriverInstallDeferredCleanup(",$start)
if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen driver query/wait helpers'}
$driverFunction=$driverBody.Substring($start,$end-$start)
$driver=Join-Path $directory 'driver-before.h';[IO.File]::WriteAllText($driver,$driverFunction)
$expected=$driverBody.Substring(0,$start)+"#include `"driver_version.h`"`n`n"+$driverBody.Substring($end)
# The independent enumeration review moved the original comparison and
# installer/delete regions into driver_install.h. Keep both extraction bounds
# exact; verify-driver-enumeration.ps1 checks that region's behavioral delta.
$compareStart=$expected.IndexOf("static BOOL`nIsNewer(")
$compareEnd=$expected.IndexOf('#include "driver_version.h"',$compareStart)
if($compareStart -lt 0 -or $compareEnd -le $compareStart){throw 'Cannot isolate frozen driver comparison'}
$expected=$expected.Substring(0,$compareStart)+$expected.Substring($compareEnd)
$installStart=$expected.IndexOf("_Use_decl_annotations_`nVOID`nDriverInstallDeferredCleanup(")
$installEnd=$expected.IndexOf('static ULONG_PTR RunDeleteDriver(',$installStart)
if($installStart -lt 0 -or $installEnd -le $installStart){throw 'Cannot isolate independent frozen driver installer'}
$expected=$expected.Substring(0,$installStart)+"#include `"driver_install.h`"`n`n"+$expected.Substring($installEnd)
# Logging-only name validation is reviewed separately from the query/wait path.
$toggleStart=$expected.IndexOf("static _Return_type_success_(return != FALSE)`nBOOL`nDisableAllOurAdapters(")
$toggleEnd=$expected.IndexOf('#include "driver_version.h"',$toggleStart)
if($toggleStart -lt 0 -or $toggleEnd -le $toggleStart){throw 'Cannot isolate independent frozen adapter toggles'}
$expected=$expected.Substring(0,$toggleStart)+"#include `"driver_toggle.h`"`n`n"+$expected.Substring($toggleEnd)
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\driver.c')).Replace("`r`n","`n") -cne $expected){throw 'Driver changed beyond query/wait, installer and toggle helper extractions'}
$cases=@(
    @{name='adapter';test='tests\safety\adapter_data_tests.c';macro='PB_ADAPTER_DATA_SOURCE';old=$adapter;libs=@('ole32.lib');code=89;marker='ADAPTER_POPULATED_WITH_INVALID_GUID'},
    @{name='driver';test='tests\safety\driver_version_tests.c';macro='PB_DRIVER_VERSION_SOURCE';old=$driver;libs=@('version.lib','ntdll.lib');code=90;marker='DRIVER_QUERY_FAILURE_ACCEPTED_AS_UNLOADED'}
)
$executables=@()
foreach($case in $cases) {foreach($mode in @('iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "$($case.name)-$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=$case.test
    if($variant -eq 'before') {
        $source=Join-Path $objects 'entry.c'
        @(('#define '+$case.macro+' "'+$case.old.Replace('\','/')+'"'),
          ('#include "'+(Join-Path $script:RepoRoot $case.test).Replace('\','/')+'"'))|
          Set-Content -LiteralPath $source -Encoding utf8
    }
    $exe=Join-Path $objects 'failure.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source)+$script:VersionArguments+@('/link',"/out:$exe")+$case.libs)
    if($variant -eq 'after'){Invoke-TestTool $exe @();Invoke-TestTool $exe @('1')}
    else {
        $output=@(& $exe 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq $case.code -and ($output -join "`n").Contains($case.marker)
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=$case.code;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected $($case.name) baseline: $($case.marker), exit=$code"}
    }
    $executables+=@{case=$case.name;variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during failure ownership audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen whole adapter.c/driver.c hashes and dependency headers; two hardware identity aliases expand to the exact original PacketBridgeV2 value under a checked shared-header hash, all other dependency bytes unchanged. Exact original helper regions extracted, not reimplemented; aggregate current sources checked against include-only metadata, query/wait, and independently audited discovery/installer/toggle/rename extractions and finite Open enum failure repair. ISO/MS Win7 API target. Old controls require 89 invalid GUID population accepted as success and 90 failed module query accepted as unloaded, after ownership cleanup. Current full matrices and repaired controls require zero. Native CLSID parser, CRT formatting, real heap and event-backed keys; SetupAPI/registry/module enumeration/operation-wait boundaries hosted. No actual kernel module enumeration, registry value/device mutations, driver installation or execution on old OS.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
