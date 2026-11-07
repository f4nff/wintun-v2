param([string]$BaselineDirectory='out\registration-owner-baseline\74362d336bb1')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Adapter name log audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$hashes=@{
    'driver.c'='5813c4ad3827bf50f985f95ebaba9e43b95ebe2c91ffa166659f00b1e2bf9fbf'
    'orphan_cleanup.h'='af38a13c6101b6084d40984a3120589435ed8a9c302ac78388c7d13f9c49fbf0'
}
foreach($name in $hashes.Keys){
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash.ToLowerInvariant() -ne $hashes[$name]){throw "Unexpected frozen source: $name"}
}
foreach($name in @('logger.h','adapter.h','driver.h','main.h','wintun.h','operation.h','orphan_owner.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne
       (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Name log dependency changed: $name"}
}
$driver=[IO.File]::ReadAllText((Join-Path $baseline 'api\driver.c'))
$start=$driver.IndexOf('static _Return_type_success_',$driver.IndexOf('struct _SP_DEVINFO_DATA_LIST'))
$end=$driver.IndexOf('#include "driver_version.h"',$start)
if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen toggle helpers'}
$toggle=$driver.Substring($start,$end-$start)
$expected=$driver.Substring(0,$start)+"#include `"driver_toggle.h`"`r`n`r`n"+$driver.Substring($end)
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\driver.c')).Replace("`r`n","`n") -cne $expected.Replace("`r`n","`n")){
    throw 'Driver changed beyond exact toggle extraction'
}
$orphan=[IO.File]::ReadAllText((Join-Path $baseline 'api\orphan_cleanup.h')).Replace("`r`n","`n")
$nameStart=$orphan.IndexOf('static VOID OrphanDeviceName(')
$nameEnd=$orphan.IndexOf('VOID AdapterCleanupOrphanedDevicesWin7(',$nameStart)
if($nameStart -lt 0 -or $nameEnd -le $nameStart){throw 'Cannot isolate frozen orphan name reader'}
$originalName=$orphan.Substring($nameStart,$nameEnd-$nameStart).TrimEnd()
$signature='static VOID OrphanDeviceName(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData, WCHAR Name[PB_MAX_ADAPTER_NAME])'
$delegate=$signature+"`n{`n    AdapterNameForLog(DevInfo, DevInfoData, Name);`n}`n`n"
$expectedOrphan=$orphan.Substring(0,$nameStart)+$delegate+$orphan.Substring($nameEnd)
$expectedOrphan=$expectedOrphan.Replace('#include "orphan_owner.h"',"#include `"orphan_owner.h`"`n#include `"adapter_name_log.h`"")
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\orphan_cleanup.h')).Replace("`r`n","`n") -cne $expectedOrphan){
    throw 'Orphan cleanup changed beyond shared name-reader delegation'
}
$expectedHelper=$originalName.Replace('OrphanDeviceName','AdapterNameForLog').Replace("{`n    DEVPROPTYPE Type", "{`n    DWORD Error = GetLastError();`n    DEVPROPTYPE Type")
$expectedHelper=$expectedHelper.Replace("    Name[PB_MAX_ADAPTER_NAME - 1] = 0;`n}","    Name[PB_MAX_ADAPTER_NAME - 1] = 0;`n    SetLastError(Error);`n}")
$helper=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\adapter_name_log.h')).Replace("`r`n","`n")
$helperStart=$helper.IndexOf('static VOID AdapterNameForLog(')
if($helperStart -lt 0 -or $helper.Substring($helperStart).TrimEnd() -cne $expectedHelper){throw 'Shared name reader changed original policy beyond last-error preservation'}
$directory=Join-Path $script:RepoRoot "out\adapter-name-log\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$frozen=Join-Path $directory 'toggle-before.h';[IO.File]::WriteAllText($frozen,$toggle)
$toggleHash=(Get-FileHash -LiteralPath $frozen).Hash.ToLowerInvariant()
if($toggleHash -ne '4f7296a814b80daee1caf22205ff77f3300f3e0a8f72fdf133c0db2b3bdbf241'){throw 'Frozen toggle function hash mismatch'}
$test=Join-Path $script:RepoRoot 'tests\safety\adapter_name_log_tests.c'
$executables=@()
foreach($mode in @('default','iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=$test
    if($variant -eq 'before') {
        $source=Join-Path $objects 'entry.c'
        @(('#define PB_DRIVER_TOGGLE_SOURCE "'+$frozen.Replace('\','/')+'"'),
          ('#include "'+$test.Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$objects\")
    if($mode -ne 'default'){$flags+="/volatile:$mode"}
    $exe=Join-Path $objects 'name-log.exe'
    Invoke-BuildTool cl.exe ($flags+@($source)+$script:VersionArguments+@('/link',"/out:$exe"))
    if($variant -eq 'after') {Invoke-TestTool $exe @();Invoke-TestTool $exe @('1')}
    else {
        $output=@(& $exe 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control-1.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 99 -and ($output -join "`n").Contains('DRIVER_BINARY_NAME_FORMATTED_AS_STRING')
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=99;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected binary-name baseline control: exit=$code"}
    }
    $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during adapter name log audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    frozen_sources=$hashes;frozen_toggle_sha256=$toggleHash;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen driver/orphan whole hashes checked; driver toggle extraction and orphan shared-reader delegation checked. Shared helper equals original orphan type/size/termination/fallback policy plus explicit last-error preservation. Default/ISO/MS Win7 targets. Old CLI1=99 observes a legal complete BINARY property passed to unbounded string logging, before an unsafe read; original toggle loops then finish and all heap nodes/private heap are reclaimed before exit. No claim of native OS crash. Safe/current formatting uses native CRT. Separate STRING corruption/fault models are not natural SDK success claims. Tests cover other legal custom types, empty/Unicode/max names, getter errors/poison outputs, preserved last error, first operation errors, best-effort enum/CM skip semantics, allocation/cancellation ownership. No real device/registry/driver/file mutations or old-OS/kernel runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
