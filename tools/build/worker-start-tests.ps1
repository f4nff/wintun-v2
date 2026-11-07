# Actual startup/rollback helper, reusing the unchanged register-owner ledger.
. (Join-Path $PSScriptRoot 'kernel-register-tests.ps1')
function Build-WorkerStartTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-start-wake')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Worker startup output must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Worker startup requires shipping fingerprint'}
    $register=Write-KernelRegister -Directory $Directory -Variant current;$encoding=[Text.UTF8Encoding]::new($false)
    $parts=@(@{name='register';source=$register.source;source_sha256=$register.source_sha256;offset=$register.offset;length=$register.length;path=$register.extracted;sha256=$register.extracted_sha256})
    foreach($name in @('worker','cleanup')){
        $source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes($source))
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=0;length=[IO.File]::ReadAllText($source).Length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $worker=$parts|Where-Object name -eq 'worker';$cleanup=$parts|Where-Object name -eq 'cleanup';$selected=[IO.File]::ReadAllText($cleanup.path)
    $before='KeSetEvent(&s->StartEvent, IO_NO_INCREMENT, FALSE);';$after='/* Test mutation: rollback StartEvent wake omitted. */'
    if([regex]::Matches($selected,[regex]::Escape($before)).Count -ne 1){throw 'Free StartEvent wake not unique'}
    $mutation=$null;if($Variant -ne 'current'){$mutation=[ordered]@{original=$before;changed=$after;offset=$selected.IndexOf($before);length=$before.Length};$selected=$selected.Replace($before,$after)}
    $cleanupSelected=Join-Path $Directory 'free-selected.h';[IO.File]::WriteAllText($cleanupSelected,$selected,$encoding)
    $freeEntry=Join-Path $Directory 'free-entry.h';[IO.File]::WriteAllText($freeEntry,@'
static VOID ObservedJoin(HANDLE,BOOLEAN,VOID *);
#define ZwWaitForSingleObject ObservedJoin
#include PB_WORKER_START_FREE
#undef ZwWaitForSingleObject
'@,$encoding)
    $registerEntry=Join-Path $Directory 'register-entry.h';[IO.File]::WriteAllText($registerEntry,@'
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
'@,$encoding)
    $adapter=Join-Path $Directory 'worker-entry.h';[IO.File]::WriteAllText($adapter,@'
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
'@,$encoding)
    $hostSource=Join-Path $script:RepoRoot 'tests\model\register_owner_tests.c';$hostHash='1bfe838ab756839abeb7d100e33018b2ecac1599dcb60ff16bd79879eda7536e'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -ne $hostHash){throw 'Register-owner host changed; review finite adaptation'}
    $hostOriginal=Join-Path $Directory 'register-host-original.c';[IO.File]::WriteAllBytes($hostOriginal,[IO.File]::ReadAllBytes($hostSource));$body=[IO.File]::ReadAllText($hostSource)
    $fieldBefore='    struct {volatile LONG V2SessionActive,AllowedMask;} Probe;';$fieldAfter='    volatile LONG Running; struct {volatile LONG V2SessionActive,AllowedMask;KEVENT RunningEvent;} Probe;'
    if([regex]::Matches($body,[regex]::Escape($fieldBefore)).Count -ne 1){throw 'Context fields marker not unique'};$body=$body.Replace($fieldBefore,$fieldAfter)
    $workerFirst='static VOID PbV2Worker(PVOID input)';$workerNext='typedef struct {VOID (*Routine)(PVOID);PVOID Input;} THREAD_START;'
    foreach($marker in @($workerFirst,$workerNext)){if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Stand-in worker marker not unique'}}
    $offset=$body.IndexOf($workerFirst);$end=$body.IndexOf($workerNext,$offset);$standin=$body.Substring($offset,$end-$offset)
    $body=$body.Substring(0,$offset)+'#include PB_WORKER_START_ADAPTER'+"`r`n"+$body.Substring($end)
    $mainBefore='int main(int argc,char **argv)';$mainAfter='int UnusedRegisterMain(int argc,char **argv)'
    if([regex]::Matches($body,[regex]::Escape($mainBefore)).Count -ne 1){throw 'Unused main marker not unique'};$body=$body.Replace($mainBefore,$mainAfter)
    $includeChanges=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{
        param($match);$path=if($match.Groups[1].Value -eq '../../driver/v2/session_cleanup.h'){$freeEntry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $match.Groups[1].Value))}
        $adapted='#include "'+$path.Replace('\','/')+'"';$includeChanges.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    $adaptedHost=Join-Path $Directory 'register-host.h';[IO.File]::WriteAllText($adaptedHost,$body,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};if($Variant -ne 'current'){$lines+='#define PB_WORKER_START_NEGATIVE 1'}
    $bindings=[ordered]@{HOST=$adaptedHost;ADAPTER=$adapter;ACTUAL=$worker.path;FREE=$cleanupSelected;REGISTER=$register.header}
    foreach($name in @('fault','completion','recycle')){$bindings[$name.ToUpperInvariant()]=Join-Path $script:RepoRoot "driver\v2\session_$name.h"}
    foreach($name in $bindings.Keys){$lines+='#define PB_WORKER_START_'+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $lines+='#define PB_REGISTER_SOURCE "'+$registerEntry.Replace('\','/')+'"';$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\worker_start_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`r`n")+"`r`n",$encoding);$exe=Join-Path $Directory 'worker-start.exe'
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",('/I'+(Join-Path $script:RepoRoot 'driver\v2')),"/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe ($flags+@($entry,'common\v2_core.c','/link',"/out:$exe"))|Out-Host
    $sources=@($hostSource,$hostOriginal,$adaptedHost,$adapter,$freeEntry,$registerEntry,$cleanupSelected,$entry,(Join-Path $script:RepoRoot 'tests\model\worker_start_tests.c'))|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;sources=@($sources);
        extractions=[ordered]@{parts=$parts;register=$register;cleanup=$cleanupSelected;cleanup_sha256=(Get-FileHash $cleanupSelected).Hash.ToLowerInvariant();mutation=$mutation;
            host_source=$hostSource;host_source_sha256=$hostHash;host_original=$hostOriginal;host_original_sha256=(Get-FileHash $hostOriginal).Hash.ToLowerInvariant();host=$adaptedHost;host_sha256=(Get-FileHash $adaptedHost).Hash.ToLowerInvariant();
            field_original=$fieldBefore;field_adapted=$fieldAfter;worker_first=$workerFirst;worker_next=$workerNext;standin_original=$standin;main_original=$mainBefore;main_adapted=$mainAfter;include_changes=@($includeChanges);
            adapter=$adapter;free_entry=$freeEntry;register_entry=$registerEntry;
            adaptation='Original frozen register-owner host changes only Running/RunningEvent fields, replacement of its synthetic worker by the recorded launcher/actual-worker entry, unused main token, and absolute quoted includes selecting the Free wrapper. Actual Worker is separately macro-renamed and runs verbatim; exact Register/Free use DDI-name observations only. Current cleanup is unchanged; the unique negative removes only its StartEvent wake. No old source file is edited.'}}
}
