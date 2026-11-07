param([string]$BaselineDirectory='out\wait-loader-baseline\401a6af2cc88')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\system-loader-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Wait/loader audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(@('api\v2\session_lifecycle.h','e319de1df293c923749f297250e1d3de050c62a46d88af0dee7d382a70e2f72e'),
    @('api\main.c','a342e841ded11aa578bc36c836a4661a7ab15887b226e3b0bfc301f8ca24be46'),
    @('setupapihost\host.c','47db589f50f93fd560e59d1368dd273fad3651c55a4e3b0becf3dae54e3fd9a4'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]){throw "Unexpected frozen source: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\wait-loader-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json';$executables=@()
function Normalize-Includes([string]$content,[string]$Folder) {
    [regex]::Replace($content,'#include "([^"]+)"',{
        param($m)
        $path=[IO.Path]::GetFullPath((Join-Path $Folder $m.Groups[1].Value))
        if(!(Test-Path -LiteralPath $path)){throw "Unknown include: $path"}
        '#include "'+$path.Replace('\','/')+'"'
    })
}
function Expect-Control([string]$Program,[string[]]$Arguments,[int]$Code,[string]$Marker,[string]$Log) {
    $output=@(& $Program @Arguments 2>&1|ForEach-Object {$_.ToString()});$actual=$LASTEXITCODE
    $output|Set-Content -LiteralPath $Log -Encoding utf8
    $expected=$actual -eq $Code -and ($output -join "`n").Contains($Marker)
    $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$actual;expected_exit_code=$Code;expected_baseline_failure=$expected;log=$Log})
    Write-TestRecording $(if($expected){'running'}else{'failed'})
    if(!$expected){throw "Unexpected baseline result: $Marker, exit=$actual"}
}
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $fixture='tests\safety\system_loader_fixture.c';$park='tests\model\lifecycle_park_tests.c'
        if($variant -eq 'before') {
            $oldMain=Join-Path $objects 'main-before.c'
            [IO.File]::WriteAllText($oldMain,(Normalize-Includes ([IO.File]::ReadAllText((Join-Path $baseline 'api\main.c'))) (Join-Path $script:RepoRoot 'api')))
            $fixture=Join-Path $objects 'loader-before.c'
            @(('#define PB_LEGACY_RUNTIME_MAIN_SOURCE "'+$oldMain.Replace('\','/')+'"'),
              ('#define PB_SYSTEM_LOADER_HOST_SOURCE "'+(Join-Path $baseline 'setupapihost\host.c').Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\system_loader_fixture.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $fixture -Encoding utf8
            $oldSession=Join-Path $objects 'session-before.c'
            $body=Normalize-Includes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c'))) (Join-Path $script:RepoRoot 'api\v2')
            $currentInclude='#include "'+(Join-Path $script:RepoRoot 'api\v2\session_lifecycle.h').Replace('\','/')+'"'
            $oldInclude='#include "'+(Join-Path $baseline 'api\v2\session_lifecycle.h').Replace('\','/')+'"'
            if(!$body.Contains($currentInclude)){throw 'Cannot isolate lifecycle implementation'}
            [IO.File]::WriteAllText($oldSession,$body.Replace($currentInclude,$oldInclude))
            $park=Join-Path $objects 'park-before.c'
            @(('#define PB_CLOSE_RETRY_SESSION_SOURCE "'+$oldSession.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\model\lifecycle_park_tests.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $park -Encoding utf8
        }
        Invoke-SystemLoaderTests -Directory $objects -BaseFlags $flags -FixtureSource $fixture -CompileOnly
        $exe=Join-Path $objects 'system-loader-tests.exe'
        foreach($kind in @('api','host')) {
            $dll=Join-Path $objects "system-loader-$kind.dll"
            if($variant -eq 'before'){Expect-Control $exe @($dll,'--baseline') 49 'DOWNLEVEL_DELAYLOAD_ABORTED' (Join-Path $objects "$kind-control.log")}
            else {Invoke-TestTool $exe @($dll,(Join-Path $objects 'decoy'))}
            $executables+=@{case="loader-$kind";variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();dll=$dll;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant()}
        }
        foreach($guard in @('ordinary','guarded')) {
            $exe=Join-Path $objects "park-$guard.exe";$caseFlags=@($flags)
            if($guard -eq 'guarded'){$caseFlags+='/DPB_GUARD_TESTS=1'}
            Invoke-BuildTool cl.exe ($caseFlags+@($park,'api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c',
                '/link',"/out:$exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
            if($variant -eq 'before'){Expect-Control $exe @('--baseline') 50 'FAILED_PARK_RETRIED_WITHOUT_BACKOFF' (Join-Path $objects "$guard-control.log")}
            else {Invoke-TestTool $exe @()}
            $executables+=@{case='lifecycle-park';variant=$variant;volatile=$mode;memory=$guard;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
    }
    $exe=Join-Path $directory "contract-$mode.exe"
    Invoke-BuildTool cl.exe @('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$directory\",'tests\model\system_library_tests.c','/link',"/out:$exe")
    Invoke-TestTool $exe @()
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during wait/loader audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;kernel_runtime_tested=$false;production_ready=$false;
    method='Actual private lifecycle pool, protected native-close handle and live mappings; zero/1/2/64 failed waits, unexpected statuses, each 1000ms backoff request gated while final loans/public/adapter handles retire, event before/after backoff, real close after recovery and zero tracked resources. Actual delay hook in API and setup host, real modules and four joined caller threads, emulate Win7 absent search flags/export, keep modern restricted search, validate System32 module paths against CWD decoy, 18 DLL lifecycles (two capability warmups, sixteen measured), exact negative controls 49/50. Shared loader guarded input/Unicode/path/error/capability matrix. ISO/MS volatile plus ordinary/guarded lifecycle memory. No SYS execution or old OS VM.';
    operations=$script:TestResultFile;executables=$executables}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
