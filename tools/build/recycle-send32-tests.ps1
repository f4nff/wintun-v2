function Build-RecycleSend32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Recycle send output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Recycle send requires shipping fingerprint'}
    . (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
    $parts=Write-IndicationPauseParts -Directory (Join-Path $Directory 'parts');$utf8=[Text.UTF8Encoding]::new($false)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$hostHash='a0a20ebbe48fdcfbf64a0f210dcafcc107e0e83cf060c4727df6301653808771'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -cne $hostHash){throw 'Frozen worker/Pause host changed; review recycle send'}
    $original=Join-Path $Directory 'worker-host-original.c';[IO.File]::WriteAllBytes($original,[IO.File]::ReadAllBytes($hostSource))
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{
        param($m)$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value));'#include "'+$path.Replace('\','/')+'"'
    })
    $hostPath=Join-Path $Directory 'worker-host.h';[IO.File]::WriteAllText($hostPath,$body,$utf8)
    $spans=@();$specs=[ordered]@{
        restart=@('driver/wintun.c','static MINIPORT_RESTART TunRestart;','static MINIPORT_PAUSE TunPause;')
        reply=@('driver/v2/session.h','static NDIS_STATUS PbV2ControlReply(','#include "session_describe.h"')
        frame=@('tests/model/control_reply_tests.c','static VOID Put16(','static VOID RunControl(')
    }
    foreach($name in $specs.Keys){
        $spec=$specs[$name];$source=Join-Path $script:RepoRoot $spec[0];$body=[IO.File]::ReadAllText($source)
        if($name -ceq 'frame' -and (Get-FileHash $source).Hash.ToLowerInvariant() -cne '70492226145283c68ab9036053a65bef96827a2b49d850da41087ba0ba013d7e'){throw 'Frozen control packet host changed; review recycle send'}
        foreach($marker in $spec[1..2]){if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Recycle send markers not unique'}}
        $start=$body.IndexOf($spec[1]);$end=$body.IndexOf($spec[2],$start);if($end -le $start){throw 'Recycle send span missing'}
        $path=Join-Path $Directory "$name-original.h";[IO.File]::WriteAllText($path,$body.Substring($start,$end-$start),$utf8)
        $snapshot=Join-Path $Directory "$name-source-original";[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source))
        $spans+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();snapshot=$snapshot;snapshot_sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant();start_marker=$spec[1];end_marker=$spec[2];offset=$start;length=$end-$start;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $types=@();$typeRecords=@();foreach($name in 'nblchecksum','nbllso','nbluso'){
        $source=Join-Path $script:WdkRoot "Include/$script:KitVersion/km/ndis/$name.h";$body=[IO.File]::ReadAllText($source);$matches=[regex]::Matches($body,'(?s)typedef struct _NDIS_.*?\n\} NDIS_[^;]+;');if($matches.Count -ne 1){throw 'Recycle send WDK type not unique'};$m=$matches[0];$types+=$m.Value
        $snapshot=Join-Path $Directory "wdk-$name-original.h";[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source))
        $typeRecords+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();snapshot=$snapshot;snapshot_sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant();offset=$m.Index;length=$m.Length;span_sha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($utf8.GetBytes($m.Value))).ToLowerInvariant()}
    }
    $typePath=Join-Path $Directory 'types.h';[IO.File]::WriteAllText($typePath,($types -join "`n"),$utf8)
    $callout=Join-Path $Directory 'callout-observe.h';$lines=@('static VOID RcsSdkComplete(NDIS_HANDLE,NET_BUFFER_LIST *,ULONG);','#define NdisMSendNetBufferListsComplete RcsSdkComplete',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/ndis_callouts.h').Replace('\','/')+'"'),'#undef NdisMSendNetBufferListsComplete');[IO.File]::WriteAllText($callout,($lines -join "`n")+"`n",$utf8)
    $send=Join-Path $Directory 'send-observe.h';$lines=@('static PB_RESULT RcsReserve(PB_POOL *,PB_TOKEN *);','static VOID RcsReject(TUN_CTX *,uint64_t,NET_BUFFER_LIST *,NET_BUFFER *,NDIS_STATUS,ULONG,ULONG,ULONG,const PB_SEND_FAILURE *,ULONG,ULONG,ULONG,PB_KERNEL_QUEUE *,ULONG);','#pragma push_macro("PbV2RejectSend")','#undef PbV2RejectSend','#define PbV2RejectSend RcsReject','#define PbPoolReserve RcsReserve',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_send.h').Replace('\','/')+'"'),'#undef PbPoolReserve','#pragma pop_macro("PbV2RejectSend")');[IO.File]::WriteAllText($send,($lines -join "`n")+"`n",$utf8)
    $controlSource=Join-Path $script:RepoRoot 'tests/model/control_batch32_tests.c';$controlHash='871a9469fb0d5c20433e5dcc853e2178818b161b6134143aeda38e5dd3dccf53'
    if((Get-FileHash $controlSource).Hash.ToLowerInvariant() -cne $controlHash){throw 'Frozen control host changed; review recycle send'}
    $controlOriginal=Join-Path $Directory 'control-host-original.c';[IO.File]::WriteAllBytes($controlOriginal,[IO.File]::ReadAllBytes($controlSource))
    $body=[IO.File]::ReadAllText($controlSource);if([regex]::Matches($body,[regex]::Escape('int main(VOID)')).Count -ne 1){throw 'Control host entry marker not unique'}
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$relative=$m.Groups[1].Value;$path=if($relative -ceq '../../driver/v2/ndis_callouts.h'){$callout}elseif($relative -ceq '../../driver/v2/session_send.h'){$send}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $controlSource) $relative))};'#include "'+$path.Replace('\','/')+'"'}).Replace('int main(VOID)','int UnusedRecycleControlMain(VOID)')
    $controlHost=Join-Path $Directory 'control-host.h';[IO.File]::WriteAllText($controlHost,$body,$utf8)
    $production=@();foreach($relative in @(rg --files driver common include)){
        $source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source-'+$relative.Replace('\','-').Replace('/','-'));[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source))
        $production+=@{relative=$relative;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}
    };if($LASTEXITCODE){throw 'Cannot enumerate recycle send production closure'}
    $test=Join-Path $script:RepoRoot 'tests/model/recycle_send32_tests.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    $bindings=[ordered]@{PB_RCS_CONTROL=$controlHost;PB_BATCH_HOST=$hostPath;PB_BATCH_TYPES=$typePath;PB_BATCH_REPLY=($spans|Where-Object name -eq 'reply').path;PB_BATCH_FRAME=($spans|Where-Object name -eq 'frame').path;PB_WP_HOST=$parts.host;PB_WP_RESTART=($spans|Where-Object name -eq 'restart').path;PB_REFERENCE_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_references.h');PB_NBL_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    foreach($part in $parts.parts){$lines+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'recycle-send32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','common/flow.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;production=$production;extractions=$parts;spans=$spans;types=$typeRecords;type_path=$typePath;type_sha256=(Get-FileHash $typePath).Hash.ToLowerInvariant();
        host_source=$hostSource;host_source_sha256=$hostHash;host_original=$original;host_original_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();control_source=$controlSource;control_source_sha256=$controlHash;control_original=$controlOriginal;control_original_sha256=(Get-FileHash $controlOriginal).Hash.ToLowerInvariant();control_host=$controlHost;control_host_sha256=(Get-FileHash $controlHost).Hash.ToLowerInvariant();callout_observer=$callout;callout_observer_sha256=(Get-FileHash $callout).Hash.ToLowerInvariant();send_observer=$send;send_observer_sha256=(Get-FileHash $send).Hash.ToLowerInvariant();
        helper=@{path=(Join-Path $PSScriptRoot 'indication-pause-tests.ps1');sha256=(Get-FileHash (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')).Hash.ToLowerInvariant()}}
}
