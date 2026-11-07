# Reuse the frozen send ledger; select complete production Read/Describe/Send.
function Build-SendDescribeTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-describe-rollback','before-current-nb-attribution')][string]$Variant='current',
        [string]$BaselineDirectory='out\lifetime-edge-baseline\9dc04c7344b3')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Send Describe output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Send Describe requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false);$parts=@()
    foreach($name in @('read','describe','send')){
        $source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$path=Join-Path $Directory "$name-original.h";[IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes($source))
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=0;length=[IO.File]::ReadAllText($source).Length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $send=$parts|Where-Object name -eq 'send';$selected=[IO.File]::ReadAllText($send.path)
    $before='                } else (VOID)PbPoolTransition(&q->TxPool, &d.Token, PB_SLOT_RESERVED, PB_SLOT_FREE);'
    $after='                } else { /* Observer134: omit failed Describe reservation release. */ }'
    if([regex]::Matches($selected,[regex]::Escape($before)).Count -ne 1){throw 'Failed Describe rollback must be unique'}
    $mutation=$null;$historical=$null
    if($Variant -eq 'without-describe-rollback'){$mutation=@{original=$before;changed=$after;offset=$selected.IndexOf($before);length=$before.Length};$selected=$selected.Replace($before,$after)}
    if($Variant -eq 'before-current-nb-attribution'){
        $path=Join-Path (Resolve-Path -LiteralPath $BaselineDirectory).Path 'driver\v2\session_send.h';$selected=[IO.File]::ReadAllText($path)
        if((Get-FileHash $path).Hash.ToLowerInvariant() -ne '9ac96a6fbf63c04b0f1cbaf82ee457de4a7dc8644809a9b71dcb216bfd0ac225'){throw 'Historical Send must be complete reviewed baseline'}
        $historical=@{path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $selectedPath=Join-Path $Directory 'send-selected.h';[IO.File]::WriteAllText($selectedPath,$selected,$encoding)
    $types=@();foreach($name in @('nblchecksum','nbllso','nbluso')){
        $source=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\$name.h";$text=[IO.File]::ReadAllText($source)
        $m=[regex]::Match($text,'(?s)typedef struct _NDIS_.*?\n\} NDIS_[^;]+;');if(!$m.Success){throw "Missing WDK metadata type:$name"}
        $path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($path,$m.Value,$encoding);$types+=,$m.Value
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$m.Index;length=$m.Length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $typesPath=Join-Path $Directory 'metadata-types.h';[IO.File]::WriteAllText($typesPath,($types -join "`n"),$encoding)
    $source=Join-Path $script:RepoRoot 'tests\model\send_window_tests.c';$original=[IO.File]::ReadAllText($source)
    $first='static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)';$last='#ifndef PB_SEND_DIAGNOSTICS_SOURCE'
    if([regex]::Matches($original,[regex]::Escape($first)).Count -ne 1 -or [regex]::Matches($original,[regex]::Escape($last)).Count -ne 1){throw 'Describe stand-in markers changed'}
    $start=$original.IndexOf($first);$end=$original.IndexOf($last,$start);if($end -le $start){throw 'Invalid Describe stand-in span'}
    $standin=Join-Path $Directory 'standin-original.h';[IO.File]::WriteAllText($standin,$original.Substring($start,$end-$start),$encoding)
    $replacement=@'
#define PbV2Describe ActualDescribe
#include PB_SEND_DESCRIBE_ACTUAL
#undef PbV2Describe
static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)
{
    Describes++;ReadingPayload=TRUE;ULONG64 before=PayloadBytes;
    NDIS_STATUS status=ActualDescribe(ctx,nbl,nb,offset,d,bytes,failure);ReadingPayload=FALSE;
    if(status==NDIS_STATUS_RESOURCES){FailedBytes=(ULONG)(PayloadBytes-before);FailedOffset=d->DataOffset;FailedToken=d->Token;}
    return status;
}
'@
    $adapted=$original.Substring(0,$start)+$replacement+"`n"+$original.Substring($end)
    $transforms=@(
        @('#define NET_BUFFER_LIST_INFO(n,k) ((n)->Info[k])',@'
static VOID **ObserveInfo(NET_BUFFER_LIST *nbl,ULONG index){CHECK(index<4 && (index!=1 || HostedNdisVersion>=0x60053));if(index==1)FutureInfoReads++;return nbl->Info+index;}
#define NET_BUFFER_LIST_INFO(n,k) (*ObserveInfo(n,k))
#define PB_NBL_VERSION HostedNdisVersion
'@),
        @('Session->Region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);','Session->Region=AllocateRegion(Session->Layout.RegionSize);'),
        @('CHECK(VirtualFree(Session->Region,0,MEM_RELEASE));','ReleaseRegion(Session->Region);'))
    foreach($pair in $transforms){if([regex]::Matches($adapted,[regex]::Escape($pair[0])).Count -ne 1){throw "Host finite marker changed:$($pair[0])"};$adapted=$adapted.Replace($pair[0],$pair[1])}
    $adapted=[regex]::Replace($adapted,'"(\.\./[^"]+)"',{
        param($m);'"'+[IO.Path]::GetFullPath((Join-Path (Split-Path $source) $m.Groups[1].Value)).Replace('\','/')+'"'
    })
    $hostPath=Join-Path $Directory 'send-host.h';[IO.File]::WriteAllText($hostPath,$adapted,$encoding)
    $read=$parts|Where-Object name -eq 'read';$describe=$parts|Where-Object name -eq 'describe'
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($Variant -eq 'without-describe-rollback'){$lines+='#define PB_SEND_DESCRIBE_WITHOUT_ROLLBACK 1'}
    if($Variant -eq 'before-current-nb-attribution'){$lines+='#define PB_SEND_DESCRIBE_OLD_ATTRIBUTION 1'}
    $lines+='#define PB_SEND_DESCRIBE_TYPES "'+$typesPath.Replace('\','/')+'"'
    $lines+='#define PB_SEND_DESCRIBE_HOST "'+$hostPath.Replace('\','/')+'"'
    $lines+='#define PB_SEND_DESCRIBE_ACTUAL "'+$describe.path.Replace('\','/')+'"'
    $lines+='#define PB_SEND_SOURCE "'+$selectedPath.Replace('\','/')+'"'
    # The old host renames the current Read to ActualRead and counts its calls.
    # Select its identical extracted complete header through one include path.
    $oldRead=(Join-Path $script:RepoRoot 'driver\v2\session_read.h').Replace('\','/')
    $adapted=[IO.File]::ReadAllText($hostPath);if([regex]::Matches($adapted,[regex]::Escape('"'+$oldRead+'"')).Count -ne 1){throw 'Read include changed'}
    $adapted=$adapted.Replace('"'+$oldRead+'"','"'+$read.path.Replace('\','/')+'"');[IO.File]::WriteAllText($hostPath,$adapted,$encoding)
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\send_describe_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $exe=Join-Path $Directory 'send-describe.exe';$versionArguments=@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",('/I'+(Join-Path $script:RepoRoot 'driver\v2')),"/Fo$Directory\",$entry,'common\packet.c')+$versionArguments+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;ndebug=$true;pb_api_tests=$false;
        extractions=[ordered]@{parts=$parts;selected=$selectedPath;selected_sha256=(Get-FileHash $selectedPath).Hash.ToLowerInvariant();mutation=$mutation;historical=$historical;
            host_source=$source;host_original_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();host_adapted=$hostPath;host_adapted_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();
            standin=$standin;standin_sha256=(Get-FileHash $standin).Hash.ToLowerInvariant();standin_offset=$start;standin_length=$end-$start;standin_start=$first;standin_end=$last;
            types=$typesPath;types_sha256=(Get-FileHash $typesPath).Hash.ToLowerInvariant();adaptation='Complete Read/Describe/Send copied exactly. Frozen existing host only replaces its Describe stand-in, observes runtime OOB evaluation, tracks heap/region allocation, normalizes source includes and selects exact Read extraction. Old main is renamed with a token macro. Production ownership, validation, send windows, pool/queue and diagnostic code are unchanged. Negative only omits the unique failed-Describe reservation release.'}}
}
