param([string]$BaselineDirectory='out\overlapped-drain-baseline\8620a488a5de')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\helper-io-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'I/O audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$old=Join-Path $baseline 'api\rundll32_process.h'
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne '7a382f4cd620aaecacbdfd01f57000352257b172952f570b8c3e181eca15a9f4'){throw 'Unexpected frozen helper source'}
$directory=Join-Path $script:RepoRoot "out\overlapped-drain-audit\$($script:SourceFingerprint.Substring(0,12))"
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
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $fixture='tests\safety\helper_io_fixture.c'
        if($variant -eq 'before') {
            $oldHelper=Join-Path $objects 'rundll32-before.c'
            $body=Normalize-ApiIncludes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\rundll32.c')))
            $currentInclude='#include "'+(Join-Path $script:RepoRoot 'api\rundll32_process.h').Replace('\','/')+'"'
            $oldInclude='#include "'+$old.Replace('\','/')+'"'
            if(!$body.Contains($currentInclude)){throw 'Cannot isolate helper implementation'}
            [IO.File]::WriteAllText($oldHelper,$body.Replace($currentInclude,$oldInclude))
            $fixture=Join-Path $objects 'fixture-before.c'
            @(('#define PB_LEGACY_HELPER_SOURCE "'+$oldHelper.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\helper_io_fixture.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $fixture -Encoding utf8
        }
        Invoke-HelperIoTests -Directory $objects -BaseFlags $flags -FixtureSource $fixture -CompileOnly
        $dll=Join-Path $objects 'helper-io-fixture.dll';$exe=Join-Path $objects 'helper-io-tests.exe'
        if($variant -eq 'after'){Invoke-TestTool $exe @($dll)}
        else {
            foreach($case in @(@(1,46,'PENDING_IO_RELEASED'),@(2,47,'CONNECT_COMPLETION_LOST'),@(3,48,'DRAIN_COMPLETION_LOST'))) {
                $arguments=@($dll,[string]$case[0]);$output=@(& $exe @arguments 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
                $log=Join-Path $objects ("control-"+$case[0]+'.log');$output|Set-Content -LiteralPath $log -Encoding utf8
                $expected=$code -eq $case[1] -and ($output -join "`n").Contains($case[2])
                $script:TestExecutions.Add([ordered]@{program=$exe;arguments=$arguments;exit_code=$code;expected_exit_code=$case[1];expected_baseline_failure=$expected;log=$log})
                Write-TestRecording $(if($expected){'running'}else{'failed'})
                if(!$expected){throw "Unexpected baseline result: $($case[2]), exit=$code"}
            }
        }
        $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();dll=$dll;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during I/O audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;kernel_runtime_tested=$false;production_ready=$false;
    method='Actual helper connection/read/drain code and real overlapped named pipes. Deferred cancellation, NOT_FOUND/ACCESS_DENIED, actual cancel/normal write/broken pipe completion on joined threads; manual-event reset interleavings, spurious signals, failed waits/resets with bounded backoff, 64 consecutive failures, terminal-before-wait, first-error preservation. Forty DLL lifecycles: zero tracked allocation/OS handle residue, every owned thread joined/closed, complete module unload. Live process thread range is diagnostic only because native Windows loader/pool workers vary. Exact frozen negative controls 46/47/48; settle real I/O before baseline exit. ISO/MS volatile; Win7 target imports. No native SYS execution.';
    operations=$script:TestResultFile;executables=$executables}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
