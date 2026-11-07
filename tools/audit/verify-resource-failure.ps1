param([string]$BaselineDirectory='out\resource-failure-baseline\ceb87447a1bd')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Resource audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$old=Join-Path $baseline 'api\resource.c'
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne '00d3b4c477fc9286857d1f0ba2a3ffd9f613d6cc6636d25a5753d252d0b63cf0'){throw 'Unexpected frozen resource source'}
if((Get-FileHash -LiteralPath (Join-Path $baseline 'api\resource.h')).Hash.ToLowerInvariant() -ne 'b333dce8632e6f014568f5d966f64504f70b122f95e9521aca59a8e320b71df8'){throw 'Unexpected frozen resource header'}
foreach($name in @('logger.h','main.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Resource dependency changed: $name"}
}
$directory=Join-Path $script:RepoRoot "out\resource-failure-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$oldSource=Join-Path $directory 'resource-before.c'
$body=[regex]::Replace([IO.File]::ReadAllText($old),'#include "([^"]+)"',{
    param($m)
    $path=Join-Path $script:RepoRoot ('api\'+$m.Groups[1].Value)
    '#include "'+$path.Replace('\','/')+'"'
})
# Header declarations have unchanged C signatures; the resource-name SAL tag
# changed to allow integer IDs. Only include paths normalize the frozen body.
[IO.File]::WriteAllText($oldSource,$body)
$before=Join-Path $directory 'before.c'
@(('#define PB_RESOURCE_SOURCE "'+$oldSource.Replace('\','/')+'"'),
  ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\resource_failure_tests.c').Replace('\','/')+'"'))|
  Set-Content -LiteralPath $before -Encoding utf8
$controls=@(@(1,81,'NUMERIC_RESOURCE_ID_FORMATTED_AS_POINTER'),@(2,81,'NUMERIC_RESOURCE_ID_FORMATTED_AS_POINTER'),
    @(3,82,'COPY_NUMERIC_RESOURCE_ID_FORMATTED_AS_POINTER'),@(4,83,'UNOWNED_TEMP_CANDIDATE_PUBLISHED'))
$executables=@()
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $source=if($variant -eq 'before'){$before}else{'tests\safety\resource_failure_tests.c'}
        $exe=Join-Path $objects 'resource.exe'
        Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source)+$script:VersionArguments+@('/link',"/out:$exe",'advapi32.lib','shlwapi.lib'))
        if($variant -eq 'after'){Invoke-TestTool $exe @()}
        else {foreach($case in $controls) {
            $arguments=@([string]$case[0]);$output=@(& $exe @arguments 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
            $log=Join-Path $objects ("control-"+$case[0]+'.log');$output|Set-Content -LiteralPath $log -Encoding utf8
            $expected=$code -eq $case[1] -and ($output -join "`n").Contains($case[2])
            $script:TestExecutions.Add([ordered]@{program=$exe;arguments=$arguments;exit_code=$code;expected_exit_code=$case[1];expected_baseline_failure=$expected;log=$log})
            Write-TestRecording $(if($expected){'running'}else{'failed'})
            if(!$expected){throw "Unexpected resource baseline: $($case[2]), exit=$code"}
        }}
        $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during resource audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen whole resource.c SHA, current C-compatible declarations; real CRT formats varargs and actual numeric-pointer access violations are caught for precise old controls 81/82. Copy-only control skips exactly one inner error format to isolate the outer log. Hosted directory table demonstrates old output publishing a foreign collision candidate (83), with no real deletion. ISO/MS each run 1011 actual production resource scenarios: IDs 0/1/101-103/65535, aliases/names/Unicode, all lookup/copy/size/load/lock/write failures, zero/real errors, short writes, live handle balance, private directory publication, returned length and MAX_PATH boundary checks. Real event-backed file handles, no real files/directories/devices created. Correct logger wrappers retained. No SYS or old-OS runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
