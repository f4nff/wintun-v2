param([string]$BaselineDirectory='out\event-init-baseline\ebc1bbfa961a')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Event initialization audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('api\cleanup.h','6d85708a11f04a4010e4f720270d91b4a550c2b96b84227976b777971df8529b'),
    @('api\rundll32_process.h','d02fb712211c770fae21a34c8d62238cebb4ba822bffa4bfe20c51ba84036d8c'),
    @('api\main.c','543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'),
    @('api\rundll32.c','6f0593fe8468ae5029fc4ef880677904679d50ab3b56584e15efd8574eb442c7'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]){throw "Unexpected frozen source: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\event-init-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
. (Join-Path $PSScriptRoot '..\build\event-init-tests.ps1')
$before=Join-Path $directory 'before.c'
@(
    ('#define PB_LEGACY_RUNTIME_MAIN_SOURCE "'+(Join-Path $baseline 'api\main.c').Replace('\','/')+'"'),
    ('#define PB_LEGACY_HELPER_SOURCE "'+(Join-Path $baseline 'api\rundll32.c').Replace('\','/')+'"'),
    ('#define PB_CLEANUP_SOURCE "'+(Join-Path $baseline 'api\cleanup.h').Replace('\','/')+'"'),
    ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\event_init_fixture.c').Replace('\','/')+'"'))|
    Set-Content -LiteralPath $before -Encoding utf8
$controls=@('MISSING_STOP_EVENTS_ADMITTED','CLEANUP_INIT_FAILURE_REPORTED_SUCCESS','FIRST_EVENT_ERROR_OVERWRITTEN','JOB_FIRST_EVENT_ERROR_LOST')
$executables=@()
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $source=if($variant -eq 'before'){$before}else{'tests\safety\event_init_fixture.c'}
        Invoke-EventInitTests -Directory $objects -BaseFlags $flags -FixtureSource $source -CompileOnly
        $test=Join-Path $objects 'event-init-tests.exe';$fixture=Join-Path $objects 'event-init-fixture.dll'
        if($variant -eq 'after'){Invoke-TestTool $test @($fixture)}
        else {for($kind=1;$kind -le 4;++$kind) {
            $output=@(& $test $fixture $kind 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
            $log=Join-Path $objects ("control-$kind.log");$output|Set-Content -LiteralPath $log -Encoding utf8
            $expected=$code -eq 60+$kind -and ($output -join "`n").Contains($controls[$kind-1])
            $script:TestExecutions.Add([ordered]@{program=$test;arguments=@($fixture,[string]$kind);exit_code=$code;expected_exit_code=60+$kind;expected_baseline_failure=$expected;log=$log})
            Write-TestRecording $(if($expected){'running'}else{'failed'})
            if(!$expected){throw "Unexpected baseline result: $($controls[$kind-1]), exit=$code"}
        }}
        foreach($path in @($test,$fixture)){$executables+=@{variant=$variant;volatile=$mode;path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during event initialization audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen whole cleanup/helper/runtime sources. Actual production cold initializer, helper admission and unpublished job construction inside a real DLL; Win7 API target. Real manual events, deterministic errors including zero and overwritten success errors. Four exact baseline controls 61-64. ISO/MS volatile, 342 checks each: all first/second event failures, every nine-acquisition job prefix, retry, event signal states, 8-thread first-use contention including failed initialization and shutdown while four leases remain. All application resources balanced after each case, all forty test threads joined. First full-matrix initialization count difference recorded; next two matrices and sixteen fresh DLL initialization/rollback/unload cycles have no handle growth. No devices changed, SYS loaded or child submitted.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
