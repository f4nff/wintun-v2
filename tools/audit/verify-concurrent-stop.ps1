param([string]$BaselineDirectory='out\concurrent-stop-baseline\e2cad0884b0c')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\runtime-wait-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Concurrent-stop audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'api\main.c')).Path
if((Get-FileHash -LiteralPath $baseline).Hash.ToLowerInvariant() -ne '771b0f54e7fe3d65793821b272be1e1995c51ab041a7052abac493213cfd0deb'){throw 'Unexpected runtime baseline'}
$directory=Join-Path $script:RepoRoot "out\concurrent-stop-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $fixture='tests\safety\runtime_wait_fixture.c'
        if($variant -eq 'before') {
            $oldMain=Join-Path $objects 'main-before.c'
            # Normalize includes to the same current declarations, retaining the
            # frozen runtime implementation and avoiding duplicate pragma-once identities.
            $body=[regex]::Replace([IO.File]::ReadAllText($baseline),'#include "([^"]+)"',{
                param($m)
                $path=Join-Path $script:RepoRoot ('api\'+$m.Groups[1].Value)
                if(!(Test-Path -LiteralPath $path)){throw "Unknown frozen include: $path"}
                '#include "'+$path.Replace('\','/')+'"'
            })
            [IO.File]::WriteAllText($oldMain,$body)
            $fixture=Join-Path $objects 'fixture-before.c'
            @(('#define PB_LEGACY_RUNTIME_MAIN_SOURCE "'+$oldMain.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\runtime_wait_fixture.c').Replace('\','/')+'"'))|
                Set-Content -LiteralPath $fixture -Encoding utf8
        }
        Invoke-RuntimeWaitTests -Directory $objects -BaseFlags $flags -FixtureSource $fixture -CompileOnly
        $dll=Join-Path $objects 'runtime-wait-fixture.dll';$exe=Join-Path $objects 'runtime-wait-tests.exe'
        if($variant -eq 'after'){Invoke-TestTool $exe @($dll)}
        else {
            foreach($case in @(@('--failure-only',42,'FAILED_WAIT_RETRIED'),@('--stop-only',43,'BLOCKED_ACQUIRE_NOT_WOKEN'))) {
                $output=@(& $exe $dll $case[0] 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
                $log=Join-Path $objects ($case[0].Substring(2)+'.log');$output|Set-Content -LiteralPath $log -Encoding utf8
                $expected=$code -eq $case[1] -and ($output -join "`n").Contains($case[2])
                $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@($dll,$case[0]);exit_code=$code;expected_exit_code=$case[1];expected_baseline_failure=$expected;log=$log})
                Write-TestRecording $(if($expected){'running'}else{'failed'})
                if(!$expected){throw "Frozen runtime did not reproduce $($case[0])"}
            }
        }
        $executables+=@{case='runtime-wait';variant=$variant;volatile=$mode;dll=$dll;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();exe=$exe;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
    foreach($guarded in @($false,$true)) {
        $memory=if($guarded){'guarded'}else{'ordinary'}
        $objects=Join-Path $directory "ownership-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        if($guarded){$flags+='/DPB_GUARD_TESTS=1'}
        $exe=Join-Path $objects 'ownership.exe'
        Invoke-BuildTool cl.exe ($flags+@('tests\model\ownership_counter_tests.c','api\v2\adapter.c','api\v2\driver_package.c',
            'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
        Invoke-TestTool $exe @()
        $executables+=@{case='queue-stop-ownership';volatile=$mode;memory=$memory;exe=$exe;exe_sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during concurrent-stop audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen/current actual runtime DLL, real delay-import unload held after last dependency release, five injected CV failures, real eight-waiter waits and spurious wakes, early stop rejection with physical unload still retained. Baseline requires exact exit/marker for both defects. Actual v2 queue/lifecycle counter paths: 40000 live cycles and 64 eight-owner stop/return/cancel schedules per ISO/MS ordinary/guarded process. No caller violates join-before-final-close; no native kernel execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
