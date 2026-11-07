param([string]$BaselineDirectory='out\retire-edge-baseline\d1dc19684d05',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\helper-drain-tests.ps1')
. (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Helper drain requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$accepted='d1dc19684d050d0acece95c8d29204dc0c9350fca6c8af7f314c5d75331c461f'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected helper drain accepted baseline'}
$dependencies=@('api\main.c','api\main.h','api\operation_runtime.h','api\operation.h','api\logger.h','api\adapter.h','api\namespace.h','api\registry.h',
    'api\ntdll.h','api\rundll32.h','api\wintun.h','common\system_library.h','include\packetbridge_identity.h','include\packetbridge_version.h',
    'tests\safety\legacy_runtime_fixture.c','tests\safety\guard_memory.h','api\rundll32.c','api\rundll32_process.h','api\resource.h',
    'api\overlapped_wait.h','api\termination_wait.h','api\event_pair.h','include\packetbridge_helper_protocol.h','api\logger.c')
function Assert-HelperDrainInputs {
    foreach($path in $dependencies){
        if((Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant() -cne $manifest.files.($path.Replace('\','/'))){throw "Frozen helper input changed:$path"}
        Assert-HelperDrainDependency -BaselineDirectory $baseline -Relative $path
    }
}
Assert-HelperDrainInputs
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\helper-drain\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
$contracts=Test-HelperDrainLedger -Directory (Join-Path $directory 'ledger-contracts') -Original ([IO.File]::ReadAllText((Join-Path $baseline 'api\rundll32_process.h'))) -Repaired ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\rundll32_process.h')))
if($contracts.checks.Count -ne 16 -or $contracts.files.Count -ne 6 -or @($contracts.checks|Where-Object {!$_.passed}).Count){throw 'Incomplete helper drain ledger contracts'}
Start-TestRecording $directory 'operation-results.json'
function Get-HelperDrainPart($build,[string]$name){$found=@($build.parts|Where-Object {$_.name -ceq $name});if($found.Count -ne 1){throw "Nonunique helper output:$name"};return $found[0]}
function Project-HelperDrainAuditIncludes([string]$body,[string]$relative){
    $base=Split-Path (Join-Path $script:RepoRoot $relative)
    return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
}
function Replace-HelperDrainAuditOnce([string]$body,[string]$before,[string]$after){
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw "Nonunique audited helper boundary:$before"};return $body.Replace($before,$after)
}
function Assert-HelperDrainProjection($build){
    if($build.sources.Count -ne 24 -or @($build.sources.relative|Select-Object -Unique).Count -ne 24 -or $build.parts.Count -ne 11 -or @($build.parts.name|Select-Object -Unique).Count -ne 11){throw 'Helper drain source/output set changed'}
    if(@(Compare-Object ($build.sources.relative|Sort-Object) ($dependencies|Sort-Object)).Count){throw 'Helper drain dependency set changed'}
    foreach($source in $build.sources){if($source.path -cne (Join-Path $script:RepoRoot $source.relative) -or $source.sha256 -cne (Get-FileHash -LiteralPath $source.path).Hash.ToLowerInvariant()){throw 'Helper drain source binding/hash changed'}}
    foreach($part in $build.parts){if((Get-FileHash -LiteralPath $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw 'Helper drain output hash changed'}}
    foreach($pair in @(@('main-original.c','api\main.c'),@('rundll-original.c','api\rundll32.c'),@('fixture-original.c','tests\safety\legacy_runtime_fixture.c'),@('helper-before.h','api\rundll32_process.h'))){
        if((Get-HelperDrainPart $build $pair[0]).sha256 -cne (Get-FileHash -LiteralPath (Join-Path $baseline $pair[1])).Hash.ToLowerInvariant()){throw 'Whole original helper source changed'}
    }
    $before=Get-HelperDrainPart $build 'helper-before.h';$current=Get-HelperDrainPart $build 'helper-current.h'
    Assert-HelperDrainFilePair -Before $before.path -Current $current.path -Relative 'api/rundll32_process.h'
    $selected=if($build.variant -ceq 'before'){$before}elseif($build.variant -ceq 'current'){$current}else{throw 'Unexpected helper drain variant'}
    $helper=Get-HelperDrainPart $build 'helper-selected.h';$main=Get-HelperDrainPart $build 'main-selected.h';$rundll=Get-HelperDrainPart $build 'rundll-selected.h';$fixture=Get-HelperDrainPart $build 'fixture-selected.h'
    if([IO.File]::ReadAllText($helper.path) -cne (Project-HelperDrainAuditIncludes ([IO.File]::ReadAllText($selected.path)) 'api\rundll32_process.h')){throw 'Whole selected helper changed beyond absolute includes'}
    if([IO.File]::ReadAllText($main.path) -cne (Project-HelperDrainAuditIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api\main.c'))) 'api\main.c')){throw 'Whole main projection changed'}
    $expected=Project-HelperDrainAuditIncludes ([IO.File]::ReadAllText((Join-Path $baseline 'api\rundll32.c'))) 'api\rundll32.c'
    $expected=Replace-HelperDrainAuditOnce $expected ('#include "'+(Join-Path $script:RepoRoot 'api\rundll32_process.h').Replace('\','/')+'"') ('#include "'+$helper.path.Replace('\','/')+'"')
    if([IO.File]::ReadAllText($rundll.path) -cne $expected){throw 'Whole rundll client projection changed'}
    $expected=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\legacy_runtime_fixture.c'))
    $expected=Replace-HelperDrainAuditOnce $expected '#define RUN_DLL32_TIMEOUT 2000u' '/* Retain production120000ms default; no public helper waiter is invoked. */'
    $expected=Replace-HelperDrainAuditOnce $expected 'if (Detached) SetEvent(Detached);' 'DrainFinalCheck(); if (Detached) SetEvent(Detached);'
    if([regex]::Matches($expected,'\bassert\(').Count -ne 4 -or $build.fixture_assert_tokens -ne 4){throw 'Original helper fixture assertions changed'}
    $expected=[regex]::Replace($expected,'\bassert\(','HD_CHECK(')
    if([IO.File]::ReadAllText($fixture.path) -cne $expected){throw 'Whole frozen runtime host differs outside finite SDK adaptation'}
    $test=Join-Path $script:RepoRoot 'tests\safety\helper_drain_tests.c'
    if($build.test -cne $test -or $build.test_sha256 -cne (Get-FileHash $test).Hash.ToLowerInvariant()){throw 'Helper drain test binding changed'}
    $shared=@();if($build.guarded){$shared+='#define PB_GUARD_TESTS 1'};if($build.variant -eq 'before'){$shared+='#define PB_HELPER_DRAIN_BEFORE 1'}
    $lines=$shared+@('#define PB_HELPER_DRAIN_FIXTURE 1')
    foreach($pair in @(@('PB_LEGACY_RUNTIME_MAIN_SOURCE',$main.path),@('PB_LEGACY_HELPER_SOURCE',$rundll.path),@('PB_HELPER_DRAIN_SOURCE',$fixture.path))){$lines+='#define '+$pair[0]+' "'+$pair[1].Replace('\','/')+'"'}
    $lines+='#include "'+$test.Replace('\','/')+'"'
    if([IO.File]::ReadAllText((Get-HelperDrainPart $build 'fixture-entry.c').path) -cne (($lines -join "`n")+"`n")){throw 'Helper drain fixture entry changed'}
    $lines=$shared+@('#include "'+$test.Replace('\','/')+'"')
    if([IO.File]::ReadAllText((Get-HelperDrainPart $build 'host-entry.c').path) -cne (($lines -join "`n")+"`n")){throw 'Helper drain host entry changed'}
    if(!$build.ndebug -or $build.pb_api_tests -or !$build.maybe_wow64_source -or '/DNDEBUG' -notin $build.arguments -or '/DMAYBE_WOW64=1' -notin $build.arguments -or "/volatile:$($build.mode)" -notin $build.arguments -or "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint" -notin $build.arguments -or @($build.arguments|Where-Object {$_ -match 'PB_API_TESTS'}).Count){throw 'Helper drain shipping/source-model flags changed'}
}
function Get-HelperDrainRows([bool]$Control,[string]$Phase){
    if($Control){return @{stage=2;stream=1;middle=0;error=1816;prior=0;expected=1816;phase=$Phase}}
    $policies=@(@{stage=1;errors=@(1816,109)},@{stage=2;errors=@(1816,995)},@{stage=3;errors=@(1816,995)})
    $classification=@{1816=1816;109=0;995=0;0=31}
    foreach($policy in $policies){foreach($stream in @(0,1)){foreach($middle in @(0,1)){foreach($prior in @(0,8)){foreach($error in $policy.errors){
        @{stage=$policy.stage;stream=$stream;middle=$middle;error=$error;prior=$prior;expected=$(if($prior){$prior}else{$classification[$error]});phase=$Phase}
    }}}}}
    foreach($stage in @(1,2,3)){foreach($stream in @(0,1)){@{stage=$stage;stream=$stream;middle=0;error=0;prior=0;expected=31;phase=$Phase}}}
    foreach($stage in @(4,5,6,7)){foreach($stream in @(0,1)){@{stage=$stage;stream=$stream;middle=0;error=0;prior=0;expected=0;phase=$Phase}}}
}
function Invoke-HelperDrainProcess($build,[bool]$Control,[int]$Expected=0){
    $arguments=@($build.fixture);if($Control){$arguments+='137'}
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start helper drain test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Helper drain watchdog30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-helper-drain.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,$encoding)
        $summary=[regex]::Match($output,'PASS: (\d+) helper drain cases warm(\d+) measure(\d+); canonical error/EOF and terminal native I/O before close, cached heap/SDDL freed at actual DLL detach; lost(\d+) handles(\d+)/(\d+)')
        $rows=@([regex]::Matches($output,'(?m)^CASE phase=(warm|measure) stage=(\d+) stream=(\d+) middle=(\d+) error=(\d+) prior=(\d+) observed=(\d+) expected=(\d+) owners=0 dll=unloaded\r?$'))
        $warm=if($Control){2}else{62};$measured=if($Control){1}else{62};$count=$warm+$measured;$lost=if($Expected -eq 137){$count}else{0}
        $expectedRows=@();if($Control){$expectedRows+=@(Get-HelperDrainRows $true 'warm');$expectedRows+=@(Get-HelperDrainRows $true 'warm')}else{$expectedRows+=@(Get-HelperDrainRows $false 'warm')};$expectedRows+=@(Get-HelperDrainRows $Control 'measure')
        $valid=$code -eq $Expected -and $summary.Success -and $rows.Count -eq $count -and $expectedRows.Count -eq $count
        if($summary.Success){$valid=$valid -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $measured -and [int]$summary.Groups[4].Value -eq $lost -and $summary.Groups[5].Value -eq $summary.Groups[6].Value}
        for($i=0;$i -lt $rows.Count;++$i){$actual=$rows[$i];$want=$expectedRows[$i];$observed=if($Expected -eq 137){0}else{$want.expected}
            if($actual.Groups[1].Value -cne $want.phase -or [int]$actual.Groups[2].Value -ne $want.stage -or [int]$actual.Groups[3].Value -ne $want.stream -or [int]$actual.Groups[4].Value -ne $want.middle -or [int]$actual.Groups[5].Value -ne $want.error -or [int]$actual.Groups[6].Value -ne $want.prior -or [int]$actual.Groups[7].Value -ne $observed -or [int]$actual.Groups[8].Value -ne $want.expected){$valid=$false}
        }
        if($Expected -eq 137 -and !$output.Contains('HELPER_POST_EXIT_DRAIN_ERROR_NOT_REPORTED: complete valid stdout, quota1816 lost from second stream; real I/O/handles/runtime cleanup and true DLL unload precede137')){$valid=$false}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=$arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 137 -and $valid);log=$log;watchdog_ms=30000;
            schedules=$rows.Count;checked_warmup_schedules=$warm;measured_schedules=$measured;lost_drain_errors=$lost;test_threads_created=0;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($valid){'running'}else{'failed'});Write-Output $summary.Value;if(!$valid){throw "Unexpected helper drain result:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','before')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-HelperDrainTests -Directory (Join-Path $directory "$mode-$memory\$variant") -Mode $mode -Guarded:$guarded -Variant $variant -BaselineDirectory $baseline
    Assert-HelperDrainInputs;Assert-HelperDrainProjection $build
    if($variant -eq 'current'){Invoke-HelperDrainProcess $build $false;Invoke-HelperDrainProcess $build $true}else{Invoke-HelperDrainProcess $build $true 137}
    $executables+=,$build
}}}
Assert-HelperDrainInputs
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during helper drain audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;unchanged_dependencies=$dependencies.Count-1;reviewed_dependency_repairs=@('api/rundll32_process.h');production_changed=$true;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    helper_drain_ledger=@{path='tools/audit/helper-drain-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')).Hash.ToLowerInvariant();checks=$contracts.checks;files=$contracts.files};
    audit_sources=@('tests\safety\helper_drain_tests.c','tools\build\helper-drain-tests.ps1','tools\audit\verify-helper-drain.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Actual complete helper/header, parent client and main/runtime share the fixed original legacy runtime SDK fixture. Whole24 source/header hashes and11 raw/selected/entry outputs are bound; independent projection allows only absolute quoted includes, explicit existing source-selection macros, removal of the original2000ms helper override to keep production120000ms, four old fixture assert tokens strengthened to always-active HD_CHECK under NDEBUG, and one detach ownership observer. The shared ledger independently constrains complete old7f57 and repaired0fea raw bytes/text and runs16 positive/negative contracts with6 hashed encoding files. Shipping fingerprint must be65 bytes and PB_API_TESTS is rejected. MAYBE_WOW64 is deliberately compiled only to execute the retained private helper source; current shipping x64 does not enable that macro. Real byte-mode named pipes, manual events, normal overlapped reads, runtime heap/security initialization, true delay-import release and actual DLL detach/unload execute. Real stdout is already a complete status0 before selected stderr quota failure; full stdout cases instead use bounded create-response length models, not claims about the fixed Win7 generator. Immediate ReadFile quota1816 is a documented legitimate buffer-page-lock failure. Other SDK errors, Peek failures and zero GLE are explicit boundary/fault models; failed API output is never assumed valid. Pending cases issue a real read and witness its native terminal completion before reporting a permitted pending-submission race, then replace only the observed terminal status at the SDK query boundary. They do not claim native page-lock failure after a successful read. Separate initial-pending cases use real CancelIoEx/terminal GetOverlappedResult, and native broken writer/zero-available/ordinary success remain covered. Middle cases limit the first native read request to a valid finite fragment; every returned byte/count is from actual ReadFile, with nonNULL Count preserving Win7. First errors come from actual ordinary HelperPump failure, then first/middle Peek/immediate/pending drain errors across both streams must retain them. Only BROKEN_PIPE/OPERATION_ABORTED remain normal EOF, and zero error failures normalize31. Every real submitted read is terminal and Pending false before actual close; original tracked handle/allocation/work counters reach0 after both streams. The real runtime still owns its cached heap1 and two native SDDL blocks2 until actual DllMain destroys/frees them; separate native Heap/Local owner ledgers verify zero at detach, and ordinary loader remains held until the fixture call returns. No job/thread/work/installer process is submitted; no SDK-thread join or process-exit scheduling claim is made. DrainTRUE is the hosted own-process-exit input boundary, not an actual installer run. Current full62 warm plus62 measured cases and selected137 two warm plusone measured require strict process handle equality. Frozen whole-before selected137 observes lost quota1816 with Job.Error0 only after real stream/runtime cleanup and actual DLL unload; it performs no counter/ownership repair and reports137 after3 safe cases. Four ISO/MS ordinary/guarded modes execute12 processes. Guarded mode covers only caller-owned mutable intermodule HD_CASE; SDK/old-host allocations retain native allocators. No real device/registry/driver/network, native Windows7 runtime, kernel/IRQL/NDIS or CPU result is claimed. Version remains2.0.0.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
