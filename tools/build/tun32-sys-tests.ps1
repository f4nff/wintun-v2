# Ordinary32 packet accounting through unchanged production and frozen hosts.
function Build-Tun32SysTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('send','receive')][string]$Direction='send',[ValidateSet(0,4,6)][int]$IpVersion=0)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'TUN32 SYS output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'TUN32 SYS requires current shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
    $inputs=@();$parts=@();$generated=@()
    $hosts=[ordered]@{
        'tests\model\send_window_tests.c'='a6df5a009da56d1617af06a4f8904055c19fc9c06b78301e680d25466bd1dcfe'
        'tests\model\send_describe_tests.c'='48c1989e129f8b315a6525691b45a4fc6d026399da848dbb933f6f96ac26076e'
        'tests\model\drain_boundary_tests.c'='50753c442cbeaaaa3956cc27d9884cb93fbe316a42953f387514f530466bc4a8'
        'tests\model\kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
    }
    foreach($relative in $hosts.Keys){$source=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $source).Hash.ToLowerInvariant()
        if($hash -cne $hosts[$relative]){throw "Frozen TUN32 host changed:$relative"}
        $snapshot=Join-Path $Directory ('original-'+[IO.Path]::GetFileName($relative));[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source))
        $inputs+=@{relative=$relative;path=$source;sha256=$hash;snapshot=$snapshot;snapshot_sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant()}
    }
    foreach($name in @('read','describe','send','receive','nbl','coalesce','return','completion','references','recycle','rx_stats')){
        $source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$snapshot=Join-Path $Directory "production-$name.h";[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source))
        $parts+=@{name=$name;source=$source;compiled_path=$source;offset=0;length=[IO.File]::ReadAllText($source).Length;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$snapshot;sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant()}
    }
    function Convert-T32Includes([string]$text,[string]$relative){
        $base=Split-Path (Join-Path $script:RepoRoot $relative)
        return [regex]::Replace($text,'"([^"\r\n]*\.[ch])"',{param($m);$path=$m.Groups[1].Value;if(![IO.Path]::IsPathFullyQualified($path)){$path=[IO.Path]::GetFullPath((Join-Path $base $path))};'"'+$path.Replace('\','/')+'"'})
    }
    function Change-T32Once([string]$text,[string]$before,[string]$after){
        if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw "Nonunique TUN32 projection:$before"};return $text.Replace($before,$after)
    }
    $entryLines=@();if($Guarded){$entryLines+='#define PB_GUARD_TESTS 1'};if($IpVersion){$entryLines+='#define PB_TEST_IP_VERSION '+$IpVersion}
    if($Direction -eq 'send'){
        $entryLines+='#define PB_TUN32_SEND 1'
        $types=@();foreach($name in @('nblchecksum','nbllso','nbluso')){
            $source=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\$name.h";$text=[IO.File]::ReadAllText($source)
            $match=[regex]::Match($text,'(?s)typedef struct _NDIS_.*?\n\} NDIS_[^;]+;');if(!$match.Success){throw 'WDK metadata type not found'}
            $path=Join-Path $Directory "$name.h";[IO.File]::WriteAllText($path,$match.Value,$encoding);$types+=,$match.Value
            $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$match.Index;length=$match.Length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
        }
        $typesPath=Join-Path $Directory 'metadata-types.h';[IO.File]::WriteAllText($typesPath,($types -join "`n"),$encoding);$generated+=@{name='types';path=$typesPath;sha256=(Get-FileHash $typesPath).Hash.ToLowerInvariant()}
        $relative='tests\model\send_window_tests.c';$body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot $relative))
        $first='static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)';$last='#ifndef PB_SEND_DIAGNOSTICS_SOURCE'
        $start=$body.IndexOf($first);$end=$body.IndexOf($last,$start)
        if($start -lt 0 -or $end -le $start -or [regex]::Matches($body,[regex]::Escape($first)).Count -ne 1 -or [regex]::Matches($body,[regex]::Escape($last)).Count -ne 1){throw 'Old Describe stand-in markers changed'}
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
        $body=$body.Substring(0,$start)+$replacement+"`n"+$body.Substring($end)
        $info=@'
static VOID **ObserveInfo(NET_BUFFER_LIST *nbl,ULONG index){CHECK(index<4 && (index!=1 || HostedNdisVersion>=0x60053));if(index==1)FutureInfoReads++;return nbl->Info+index;}
#define NET_BUFFER_LIST_INFO(n,k) (*ObserveInfo(n,k))
#define PB_NBL_VERSION HostedNdisVersion
'@
        $body=Change-T32Once $body '#define NET_BUFFER_LIST_INFO(n,k) ((n)->Info[k])' $info
        $body=Change-T32Once $body 'Session->Region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);' 'Session->Region=AllocateRegion(Session->Layout.RegionSize);'
        $body=Change-T32Once $body 'CHECK(VirtualFree(Session->Region,0,MEM_RELEASE));' 'ReleaseRegion(Session->Region);'
        $sendHost=Join-Path $Directory 'send-window-host.h';[IO.File]::WriteAllText($sendHost,(Convert-T32Includes $body $relative),$encoding);$generated+=@{name='send-window';path=$sendHost;sha256=(Get-FileHash $sendHost).Hash.ToLowerInvariant();standin_start=$first;standin_end=$last;standin_offset=$start;standin_length=$end-$start}
        $relative='tests\model\send_describe_tests.c';$body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot $relative))
        $body=Change-T32Once $body 'int main(int argc,char **argv)' 'int UnusedTun32DescribeMain(int argc,char **argv)'
        $describeHost=Join-Path $Directory 'send-describe-host.c';[IO.File]::WriteAllText($describeHost,(Convert-T32Includes $body $relative),$encoding);$generated+=@{name='send-describe';path=$describeHost;sha256=(Get-FileHash $describeHost).Hash.ToLowerInvariant()}
        $observe=@'
