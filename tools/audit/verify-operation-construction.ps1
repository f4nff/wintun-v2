param([string]$BaselineDirectory='out\stop-edge-baseline\d4faced72bc1',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\operation-construction-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Operation construction requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$accepted='d4faced72bc16d0b7db5a4baad4cbb23f1956e117c492265441b8b57982ff80c'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -ne $accepted -or $manifest.status -ne 'accepted'){throw 'Unexpected accepted operation construction baseline'}
$dependencies=@('api\main.c','api\main.h','api\operation_runtime.h','api\operation.h','api\logger.h','api\adapter.h','api\namespace.h','api\registry.h',
    'api\ntdll.h','api\rundll32.h','api\wintun.h','common\system_library.h','include\packetbridge_identity.h','include\packetbridge_version.h',
    'tests\safety\legacy_operation_fixture.c','tests\safety\guard_memory.h')
function Assert-OperationConstructionDependencies {
    foreach($path in $dependencies){
        $old=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant()
        if($manifest.files.($path.Replace('\','/')) -cne $old -or (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $path)).Hash.ToLowerInvariant() -cne $old){throw "Unchanged construction dependency differs:$path"}
    }
}
Assert-OperationConstructionDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\operation-construction\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
Start-TestRecording $directory 'operation-results.json'
function Get-OperationConstructionPart($build,[string]$name){
    $found=@($build.parts|Where-Object {$_.name -ceq $name});if($found.Count -ne 1){throw "Nonunique generated construction part:$name"};return $found[0]
}
function Convert-OperationConstructionIncludes([string]$body,[string]$relative){
    $base=Split-Path (Join-Path $script:RepoRoot $relative)
    return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
}
function Change-OperationConstructionOnce([string]$body,[string]$before,[string]$after){
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw "Nonunique audited construction boundary:$before"};return $body.Replace($before,$after)
}
function Assert-OperationConstructionProjection($build){
    if($build.sources.Count -ne 16 -or @($build.sources.relative|Select-Object -Unique).Count -ne 16 -or $build.parts.Count -ne 8 -or @($build.parts.name|Select-Object -Unique).Count -ne 8){throw 'Construction source/part set changed'}
    if(@(Compare-Object ($build.sources.relative|Sort-Object) ($dependencies|Sort-Object)).Count){throw 'Construction dependency list changed'}
    foreach($source in $build.sources){if($source.path -cne (Join-Path $script:RepoRoot $source.relative) -or $source.sha256 -cne (Get-FileHash -LiteralPath (Join-Path $baseline $source.relative)).Hash.ToLowerInvariant()){throw 'Construction source/hash changed'}}
    foreach($part in $build.parts){if((Get-FileHash -LiteralPath $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw 'Generated construction hash changed'}}
    foreach($pair in @(@('main-original.c','api\main.c'),@('operation-original.h','api\operation_runtime.h'),@('fixture-original.c','tests\safety\legacy_operation_fixture.c'))){
        if((Get-OperationConstructionPart $build $pair[0]).sha256 -cne (Get-FileHash -LiteralPath (Join-Path $baseline $pair[1])).Hash.ToLowerInvariant()){throw 'Complete original construction source changed'}
    }
    $opPart=Get-OperationConstructionPart $build 'operation-selected.h';$mainPart=Get-OperationConstructionPart $build 'main-selected.h';$fixturePart=Get-OperationConstructionPart $build 'fixture-selected.h'
    $operation=[IO.File]::ReadAllText((Join-Path $baseline 'api\operation_runtime.h'))
    $before='    if (!job) { OperationUnreserve(); SetLastError(ERROR_OUTOFMEMORY); return 0; }'
    $after='    if (!job) { /* Test136: omit only construction reservation return. */ SetLastError(ERROR_OUTOFMEMORY); return 0; }'
    $offset=$operation.IndexOf($before);$first=$operation.IndexOf('ULONG_PTR MainRunOperation(');$next=$operation.IndexOf('BOOL MainOperationShutdown(')
    if($first -lt 0 -or $offset -le $first -or $next -le $offset){throw 'Negative reservation release is outside actual MainRunOperation'}
    if($build.variant -eq 'without-construction-unreserve'){
        if($build.mutation.offset -ne $offset -or $build.mutation.length -ne $before.Length -or $build.mutation.before -cne $before -or $build.mutation.after -cne $after){throw 'Construction single-mutation metadata changed'}
        $operation=Change-OperationConstructionOnce $operation $before $after
    }elseif($build.variant -cne 'current' -or $null -ne $build.mutation){throw 'Unexpected construction variant'}
    if([IO.File]::ReadAllText($opPart.path) -cne $operation){throw 'Operation changed outside unique construction release omission'}
    $main=Convert-OperationConstructionIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api\main.c'))) 'api\main.c'
    $main=Change-OperationConstructionOnce $main ('#include "'+(Join-Path $script:RepoRoot 'api\operation_runtime.h').Replace('\','/')+'"') ('#include "'+$opPart.path.Replace('\','/')+'"')
    if([IO.File]::ReadAllText($mainPart.path) -cne $main){throw 'Actual main projection changed'}
    $fixtureBody=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\legacy_operation_fixture.c'))
    $replacements=@(
        @('#define MAIN_OPERATION_TIMEOUT 250u','/* Production120000ms default retained; explicit shutdown cancels construction. */'),
        @('#define HeapAlloc Allocate','#define HeapAlloc ConstructionAllocate'),
        @('if(Detached) SetEvent(Detached);','ConstructionFinalCheck(); if(Detached) SetEvent(Detached);'))
    if($build.adaptations.Count -ne 4){throw 'Unexpected construction fixture adaptation count'}
    for($i=0;$i -lt 3;++$i){$pair=$replacements[$i];$record=$build.adaptations[$i];if($record.kind -cne 'replace-once' -or $record.before -cne $pair[0] -or $record.after -cne $pair[1]){throw 'Construction fixture replacement metadata changed'};$fixtureBody=Change-OperationConstructionOnce $fixtureBody $pair[0] $pair[1]}
    $asserts=[regex]::Matches($fixtureBody,'\bassert\(').Count;$record=$build.adaptations[3]
    if($asserts -ne 6 -or $record.kind -cne 'token-rename' -or $record.before -cne 'assert(' -or $record.after -cne 'OC_CHECK(' -or $record.count -ne 6){throw 'Original fixture assertion protection changed'}
    $fixtureBody=[regex]::Replace($fixtureBody,'\bassert\(','OC_CHECK(')
    $fixtureBody=Convert-OperationConstructionIncludes $fixtureBody 'tests\safety\legacy_operation_fixture.c'
    $fixtureBody=Change-OperationConstructionOnce $fixtureBody ('#include "'+(Join-Path $script:RepoRoot 'api\main.c').Replace('\','/')+'"') ('#include "'+$mainPart.path.Replace('\','/')+'"')
    if([IO.File]::ReadAllText($fixturePart.path) -cne $fixtureBody){throw 'Original SDK host differs outside finite adaptations'}
    $test=Join-Path $script:RepoRoot 'tests\safety\operation_construction_tests.c'
    if($build.test -cne $test -or $build.test_sha256 -cne (Get-FileHash $test).Hash.ToLowerInvariant()){throw 'Construction actual test binding changed'}
    $shared=@();if($build.guarded){$shared+='#define PB_GUARD_TESTS 1'};if($build.variant -ne 'current'){$shared+='#define PB_OPERATION_CONSTRUCTION_NEGATIVE 1'}
    $lines=$shared+@('#define PB_OPERATION_CONSTRUCTION_FIXTURE 1');$lines+='#define PB_OPERATION_CONSTRUCTION_SOURCE "'+$fixturePart.path.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    if([IO.File]::ReadAllText((Get-OperationConstructionPart $build 'fixture-entry.c').path) -cne (($lines -join "`n")+"`n")){throw 'Construction fixture entry changed'}
    $lines=$shared+@('#include "'+$test.Replace('\','/')+'"')
    if([IO.File]::ReadAllText((Get-OperationConstructionPart $build 'host-entry.c').path) -cne (($lines -join "`n")+"`n")){throw 'Construction host entry changed'}
    if(!$build.ndebug -or $build.pb_api_tests -or '/DNDEBUG' -notin $build.arguments -or "/volatile:$($build.mode)" -notin $build.arguments -or "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint" -notin $build.arguments -or @($build.arguments|Where-Object {$_ -match 'PB_API_TESTS'}).Count){throw 'Construction shipping build flags changed'}
}
function Invoke-OperationConstructionProcess($build,[bool]$Control,[int]$Expected=0){
    $arguments=@($build.fixture);if($Control){$arguments+='136'}
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start construction regression'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Construction process watchdog30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-operation-construction.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) operation construction cases warm(\d+) measure(\d+); actual reservation/first job allocation/shutdown retry, joined(\d+) repairs(\d+) handles(\d+)/(\d+); all SDK owners zero at actual DLL detach')
        $rows=@([regex]::Matches($output,'(?m)^CASE phase=(warm|measure) ambient=(\d+) gateSteps=4 failedSteps=5 jobsBefore=1 sdkEvents=2 sdkPools=1 jobWorkPinsUsers=0 initialShutdown=1460 allocationError=14 retry=1 joined=3 repairs=(0|1) detach=1 owners=0\r?$'))
        $warm=if($Control){2}else{4};$measured=if($Control){1}else{4};$count=$warm+$measured;$repair=if($Expected -eq 136){1}else{0};$errors=@(0,5,31,1816)
        $valid=$code -eq $Expected -and $summary.Success -and $rows.Count -eq $count
        if($summary.Success){$valid=$valid -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $measured -and [int]$summary.Groups[4].Value -eq 3*$count -and [int]$summary.Groups[5].Value -eq $repair*$count -and $summary.Groups[6].Value -eq $summary.Groups[7].Value}
        for($i=0;$i -lt $rows.Count;++$i){$row=$rows[$i];$phase=if($i -lt $warm){'warm'}else{'measure'};if($row.Groups[1].Value -cne $phase -or [int]$row.Groups[2].Value -ne $errors[$i%4] -or [int]$row.Groups[3].Value -ne $repair){$valid=$false}}
        if($Expected -eq 136 -and !$output.Contains('OPERATION_CONSTRUCTION_RESERVATION_NOT_RETURNED: actual caller returned OOM with one unowned reservation; original OperationUnreserve compensation and complete shutdown/thread/SDK/DLL cleanup precede136')){$valid=$false}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=$arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 136 -and $valid);log=$log;watchdog_ms=30000;
            schedules=$rows.Count;checked_warmup_schedules=$warm;measured_schedules=$measured;test_threads_joined=3*$rows.Count;negative_compensations=$repair*$rows.Count;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($valid){'running'}else{'failed'});if(!$valid){throw "Unexpected construction result:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-construction-unreserve')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-OperationConstructionTests -Directory (Join-Path $directory "$mode-$memory\$variant") -Mode $mode -Guarded:$guarded -Variant $variant
    Assert-OperationConstructionDependencies;Assert-OperationConstructionProjection $build
    if($variant -eq 'current'){Invoke-OperationConstructionProcess $build $false;Invoke-OperationConstructionProcess $build $true}else{Invoke-OperationConstructionProcess $build $true 136}
    $executables+=,$build
}}}
Assert-OperationConstructionDependencies
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during operation construction audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;unchanged_dependencies=$dependencies.Count;production_changed=$false;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\safety\operation_construction_tests.c','tools\build\operation-construction-tests.ps1','tools\audit\verify-operation-construction.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only small real-DLL construction/shutdown regression, with16 fixed accepted production/old-host dependencies and complete main.c/operation_runtime.h/legacy_operation_fixture snapshots. Independent projection permits only absolute includes, the selected operation include, old fixture HeapAlloc SDK-boundary routing, removal of the old250ms override to retain production120000ms, six original fixture assert tokens protected as always-active OC_CHECK under NDEBUG, and one detach ownership observer. Production assertions follow NDEBUG; PB_API_TESTS is rejected and shipping fingerprint must be65 bytes. The actual MainRunOperation has reserved one slot and created two real manual events plus one real min-zero private pool before reaching the first job HeapAlloc. The observer parks before its existing host allocation wrapper: exact Step4, Jobs1, idle unsignaled, no job/allocation/Work/module pin/result or actual runtime lease/heap/security initialization. A separately joined OS thread calls the actual fixture composite MainRuntimeBeginShutdown/MainOperationShutdown/MainRuntimeShutdown; initial shutdown0 correctly times out1460 while that construction reservation remains. Releasing the gate invokes the original allocator failure at Step5 with SDK error8; complete production MainRunOperation independently normalizes this branch to ERROR_OUTOFMEMORY14 after returning the reservation. The constructor OS thread is joined before observing jobless cleanup. A second actual shutdown OS thread then succeeds, and all3 test-owned thread handles/IDs are individually captured, waited and closed. Ordinary LoadLibrary remains owned until every public/fixture call has returned; actual DllMain closes both cached events and pool, checks all original SDK counters plus actual reservation/runtime owners zero, and returns before host module-absence and host-allocation/handle checks. No Work or callback is created and no private SDK pool-thread enumeration/join is claimed. Four ambient errors cross fresh DLL loads; full current warms4 and measures4 cases with strict process handle equality, selected136 warms2 and measures1. Negative136 removes only OperationUnreserve in the unique first-job allocation failure branch. After constructor/initial shutdown have joined, the ordinary loader and all cached SDK owners remain while the failure has no job/runtime owner but actual Jobs1/idle0. One call to the original OperationUnreserve compensates exactly once, then real shutdown retry, all OS joins, full actual detach and strict process-handle equality finish before136. Current never compensates, no counter is directly edited, and no production source is changed. Guarded mode covers only the host-owned intermodule config/snapshot spans; SDK pool/events and old host resources remain native. No driver/device/registry/network, actual old Windows runtime, kernel/IRQL/NDIS, native resource-exhaustion or CPU-performance result is claimed. Version remains2.0.0.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
