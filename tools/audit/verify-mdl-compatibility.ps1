param([string]$BaselineDirectory='out\reclamation-followup-baseline\7b618fdac6f5')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'This audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'driver\v2\session_read.h')).Path
if((Get-FileHash -LiteralPath $baseline).Hash.ToLowerInvariant() -ne 'aef73de4b402d10f85a3c289f71fa925d55cae4bb46422cbc4c20c1eae44c6da'){throw 'Unexpected MDL baseline'}
$directory=Join-Path $script:RepoRoot "out\mdl-compatibility\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@();$controls=@()
foreach($mode in @('iso','ms')) {
    $objects=Join-Path $directory $mode;New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/wd4324',"/volatile:$mode",'/D_AMD64_',
        '/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/DPOOL_NX_OPTIN=1','/DPOOL_ZERO_DOWN_LEVEL_SUPPORT',
        "/I$script:WdkRoot\Include\$script:KitVersion\km","/Fo$objects\")+$script:VersionArguments
    $exe=Join-Path $objects 'current.exe'
    Invoke-BuildTool cl.exe ($flags+@('/DPB_MDL_DIRECT_MAPPING=1','tests\model\mdl_mapping_wdk.c','tests\model\mdl_mapping_tests.c','/link',"/out:$exe"))
    Invoke-TestTool $exe @()
    $executables+=@{case='actual-wdk-mapping';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    $source=Join-Path $objects 'baseline.c'
    @(('#define PB_MDL_READ_SOURCE "'+$baseline.Replace('\','/')+'"'),
      ('#include "'+(Join-Path $script:RepoRoot 'tests\model\mdl_mapping_wdk.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $objects 'baseline.exe'
    Invoke-BuildTool cl.exe ($flags+@($source,'tests\model\mdl_mapping_tests.c','/link',"/out:$exe"))
    $output=@(& $exe '--win7-only' 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
    $log=Join-Path $objects 'expected-win7-contract-failure.log';$output|Set-Content -LiteralPath $log -Encoding utf8
    $expected=$code -eq 1 -and ($output -join "`n").Contains('PbV2Read(&nb,skip,out,size) os=6.1')
    $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--win7-only');exit_code=$code;expected_exit_code=1;expected_contract_failure=$expected;log=$log})
    Write-TestRecording $(if($expected){'running'}else{'failed'})
    if(!$expected){throw 'Baseline did not fail at the Win7 mapping contract check'}
    $executables+=@{case='frozen-win7-contract-control';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    $exe=Join-Path $objects 'recycle.exe'
    Invoke-BuildTool cl.exe ($flags+@('tests\model\recycle_budget_tests.c','/link',"/out:$exe"))
    Invoke-TestTool $exe @()
    $executables+=@{case='actual-recycle-budget';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}
$objects=Join-Path $directory 'invalid-builds';New-Item -ItemType Directory -Path $objects -Force|Out-Null
$source=Join-Path $objects 'mapping.c'
@('#include <ntddk.h>',('#include "'+(Join-Path $script:RepoRoot 'driver\v2\platform_mdl.h').Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
foreach($entry in @(@('missing-optin',@('/DPOOL_NX_OPTIN=0')),@('optout',@('/DPOOL_NX_OPTIN=1','/DPOOL_NX_OPTOUT=1')),
    @('unconditional-optin',@('/DPOOL_NX_OPTIN=1','/DPOOL_NX_OPTIN_AUTO=1')))) {
    $log=Join-Path $objects "$($entry[0]).log"
    & cl.exe /nologo /c /W4 /WX /wd4324 /D_AMD64_ /DNTDDI_VERSION=0x06010000 "/I$script:WdkRoot\Include\$script:KitVersion\km" "/Fo$objects\mapping.obj" @($entry[1]) $source *> $log
    $code=$LASTEXITCODE;$message=Get-Content -LiteralPath $log -Raw
    if($code -eq 0 -or !$message.Contains('C1189') -or !$message.Contains('PacketBridge requires runtime POOL_NX_OPTIN')){throw "Build guard did not reject $($entry[0])"}
    $controls+=@{case=$entry[0];exit_code=$code;expected_compile_rejection=$true;log=$log}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during MDL audit'}
Write-TestRecording 'passed'
$headers=@('driver\v2\platform_mdl.h',"$script:WdkRoot\Include\$script:KitVersion\km\wdm.h")|ForEach-Object {
    @{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}
}
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;kernel_runtime_tested=$false;production_ready=$false;
    method='Actual WDK runtime NX initialization, MDL structures and mapping inlines; only imported version discovery/routine lookup/raw map calls are hosted. Real readonly and no-access pages. Frozen real production read must fail the Win7 mapping contract, not crash or time out. ISO/MS volatile; actual recycle body/core, bounded four-batch continuation and invalid-token prefix matrix. Three compile controls prohibit configurations that would drop downlevel NX opt-in.';
    baseline=$baseline;operations=$script:TestResultFile;executables=$executables;build_controls=$controls;headers=$headers}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
