function Build-ReferenceHandoff32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Reference handoff output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Reference handoff requires shipping fingerprint'}
    . (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
    $parts=Write-IndicationPauseParts -Directory (Join-Path $Directory 'parts');$utf8=[Text.UTF8Encoding]::new($false)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$hostHash='a0a20ebbe48fdcfbf64a0f210dcafcc107e0e83cf060c4727df6301653808771'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -cne $hostHash){throw 'Frozen worker/Pause host changed; review reference handoff'}
    $original=Join-Path $Directory 'worker-host-original.c';[IO.File]::WriteAllBytes($original,[IO.File]::ReadAllBytes($hostSource))
    $driver=Join-Path $script:RepoRoot 'driver/wintun.c';$body=[IO.File]::ReadAllText($driver);$first='static MINIPORT_RESTART TunRestart;';$last='static MINIPORT_PAUSE TunPause;'
    foreach($marker in @($first,$last)){if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Handoff Restart markers not unique'}}
    $start=$body.IndexOf($first);$end=$body.IndexOf($last,$start);if($end -le $start){throw 'Handoff Restart span missing'}
    $restart=Join-Path $Directory 'restart-original.h';[IO.File]::WriteAllText($restart,$body.Substring($start,$end-$start),$utf8)
    $restartRecord=@{source=$driver;source_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();offset=$start;length=$end-$start;path=$restart;sha256=(Get-FileHash $restart).Hash.ToLowerInvariant()}
    $reference=Join-Path $Directory 'reference-observe.h';$lines=@('static VOID HandoffClear(KEVENT *);','static VOID HandoffReferenceAcquire(KSPIN_LOCK *,KLOCK_QUEUE_HANDLE *);','#define KeClearEvent HandoffClear','#define KeAcquireInStackQueuedSpinLock HandoffReferenceAcquire',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_references.h').Replace('\','/')+'"'),'#undef KeClearEvent','#undef KeAcquireInStackQueuedSpinLock');[IO.File]::WriteAllText($reference,($lines -join "`n")+"`n",$utf8)
    $receive=Join-Path $Directory 'receive-observe.h';$lines=@('static VOID HandoffRetain(PB_KERNEL_SESSION *,ULONG,ULONG);','#define PbV2RetainReceives HandoffRetain',('#include "'+($parts.parts|Where-Object name -ceq 'receive').path.Replace('\','/')+'"'),'#undef PbV2RetainReceives');[IO.File]::WriteAllText($receive,($lines -join "`n")+"`n",$utf8)
    $pause=Join-Path $Directory 'pause-observe.h';$lines=@('static KIRQL HandoffPauseAcquire(SRWLOCK *);','#define ExAcquireSpinLockExclusive HandoffPauseAcquire',('#include "'+($parts.parts|Where-Object name -ceq 'tunpause').path.Replace('\','/')+'"'),'#undef ExAcquireSpinLockExclusive');[IO.File]::WriteAllText($pause,($lines -join "`n")+"`n",$utf8)
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{
        param($m)$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value));'#include "'+$path.Replace('\','/')+'"'
    })
    $hostPath=Join-Path $Directory 'worker-host.h';[IO.File]::WriteAllText($hostPath,$body,$utf8)
    $production=@();foreach($relative in @(rg --files driver common include)){
        $source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source-'+$relative.Replace('\','-').Replace('/','-'));[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source))
        $production+=@{relative=$relative;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}
    };if($LASTEXITCODE){throw 'Cannot enumerate reference handoff production closure'}
    $test=Join-Path $script:RepoRoot 'tests/model/reference_handoff32_tests.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    $bindings=[ordered]@{PB_HO_HOST=$hostPath;PB_WP_HOST=$parts.host;PB_WP_RESTART=$restart;PB_REFERENCE_SOURCE=$reference;PB_NBL_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    foreach($part in $parts.parts){$path=if($part.name -ceq 'receive'){$receive}elseif($part.name -ceq 'tunpause'){$pause}else{$part.path};$lines+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$path.Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'reference-handoff32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;production=$production;extractions=$parts;restart=$restartRecord;
        host_source=$hostSource;host_source_sha256=$hostHash;host_original=$original;host_original_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();reference_observer=$reference;reference_observer_sha256=(Get-FileHash $reference).Hash.ToLowerInvariant();receive_observer=$receive;receive_observer_sha256=(Get-FileHash $receive).Hash.ToLowerInvariant();pause_observer=$pause;pause_observer_sha256=(Get-FileHash $pause).Hash.ToLowerInvariant();
        helper=@{path=(Join-Path $PSScriptRoot 'indication-pause-tests.ps1');sha256=(Get-FileHash (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')).Hash.ToLowerInvariant()}}
}
