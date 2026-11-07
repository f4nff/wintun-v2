param([string]$BaselineDirectory='out\reclamation-baseline\31293a872711')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Control boundary audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$hashes=@{
    'api\registry.c'='c1bb78e4ff73d86cf4e20c1ea99621e13b7ffd4499f950797f2b56e9262d4b33'
    'api\rundll32_process.h'='b16b6ea2b866724af36a3f7ad3f95ed2a7e096109ec8a220d1b19722aec939a8'
    'api\adapter_win7.h'='346ff77cb69dfe8a177b79487ffa22419dd37442a0d9fb2c7afe70651ff2131d'
}
foreach($name in $hashes.Keys){
    if((Get-FileHash -LiteralPath (Join-Path $baseline $name)).Hash.ToLowerInvariant() -ne $hashes[$name]){throw "Unexpected frozen source: $name"}
}
foreach($name in @('logger.h','main.h','registry.h','resource.h','wintun.h','overlapped_wait.h','termination_wait.h','event_pair.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Control dependency changed: $name"}
}
$directory=Join-Path $script:RepoRoot "out\control-boundaries\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
function Normalize-ApiIncludes([string]$Body) {
    [regex]::Replace($Body,'#include "([^"]+)"',{
        param($m)
        $path=Join-Path $script:RepoRoot ('api\'+$m.Groups[1].Value)
        if(!(Test-Path -LiteralPath $path)){throw "Unknown frozen include: $path"}
        '#include "'+$path.Replace('\','/')+'"'
    })
}
$registry=Join-Path $directory 'registry-before.c'
[IO.File]::WriteAllText($registry,(Normalize-ApiIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api\registry.c')))))
$helper=Join-Path $directory 'helper-before.h'
[IO.File]::WriteAllText($helper,(Normalize-ApiIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api\rundll32_process.h')))))
$win7=Join-Path $directory 'win7-before.h'
$win7Body=[IO.File]::ReadAllText((Join-Path $baseline 'api\adapter_win7.h'))
$start=$win7Body.IndexOf('_Must_inspect_result_');$end=$win7Body.IndexOf('_Must_inspect_result_',$start+1)
if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen Win7 interface wait'}
$win7Function=$win7Body.Substring($start,$end-$start)
if(!$win7Function.Contains('WaitForInterfaceWin7(') -or $win7Function.Contains('CreateAdapterWin7(')){throw 'Unexpected Win7 extraction'}
[IO.File]::WriteAllText($win7,$win7Function)
$cases=@(
    @{name='registry';test='tests\safety\registry_string_tests.c';macro='PB_REGISTRY_SOURCE';old=$registry;libs=@('advapi32.lib');controls=@(@(1,84,'REGISTRY_SHRINK_DID_NOT_TERMINATE_STRING'),@(2,85,'REGISTRY_EXPAND_INPUT_NOT_TERMINATED'))},
    @{name='helper';test='tests\safety\helper_start_tests.c';macro='PB_HELPER_SOURCE';old=$helper;libs=@();controls=@(,@(1,88,'HELPER_COMBINED_TRUNCATED_WINDOWS_DIRECTORY'))},
    @{name='win7';test='tests\safety\win7_readiness_tests.c';macro='PB_WIN7_WAIT_SOURCE';old=$win7;libs=@();controls=@(,@(1,87,'WIN7_INTERFACE_READY_BEFORE_REGISTRY_VALUE'))}
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
    $exe=Join-Path $objects 'control.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source)+$script:VersionArguments+@('/link',"/out:$exe")+$case.libs)
    if($variant -eq 'after') {
        Invoke-TestTool $exe @()
        foreach($control in $case.controls){Invoke-TestTool $exe @([string]$control[0])}
    } else {foreach($control in $case.controls) {
        $arguments=@([string]$control[0]);$output=@(& $exe @arguments 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects ("control-"+$control[0]+'.log');$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq $control[1] -and ($output -join "`n").Contains($control[2])
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=$arguments;exit_code=$code;expected_exit_code=$control[1];expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected $($case.name) baseline: $($control[2]), exit=$code"}
    }}
    $executables+=@{case=$case.name;variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during control boundary audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen whole registry/helper/Win7-header hashes checked. Registry/helper include paths normalize to unchanged dependency headers. Win7 wait extracted verbatim from frozen header. ISO/MS Win7 API target. Old exact controls: 84 missing wide terminator after actual heap shrink; 85 unterminated source at the environment-expansion boundary; 87 missing registry value accepted as interface-ready; 88 insufficient-buffer Windows directory consumed by helper path combine. Observers check input before any unsafe read. Registry heap and expansion use actual Windows APIs, with poison/fault/ownership wrappers. Win7 registry/device boundaries are hosted with real owned event handles. Helper setup stops at first path-combine boundary: no extraction, pipes or process creation. All current full matrices and all repaired controls must exit zero. No real registry/device mutations, driver installation, native kernel or old-OS runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
