param([string]$BaselineDirectory='out\termination-wait-baseline\6ca33c59d88e')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot 'helper-test-root.ps1')
. (Join-Path $PSScriptRoot '..\build\helper-termination-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Termination audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(@('api\session.c','dac797ec96ca0f98f8047a9659759490c322f8e4e435e3e18ce19cff01ff7425'),
    @('api\rundll32_process.h','703eb3a16fbb6936fa6d29f23e386d115bcd7cad7a1f1b9a8c10438d9066f990'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]){throw "Unexpected frozen source: $($entry[0])"}
}
Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative 'common/offload_policy.c'
$directory=Join-Path $script:RepoRoot "out\termination-wait-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json';$executables=@()
$helperRootContracts=@(Test-HelperTestRootContracts -RepoRoot $script:RepoRoot)
if($helperRootContracts.Count -ne 13){throw 'Helper test root contract count changed'}
$script:HelperRootObservations=[Collections.Generic.List[object]]::new()
function Normalize-ApiIncludes([string]$content) {
    [regex]::Replace($content,'#include "([^"]+)"',{
        param($m)
        $path=Join-Path $script:RepoRoot ('api\'+$m.Groups[1].Value)
        if(!(Test-Path -LiteralPath $path)){throw "Unknown source include: $path"}
        '#include "'+$path.Replace('\','/')+'"'
    })
}
function Expect-TerminationControl([string]$Program,[string[]]$Arguments,[int]$Code,[string]$Marker,[string]$Log) {
    $output=@(& $Program @Arguments 2>&1|ForEach-Object {$_.ToString()});$actual=$LASTEXITCODE
    $output|Set-Content -LiteralPath $Log -Encoding utf8
    $expected=$actual -eq $Code -and ($output -join "`n").Contains($Marker)
    $cleanup=$null;$cleanupFailure=''
    if($Code -eq 45 -and $expected){
        try{
            $cleanup=Assert-HelperTestRootCompletion -Lines $output -RepoRoot $script:RepoRoot -BaselineCleanup
            $script:HelperRootObservations.Add($cleanup)
        }catch{$expected=$false;$cleanupFailure=$_.Exception.Message}
    }elseif($Code -eq 44 -and @($output|Where-Object {$_.StartsWith('OWNED_ROOT',[StringComparison]::Ordinal)}).Count){
        # Legacy44 controls never create helper roots and never invoke cleanup.
        $expected=$false;$cleanupFailure='Unexpected helper root marker in legacy44 control'
    }
    $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$actual;expected_exit_code=$Code;expected_baseline_failure=$expected;log=$Log;helper_root_cleanup=$cleanup;helper_root_cleanup_failure=$cleanupFailure})
    Write-TestRecording $(if($expected){'running'}else{'failed'})
    if(!$expected){throw "Unexpected baseline result: $Marker, exit=$actual $cleanupFailure"}
}
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/wd4200',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $compat='tests\model\compat_session_tests.c';$fixture='tests\safety\helper_termination_fixture.c'
        if($variant -eq 'before') {
            $oldSession=Join-Path $objects 'session-before.c'
            [IO.File]::WriteAllText($oldSession,(Normalize-ApiIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api\session.c')))))
            $compat=Join-Path $objects 'compat-before.c'
            @(('#define PB_COMPAT_SESSION_SOURCE "'+$oldSession.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\model\compat_session_tests.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $compat -Encoding utf8
            $oldHelper=Join-Path $objects 'rundll32-before.c'
            $body=Normalize-ApiIncludes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\rundll32.c')))
            $currentInclude='#include "'+(Join-Path $script:RepoRoot 'api\rundll32_process.h').Replace('\','/')+'"'
            $oldInclude='#include "'+(Join-Path $baseline 'api\rundll32_process.h').Replace('\','/')+'"'
            if(!$body.Contains($currentInclude)){throw 'Cannot isolate helper process implementation'}
            [IO.File]::WriteAllText($oldHelper,$body.Replace($currentInclude,$oldInclude))
            $fixture=Join-Path $objects 'fixture-before.c'
            @(('#define PB_LEGACY_HELPER_SOURCE "'+$oldHelper.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\helper_termination_fixture.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $fixture -Encoding utf8
        }
        $exe=Join-Path $objects 'compat.exe'
        Invoke-BuildTool cl.exe ($flags+@('/DPB_API_TESTS=1',$compat,'api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c',
            'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
        if($variant -eq 'after'){Invoke-TestTool $exe @()}
        else {foreach($case in @('end','start')) {Expect-TerminationControl $exe @("--termination-$case") 44 'LIVE_WORKER_MAPPING_RELEASED' (Join-Path $objects "$case-control.log")}}
        $executables+=@{case='legacy-join';variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        Invoke-HelperTerminationTests -Directory $objects -BaseFlags $flags -FixtureSource $fixture -CompileOnly
        $dll=Join-Path $objects 'helper-termination-fixture.dll';$exe=Join-Path $objects 'helper-termination-tests.exe'
        if($variant -eq 'after'){
            $helperLines=@(Invoke-TestTool $exe @($dll));$helperLines|Out-Host
            $cleanup=Assert-HelperTestRootCompletion -Lines $helperLines -RepoRoot $script:RepoRoot
            $script:HelperRootObservations.Add($cleanup)
            $script:TestExecutions[$script:TestExecutions.Count-1]['helper_root_cleanup']=$cleanup
            Write-TestRecording 'running'
        }
        else {Expect-TerminationControl $exe @($dll,'--baseline-control') 45 'LIVE_PROCESS_RELEASE_AFTER_FAILED_WAIT' (Join-Path $objects 'helper-control.log')}
        $executables+=@{case='process-join';variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();dll=$dll;dll_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant()}
    }
    $objects=Join-Path $directory "contract-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $exe=Join-Path $objects 'contract.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\",'tests\model\termination_wait_tests.c','/link',"/out:$exe"))
    Invoke-TestTool $exe @()
}
if($script:HelperRootObservations.Count -ne 4 -or @($script:HelperRootObservations|Where-Object {$_.mode -ceq 'normal'}).Count -ne 2 -or
    @($script:HelperRootObservations|Where-Object {$_.mode -ceq 'baseline45'}).Count -ne 2){throw 'Expected two normal roots and two finite45 cleanups'}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during termination audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;kernel_runtime_tested=$false;production_ready=$false;
    production_changed=$true;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    helper_test_root=@{path='tools/audit/helper-test-root.ps1';sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'helper-test-root.ps1')).Hash.ToLowerInvariant();contracts=$helperRootContracts;observations=@($script:HelperRootObservations.ToArray());
        sources=@('tests\safety\helper_termination_tests.c','tests\safety\helper_test_root.h'|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})};
    method='Actual legacy/v2 worker and actual helper DLL with real process/thread handles. Inject terminal wait failures/statuses, intercept release before unsafe free, exact baseline markers/codes44 and45; rescue/join baseline child before exit. Helper roots are exclusively created run-PID-CreationTime-attempt children of the fixed test base. The parent requires exactly one canonical OWNED_ROOT marker, decimal/hex PID agreement, attempt01..32 and no reparse paths. Normal helpers must remove their own root. Expected45 preserves exactly one helper-PID-1 child and one zero-byte setupapihost.dll; the parent validates the complete tree before removing only those three literal paths without recursion. Historical44 controls have no marker and never use this cleanup. Thirteen PowerShell contracts verify malformed/missing/duplicate/outside/PID/attempt input rejection, unchanged files on rejected layouts, exact45 cleanup and already-absent normal root. Original Scenario/child/loops/flags/deadlines and negative codes remain unchanged. Current12 worker schedules, startup error preservation, process cancellation/late exit/exit code259 and dropped caller module reference. Verify each backoff request, all retained resource witnesses, real signal before cleanup, and clean recovery. First-error/zero-error/64-failure contract tests, ISO/MS volatile. No native SYS execution. Historical policy dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair; all other source bindings and test paths retain their existing contracts.';
    operations=$script:TestResultFile;executables=$executables}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
