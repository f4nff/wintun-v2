function Build-Tun32MultiqueueDrainTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Multiqueue drain output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Multiqueue drain requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
    $hosts=[ordered]@{'tests/model/drain_boundary_tests.c'='4fd8afe2b998c6ca628ff05fa266b689e4602be4e68109a477cc5f3001fa7df5';'tests/model/kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'}
    $inputs=@();foreach($relative in $hosts.Keys){$source=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $source).Hash.ToLowerInvariant();if($hash -cne $hosts[$relative]){throw "Frozen multiqueue host differs: $relative"};$copy=Join-Path $Directory ('original-'+[IO.Path]::GetFileName($source));[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$inputs+=@{relative=$relative;source=$source;source_sha256=$hash;path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}}
    $source=Join-Path $script:RepoRoot 'tests/model/drain_boundary_tests.c';$body=[IO.File]::ReadAllText($source)
    $body=[regex]::Replace($body,'"([^"\r\n]*\.[ch])"',{param($m);'"'+[IO.Path]::GetFullPath((Join-Path (Split-Path $source) $m.Groups[1].Value)).Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'drain-host.h';[IO.File]::WriteAllText($hostPath,$body,$utf8)
    $observer=@'
static BOOLEAN NdrReceive(PB_KERNEL_QUEUE *);
static VOID NdrWait(ULONG,VOID **,int,int,int,BOOLEAN,VOID *,VOID *);
static VOID NdrExit(LONG);
#define PbV2ReceiveBatch NdrReceive
#define KeWaitForMultipleObjects NdrWait
#define PsTerminateSystemThread NdrExit
#include PB_NDR_WORKER
#undef PsTerminateSystemThread
#undef KeWaitForMultipleObjects
#undef PbV2ReceiveBatch
'@
    $worker=Join-Path $Directory 'worker-observe.h';[IO.File]::WriteAllText($worker,$observer,$utf8)
    $bindings=[ordered]@{PB_NDR_HOST=$hostPath;PB_WORKER_SOURCE=$worker;PB_NDR_WORKER=(Join-Path $script:RepoRoot 'driver/v2/session_worker.h')}
    $entry=Join-Path $Directory 'entry.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $test=Join-Path $script:RepoRoot 'tests/model/tun32_multiqueue_drain_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $production=@();foreach($relative in @(rg --files driver common include)){$actual=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source-'+$relative.Replace('\','-').Replace('/','-'));[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($actual));$production+=@{relative=$relative;source=$actual;source_sha256=(Get-FileHash $actual).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}}
    if($LASTEXITCODE){throw 'Cannot enumerate multiqueue production closure'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'tun32-multiqueue-drain.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;inputs=$inputs;host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();observer=$worker;observer_sha256=(Get-FileHash $worker).Hash.ToLowerInvariant();production=$production}
}
