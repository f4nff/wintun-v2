param([string]$BaselineDirectory='out\helper-pump-baseline\6d806eef06ca')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\helper-io-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Helper pump audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$old=Join-Path $baseline 'api\rundll32_process.h'
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne '450b2eff011f0f0c4affb2d068f7ba0fdf29ed86fb1dd4391ad500b371694451'){throw 'Unexpected frozen helper source'}
if((Get-FileHash -LiteralPath (Join-Path $baseline 'api\overlapped_wait.h')).Hash.ToLowerInvariant() -ne '0f05d0b967824df99b2c23690192d2feb96b6f838af6eb43953665f2471234c7'){throw 'Unexpected frozen I/O wait source'}
$directory=Join-Path $script:RepoRoot "out\helper-pump-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json';$executables=@()
function Normalize-ApiIncludes([string]$content) {
    [regex]::Replace($content,'#include "([^"]+)"',{
        param($m)
        $path=Join-Path $script:RepoRoot ('api\'+$m.Groups[1].Value)
        if(!(Test-Path -LiteralPath $path)){throw "Unknown include: $path"}
        '#include "'+$path.Replace('\','/')+'"'
    })
}
$oldBody=Normalize-ApiIncludes ([IO.File]::ReadAllText($old))
$needle='if (!okay && error == ERROR_IO_INCOMPLETE) return;'
if($oldBody.IndexOf($needle) -lt 0 -or $oldBody.IndexOf($needle) -ne $oldBody.LastIndexOf($needle)){throw 'Cannot isolate pending pump branch'}
$naive=$oldBody.Replace($needle,'if (!okay && error == ERROR_IO_INCOMPLETE) { ResetEvent(Stream->Io.hEvent); return; }')
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','reset-only','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $fixture='tests\safety\helper_io_fixture.c'
        if($variant -ne 'after') {
            $header=Join-Path $objects 'helper.h';[IO.File]::WriteAllText($header,$(if($variant -eq 'before'){$oldBody}else{$naive}))
            $entry=Normalize-ApiIncludes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\rundll32.c')))
            $currentInclude='#include "'+(Join-Path $script:RepoRoot 'api\rundll32_process.h').Replace('\','/')+'"'
            if(!$entry.Contains($currentInclude)){throw 'Cannot isolate helper entry'}
            $entryPath=Join-Path $objects 'rundll32.c';[IO.File]::WriteAllText($entryPath,$entry.Replace($currentInclude,'#include "'+$header.Replace('\','/')+'"'))
            $fixture=Join-Path $objects 'fixture.c'
            @(('#define PB_LEGACY_HELPER_SOURCE "'+$entryPath.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\helper_io_fixture.c').Replace('\','/')+'"'))|
              Set-Content -LiteralPath $fixture -Encoding utf8
        }
        Invoke-HelperIoTests -Directory $objects -BaseFlags $flags -FixtureSource $fixture -CompileOnly
        $dll=Join-Path $objects 'helper-io-fixture.dll';$exe=Join-Path $objects 'helper-io-tests.exe'
        if($variant -eq 'after'){Invoke-TestTool $exe @($dll)}
        else {
            $case=if($variant -eq 'before'){4}else{5};$expectedCode=if($variant -eq 'before'){77}else{78}
            $marker=if($variant -eq 'before'){'PENDING_HELPER_EVENT_LEFT_SIGNALED'}else{'PUMP_LOST_COMPLETION_AFTER_RESET'}
            $arguments=@($dll,[string]$case);$output=@(& $exe @arguments 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
            $log=Join-Path $objects 'control.log';$output|Set-Content -LiteralPath $log -Encoding utf8
            $expected=$code -eq $expectedCode -and ($output -join "`n").Contains($marker)
            $script:TestExecutions.Add([ordered]@{program=$exe;arguments=$arguments;exit_code=$code;expected_exit_code=$expectedCode;expected_baseline_failure=$expected;log=$log;variant=$variant})
            Write-TestRecording $(if($expected){'running'}else{'failed'})
            if(!$expected){throw "Unexpected $variant result: $marker, exit=$code"}
        }
        $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();dll=$dll;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during helper pump audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;kernel_runtime_tested=$false;production_ready=$false;
    method='Frozen whole helper hash, shared current I/O headers to isolate ordinary pump behavior. Real pending overlapped named-pipe read and manual SetEvent signal; old pump loops without clearing, exact control 77. One out-only reset-without-requery mutation must fail exact control 78 when real completion precedes reset. Both controls settle real I/O and close owned handles before exit. Current ISO/MS each run 9080 schedules across forty full DLL unloads, with 3200 owned threads actually joined and exact handle/heap/work ownership balance. Ordinary pump is bounded to two queries/one reset and no blocking wait per turn, finite spurious signals, reset races, 64 consecutive reset failures, zero/prior errors, native completion during failed reset, and other-stream progress while first stays pending. Existing connect/cancel/drain tests retained. No child process, device mutation, SYS loading or old-OS runtime execution.';
    operations=$script:TestResultFile;executables=$executables}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
