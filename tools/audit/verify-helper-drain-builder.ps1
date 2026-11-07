param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\helper-drain-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Helper drain builder requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$directory=if(!$OutputDirectory){Join-Path $script:RepoRoot "out\helper-drain-builder\$($script:SourceFingerprint.Substring(0,12))"}elseif([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}
$null=New-Item -ItemType Directory -Path $directory -Force;$encoding=[Text.UTF8Encoding]::new($false)
# Reuse the frozen independent parser and projections, not a historical build
# or a reconstructed old helper. This reader itself needs no old out directory.
$reader=Join-Path $PSScriptRoot 'verify-helper-drain.ps1'
if((Get-FileHash $reader).Hash.ToLowerInvariant() -cne 'cfe153db3ebf242380f2614850ec556363619cc17156aed3292b15bdc0a2eee4'){throw 'Frozen helper drain verifier changed'}
$tokens=$null;$errors=$null;$ast=[Management.Automation.Language.Parser]::ParseFile($reader,[ref]$tokens,[ref]$errors)
if($errors.Count){throw 'Frozen helper drain verifier does not parse'}
$loaded=@();foreach($name in @('Get-HelperDrainPart','Project-HelperDrainAuditIncludes','Replace-HelperDrainAuditOnce','Get-HelperDrainRows','Invoke-HelperDrainProcess')){
    $nodes=@($ast.FindAll({param($n)$n -is [Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -ceq $name},$true))
    if($nodes.Count -ne 1){throw "Frozen helper verifier function changed:$name"};. ([ScriptBlock]::Create($nodes[0].Extent.Text));$loaded+=@{name=$name;body=$nodes[0].Extent.Text}
}
$deps=@($ast.EndBlock.Statements|Where-Object {$_ -is [Management.Automation.Language.AssignmentStatementAst] -and $_.Left.Extent.Text -ceq '$dependencies'})
if($deps.Count -ne 1){throw 'Frozen helper drain dependency set changed'};. ([ScriptBlock]::Create($deps[0].Extent.Text))
function Assert-CurrentHelperDrainBuild($build){
    $names=@('main-original.c','main-selected.h','rundll-original.c','rundll-selected.h','fixture-original.c','fixture-selected.h','helper-current.h','helper-selected.h','fixture-entry.c','host-entry.c')
    if($build.source_mode -cne 'current-source' -or $build.baseline -or $build.variant -cne 'current' -or $build.sources.Count -ne 24 -or $build.parts.Count -ne 10 -or
        ($build.parts.name -join '|') -cne ($names -join '|') -or @(Compare-Object ($build.sources.relative|Sort-Object) ($dependencies|Sort-Object)).Count -or
        @($build.sources.relative|Select-Object -Unique).Count -ne 24 -or (Test-Path -LiteralPath (Join-Path (Split-Path $build.path) 'helper-before.h'))){throw 'Current helper build retained a historical dependency or changed source/output set'}
    foreach($source in $build.sources){if($source.path -cne (Join-Path $script:RepoRoot $source.relative) -or $source.sha256 -cne (Get-FileHash $source.path).Hash.ToLowerInvariant()){throw 'Current helper raw source binding changed'}}
    foreach($part in $build.parts){if((Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw 'Current helper artifact hash changed'}}
    foreach($pair in @(@('main-original.c','api\main.c'),@('rundll-original.c','api\rundll32.c'),@('fixture-original.c','tests\safety\legacy_runtime_fixture.c'),@('helper-current.h','api\rundll32_process.h'))){
        if((Get-HelperDrainPart $build $pair[0]).sha256 -cne (Get-FileHash (Join-Path $script:RepoRoot $pair[1])).Hash.ToLowerInvariant()){throw 'Whole current helper input copy changed'}
    }
    $helper=Get-HelperDrainPart $build 'helper-selected.h';$main=Get-HelperDrainPart $build 'main-selected.h';$rundll=Get-HelperDrainPart $build 'rundll-selected.h';$fixture=Get-HelperDrainPart $build 'fixture-selected.h'
    foreach($pair in @(@($helper,'api\rundll32_process.h'),@($main,'api\main.c'))){if([IO.File]::ReadAllText($pair[0].path) -cne (Project-HelperDrainAuditIncludes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot $pair[1]))) $pair[1])){throw 'Current whole source exceeded absolute-include projection'}}
    $expected=Project-HelperDrainAuditIncludes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\rundll32.c'))) 'api\rundll32.c'
    $expected=Replace-HelperDrainAuditOnce $expected ('#include "'+(Join-Path $script:RepoRoot 'api\rundll32_process.h').Replace('\','/')+'"') ('#include "'+$helper.path.Replace('\','/')+'"')
    if([IO.File]::ReadAllText($rundll.path) -cne $expected){throw 'Current rundll client exceeded helper/include projection'}
    $expected=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'tests\safety\legacy_runtime_fixture.c'))
    $expected=Replace-HelperDrainAuditOnce $expected '#define RUN_DLL32_TIMEOUT 2000u' '/* Retain production120000ms default; no public helper waiter is invoked. */'
    $expected=Replace-HelperDrainAuditOnce $expected 'if (Detached) SetEvent(Detached);' 'DrainFinalCheck(); if (Detached) SetEvent(Detached);'
    if([regex]::Matches($expected,'\bassert\(').Count -ne 4 -or $build.fixture_assert_tokens -ne 4){throw 'Current fixture assert strengthening changed'}
    if([IO.File]::ReadAllText($fixture.path) -cne [regex]::Replace($expected,'\bassert\(','HD_CHECK(')){throw 'Current fixture exceeded original finite projection'}
    $test=Join-Path $script:RepoRoot 'tests\safety\helper_drain_tests.c';$shared=@();if($build.guarded){$shared+='#define PB_GUARD_TESTS 1'}
    $lines=$shared+@('#define PB_HELPER_DRAIN_FIXTURE 1');foreach($pair in @(@('PB_LEGACY_RUNTIME_MAIN_SOURCE',$main.path),@('PB_LEGACY_HELPER_SOURCE',$rundll.path),@('PB_HELPER_DRAIN_SOURCE',$fixture.path))){$lines+='#define '+$pair[0]+' "'+$pair[1].Replace('\','/')+'"'}
    $lines+='#include "'+$test.Replace('\','/')+'"'
    if([IO.File]::ReadAllText((Get-HelperDrainPart $build 'fixture-entry.c').path) -cne (($lines -join "`n")+"`n")){throw 'Current helper fixture entry changed'}
    $lines=$shared+@('#include "'+$test.Replace('\','/')+'"')
    if([IO.File]::ReadAllText((Get-HelperDrainPart $build 'host-entry.c').path) -cne (($lines -join "`n")+"`n") -or $build.test -cne $test -or $build.test_sha256 -cne (Get-FileHash $test).Hash.ToLowerInvariant()){throw 'Current helper host/test entry changed'}
    if(!$build.ndebug -or $build.pb_api_tests -or !$build.maybe_wow64_source -or '/DNDEBUG' -notin $build.arguments -or '/DMAYBE_WOW64=1' -notin $build.arguments -or "/volatile:$($build.mode)" -notin $build.arguments -or "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint" -notin $build.arguments -or @($build.arguments|Where-Object {$_ -match 'PB_API_TESTS'}).Count){throw 'Current helper build flags changed'}
    if((Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256 -or (Get-FileHash $build.fixture).Hash.ToLowerInvariant() -cne $build.fixture_sha256){throw 'Current helper executable/fixture changed'}
}
function Invoke-CurrentHelperDrainProcess($build,[bool]$Control){
    Invoke-HelperDrainProcess $build $Control
    $record=$script:TestExecutions[$script:TestExecutions.Count-1];$text=[IO.File]::ReadAllText($record.log)
    $summaries=[regex]::Matches($text,'(?m)^PASS: (\d+) helper drain cases warm(\d+) measure(\d+); canonical error/EOF and terminal native I/O before close, cached heap/SDDL freed at actual DLL detach; lost(\d+) handles(\d+)/(\d+)\r?$')
    if($summaries.Count -ne 1){throw 'Current helper metrics require one actual process summary'};$summary=$summaries[0]
    if([int]$summary.Groups[1].Value -ne $record.schedules -or $summary.Groups[5].Value -cne $summary.Groups[6].Value -or [int]$summary.Groups[4].Value){throw 'Current helper metrics disagree with validated execution'}
    $record.metrics=@{cases=[int]$summary.Groups[1].Value;checked_warmup_schedules=[int]$summary.Groups[2].Value;measured_schedules=[int]$summary.Groups[3].Value;
        process_handles_before=[int]$summary.Groups[5].Value;process_handles_after=[int]$summary.Groups[6].Value;lost_drain_errors=0}
    $record.mode=$build.mode;$record.guarded=$build.guarded;$record.source_mode=$build.source_mode
    Write-TestRecording 'running'
}
# A fresh empty repository root makes the default historical path unavailable
# without moving/removing any real snapshot or fabricating original bytes.
$realRepo=$script:RepoRoot;$emptyRepo=Join-Path $directory ('missing-history-'+[Guid]::NewGuid().ToString('N'));$null=New-Item -ItemType Directory -Path $emptyRepo
$missing=Join-Path $emptyRepo 'explicit-missing';$early=@()
foreach($case in @(@{name='before-default-missing';variant='before';explicit=$false},@{name='before-explicit-missing';variant='before';explicit=$true},@{name='current-explicit-missing';variant='current';explicit=$true})){
    $destination=Join-Path $emptyRepo $case.name;$script:RepoRoot=$emptyRepo;$rejected=$false;$errorText=$null
    try{
        $buildArguments=@{Directory=$destination;Variant=$case.variant};if($case.explicit){$buildArguments.BaselineDirectory=$missing}
        try{$null=Build-HelperDrainTests @buildArguments}catch{$errorText=$_.ToString();$rejected=$_.CategoryInfo.Category -eq [Management.Automation.ErrorCategory]::ObjectNotFound -and $errorText.Contains($(if($case.explicit){$missing}else{Join-Path $emptyRepo 'out\retire-edge-baseline\d1dc19684d05'}))}
    }finally{$script:RepoRoot=$realRepo}
    if(!$rejected -or (Test-Path -LiteralPath $destination)){throw "Missing historical baseline was not rejected before output/source access:$($case.name):$errorText"}
    $early+=@{name=$case.name;expected_rejection=$true;no_output_created=$true;error=$errorText}
}
$defaultHistory=Join-Path $script:RepoRoot 'out\retire-edge-baseline\d1dc19684d05';$defaultHistoryPresent=Test-Path -LiteralPath $defaultHistory
Start-TestRecording $directory 'operation-results.json';$builds=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-HelperDrainTests -Directory (Join-Path $directory "$mode-$memory") -Mode $mode -Guarded:$guarded
    Assert-CurrentHelperDrainBuild $build;Invoke-CurrentHelperDrainProcess $build $false;Invoke-CurrentHelperDrainProcess $build $true;$builds+=,$build
}}
$totalCases=($script:TestExecutions|ForEach-Object {$_.schedules}|Measure-Object -Sum).Sum
if($early.Count -ne 3 -or $script:TestExecutions.Count -ne 8 -or $totalCases -ne 508){throw 'Current helper drain builder contract matrix incomplete'}
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during current helper drain builder audit'}
Write-TestRecording 'passed'
[ordered]@{result='passed';version=$script:Version;source_tree_sha256=$script:SourceFingerprint;production_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    source_mode='current-source';default_historical_path=$defaultHistory;default_historical_path_present=$defaultHistoryPresent;historical_replay=$false;missing_baseline_contracts=$early;operations=$script:TestResultFile;executables=$builds;
    frozen_verifier=@{path=$reader;sha256=(Get-FileHash $reader).Hash.ToLowerInvariant();functions=$loaded};audit_sources=@('tools\build\helper-drain-tests.ps1','tools\audit\verify-helper-drain-builder.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Current default builder uses24 current raw dependencies and four unchanged fixed source hashes; it neither resolves a historical baseline nor creates a helper-before artifact. Ten current whole/projection/entry parts, source paths/hashes and shipping flags are independently reconstructed. Before or explicitly supplied baseline keeps the original strict historical contract; three real missing-path tests fail before output creation, including before default under a fresh empty root. Four ISO/MS ordinary/guarded current builds execute original full124 plus CLI1373 cases each,8 processes508 cases with complete native I/O terminal/handle/runtime/DLL-unload assertions and strict warmed process-handle equality. Parser/projection helpers are copied verbatim from the fixed original verifier, not from the builder. No before7f bytes are reconstructed, no missing historical replay or old evidence is claimed. MAYBE_WOW64 is a retained source-branch model, not shipping cross-architecture or Windows7 execution. Guard scope remains caller-owned HD_CASE storage; SDK allocations retain native contracts. No production, old host, test C, timeout or cleanup algorithm changes.'}|
    ConvertTo-Json -Depth 15|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