static PB_RESULT T32Push(PB_QUEUE_VIEW *,const PB_DESCRIPTOR *,uint32_t,uint32_t *,uint32_t *);
static VOID T32Complete(NDIS_HANDLE,NET_BUFFER_LIST *);
#define PbQueuePush T32Push
#define PbCompleteSend T32Complete
#include PB_TUN32_SEND_ACTUAL
#undef PbQueuePush
#undef PbCompleteSend
'@
        $observePath=Join-Path $Directory 'send-observe.h';[IO.File]::WriteAllText($observePath,$observe,$encoding);$generated+=@{name='send-observer';path=$observePath;sha256=(Get-FileHash $observePath).Hash.ToLowerInvariant()}
        $bindings=[ordered]@{PB_TUN32_SEND_HOST=$describeHost;PB_SEND_DESCRIBE_HOST=$sendHost;PB_SEND_DESCRIBE_TYPES=$typesPath;PB_SEND_DESCRIBE_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_describe.h');PB_SEND_SOURCE=$observePath;PB_TUN32_SEND_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_send.h')}
        foreach($name in $bindings.Keys){$entryLines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
        $compileSources=@('common\packet.c')
    }else{
        $relative='tests\model\drain_boundary_tests.c';$drainHost=Join-Path $Directory 'drain-host.h'
        [IO.File]::WriteAllText($drainHost,(Convert-T32Includes ([IO.File]::ReadAllText((Join-Path $script:RepoRoot $relative))) $relative),$encoding);$generated+=@{name='drain';path=$drainHost;sha256=(Get-FileHash $drainHost).Hash.ToLowerInvariant()}
        $receive=@'
static PB_RESULT T32Pop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t,uint32_t *);
static VOID T32Indicate(VOID *,NET_BUFFER_LIST *,ULONG,ULONG);
#define PbQueuePopBudget T32Pop
#define PbIndicateReceive T32Indicate
#include PB_TUN32_RECEIVE_ACTUAL
#undef PbQueuePopBudget
#undef PbIndicateReceive
'@
        $receivePath=Join-Path $Directory 'receive-observe.h';[IO.File]::WriteAllText($receivePath,$receive,$encoding);$generated+=@{name='receive-observer';path=$receivePath;sha256=(Get-FileHash $receivePath).Hash.ToLowerInvariant()}
        $nbl=@'
static VOID T32NblCopy(VOID *,const VOID *,SIZE_T);
static VOID T32Ethernet(UCHAR *,const UCHAR *,const UCHAR *,const UCHAR *,uint32_t);
#pragma push_macro("RtlCopyMemory")
#undef RtlCopyMemory
#define RtlCopyMemory T32NblCopy
#define PbEthernetHeader T32Ethernet
#include PB_TUN32_NBL_ACTUAL
#undef PbEthernetHeader
#pragma pop_macro("RtlCopyMemory")
'@
        $nblPath=Join-Path $Directory 'nbl-observe.h';[IO.File]::WriteAllText($nblPath,$nbl,$encoding);$generated+=@{name='nbl-observer';path=$nblPath;sha256=(Get-FileHash $nblPath).Hash.ToLowerInvariant()}
        $bindings=[ordered]@{PB_TUN32_RECEIVE_HOST=$drainHost;PB_RECEIVE_SOURCE=$receivePath;PB_NBL_SOURCE=$nblPath;PB_TUN32_RECEIVE_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_receive.h');PB_TUN32_NBL_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h')}
        foreach($name in $bindings.Keys){$entryLines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
        $compileSources=@('common\v2_core.c','common\coalesce.c','common\packet.c','common\ethernet.c','common\flow.c')
    }
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c';$entryLines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($entryLines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",('/I'+(Join-Path $script:RepoRoot 'driver\v2')),('/I'+(Join-Path $script:RepoRoot 'tests\model')),"/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory "tun32-$Direction.exe";Invoke-BuildTool cl.exe ($flags+@($entry)+$compileSources+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();direction=$Direction;mode=$Mode;guarded=[bool]$Guarded;ip_version=$IpVersion;ndebug=$true;pb_api_tests=$false;arguments=$flags;
        test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();extractions=@{inputs=$inputs;parts=$parts;generated=$generated;
            method='Four whole frozen hosts and whole original production snapshots are hash-recorded. Production headers compile directly from unchanged original paths. Send reuses the actual Read/Describe fixture through its existing finite stand-in/OOB/allocation projection, an outer unused-main rename and absolute includes. Explicit wrappers forward every production push/complete/pop/indicate/copy/header call; receive only selects these include wrappers and absolute original drain-host includes. No production synchronization or packet algorithm is replaced.'}}
}
