# Build helper only: caller initializes common.ps1 and owns process recording.
function Write-IndicationPauseParts {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-indication-wait')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Indication/pause output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$parts=@()
    $driver=Join-Path $script:RepoRoot 'driver\wintun.c';$text=[IO.File]::ReadAllText($driver)
    $anchors=[ordered]@{
        outer=@('static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;','#define TUN_FORCE_UNREGISTRATION')
        tunpause=@('static MINIPORT_PAUSE TunPause;','static MINIPORT_DEVICE_PNP_EVENT_NOTIFY TunDevicePnPEventNotify;')
    }
    foreach($name in $anchors.Keys){
        $startMarker=$anchors[$name][0];$endMarker=$anchors[$name][1]
        if([regex]::Matches($text,[regex]::Escape($startMarker)).Count -ne 1 -or [regex]::Matches($text,[regex]::Escape($endMarker)).Count -ne 1){throw "Nonunique $name markers"}
        $start=$text.IndexOf($startMarker);$end=$text.IndexOf($endMarker,$start)
        if($start -lt 0 -or $end -le $start){throw "Cannot isolate $name"}
        $path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($path,$text.Substring($start,$end-$start),$encoding)
        $parts+=@{name=$name;source=$driver;source_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();offset=$start;length=$end-$start;
            start_marker=$startMarker;end_marker=$endMarker;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    foreach($name in @('receive','gate','pause')){
        $relative=switch($name){'receive'{'driver\v2\session_receive.h'}'gate'{'driver\v2\indication_gate.h'}'pause'{'driver\v2\session_pause.h'}}
        $source=Join-Path $script:RepoRoot $relative;$body=[IO.File]::ReadAllText($source);$start=0;$marker='[entire header]'
        if($name -eq 'receive'){$marker='static BOOLEAN PbV2ReceiveBatch(';if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Receive marker is not unique'};$start=$body.IndexOf($marker)}
        $path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($path,$body.Substring($start),$encoding)
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$start;length=$body.Length-$start;
            start_marker=$marker;end_marker='[end of source]';path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $pause=$parts|Where-Object name -eq 'pause';$pauseText=[IO.File]::ReadAllText($pause.path)
    $wait='    PbWaitIndications(ctx, 0); PbWaitIndications(ctx, 1);';$replacement='    /* Test mutation: both indication-bank waits omitted. */'
    if([regex]::Matches($pauseText,[regex]::Escape($wait)).Count -ne 1){throw 'Pause bank wait line is not unique'}
    if($Variant -eq 'without-indication-wait'){$pauseText=$pauseText.Replace($wait,$replacement)}
    $pauseHeader=Join-Path $Directory 'pause-selected.h';[IO.File]::WriteAllText($pauseHeader,$pauseText,$encoding)
    $hostSource=Join-Path $script:RepoRoot 'tests\model\kernel_nbl_fixture.h';$hostText=[IO.File]::ReadAllText($hostSource)
    $hostHash=(Get-FileHash $hostSource).Hash.ToLowerInvariant()
    if($hostHash -ne 'c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'){throw 'NBL host changed; review finite indication-field adaptation'}
    $hostOriginal=Join-Path $Directory 'kernel-host-original.h';[IO.File]::WriteAllBytes($hostOriginal,[IO.File]::ReadAllBytes($hostSource))
    $anchor='        EX_RUNDOWN_REF ReturnRundown;'
    $fields='        KSPIN_LOCK IndicationLock; KEVENT IndicationIdle[2]; ULONG IndicationBank; LONG IndicationCount[2];'
    if([regex]::Matches($hostText,[regex]::Escape($anchor)).Count -ne 1){throw 'NBL host field marker is not unique'}
    $fieldOffset=$hostText.IndexOf($anchor);$adapted=$hostText.Replace($anchor,$anchor+"`r`n"+$fields)
    $includeChanges=@([regex]::Matches($adapted,'(?m)^#include "([^"]+)"')|ForEach-Object {
        @{original=$_.Value;adapted='#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $_.Groups[1].Value)).Replace('\','/')+'"'}
    })
    foreach($change in $includeChanges){$adapted=$adapted.Replace($change.original,$change.adapted)}
    $hostPath=Join-Path $Directory 'kernel-host.h';[IO.File]::WriteAllText($hostPath,$adapted,$encoding)
    return [ordered]@{variant=$Variant;parts=$parts;pause=$pauseHeader;pause_sha256=(Get-FileHash $pauseHeader).Hash.ToLowerInvariant();mutation_original=$wait;mutation_replacement=$replacement;
        host_source=$hostSource;host_source_sha256=$hostHash;host_original=$hostOriginal;host_original_sha256=(Get-FileHash $hostOriginal).Hash.ToLowerInvariant();
        host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();field_offset=$fieldOffset;field_marker=$anchor;field_inserted=$fields;include_changes=$includeChanges;
        adaptation='Only one extra indication-bank field declaration after ReturnRundown plus relative quoted includes made absolute. Every original hosted DDI/allocation/free/hook function body is unchanged; macro-based reference/NBL includes are separately bound to actual production headers.'}
}
function Build-IndicationPauseTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-indication-wait')][string]$Variant='current')
    if($script:SourceFingerprint -cnotmatch '^[0-9a-f]{64}$'){throw 'Indication/pause builder requires current shipping source fingerprint'}
    $extraction=Write-IndicationPauseParts -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($Variant -eq 'without-indication-wait'){$lines+='#define PB_INDICATION_WITHOUT_WAIT 1'}
    $lines+='#define PB_INDICATION_NBL_HOST "'+$extraction.host.Replace('\','/')+'"'
    $lines+='#define PB_REFERENCE_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_references.h').Replace('\','/')+'"'
    $lines+='#define PB_NBL_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h').Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_PAUSE_SOURCE "'+$extraction.pause.Replace('\','/')+'"'
    foreach($part in $extraction.parts|Where-Object name -ne 'pause'){$lines+='#define PB_INDICATION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\indication_pause_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'indication-pause.exe'
    $versionArguments=@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",$entry,
        'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')+$versionArguments+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;extractions=$extraction}
}
