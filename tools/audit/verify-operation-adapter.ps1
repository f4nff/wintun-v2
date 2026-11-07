param([string]$BaselineDirectory='out\stop-progress-baseline\124a7befbedb',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\operation-adapter-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Operation/adapter audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\stop-progress-baseline\124a7befbedb')){throw 'Unexpected operation/adapter accepted baseline'}
$locked=@{
    'api\main.c'='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f';
    'api\adapter.c'='e604c130f502dd453332236f7e02a95c54fbe3e3ceaa3df3e6092e4bed79215f';
    'api\operation_runtime.h'='10a9d55a690c29c4128af45960be07e52ede302af051e6237cd37d267ea3ef8a';
    'tests\safety\adapter_open_tests.c'='37b8c06e4152f2b05e7acd6809a07c432a595e7ccd3319d17793f7817b39ce52';
    'tests\safety\legacy_operation_fixture.c'='ad43448e89325c21a558a022ffd9b61f85519051dddcad00ea46988200274812'
}
foreach($relative in $locked.Keys){if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant() -ne $locked[$relative]){throw "Frozen whole source changed: $relative"}}
$dependencies=@(rg --files api common include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate operation/adapter dependencies'}
$dependencies+=@('tests\safety\adapter_open_tests.c','tests\safety\legacy_operation_fixture.c','tests\safety\guard_memory.h')
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{Assert-HelperDrainDependency -BaselineDirectory $baseline -Relative $relative}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\operation-adapter\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Normalize-Expected([string]$text,[string]$relative){$base=Split-Path (Join-Path $script:RepoRoot $relative);return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Remove-Expected([string]$text,[string]$first,[string]$next){$a=$text.IndexOf($first);$b=$text.IndexOf($next,$a);if($a -lt 0 -or $b -le $a){throw 'Missing independent host region'};return $text.Substring(0,$a)+$text.Substring($b)}
function Assert-OperationAdapterProjection($build){
    $p=@{};foreach($part in $build.parts){if($p.ContainsKey($part.name)){throw 'Duplicate operation/adapter part'};$p[$part.name]=$part;if((Get-FileHash -LiteralPath $part.path).Hash.ToLowerInvariant() -ne $part.sha256){throw 'Generated part hash changed'}}
    foreach($source in $build.sources){if((Get-FileHash -LiteralPath $source.path).Hash.ToLowerInvariant() -ne $source.sha256 -or $source.sha256 -ne $locked[$source.relative]){throw 'Whole source binding changed'}}
    function RequirePart([string]$name,[string]$expected){if([IO.File]::ReadAllText($p[$name].path) -cne $expected){throw "Operation/adapter finite projection differs: $name"}}
    $adapter=[IO.File]::ReadAllText((Join-Path $baseline 'api\adapter.c'));$name=$adapter.IndexOf('OpenAdapterInternal(LPCWSTR Name)');$a=$adapter.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$name);$b=$adapter.IndexOf('typedef struct ADAPTER_OPERATION_INPUT',$name)
    RequirePart 'open-actual.h' $adapter.Substring($a,$b-$a);if($p['open-actual.h'].offset -ne $a -or $p['open-actual.h'].length -ne $b-$a){throw 'Open extraction bounds changed'}
    $name=[regex]::Match($adapter,'VOID WINAPI\r?\nWintunCloseAdapter\(').Index;$a=$adapter.LastIndexOf('_Use_decl_annotations_',$name);$b=$adapter.IndexOf('#include "adapter_rename.h"',$name)
    RequirePart 'close-actual.h' $adapter.Substring($a,$b-$a);if($p['close-actual.h'].offset -ne $a -or $p['close-actual.h'].length -ne $b-$a){throw 'Close extraction bounds changed'}
    $a=$adapter.IndexOf('typedef struct ADAPTER_OPERATION_INPUT');$name=$adapter.IndexOf('AdapterRemoveInstance(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData)',$a);$b=$adapter.LastIndexOf('_Use_decl_annotations_',$name)
    RequirePart 'public-actual.h' (Normalize-Expected $adapter.Substring($a,$b-$a) 'api\adapter.c');if($p['public-actual.h'].offset -ne $a -or $p['public-actual.h'].length -ne $b-$a){throw 'Public wrappers extraction bounds changed'}
    $op=[IO.File]::ReadAllText((Join-Path $baseline 'api\operation_runtime.h'));RequirePart 'operation-original.h' $op
    if($build.variant -eq 'without-late-dispose'){$tail='    if (!claimed && result && job->Dispose) job->Dispose(result);';if([regex]::Matches($op,[regex]::Escape($tail)).Count -ne 1){throw 'Unclaimed Dispose not unique'};$op=$op.Replace($tail,'    /* Observer125: omit only unclaimed successful-result Dispose. */')}
    RequirePart 'operation-actual.h' $op
    RequirePart 'operation-entry.h' ("#define MainRuntimeRelease() ObserveOperationRelease(job)`n#include `""+$p['operation-actual.h'].path.Replace('\','/')+"`"`n#undef MainRuntimeRelease`n")
    $main=[IO.File]::ReadAllText((Join-Path $baseline 'api\main.c'));RequirePart 'main-original.c' $main;$main=Normalize-Expected $main 'api\main.c'
    $main=$main.Replace('#include "'+(Join-Path $script:RepoRoot 'api\operation_runtime.h').Replace('\','/')+'"','#include "'+$p['operation-entry.h'].path.Replace('\','/')+'"');RequirePart 'main-actual.h' $main
    $openHost=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\adapter_open_tests.c'))
    $openHost=$openHost.Replace('HANDLE ModuleHeap;','/* Actual main owns ModuleHeap. */').Replace('BOOL IsWindows7;','/* Actual EnvInit owns IsWindows7. */').Replace('Closes,Leases;','Closes;')
    $openHost=Remove-Expected $openHost 'BOOL MainRuntimeAcquire(VOID)' 'HANDLE NamespaceTakeDeviceInstallationMutex(VOID)'
    $openHost=Remove-Expected $openHost 'BOOL MainOperationSleep(DWORD delay)' 'static HDEVINFO WINAPI TestSet('
    $openHost=Remove-Expected $openHost 'VOID WINAPI WintunCloseAdapter(' 'static VOID RenderLog('
    $openHost=$openHost.Replace('Blocks[i]=(BLOCK){0};--Live;++Frees;SetLastError(ERROR_INVALID_HANDLE);return TRUE;','Blocks[i]=(BLOCK){0};--Live;++Frees;if(memory==LiveAdapter){LiveAdapter=NULL;++Closes;}SetLastError(ERROR_INVALID_HANDLE);return TRUE;')
    $openHost=$openHost.Replace('CHECK(Leases==1&&!InstallMutex);','CHECK(Leases==2&&!InstallMutex);').Replace('&&Leases==2&&!DeviceSet);','&&Leases==3&&!DeviceSet);')
    foreach($pair in @(@('Fail','OpenHostFail'),@('Run','OpenHostRun'),@('Allocations','LegacyAllocations'),@('Frees','LegacyFrees'),@('PopulateAdapterData','HostedPopulate'))){$openHost=[regex]::Replace($openHost,'\b'+$pair[0]+'\b',$pair[1])}
    RequirePart 'open-host.h' (Normalize-Expected $openHost 'tests\safety\adapter_open_tests.c')
    $hosted=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\legacy_operation_fixture.c'))
    $hosted=$hosted.Replace('#define MAIN_OPERATION_TIMEOUT 250u','/* Production 120000ms default retained; actual stop broadcast cancels. */')
    $hosted=$hosted.Replace('{ assert(Pins>0); InterlockedDecrement(&Pins); FreeLibraryWhenCallbackReturns(instance,module); }','{ assert(Pins>0); ObserveCallbackThread(); InterlockedDecrement(&Pins); FreeLibraryWhenCallbackReturns(instance,module); }')
    $hosted=$hosted.Replace('if(Detached) SetEvent(Detached);','OperationAdapterFinalCheck(); if(Detached) SetEvent(Detached);');$hosted=Normalize-Expected $hosted 'tests\safety\legacy_operation_fixture.c'
    $hosted=$hosted.Replace('#include "'+(Join-Path $script:RepoRoot 'api\main.c').Replace('\','/')+'"','#include "'+$p['main-actual.h'].path.Replace('\','/')+'"');RequirePart 'operation-host.h' $hosted
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    foreach($pair in @(@('OPEN_SOURCE','open-actual.h'),@('OPEN_HOST','open-host.h'),@('OPERATION_HOST','operation-host.h'),@('CLOSE_SOURCE','close-actual.h'),@('PUBLIC_SOURCE','public-actual.h'))){$lines+='#define PB_OPERATION_ADAPTER_'+$pair[0]+' "'+$p[$pair[1]].path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\operation_adapter_fixture.c').Replace('\','/')+'"';RequirePart 'fixture-entry.c' ($lines -join "`n")
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\operation_adapter_tests.c').Replace('\','/')+'"';RequirePart 'host-entry.c' ($lines -join "`n")
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-OperationAdapterProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start operation/adapter test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Operation/adapter watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-operation-adapter.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='OPERATION_LATE_ADAPTER_NOT_DISPOSED: abandoned actual Open left RuntimeOwned adapter; one real Close compensation while loader owned, final operation release/unload, all joins and DLL detach completed'
        $summary=[regex]::Match($output,'PASS: (\d+) operation/adapter schedules, (\d+) public caller joins, (\d+) callback-thread joins, (\d+) actual Close compensations;')
        $count=if($Arguments.Count -eq 1){32}else{1};$repairs=if($Expected -eq 125){1}else{0}
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 125 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $count -and [int]$summary.Groups[3].Value -eq $count -and [int]$summary.Groups[4].Value -eq $repairs}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 125 -and $correct);watchdog_ms=30000;log=$log;
            schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unexpected operation/adapter result exit=$code expected=$Expected"}
    }finally{$process.Dispose()}
}
$artifacts=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-late-dispose')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $build=Build-OperationAdapterTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-OperationAdapterProjection $build;$artifacts+=$build
    if($variant -eq 'current'){Invoke-OperationAdapterProcess $build.path @($build.fixture);Invoke-OperationAdapterProcess $build.path @($build.fixture,'125')}
    else{Invoke-OperationAdapterProcess $build.path @($build.fixture,'125') 125}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during operation/adapter audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$locked;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=($dependencies.Count-2);operations=$script:TestResultFile;artifacts=$artifacts;
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;
    reviewed_dependency_repairs=@('api\rundll32_process.h','common/offload_policy.c');helper_drain_ledger=@{path='tools/audit/helper-drain-ledger.ps1';sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    method='Coverage-only regression. Fixed whole main.c/operation_runtime.h/adapter.c and two reused fixture hashes; production dependencies preserve original byte contracts except the reviewed api/rundll32_process.h post-exit drain repair and common/offload_policy.c LSO/checksum repair, each admitted only by its separate strict complete-file ledger. Historical controls and existing finite projections are retained. Complete OpenAdapterInternal, Close and public RunAdapterOperation/DisposeAdapterOperation/Open wrappers are extracted with offsets/lengths; main.c and operation_runtime.h remain complete. Independent projection checks allow only local include paths, one main operation include entry and the negative unclaimed-result Dispose omission. Hash-locked Open SDK host loses fake runtime functions/sleep/Close and duplicate state; its Leases assertions alias actual DependencyUsers with only two executor-nesting expectation changes, adapter heap-free observer, populate symbol proxy and collision renames. Old operation fixture retains its original real allocator/events/token/pool/work/module tracking, removes only its shortened250ms override so production120000ms default applies, observes actual callback thread before native tail-pin transfer and adds actual detach SDK/adapter/dependency checks. Discovery/metadata are hosted with real event-backed SDK set and real unnamed installation mutex, not device creation/registry/installer. Actual WintunOpenAdapter executes MainRunOperation; metadata has genuinely succeeded in its SDK host before a final boundary gate. Explicit actual MainRuntimeBeginShutdown and MainOperationShutdown broadcast cancel the caller with ERROR_CANCELLED, not a manufactured timeout. Public caller is joined while actual job, two native heap blocks, SDK set/mutex and three actual runtime leases remain. SDK gate release lets actual Open succeed late; actual worker State remains Abandoned/References1, SDK set/mutex close, actual Dispose calls WintunCloseAdapter and leaves only the operation lease. Transparent include wrapper gates the single actual worker-tail release call, never edits counts or job state. Only after public/fixture calls return does host release ordinary LoadLibrary; actual callback tail pin alone protects remaining release/unreserve/drop, genuine advapi32 dependency unload, job/event/work/pool/privateheap/security teardown and actual aggregate DllDetach. Caller and real callback OS-thread witness handles are all joined/closed; full current workload warms16 then measures16 with exact process handle equality. Guarded mode protects legacy adapter allocations and caller-owned intermodule config/snapshot buffers; real security/job resources retain their original allocator contracts. Negative125 omits only actual unclaimed-success Dispose: at operation-release gate SDK owners are gone but RuntimeOwned adapter/path remain with actual Users2; ordinary loader still owns one true WintunCloseAdapter compensation, bringing Users1/nativeblocks0 before final worker release. All joins/unload/detach precede125; current never compensates. No real devices/registry/driver, old Windows runtime, NDIS/IRQL/security-device or kernel CPU execution claim. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.'}|
    ConvertTo-Json -Depth 17|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
