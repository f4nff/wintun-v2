param([string]$BaselineDirectory='out\return-stop-baseline\e6c767571bed',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\worker-start-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Worker startup requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate worker dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\safety\guard_memory.h','tests\model\register_owner_tests.c','tools\build\kernel-register-tests.ps1')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\worker-start\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-WorkerStartProjection($build) {
    $e=$build.extractions
    if(@($e.parts).Count -ne 3 -or @($e.parts.name|Select-Object -Unique).Count -ne 3){throw 'Unexpected production part set'}
    foreach($part in $e.parts){
        if([IO.File]::ReadAllText($part.path) -cne [IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length) -or
            (Get-FileHash $part.path).Hash.ToLowerInvariant() -ne $part.sha256 -or (Get-FileHash $part.source).Hash.ToLowerInvariant() -ne $part.source_sha256){throw "Actual production extraction changed:$($part.name)"}
    }
    $registration=$e.parts|Where-Object name -eq 'register';$source=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session.h'))
    $first='static NTSTATUS PbV2Register(TUN_CTX *ctx, IRP *irp)';$next='#include "session_unregister.h"';$start=$source.IndexOf($first);$end=$source.IndexOf($next,$start)
    if($registration.offset -ne $start -or $registration.length -ne $end-$start -or [IO.File]::ReadAllText($registration.path) -cne $source.Substring($start,$end-$start) -or $e.register.variant -ne 'current' -or
        [IO.File]::ReadAllText($e.register.header) -cne ("#pragma once`r`n"+$source.Substring($start,$end-$start))){throw 'Actual Register function/header changed'}
    foreach($name in @('worker','cleanup')){
        $part=$e.parts|Where-Object name -eq $name;$path=Join-Path $script:RepoRoot "driver\v2\session_$name.h"
        if($part.source -cne $path -or $part.offset -ne 0 -or $part.length -ne [IO.File]::ReadAllText($path).Length){throw "Entire production header not selected:$name"}
    }
    $cleanup=$e.parts|Where-Object name -eq 'cleanup';$selected=[IO.File]::ReadAllText($cleanup.path)
    $before='KeSetEvent(&s->StartEvent, IO_NO_INCREMENT, FALSE);';$after='/* Test mutation: rollback StartEvent wake omitted. */'
    if([regex]::Matches($selected,[regex]::Escape($before)).Count -ne 1){throw 'Free Start wake not unique'}
    if($build.variant -ne 'current'){
        if($e.mutation.original -cne $before -or $e.mutation.changed -cne $after -or $e.mutation.offset -ne $selected.IndexOf($before) -or $e.mutation.length -ne $before.Length){throw 'Free-only Start wake mutation metadata changed'}
        $selected=$selected.Replace($before,$after)
    }elseif($null -ne $e.mutation){throw 'Current Free unexpectedly mutated'}
    if($selected -cne [IO.File]::ReadAllText($e.cleanup)){throw 'Free changed beyond the unique Start wake removal'}
    $hostSource=Join-Path $script:RepoRoot 'tests\model\register_owner_tests.c';$hostHash='1bfe838ab756839abeb7d100e33018b2ecac1599dcb60ff16bd79879eda7536e'
    if($e.host_source -cne $hostSource -or $e.host_source_sha256 -ne $hostHash -or $e.host_original_sha256 -ne $hostHash -or (Get-FileHash $e.host_original).Hash.ToLowerInvariant() -ne $hostHash -or
        (Get-FileHash (Join-Path $baseline 'tests\model\register_owner_tests.c')).Hash.ToLowerInvariant() -ne $hostHash){throw 'Frozen original register host mismatch'}
    $body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\register_owner_tests.c'))
    $fieldBefore='    struct {volatile LONG V2SessionActive,AllowedMask;} Probe;';$fieldAfter='    volatile LONG Running; struct {volatile LONG V2SessionActive,AllowedMask;KEVENT RunningEvent;} Probe;'
    if($e.field_original -cne $fieldBefore -or $e.field_adapted -cne $fieldAfter -or [regex]::Matches($body,[regex]::Escape($fieldBefore)).Count -ne 1){throw 'Hosted Context field adaptation changed'}
    $body=$body.Replace($fieldBefore,$fieldAfter);$workerFirst='static VOID PbV2Worker(PVOID input)';$workerNext='typedef struct {VOID (*Routine)(PVOID);PVOID Input;} THREAD_START;'
    $offset=$body.IndexOf($workerFirst);$end=$body.IndexOf($workerNext,$offset)
    if($e.worker_first -cne $workerFirst -or $e.worker_next -cne $workerNext -or $e.standin_original -cne $body.Substring($offset,$end-$offset)){throw 'Original synthetic worker span changed'}
    $body=$body.Substring(0,$offset)+'#include PB_WORKER_START_ADAPTER'+"`r`n"+$body.Substring($end)
    $mainBefore='int main(int argc,char **argv)';$mainAfter='int UnusedRegisterMain(int argc,char **argv)'
    if($e.main_original -cne $mainBefore -or $e.main_adapted -cne $mainAfter -or [regex]::Matches($body,[regex]::Escape($mainBefore)).Count -ne 1){throw 'Unused main rename changed'}
    $body=$body.Replace($mainBefore,$mainAfter);$changes=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{
        param($match);$path=if($match.Groups[1].Value -eq '../../driver/v2/session_cleanup.h'){$e.free_entry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $match.Groups[1].Value))}
        $adapted='#include "'+$path.Replace('\','/')+'"';$changes.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    if($body -cne [IO.File]::ReadAllText($e.host) -or ($changes|ConvertTo-Json -Depth 4 -Compress) -cne ($e.include_changes|ConvertTo-Json -Depth 4 -Compress)){throw 'Host changed beyond exact fields/synthetic-worker/main/include projection'}
    $freeEntry=@'
static VOID ObservedJoin(HANDLE,BOOLEAN,VOID *);
#define ZwWaitForSingleObject ObservedJoin
#include PB_WORKER_START_FREE
#undef ZwWaitForSingleObject
'@
    $registerEntry=@'
static VOID *ObservedAllocate(ULONG,SIZE_T,ULONG);
static NTSTATUS ObservedReference(HANDLE,ACCESS_MASK,VOID *,UCHAR,PKEVENT *,VOID *);
static NTSTATUS ObservedCreate(HANDLE *,ACCESS_MASK,OBJECT_ATTRIBUTES *,HANDLE,VOID *,VOID (*)(PVOID),PVOID);
#define ExAllocatePoolZero ObservedAllocate
#define ObReferenceObjectByHandle ObservedReference
#define PsCreateSystemThread ObservedCreate
#include PB_WORKER_START_REGISTER
#undef ExAllocatePoolZero
#undef ObReferenceObjectByHandle
#undef PsCreateSystemThread
'@
    $adapter=@'
#pragma once
typedef VOID KSTART_ROUTINE(PVOID);
#define WaitAny 0
static VOID HostBeforeWorker(PVOID),HostStartWait(KEVENT *,int,int,BOOLEAN,VOID *),HostClear(KEVENT *),HostTerminate(NTSTATUS);
static VOID HostMultipleWait(ULONG,VOID **,int,int,int,BOOLEAN,VOID *,VOID *);
static PB_RESULT HostRecycle(PB_KERNEL_QUEUE *),HostArm(PB_QUEUE_VIEW *,uint32_t *);
static VOID HostFlush(PB_KERNEL_QUEUE *);
static BOOLEAN PbV2ReceiveBatch(PB_KERNEL_QUEUE *q){(void)q;CHECK(!UnexpectedBody);return FALSE;}
#include PB_WORKER_START_FAULT
#include PB_WORKER_START_COMPLETION
#include PB_WORKER_START_RECYCLE
#define PbV2Worker ActualWorker
#define KeWaitForSingleObject HostStartWait
#define KeWaitForMultipleObjects HostMultipleWait
#define KeClearEvent HostClear
#define PsTerminateSystemThread HostTerminate
#define PbV2RecycleLocked HostRecycle
#define PbV2FlushLocked HostFlush
#define PbQueueArm HostArm
#include PB_WORKER_START_ACTUAL
#undef PbV2Worker
#undef KeWaitForSingleObject
#undef KeWaitForMultipleObjects
#undef KeClearEvent
#undef PsTerminateSystemThread
#undef PbV2RecycleLocked
#undef PbV2FlushLocked
#undef PbQueueArm
static VOID PbV2Worker(PVOID input){HostBeforeWorker(input);ActualWorker(input);}
'@
    if([IO.File]::ReadAllText($e.free_entry) -cne $freeEntry -or [IO.File]::ReadAllText($e.register_entry) -cne $registerEntry -or [IO.File]::ReadAllText($e.adapter) -cne $adapter){throw 'Explicit DDI/actual-worker wrappers changed'}
    $worker=$e.parts|Where-Object name -eq 'worker';$lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};if($build.variant -ne 'current'){$lines+='#define PB_WORKER_START_NEGATIVE 1'}
    $bindings=[ordered]@{HOST=$e.host;ADAPTER=$e.adapter;ACTUAL=$worker.path;FREE=$e.cleanup;REGISTER=$e.register.header}
    foreach($name in @('fault','completion','recycle')){$bindings[$name.ToUpperInvariant()]=Join-Path $script:RepoRoot "driver\v2\session_$name.h"}
    foreach($name in $bindings.Keys){$lines+='#define PB_WORKER_START_'+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $lines+='#define PB_REGISTER_SOURCE "'+$e.register_entry.Replace('\','/')+'"';$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\worker_start_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`r`n")+"`r`n")){throw 'Compilation entry changed'}
    foreach($item in $build.sources){if((Get-FileHash $item.path).Hash.ToLowerInvariant() -ne $item.sha256){throw 'Recorded source/artifact hash changed'}}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-WorkerStartProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start worker startup test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Worker startup external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-worker-start.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) actual worker/Register/Free startup cases plus (\d+) checked warmup; (\d+) worker starts/terminations/joins, zero clear/recycle/flush/receive/arm/multiple-wait, every caller joined and owned resources zero; handles(\d+)/(\d+); rescues(\d+)/(\d+)')
        $full=$Arguments.Count -eq 0;$cases=if($full){60}else{1};$warm=if($full){60}else{2};$workers=if($full){210}else{7};$rescues=if($Expected -eq 131){1}else{0};$warmRescues=if($Expected -eq 131){2}else{0}
        $marker='WORKER_ROLLBACK_START_WAKE_MISSING: Free published Stopping and StopEvent but Start remained unsignaled; one original Start-event rescue per negative case, actual worker early exit and complete joins/reclamation before131'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 131 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $workers -and $summary.Groups[4].Value -eq $summary.Groups[5].Value -and [int]$summary.Groups[6].Value -eq $rescues -and [int]$summary.Groups[7].Value -eq $warmRescues}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 131 -and $correct);watchdog_ms=30000;log=$log;
            measured_schedules=$cases;checked_warmup_schedules=$warm;schedules=$cases+$warm;measured_worker_joins=$workers;all_worker_joins=$(if($full){420}else{21});all_caller_joins=$cases+$warm;rescue_signals=$rescues+$warmRescues;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Worker startup test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-start-wake')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant";$built=Build-WorkerStartTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-WorkerStartProjection $built
    if($variant -eq 'current'){Invoke-WorkerStartProcess $built.path @();Invoke-WorkerStartProcess $built.path @('131')}else{Invoke-WorkerStartProcess $built.path @('131') 131};$executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during worker startup audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');
    unchanged_dependencies=($dependencies.Count-3);production_changed=$true;kernel_runtime_tested=$false;production_ready=$false;
    operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\worker_start_tests.c','tools\build\worker-start-tests.ps1','tools\audit\verify-worker-start.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only actual Register/worker/Free startup combination with the frozen original register-owner fixture/ledger. The complete original host is snapshotted and hash-locked, with only Running/RunningEvent fields, replacement of the old synthetic worker by an explicit launcher, an unused main rename and recorded absolute/include selection changes in ignored output. Actual Register is verbatim extracted, actual Worker/Free are complete verbatim headers; exact generated wrappers bind DDI observations, macro-rename Worker to ActualWorker and invoke it through the original threaded launch DDI. Ordinary pool and guarded pool/region ownership, true SDK locks/events, original handle-to-object reference accounting and original thread/MDL/map/pool cleanup bodies remain unchanged. NDEBUG is required and PB_API_TESTS rejected; the old host deliberately retains NT_ASSERT=CHECK as stronger model assertions, while these early-stop schedules never enter quiescence/reference assertions or packet processing. Shipping fingerprint must be65 bytes. One/two/eight queue layouts cover successful registration immediately unregistered from an initially paused modeled Ctx, and required pool/event-reference/thread failure in the last queue after the preceding workers were created. Single-queue failed prefixes contain no worker and only stage0 is retained. Three real scheduling gates select before the actual worker invocation, before its actual SDK Start wait, or after that true wait returns but before the actual worker continues. DDI failures are observed only after the original frozen DDI returned its own failure, then release the original Register failure/Free path. Partial workers are not permitted to execute packet transactions before publication, and every schedule publishes actual Stopping and begins actual Free joins before letting selected workers continue. Each worker performs exactly one Start wait, one real wait return, one production Terminate call and one actual Free join/close; independent hooks reject worker clear/recycle/flush/receive/arm/multiple-wait, so no packet path or busy loop executes. Entry markers are SDK boundary observations, not claims about opaque scheduler parking. Per-case all original pools/MDLs/page locks/maps/object references/worker handles and separately counted harness caller/event handles return to zero after actual caller and worker thread joins. Full processes warm60 then measure60 cases, CLI131 warms2 then measures1, all with strict process-handle equality. Negative131 omits only the unique StartEvent signal inside actual Free, retaining Stopping publication, StopEvent signal, original Register publication wake and all joins/barriers. Seven actual workers had selected the real Start wait while Start was false; Free reaches its first join with Stopping1/StopEvent1 but Start0. This deterministic missing signal witness is observed before any resources are released. The controller invokes the original KeSetEvent(Start) exactly once for safe rescue, then actual waits return, workers remain gated until their stop/ownership snapshot is checked, and actual early termination, all real joins and complete Free/caller cleanup finish before131. Current never rescues and no ref/counter/slot/generation is repaired. No Halt, initialization/NDIS callback during Initializing, native kernel DDI/IRQL/NDIS, Win7 runtime, driver/device/network/registry or CPU-performance result is claimed. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
