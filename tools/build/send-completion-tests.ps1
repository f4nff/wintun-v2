# Reuse existing hash-locked indication/NBL host only in ignored build output.
. (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
function Write-SendCompletionParts {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-returns-wait')][string]$Variant='current')
    $encoding=[Text.UTF8Encoding]::new($false);$base=Write-IndicationPauseParts -Directory $Directory
    $hostText=[IO.File]::ReadAllText($base.host)
    $deviceOld='struct { SRWLOCK RegistrationLock; KEVENT Disconnected; } Device;'
    $deviceNew='struct { SRWLOCK RegistrationLock; KEVENT Disconnected; VOID *OwningFileObject; HANDLE OwningProcessId; LIST_ENTRY Entry; } Device;'
    if([regex]::Matches($hostText,[regex]::Escape($deviceOld)).Count -ne 1){throw 'NBL host Device type is not unique'}
    $hostPath=Join-Path $Directory 'completion-kernel-host.h';[IO.File]::WriteAllText($hostPath,$hostText.Replace($deviceOld,$deviceNew),$encoding)
    $driver=Join-Path $script:RepoRoot 'driver\wintun.c';$text=[IO.File]::ReadAllText($driver)
    $start=$text.IndexOf('#define TUN_FORCE_UNREGISTRATION');$end=[regex]::Match($text,'(?m)^_IRQL_requires_max_\(PASSIVE_LEVEL\)\r?\nstatic VOID\r?\nTunProcessNotification\(')
    if($start -lt 0 -or !$end.Success -or $end.Index -le $start){throw 'Cannot isolate actual FILE unregister functions'}
    $common=Join-Path $Directory 'common-extracted.h';[IO.File]::WriteAllText($common,$text.Substring($start,$end.Index-$start),$encoding)
    $parts=@(@{name='common';source=$driver;source_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();offset=$start;length=$end.Index-$start;
        start_marker='#define TUN_FORCE_UNREGISTRATION';end_marker='_IRQL_requires_max_(PASSIVE_LEVEL) / static VOID / TunProcessNotification(';path=$common;sha256=(Get-FileHash $common).Hash.ToLowerInvariant()})
    foreach($name in @('send','cleanup')){
        $source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$path=Join-Path $Directory "$name-extracted.h"
        [IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes($source));$parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=0;
            length=[IO.File]::ReadAllText($source).Length;start_marker='[entire header]';end_marker='[end of source]';path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $cleanup=$parts|Where-Object name -eq 'cleanup';$cleanupText=[IO.File]::ReadAllText($cleanup.path)
    $wait='    ExWaitForRundownProtectionRelease(&s->Returns);';$replacement='    /* Test mutation: Free Returns wait omitted. */'
    if([regex]::Matches($cleanupText,[regex]::Escape($wait)).Count -ne 1){throw 'Free Returns wait is not unique'}
    if($Variant -eq 'without-returns-wait'){$cleanupText=$cleanupText.Replace($wait,$replacement)}
    $cleanupPath=Join-Path $Directory 'cleanup-selected.h';[IO.File]::WriteAllText($cleanupPath,$cleanupText,$encoding)
    $baseSource=Join-Path $script:RepoRoot 'tests\model\indication_pause_tests.c'
    $baseHash=(Get-FileHash $baseSource).Hash.ToLowerInvariant()
    if($baseHash -ne '6fa4dfe69386741491e20eb7d772224d931f759e33576aaf16ce88015816e0fb'){throw 'Reused indication host changed; review observer adaptations'}
    $baseOriginal=Join-Path $Directory 'indication-host-original.c';[IO.File]::WriteAllBytes($baseOriginal,[IO.File]::ReadAllBytes($baseSource))
    $body=[IO.File]::ReadAllText($baseSource)
    $includeChanges=@([regex]::Matches($body,'(?m)^#include "([^"]+)"')|ForEach-Object {
        $path=[IO.Path]::GetFullPath((Join-Path (Split-Path $baseSource) $_.Groups[1].Value));if($_.Groups[1].Value -eq '../../driver/v2/session_cleanup.h'){$path=$cleanupPath}
        @{original=$_.Value;adapted='#include "'+$path.Replace('\','/')+'"'}
    })
    foreach($change in $includeChanges){$body=$body.Replace($change.original,$change.adapted)}
    $observers=@(
        @{original='CHECK(!reason&&!mode&&!alertable&&!timeout&&!TransitionHeld);';adapted='CHECK(!reason&&!mode&&!alertable&&!timeout&&!TransitionHeld); ProbeWait(event);'},
        @{original='static VOID ObDereferenceObject(KEVENT *e){CHECK';adapted='static VOID ObDereferenceObject(KEVENT *e){ProbeResource(e);CHECK'},
        @{original='static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r){AcquireSRWLockExclusive';adapted='static VOID ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF *r){ProbeRundownWait(r);AcquireSRWLockExclusive'}
    )
    foreach($change in $observers){if([regex]::Matches($body,[regex]::Escape($change.original)).Count -ne 1){throw 'Host observer marker is not unique'};$body=$body.Replace($change.original,$change.adapted)}
    $adapted=Join-Path $Directory 'indication-host.h';[IO.File]::WriteAllText($adapted,$body,$encoding)
    return [ordered]@{variant=$Variant;base=$base;parts=$parts;host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();device_original=$deviceOld;device_adapted=$deviceNew;
        cleanup=$cleanupPath;cleanup_sha256=(Get-FileHash $cleanupPath).Hash.ToLowerInvariant();mutation_original=$wait;mutation_replacement=$replacement;
        reused_source=$baseSource;reused_source_sha256=$baseHash;reused_original=$baseOriginal;reused_original_sha256=(Get-FileHash $baseOriginal).Hash.ToLowerInvariant();
        reused=$adapted;reused_sha256=(Get-FileHash $adapted).Hash.ToLowerInvariant();include_changes=$includeChanges;observer_changes=$observers;
        adaptation='NBL host: prior exact bank-field/include adaptation plus only Device owner/list fields. Existing indication test: only quoted include bindings, one before-wait observer, one before-object-release observer and one before-rundown-wait observer; its main is token-renamed and never executed. All existing allocation/free/lock routines stay unchanged apart from these explicit observer calls. Actual production bodies are not adapted, except the selected single negative Free wait removal.'}
}
function Build-SendCompletionTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-returns-wait')][string]$Variant='current')
    if($script:SourceFingerprint -cnotmatch '^[0-9a-f]{64}$'){throw 'Send completion builder requires current shipping source fingerprint'}
    $extraction=Write-SendCompletionParts -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($Variant -eq 'without-returns-wait'){$lines+='#define PB_SEND_COMPLETION_WITHOUT_WAIT 1'}
    $lines+='#define PB_SEND_COMPLETION_HOST "'+$extraction.reused.Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_NBL_HOST "'+$extraction.host.Replace('\','/')+'"'
    $lines+='#define PB_REFERENCE_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_references.h').Replace('\','/')+'"'
    $lines+='#define PB_NBL_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h').Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_PAUSE_SOURCE "'+$extraction.base.pause.Replace('\','/')+'"'
    foreach($part in $extraction.base.parts|Where-Object name -ne 'pause'){$lines+='#define PB_INDICATION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    foreach($part in $extraction.parts|Where-Object name -ne 'cleanup'){$lines+='#define PB_SEND_COMPLETION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\send_completion_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'send-completion.exe'
    $versionArguments=@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",('/I'+(Join-Path $script:RepoRoot 'driver\v2')),
        "/Fo$Directory\",$entry,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')+$versionArguments+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;extractions=$extraction}
}
