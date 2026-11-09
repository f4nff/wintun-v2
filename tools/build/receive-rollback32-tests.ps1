function Build-ReceiveRollback32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Receive rollback output must be absolute'}
    if($script:SourceFingerprint-cnotmatch'^[a-f0-9]{64}$'){throw 'Receive rollback requires shipping fingerprint'}
    . (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
    $parts=Write-IndicationPauseParts -Directory (Join-Path $Directory 'parts');$utf8=[Text.UTF8Encoding]::new($false)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$hostHash='a0a20ebbe48fdcfbf64a0f210dcafcc107e0e83cf060c4727df6301653808771'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant()-cne$hostHash){throw 'Frozen worker/Pause host changed; review rollback'}
    $original=Join-Path $Directory 'worker-host-original.c';[IO.File]::WriteAllBytes($original,[IO.File]::ReadAllBytes($hostSource))
    $driver=Join-Path $script:RepoRoot 'driver/wintun.c';$body=[IO.File]::ReadAllText($driver);$first='static MINIPORT_RESTART TunRestart;';$last='static MINIPORT_PAUSE TunPause;'
    foreach($marker in @($first,$last)){if([regex]::Matches($body,[regex]::Escape($marker)).Count-ne1){throw 'Rollback Restart markers not unique'}}
    $start=$body.IndexOf($first);$end=$body.IndexOf($last,$start);if($end-le$start){throw 'Rollback Restart span missing'}
    $restart=Join-Path $Directory 'restart-original.h';[IO.File]::WriteAllText($restart,$body.Substring($start,$end-$start),$utf8);$restartRecord=@{source=$driver;source_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();offset=$start;length=$end-$start;path=$restart;sha256=(Get-FileHash $restart).Hash.ToLowerInvariant()}
    $wrappers=[ordered]@{
        nbl=@('static NET_BUFFER_LIST *RbNbl(VOID *,ULONG,ULONG,MDL *,ULONG,ULONG);','#define NdisAllocateNetBufferAndNetBufferList RbNbl',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h').Replace('\','/')+'"'),'#undef NdisAllocateNetBufferAndNetBufferList')
        receive=@('static VOID RbIndicate(VOID *,NET_BUFFER_LIST *,ULONG,ULONG);','#define PbIndicateReceive RbIndicate',('#include "'+($parts.parts|Where-Object name -ceq 'receive').path.Replace('\','/')+'"'),'#undef PbIndicateReceive')
        tunpause=@('static KIRQL RbPauseAcquire(SRWLOCK *);','#define ExAcquireSpinLockExclusive RbPauseAcquire',('#include "'+($parts.parts|Where-Object name -ceq 'tunpause').path.Replace('\','/')+'"'),'#undef ExAcquireSpinLockExclusive')
        pause=@('static VOID RbBankWait(TUN_CTX *,ULONG);','#define PbWaitIndications RbBankWait',('#include "'+($parts.parts|Where-Object name -ceq 'pause').path.Replace('\','/')+'"'),'#undef PbWaitIndications')
        worker=@('static VOID RbWorkerWait(ULONG,VOID **,int,int,int,BOOLEAN,VOID *,VOID *);','#define KeWaitForMultipleObjects RbWorkerWait',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_worker.h').Replace('\','/')+'"'),'#undef KeWaitForMultipleObjects')
    }
    $observers=@();$paths=@{};foreach($name in $wrappers.Keys){$path=Join-Path $Directory "$name-observe.h";[IO.File]::WriteAllText($path,($wrappers[$name]-join"`n")+"`n",$utf8);$paths[$name]=$path;$observers+=@{name=$name;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}}
    $body=[IO.File]::ReadAllText($hostSource);$marker='#include "../../driver/v2/session_worker.h"';if([regex]::Matches($body,[regex]::Escape($marker)).Count-ne1){throw 'Rollback worker include not unique'}
    $body=$body.Replace($marker,('#include "'+$paths.worker.Replace('\','/')+'"'));$body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$p=$m.Groups[1].Value;if(![IO.Path]::IsPathFullyQualified($p)){$p=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $p))};'#include "'+$p.Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'worker-host.h';[IO.File]::WriteAllText($hostPath,$body,$utf8)
    $production=@();foreach($relative in @(rg --files driver common include)){$relative=$relative.Replace('\','/');$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source-'+$relative.Replace('/','-'));[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$production+=@{relative=$relative;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}};if($LASTEXITCODE){throw 'Cannot enumerate rollback production closure'}
    $test=Join-Path $script:RepoRoot 'tests/model/receive_rollback32_tests.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    $bindings=[ordered]@{PB_RB_HOST=$hostPath;PB_WP_HOST=$parts.host;PB_WP_RESTART=$restart;PB_REFERENCE_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_references.h');PB_NBL_SOURCE=$paths.nbl}
    foreach($part in $parts.parts){$bindings['PB_WP_'+$part.name.ToUpperInvariant()]=if($paths.ContainsKey($part.name)){$paths[$part.name]}else{$part.path}}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"';$entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines-join"`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_-notlike'/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'receive-rollback32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;production=$production;extractions=$parts;restart=$restartRecord;host_source=$hostSource;host_source_sha256=$hostHash;host_original=$original;host_original_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();observers=$observers;helper=@{path=(Join-Path $PSScriptRoot 'indication-pause-tests.ps1');sha256=(Get-FileHash (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')).Hash.ToLowerInvariant()}}
}
