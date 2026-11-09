# Actual bounded receive/coalesce/return composition. No packet or lock algorithm
# is copied into a replacement implementation; wrappers forward original calls.
function Build-Tun32CoalesceDrainTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,[ValidateSet(0,4,6)][int]$IpVersion=0)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'TUN32 coalesce drain output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'TUN32 coalesce drain requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
    $inputs=@();$parts=@();$generated=@();$hosts=[ordered]@{
        'tests\model\drain_boundary_tests.c'='50753c442cbeaaaa3956cc27d9884cb93fbe316a42953f387514f530466bc4a8'
        'tests\model\kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
    }
    foreach($relative in $hosts.Keys){$source=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $source).Hash.ToLowerInvariant();if($hash -cne $hosts[$relative]){throw "Frozen coalesce drain host changed:$relative"}
        $snapshot=Join-Path $Directory ('original-'+[IO.Path]::GetFileName($relative));[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source));$inputs+=@{relative=$relative;path=$source;sha256=$hash;snapshot=$snapshot;snapshot_sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant()}}
    $production=@('receive','nbl','coalesce','return','completion','references','recycle','rx_stats','drain','query','worker','fault_drain')|ForEach-Object {"driver\v2\session_$_.h"}
    $production+=@('driver\v2\platform_nbl.h','common\v2_core.c','common\coalesce.c','common\packet.c','common\ethernet.c','common\flow.c')
    foreach($relative in $production){$source=Join-Path $script:RepoRoot $relative;$snapshot=Join-Path $Directory ('production-'+$relative.Replace('\','-'));[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source));$parts+=@{relative=$relative;source=$source;compiled_path=$source;offset=0;length=[IO.File]::ReadAllText($source).Length;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$snapshot;sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant()}}
    $relative='tests\model\drain_boundary_tests.c';$base=Split-Path (Join-Path $script:RepoRoot $relative);$body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot $relative))
    $body=[regex]::Replace($body,'"([^"\r\n]*\.[ch])"',{param($m);$path=$m.Groups[1].Value;if(![IO.Path]::IsPathFullyQualified($path)){$path=[IO.Path]::GetFullPath((Join-Path $base $path))};'"'+$path.Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'drain-host.h';[IO.File]::WriteAllText($hostPath,$body,$encoding);$generated+=@{name='drain';path=$hostPath;sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant()}
    $nbl=@'
static VOID ObserveNblCopy(VOID *,const VOID *,SIZE_T);
static VOID **ObserveNblInfo(NET_BUFFER_LIST *,ULONG);
#pragma push_macro("RtlCopyMemory")
#pragma push_macro("NET_BUFFER_LIST_INFO")
#undef RtlCopyMemory
#undef NET_BUFFER_LIST_INFO
#define RtlCopyMemory ObserveNblCopy
#define NET_BUFFER_LIST_INFO(n,f) (*ObserveNblInfo(n,f))
#include PB_TUN32_COALESCE_NBL
#pragma pop_macro("NET_BUFFER_LIST_INFO")
#pragma pop_macro("RtlCopyMemory")
'@
    $nblPath=Join-Path $Directory 'nbl-observe.h';[IO.File]::WriteAllText($nblPath,$nbl,$encoding);$generated+=@{name='nbl-observer';path=$nblPath;sha256=(Get-FileHash $nblPath).Hash.ToLowerInvariant()}
    $receive=@'
static VOID ObserveSnapshot(VOID *,const VOID *,SIZE_T);
static PB_PACKET_STATUS ObserveBegin(PB_COALESCE *,uint8_t *,uint32_t,const uint8_t *,uint32_t);
static PB_PACKET_STATUS ObserveAppend(PB_COALESCE *,const uint8_t *,uint32_t);
static PB_PACKET_STATUS ObserveFinish(PB_COALESCE *);
#pragma push_macro("RtlCopyMemory")
#undef RtlCopyMemory
#define RtlCopyMemory ObserveSnapshot
#define PbCoalesceBegin ObserveBegin
#define PbCoalesceAppend ObserveAppend
#define PbCoalesceFinish ObserveFinish
#include PB_TUN32_COALESCE_CORE
#undef PbCoalesceBegin
#undef PbCoalesceAppend
#undef PbCoalesceFinish
#pragma pop_macro("RtlCopyMemory")
static PB_RESULT ObservePop(PB_QUEUE_VIEW *,PB_DESCRIPTOR *,uint32_t,uint32_t,uint32_t *);
static VOID ObserveIndicate(VOID *,NET_BUFFER_LIST *,ULONG,ULONG);
#define PbQueuePopBudget ObservePop
#define PbIndicateReceive ObserveIndicate
#include PB_TUN32_COALESCE_RECEIVE
#undef PbQueuePopBudget
#undef PbIndicateReceive
'@
    $receivePath=Join-Path $Directory 'receive-observe.h';[IO.File]::WriteAllText($receivePath,$receive,$encoding);$generated+=@{name='receive-observer';path=$receivePath;sha256=(Get-FileHash $receivePath).Hash.ToLowerInvariant()}
    $bindings=[ordered]@{PB_NBL_SOURCE=$nblPath;PB_RECEIVE_SOURCE=$receivePath;PB_TUN32_COALESCE_DRAIN_HOST=$hostPath;PB_TUN32_COALESCE_NBL=(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h');PB_TUN32_COALESCE_CORE=(Join-Path $script:RepoRoot 'driver\v2\session_coalesce.h');PB_TUN32_COALESCE_RECEIVE=(Join-Path $script:RepoRoot 'driver\v2\session_receive.h')}
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};if($IpVersion){$lines+='#define PB_TEST_IP_VERSION '+$IpVersion};foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_coalesce_drain_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'tun32-coalesce-drain.exe';$sources=@('common\v2_core.c','common\coalesce.c','common\packet.c','common\ethernet.c','common\flow.c')
    Invoke-BuildTool cl.exe ($flags+@($entry)+$sources+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;ip_version=$IpVersion;ndebug=$true;pb_api_tests=$false;arguments=$flags;test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();extractions=@{inputs=$inputs;parts=$parts;generated=$generated;method='Two whole frozen hosts, absolute-include projection only; one unused-main binding in the new test. Actual production headers compile from original paths and whole snapshots record their text. Explicit wrappers forward original snapshot/core begin/append/finish, NBL metadata, budget pop and indication functions without changing packet algorithms, locks or ownership.'}}
}
