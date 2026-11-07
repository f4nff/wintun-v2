param([string]$BaselineDirectory='out\stop-reclaim-baseline\91ccf2ef652c',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\facade-backpressure-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Backpressure audit requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path $BaselineDirectory).Path
$frozen=@{'api\session.c'='6860c68f05159a0bc93046c1dd21d3520a1e936081bbc44ea77f89c0d984864b';'api\compat_worker.h'='08ad5597464f1e165b2bac2a1394d93c015480a241cf234b061d8eedfc38019d';
 'tests\safety\facade_stop_fixture.c'='42ed2b2b099bca8b6d5be8c8d6fb843ee529ad8be5549b0643899d813bc9fcc3';'tests\safety\native_policy_fixture.c'='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'}
foreach($path in $frozen.Keys){if((Get-FileHash (Join-Path $baseline $path)).Hash.ToLowerInvariant() -ne $frozen[$path]){throw "Frozen source changed:$path"}}
$dependencies=@(rg --files api/v2 common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate native dependencies'}
$dependencies+=@('api\session.c','api\compat_worker.h','api\compat_pump.h','api\termination_wait.h','api\adapter.h','api\logger.h','api\main.h','api\wintun.h',
 'include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h','tests\safety\facade_stop_fixture.c','tests\safety\native_policy_fixture.c',
 'tests\model\facade_stop_fixture.h','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){
    if($path.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $path}
    elseif((Get-FileHash (Join-Path $baseline $path)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $path)).Hash){throw "Unchanged dependency changed:$path"}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\facade-backpressure\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Project-Expected([string]$text,[string]$base){return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-BackpressureProjection($build){
    $e=$build.extraction;$api=Join-Path $script:RepoRoot 'api';$v2=Join-Path $api 'v2'
    $worker=[IO.File]::ReadAllText((Join-Path $baseline 'api\compat_worker.h'))
    if([IO.File]::ReadAllText($e.worker_original) -cne $worker -or (Get-FileHash $e.worker_original).Hash.ToLowerInvariant() -ne $frozen['api\compat_worker.h']){throw 'Original worker snapshot changed'}
    $before='            result = PbCompatWait(q, s->Wake, !pending.Count, INFINITE);';$after='            result = PbCompatWait(q, s->Wake, TRUE, INFINITE);'
    if([regex]::Matches($worker,[regex]::Escape($before)).Count -ne 1){throw 'Wait selection not unique'}
    if($build.variant -ne 'current'){
        if($build.mutation.original -cne $before -or $build.mutation.changed -cne $after -or $build.mutation.offset -ne $worker.IndexOf($before) -or $build.mutation.length -ne $before.Length){throw 'Negative mutation ledger changed'}
        $worker=$worker.Replace($before,$after)
    }elseif($null -ne $build.mutation){throw 'Current unexpectedly mutated'}
    if([IO.File]::ReadAllText($e.worker) -cne (Project-Expected $worker $api)){throw 'Worker changed beyond include projection and selected single negative'}
    $legacy=[IO.File]::ReadAllText((Join-Path $baseline 'api\session.c'))
    $legacy=[regex]::Replace($legacy,'(?m)^#include "([^"]+)"',{
        param($m);$path=if($m.Groups[1].Value -eq 'compat_worker.h'){$e.worker}else{[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'
    })
    if($legacy -cne [IO.File]::ReadAllText($e.legacy)){throw 'Legacy session changed beyond exact includes'}
    if((Project-Expected ([IO.File]::ReadAllText((Join-Path $baseline 'api\v2\session.c'))) $v2) -cne [IO.File]::ReadAllText($e.native)){throw 'Native session changed beyond includes'}
    $base=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\facade_stop_fixture.c'))
    if($e.base_source_sha256 -ne $frozen['tests\safety\facade_stop_fixture.c'] -or [IO.File]::ReadAllText($e.base_original) -cne $base -or
        (Get-FileHash $e.base_original).Hash.ToLowerInvariant() -ne $e.base_source_sha256 -or (Project-Expected $base (Split-Path $e.base_source)) -cne [IO.File]::ReadAllText($e.base)){throw 'Reused facade host changed beyond include paths'}
    $entry=@'
#undef DeviceIoControl
#define DeviceIoControl FacadeIo
#define WaitForMultipleObjects BackWait
#define PbQueueArm BackArm
#include PB_FACADE_NATIVE_SOURCE
#undef WaitForMultipleObjects
#undef PbQueueArm
'@
    if([IO.File]::ReadAllText($e.entry) -cne $entry -or !$build.ndebug -or $build.pb_api_tests){throw 'Shipping SDK observation entry/flags changed'}
    $lines=@('#define BP_FIXTURE 1');if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_FACADE_NATIVE_SOURCE "'+$e.native.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'
    $lines+='#define PB_FACADE_LEGACY_SOURCE "'+$e.legacy.Replace('\','/')+'"';$lines+='#define PB_BACKPRESSURE_BASE "'+$e.base.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\facade_backpressure_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Final source entry changed'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -ne $source.sha256){throw 'Source/artifact hash changed'}}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-BackpressureProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start backpressure test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Backpressure watchdog expired30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-facade-backpressure.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) facade backpressure cases, callback joins(\d+) ready cases(\d+), handles(\d+)/(\d+); NDEBUG real wait, legal slot generation reuse, three public copy returns, actual End5000 and unload')
        $full=$Arguments.Count -eq 1;$cases=if($full){8}else{3};$ready=if($Expected -eq 129){3}else{0}
        $marker='FACADE_BACKPRESSURE_ARMED_UNCONSUMABLE_READY: bounded readiness observer, actual public packet returns/End/close/joins/unload complete'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 129 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $cases -and [int]$summary.Groups[3].Value -eq $ready -and $summary.Groups[4].Value -eq $summary.Groups[5].Value}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 129 -and $correct);watchdog_ms=30000;log=$log;
            schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Backpressure failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-pending-wait-filter')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$built=Build-FacadeBackpressureTests -Directory (Join-Path $directory "$mode-$memory\$variant") -Mode $mode -Guarded:$guarded -Variant $variant
    Assert-BackpressureProjection $built;$executables+=,$built
    if($variant -eq 'current'){Invoke-BackpressureProcess $built.path @($built.fixture);Invoke-BackpressureProcess $built.path @($built.fixture,'129')}
    else{Invoke-BackpressureProcess $built.path @($built.fixture,'129') 129}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during backpressure audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;unchanged_dependencies=($dependencies.Count-1);operations=$script:TestResultFile;executables=$executables;
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;ndebug=$true;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    method='Coverage-only normal facade recovery. The new single C source builds host and fixture; complete frozen facade-stop/native-policy ledgers remain unchanged, projected only by exact include paths. Actual legacy session/pump/default PB_COMPAT_OPEN and non-PB_API_TESTS native session execute with NDEBUG, disabling only the production ValidOwnership debug walk; SAFETY_CHECK assertions remain active. Layout is64slots/128ring. A hosted producer publishes64 real descriptors. The actual worker copies one maximum old byte-ring packet, returns that token, retains63 pending loans, and selects actual SDK wait. Host pops the real recycle slot0/gen1 token then legally republishes slot0/gen2, leaving TX_READY unread without fake FULL or partial acceptance. Old API users receive/release copies0/1/2 before resuming actual waits, proving Wake-before-SDK entry persistence and continued copy/retirement across byte-ring wrap. Payloads65535/65531 and two initially empty byte cursor positions0/Capacity-4 form4checked warm plus4measured cases; selected129 uses2checked warm plus1measured. Current makes4 selected real SDK waits and never arms the unread unconsumable TX tail; actual armed bits/notifications and raw queue operations remain real. The imported slots1/2 retire normally; gen2slot0 remains native-owned/unimported. All issued old pointers are returned and every synchronous packet call ends before actual WintunEndSession begins. Its original Stopping publication is observed while worker remains hard-gated and therefore old ring storage is owned; only then release the final SDK/arm gate. Actual stopped pending return, unchanged Close5000, separate hosted NativeActive release, native/legacy worker completion, callback OS-thread joins and true DLL unload check every original owned-resource ledger. Negative129 changes only the worker receive argument !pending.Count to TRUE. After the actual TX arm returns ready1 with no local byte space, a bounded observation gate prevents a real spin loop; two such readiness decisions are observed, then identical actual public returns/End/normal cleanup completes before129. No owner/refcount/token repair or unknown SDK handle closure is performed. Every process requires strict post-warm handle equality. Guarded mode guards native core/slot/map objects using the original ledger; legacy rings retain VirtualAlloc and host configuration is ordinary live storage. Legacy runtime acquisition uses the inherited hosted ledger, not actual MainRuntime. Native producer/IOCTL/reference state are hosted, no actual SYS/NDIS/device/registry/Win7 runtime or kernel IRQL/CPU performance proof is claimed. SDK wait markers mean selected API entry, not opaque kernel parking. Host and fixture reject PB_API_TESTS, missing NDEBUG and nonshipping fingerprint layout. Version remains2.0.0. Historical policy dependency replay admits only the separately reviewed complete-file LSO/checksum configuration repair; all other source bindings and test paths retain their existing contracts.';
    audit_sources=@('tests\model\facade_backpressure_tests.c','tools\build\facade-backpressure-tests.ps1','tools\audit\verify-facade-backpressure.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|
    ConvertTo-Json -Depth 16|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
