param([string]$BaselineDirectory='out\retirement-retry-baseline\03bf04fcdbb3',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\cleanup-namespace-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Cleanup/namespace audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\retirement-retry-baseline\03bf04fcdbb3')){throw 'Unexpected cleanup/namespace baseline'}
$locked=@{
    'api\main.c'='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f';
    'api\namespace.c'='c8fe5ff67df4c1ddaebfde58385a7adef8831ff661c082ef8ef7e6f4a1252c97';
    'api\cleanup.h'='b02f11371520e0133726274703bb556fb77a88a23735e72f2b2939a566802421';
    'api\adapter.c'='e604c130f502dd453332236f7e02a95c54fbe3e3ceaa3df3e6092e4bed79215f';
    'api\logger.c'='e4e355f19691248274f36d831142a77266ff1ff876a039243ad33872e32ca525'
}
foreach($relative in $locked.Keys){if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant() -ne $locked[$relative]){throw "Frozen complete source changed: $relative"}}
$dependencies=@(rg --files api common include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate cleanup/namespace dependencies'};$dependencies+='tests\safety\guard_memory.h'
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{Assert-HelperDrainDependency -BaselineDirectory $baseline -Relative $relative}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\cleanup-namespace\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Normalize-Expected([string]$text,[string]$relative){$base=Split-Path (Join-Path $script:RepoRoot $relative);return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-CleanupNamespaceProjection($build){
    $p=@{};foreach($part in $build.parts){if($p.ContainsKey($part.name)){throw 'Duplicate cleanup/namespace generated part'};$p[$part.name]=$part;if((Get-FileHash -LiteralPath $part.path).Hash.ToLowerInvariant() -ne $part.sha256){throw 'Generated source hash changed'}}
    foreach($source in $build.sources){if($source.sha256 -ne $locked[$source.relative] -or (Get-FileHash -LiteralPath $source.path).Hash.ToLowerInvariant() -ne $source.sha256){throw 'Whole source binding changed'}}
    function RequirePart([string]$name,[string]$expected){if([IO.File]::ReadAllText($p[$name].path) -cne $expected){throw "Finite cleanup/namespace projection differs: $name"}}
    $main=[IO.File]::ReadAllText((Join-Path $baseline 'api\main.c'));RequirePart 'main-original.c' $main;RequirePart 'main-actual.h' (Normalize-Expected $main 'api\main.c')
    $ns=[IO.File]::ReadAllText((Join-Path $baseline 'api\namespace.c'));RequirePart 'namespace-original.c' $ns
    $tail='    if (PendingBoundary) DeleteBoundaryDescriptor(PendingBoundary);';if([regex]::Matches($ns,[regex]::Escape($tail)).Count -ne 1){throw 'Pending boundary failure deletion not unique'}
    if($build.variant -eq 'without-pending-boundary-delete'){$ns=$ns.Replace($tail,'    /* Observer128: omit only pending failure boundary deletion. */')}
    RequirePart 'namespace-actual.h' (Normalize-Expected $ns 'api\namespace.c')
    $scheduler=[IO.File]::ReadAllText((Join-Path $baseline 'api\cleanup.h'));RequirePart 'scheduler-actual.h' (Normalize-Expected $scheduler 'api\cleanup.h')
    $adapter=[IO.File]::ReadAllText((Join-Path $baseline 'api\adapter.c'));$a=$adapter.IndexOf('static VOID CleanupRun(VOID)');$b=$adapter.IndexOf('BOOL WINAPI WintunShutdown(DWORD Timeout)',$a);$c=$adapter.IndexOf('VOID AdapterCleanupDetach(VOID)',$b)
    RequirePart 'cleanup-run-actual.h' $adapter.Substring($a,$b-$a);RequirePart 'shutdown-actual.h' $adapter.Substring($b,$c-$b)
    if($p['cleanup-run-actual.h'].offset -ne $a -or $p['cleanup-run-actual.h'].length -ne $b-$a -or $p['shutdown-actual.h'].offset -ne $b -or $p['shutdown-actual.h'].length -ne $c-$b){throw 'Actual function extraction bounds changed'}
    if([regex]::Matches($adapter.Substring($a,$b-$a),'MainRuntimeRelease\(\)').Count -ne 1){throw 'Actual CleanupRun release boundary changed'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    foreach($pair in @(@('MAIN_SOURCE','main-actual.h'),@('SOURCE','namespace-actual.h'),@('SCHEDULER_SOURCE','scheduler-actual.h'),@('RUN_SOURCE','cleanup-run-actual.h'),@('SHUTDOWN_SOURCE','shutdown-actual.h'))){$lines+='#define PB_CLEANUP_NAMESPACE_'+$pair[0]+' "'+$p[$pair[1]].path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\cleanup_namespace_fixture.c').Replace('\','/')+'"';RequirePart 'fixture-entry.c' ($lines -join "`n")
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\cleanup_namespace_tests.c').Replace('\','/')+'"';RequirePart 'host-entry.c' ($lines -join "`n")
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-CleanupNamespaceProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start cleanup/namespace test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Cleanup/namespace watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-cleanup-namespace.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='CLEANUP_NAMESPACE_PENDING_BOUNDARY_NOT_DELETED: cancelled namespace unpublished and CS released but final boundary still owned; one actual delete per DLL before runtime-release gate, callback tail and actual unload complete'
        $summary=[regex]::Match($output,'PASS: (\d+) cleanup/namespace schedules, (\d+) pending cancellations, (\d+) cached successes, (\d+) actual boundary compensations;')
        $full=$Arguments.Count -eq 1;$count=if($full){32}else{3};$cancels=if($full){16}else{3};$cached=if($full){16}else{0};$repairs=if($Expected -eq 128){3}else{0}
        $handles=[regex]::Match($output,'PASS: cleanup/namespace strict warmup/measured handles (\d+)/(\d+);')
        $correct=$code -eq $Expected -and $summary.Success -and $handles.Success -and ($Expected -ne 128 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $cancels -and [int]$summary.Groups[3].Value -eq $cached -and [int]$summary.Groups[4].Value -eq $repairs}
        if($handles.Success){$correct=$correct -and $handles.Groups[1].Value -eq $handles.Groups[2].Value}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 128 -and $correct);watchdog_ms=30000;log=$log;
            schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unexpected cleanup/namespace result exit=$code expected=$Expected"}
    }finally{$process.Dispose()}
}
$artifacts=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-pending-boundary-delete')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $build=Build-CleanupNamespaceTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-CleanupNamespaceProjection $build;$artifacts+=$build
    if($variant -eq 'current'){Invoke-CleanupNamespaceProcess $build.path @($build.fixture);Invoke-CleanupNamespaceProcess $build.path @($build.fixture,'128')}
    else{Invoke-CleanupNamespaceProcess $build.path @($build.fixture,'128') 128}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during cleanup/namespace audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$locked;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=($dependencies.Count-2);operations=$script:TestResultFile;artifacts=$artifacts;
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;
    reviewed_dependency_repairs=@('api\rundll32_process.h','common/offload_policy.c');helper_drain_ledger=@{path='tools/audit/helper-drain-ledger.ps1';sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    method='Coverage-only regression. Fixed whole main.c/namespace.c/cleanup.h/adapter.c/logger.c hashes; production/guard dependencies preserve original byte contracts except the reviewed api/rundll32_process.h post-exit drain repair and common/offload_policy.c LSO/checksum repair, each admitted only by its separate strict complete-file ledger. Historical controls and existing finite projections are retained. Complete main and namespace bodies and actual scheduler are projected only by normalizing local include paths, with exact complete CleanupRun/WintunShutdown regions extracted under offsets/lengths. No old fixture is reused or edited. Actual main/runtime/InitOnce/security initialization, namespace critical section/SRW/SID/boundary descriptor, callback scheduler, real default TrySubmitThreadpoolCallback/CallbackMayRunLong and module pin/tail, and exact Shutdown cancellation execute. Native CreateBoundaryDescriptor/AddSID/Delete use the latest real SDK pointer. Namespace naming SDK calls use one unnamed event; installation mutex names use a real unnamed mutex with actual MainWaitForObject; installer scan bodies are hosted and the success orphan stub reuses the actual cached namespace path. No project namespace/device/registry is created or queried. Pending namespace SDK success is gated before actual cache publication; Shutdown0 sets real runtime/cleanup stops, returns timeout and retains actual Users1 and pending owners. SDK gate release makes real NamespaceRuntimeInit observe stop, close pending namespace and delete boundary, leaving cache empty, CS available, TakeError1223 and no install scans. A transparent wrapper maps only the extracted CleanupRun last release call and preserves its thread error; the actual main function/counters are unchanged. After every public/fixture call returns, ordinary LoadLibrary can drop while the native callback tail pin protects remaining actual release/unload/return and actual DLL detach. Success case retains one cached namespace/boundary after actual Users0 and genuine advapi32 unload, then actual NamespaceDone deletes them at detach. The actual default global SDK pool owns its OS threads; no test thread handle is opened, and no claim of joining/exiting SDK worker threads is made. Callback return, native FreeLibraryWhenCallbackReturns, actual DLL unload and zero owned-resource ledgers are separate witnesses. Guarded mode protects caller-owned intermodule config/snapshot buffers; SDK security/boundary allocations retain real allocator contracts. Full32 uses16checked warmup+16measured pending/cached cases; CLI128 uses2checked pending warmup+1measured, all retain strict process handle equality. Observer128 omits only namespace pending-failure Delete, never cached NamespaceDone deletion: at last-release gate actual Users1, CS released, pending namespace closed/cache0 but one real final boundary remains; ordinary loader still owns exactly one actual Delete compensation per fresh DLL before actual release/callback-tail/unload. Current never compensates. All observations are hosted fault/ownership coverage, not a current production defect, old Windows execution, true private-namespace security, NDIS/IRQL or CPU measurement. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.'}|
    ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
