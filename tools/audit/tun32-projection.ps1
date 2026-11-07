. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
# Shared exact host/header projection for the32 packet hosted fixtures.
function Get-T32Generated($build,[string]$name){$found=@($build.extractions.generated|Where-Object {$_.name -ceq $name});if($found.Count -ne 1){throw "Nonunique TUN32 generated part:$name"};return $found[0]}
function Normalize-T32Includes([string]$body,[string]$relative){
    $base=Split-Path (Join-Path $script:RepoRoot $relative)
    return [regex]::Replace($body,'"([^"\r\n]*\.[ch])"',{param($m);$path=$m.Groups[1].Value;if(![IO.Path]::IsPathFullyQualified($path)){$path=[IO.Path]::GetFullPath((Join-Path $base $path))};'"'+$path.Replace('\','/')+'"'})
}
function Replace-T32Once([string]$body,[string]$before,[string]$after){if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw "Nonunique TUN32 allowed transform:$before"};return $body.Replace($before,$after)}
function Assert-T32Projection($build){
    $e=$build.extractions;$isSend=$build.direction -ceq 'send';$production=@('read','describe','send','receive','nbl','coalesce','return','completion','references','recycle','rx_stats')
    if(@($e.inputs).Count -ne 4 -or @($e.inputs.relative|Select-Object -Unique).Count -ne 4 -or @($e.parts).Count -ne $(if($isSend){14}else{11}) -or @($e.parts.name|Select-Object -Unique).Count -ne @($e.parts).Count){throw 'TUN32 source evidence shape changed'}
    foreach($inputRecord in $e.inputs){$source=Join-Path $script:RepoRoot $inputRecord.relative;$old=Join-Path $baseline $inputRecord.relative
        if($inputRecord.path -cne $source -or $inputRecord.sha256 -cne (Get-FileHash $old).Hash.ToLowerInvariant() -or $inputRecord.snapshot_sha256 -cne $inputRecord.sha256 -or (Get-FileHash $inputRecord.snapshot).Hash.ToLowerInvariant() -cne $inputRecord.sha256){throw 'Whole immutable host evidence changed'}
    }
    foreach($name in $production){$parts=@($e.parts|Where-Object {$_.name -ceq $name});if($parts.Count -ne 1){throw 'Missing actual header evidence'};$part=$parts[0]
        $source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$old=Join-Path $baseline "driver\v2\session_$name.h"
        Assert-NetworkDependencyFilePair -Before $old -Current $source -Relative "driver/v2/session_$name.h"
        if($part.source -cne $source -or $part.compiled_path -cne $source -or $part.offset -ne 0 -or $part.length -ne [IO.File]::ReadAllText($source).Length -or
            $part.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or $part.sha256 -cne $part.source_sha256 -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw "Complete actual header evidence changed:$name"}
    }
    foreach($record in $e.generated){if((Get-FileHash $record.path).Hash.ToLowerInvariant() -cne $record.sha256){throw 'Generated TUN32 artifact hash changed'}}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $family=if($null -eq $build.ip_version){0}else{[int]$build.ip_version}
    if($family -notin @(0,4,6)){throw 'Invalid TUN32 projection IP family'}
    if($family){$lines+='#define PB_TEST_IP_VERSION '+$family}
    if($isSend){
        if(@($e.generated).Count -ne 4){throw 'Send generated part set changed'};$lines+='#define PB_TUN32_SEND 1'
        $types=@();foreach($name in @('nblchecksum','nbllso','nbluso')){
            $part=$e.parts|Where-Object name -eq $name;$source=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\$name.h";$text=[IO.File]::ReadAllText($source)
            $match=[regex]::Match($text,'(?s)typedef struct _NDIS_.*?\n\} NDIS_[^;]+;')
            if(!$match.Success -or $part.source -cne $source -or $part.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or $part.offset -ne $match.Index -or $part.length -ne $match.Length -or
                [IO.File]::ReadAllText($part.path) -cne $match.Value -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw 'WDK concrete metadata span changed'};$types+=,$match.Value
        }
        $typePart=Get-T32Generated $build 'types';if([IO.File]::ReadAllText($typePart.path) -cne ($types -join "`n")){throw 'WDK metadata aggregate changed'}
        $body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\send_window_tests.c'));$hostPart=Get-T32Generated $build 'send-window'
        $first='static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)';$last='#ifndef PB_SEND_DIAGNOSTICS_SOURCE'
        $start=$body.IndexOf($first);$end=$body.IndexOf($last,$start)
        if($start -lt 0 -or $end -le $start -or $hostPart.standin_start -cne $first -or $hostPart.standin_end -cne $last -or $hostPart.standin_offset -ne $start -or $hostPart.standin_length -ne $end-$start){throw 'Frozen Describe stand-in boundary changed'}
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
        $body=Replace-T32Once $body '#define NET_BUFFER_LIST_INFO(n,k) ((n)->Info[k])' $info
        $body=Replace-T32Once $body 'Session->Region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);' 'Session->Region=AllocateRegion(Session->Layout.RegionSize);'
        $body=Replace-T32Once $body 'CHECK(VirtualFree(Session->Region,0,MEM_RELEASE));' 'ReleaseRegion(Session->Region);'
        if([IO.File]::ReadAllText($hostPart.path) -cne (Normalize-T32Includes $body 'tests\model\send_window_tests.c')){throw 'Send host changed outside finite actual-Describe/OOB/allocation/include projection'}
        $describePart=Get-T32Generated $build 'send-describe';$body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\send_describe_tests.c'))
        $body=Replace-T32Once $body 'int main(int argc,char **argv)' 'int UnusedTun32DescribeMain(int argc,char **argv)'
        if([IO.File]::ReadAllText($describePart.path) -cne (Normalize-T32Includes $body 'tests\model\send_describe_tests.c')){throw 'Actual Read/Describe host changed beyond unused main/includes'}
        $observer=Get-T32Generated $build 'send-observer';$expected=@'
static PB_RESULT T32Push(PB_QUEUE_VIEW *,const PB_DESCRIPTOR *,uint32_t,uint32_t *,uint32_t *);
static VOID T32Complete(NDIS_HANDLE,NET_BUFFER_LIST *);
#define PbQueuePush T32Push
#define PbCompleteSend T32Complete
#include PB_TUN32_SEND_ACTUAL
#undef PbQueuePush
#undef PbCompleteSend
'@
        if([IO.File]::ReadAllText($observer.path) -cne $expected){throw 'Actual Send forwarding observers changed'}
        $bindings=[ordered]@{PB_TUN32_SEND_HOST=$describePart.path;PB_SEND_DESCRIBE_HOST=$hostPart.path;PB_SEND_DESCRIBE_TYPES=$typePart.path;PB_SEND_DESCRIBE_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_describe.h');PB_SEND_SOURCE=$observer.path;PB_TUN32_SEND_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_send.h')}
    }else{
        if($build.direction -cne 'receive' -or @($e.generated).Count -ne 3){throw 'Receive generated part set changed'}
        $hostPart=Get-T32Generated $build 'drain';$body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\drain_boundary_tests.c'))
        if([IO.File]::ReadAllText($hostPart.path) -cne (Normalize-T32Includes $body 'tests\model\drain_boundary_tests.c')){throw 'Receive old host changed beyond source includes'}
        $observer=Get-T32Generated $build 'receive-observer';$expected=@'
static PB_RESULT T32Pop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t,uint32_t *);
static VOID T32Indicate(VOID *,NET_BUFFER_LIST *,ULONG,ULONG);
#define PbQueuePopBudget T32Pop
#define PbIndicateReceive T32Indicate
#include PB_TUN32_RECEIVE_ACTUAL
#undef PbQueuePopBudget
#undef PbIndicateReceive
'@
        if([IO.File]::ReadAllText($observer.path) -cne $expected){throw 'Receive forwarding observers changed'}
        $nbl=Get-T32Generated $build 'nbl-observer';$expected=@'
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
        if([IO.File]::ReadAllText($nbl.path) -cne $expected){throw 'NBL copy/header forwarding observers changed'}
        $bindings=[ordered]@{PB_TUN32_RECEIVE_HOST=$hostPart.path;PB_RECEIVE_SOURCE=$observer.path;PB_NBL_SOURCE=$nbl.path;PB_TUN32_RECEIVE_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_receive.h');PB_TUN32_NBL_ACTUAL=(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h')}
    }
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`n")+"`n") -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256 -or $build.test -cne $test -or $build.test_sha256 -cne (Get-FileHash $test).Hash.ToLowerInvariant()){throw 'Actual TUN32 compilation binding changed'}
    if(!$build.ndebug -or $build.pb_api_tests -or '/DNDEBUG' -notin $build.arguments -or "/volatile:$($build.mode)" -notin $build.arguments -or "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint" -notin $build.arguments -or @($build.arguments|Where-Object {$_ -match 'PB_API_TESTS'}).Count){throw 'TUN32 shipping flags changed'}
}
