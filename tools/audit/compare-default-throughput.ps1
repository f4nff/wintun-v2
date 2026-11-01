# SPDX-License-Identifier: GPL-2.0
param(
    [ValidateRange(1,21)][int]$Rounds=7,
    [ValidateRange(16384,100000000)][int]$Objects=4194304,
    [string]$OutputDirectory
)
# Compare current production defaults with isolated old parameter overrides.
# No driver installation; the hosted peer measures descriptor exchange only.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
if(-not $OutputDirectory){$OutputDirectory=Join-Path $script:RepoRoot "out\exchange\defaults-$($script:SourceFingerprint.Substring(0,12))"}
$directory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'comparison-tests.json'
$started=[DateTimeOffset]::Now.ToString('o')
$programs=@{};$artifacts=@()
foreach($variant in @('old','production')) {
    $objectsDir=Join-Path $directory $variant
    New-Item -ItemType Directory -Path $objectsDir -Force|Out-Null
    $flags=@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX','/wd4505',"/I$script:RepoRoot\common","/Fo$objectsDir\")
    $overrides=@{}
    if($variant -eq 'old') {
        $header=Join-Path $objectsDir 'old-default-limits.h'
        $include='#include "'+(Join-Path $script:RepoRoot 'include\packetbridge_v2_abi.h').Replace('\','/')+'"'
        @($include,
            '#undef PB_MAX_BATCH','#define PB_MAX_BATCH 32u',
            '#undef PB_MAX_REGION','#define PB_MAX_REGION (64u * 1024u * 1024u)')|
            Set-Content -LiteralPath $header -Encoding ascii
        $flags+="/FI$header"
        $overrides=@{PB_MAX_BATCH=32;PB_MAX_REGION=67108864}
    }
    foreach($kind in @('tests','bench')) {
        $source=if($kind -eq 'tests'){'tests\model\exchange_tests.c'}else{'tests\bench\exchange_bench.c'}
        $exe=Join-Path $objectsDir "$kind.exe"
        Invoke-BuildTool 'cl.exe' ($flags+@('common\v2_core.c',$source,'/link',"/out:$exe"))
        if($kind -eq 'tests'){Invoke-TestTool $exe @()}
        else{$programs[$variant]=$exe}
        $artifacts+=@{variant=$variant;kind=$kind;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();compile_overrides=$overrides}
    }
}
$cases=@()
foreach($length in @(64,1500,32768)) {foreach($direction in @('rx','tx')) {
    # Keep the three profiles adjacent, reversing their order every round.
    foreach($profile in @('old_default','single_queue_depth','current_default')) {
        $cases+=@{profile=$profile;direction=$direction;object_bytes=$length}
    }
}}
$cases|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $directory 'comparison-cases.json') -Encoding utf8
$raw=Join-Path $directory 'comparison-samples.jsonl'
[IO.File]::WriteAllText($raw,'',[Text.UTF8Encoding]::new($false))
$done=0
foreach($round in 0..($Rounds-1)) {
    $ordered=@($cases);if($round%2){[Array]::Reverse($ordered)}
    foreach($case in $ordered) {
        if($case.profile -eq 'current_default') {
            $exe=$programs.production
            # No /FI overrides: config and directional budget come from actual headers.
            $arguments=@('--defaults',$case.direction,[string]$case.object_bytes,[string]$Objects)
        } else {
            $exe=$programs.old
            $slots=if($case.profile -eq 'old_default'){32}else{128}
            $ring=if($case.profile -eq 'old_default'){64}else{128}
            $arguments=@($case.direction,'1',[string]$slots,[string]$ring,'131072',[string]$case.object_bytes,[string]$Objects,'32')
        }
        $lines=@(& $exe @arguments 2>&1);$code=$LASTEXITCODE
        if($code -ne 0){$lines|Write-Output;throw "Benchmark failed: $($case|ConvertTo-Json -Compress), exit=$code"}
        $json=@($lines|Where-Object{$_.ToString().StartsWith('{')})
        if($json.Count -ne 1){throw 'Expected exactly one benchmark JSON result.'}
        $sample=$json[0]|ConvertFrom-Json
        if($sample.direction -ne $case.direction -or $sample.object_bytes -ne $case.object_bytes -or $sample.objects -ne $Objects){throw 'Benchmark result does not match requested case.'}
        $sample|Add-Member round $round
        $sample|Add-Member profile $case.profile
        $sample|Add-Member config_init ($case.profile -eq 'current_default')
        $sample|Add-Member exit_code $code
        $sample|Add-Member token_validation 'filter'
        [IO.File]::AppendAllText($raw,($sample|ConvertTo-Json -Compress)+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))
        $done++
    }
    Write-Output "Progress: round $($round+1)/$Rounds, $done/$($Rounds*$cases.Count) samples"
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during measurement.'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;started=$started;finished=[DateTimeOffset]::Now.ToString('o');
    rounds=$Rounds;objects_per_sample=$Objects;warmup_objects=8192;cases=$cases.Count;samples=$done;
    processor=(Get-CimInstance Win32_Processor|Select-Object Name,NumberOfCores,NumberOfLogicalProcessors);
    os=(Get-CimInstance Win32_OperatingSystem|Select-Object Caption,Version,BuildNumber);
    artifacts=$artifacts;raw_samples=$raw;tests=(Join-Path $directory 'comparison-tests.json');
    production_source_overrides=$false;kernel_runtime_tested=$false;
    method='Current defaults: PB_CONFIG_INIT, PB_MAX_BATCH and directional budgets from production headers, without forced includes. Old baseline: current same source with isolated batch32/region64MiB overrides and explicit 1queue/32slots/ring64/128KiB budgets. Depth control: same old binary, 1queue/128slots/ring128/batch32. Adjacent profiles, reverse order every round. Actual DLL queue bodies and shared core, real Windows events/SRW locks, pinned user/hosted-peer threads (2 per queue), immediate completion/recycle, pre-touched buffers and exact ordered 16-byte markers per object. No full payload copy, SYS/NDIS callbacks, crypto or network; directions measured separately. SetEvent/wait call counts are not context switches.'}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory 'comparison-evidence.json') -Encoding utf8
Write-Output "Evidence: $directory"
