function Build-ReceiveAdmission32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Receive admission output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Receive admission requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/drain_boundary_tests.c';$hostHash='50753c442cbeaaaa3956cc27d9884cb93fbe316a42953f387514f530466bc4a8'
    $kernel=Join-Path $script:RepoRoot 'tests/model/kernel_nbl_fixture.h';$kernelHash='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -cne $hostHash -or (Get-FileHash $kernel).Hash.ToLowerInvariant() -cne $kernelHash){throw 'Frozen admission DDI host changed'}
    $original=Join-Path $Directory 'drain-host-original.c';[IO.File]::WriteAllBytes($original,[IO.File]::ReadAllBytes($hostSource))
    $body=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value)).Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'drain-host.h';[IO.File]::WriteAllText($hostPath,$body,$utf8)
    $observer=Join-Path $Directory 'receive-observe.h';$lines=@('static VOID ObserveAdmissionComplete(PB_KERNEL_QUEUE *,const PB_DESCRIPTOR *,ULONG);','#define PbV2CompleteLocked ObserveAdmissionComplete',('#include "'+(Join-Path $script:RepoRoot 'driver/v2/session_receive.h').Replace('\','/')+'"'),'#undef PbV2CompleteLocked')
    [IO.File]::WriteAllText($observer,($lines -join "`n")+"`n",$utf8)
    $production=@();foreach($relative in @(rg --files driver common include)){
        $source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source/'+$relative);New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source))
        $production+=@{relative=$relative.Replace('\','/');source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}
    };if($LASTEXITCODE){throw 'Cannot enumerate admission production closure'}
    $test=Join-Path $script:RepoRoot 'tests/model/receive_admission32_tests.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    $bindings=[ordered]@{PB_ADMISSION_HOST=$hostPath;PB_RECEIVE_SOURCE=$observer;PB_WORKER_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_worker.h');PB_QUERY_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_query.h')}
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'};$lines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'receive-admission32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;production=$production;
        host_source=$hostSource;host_source_sha256=$hostHash;host_original=$original;host_original_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();observer=$observer;observer_sha256=(Get-FileHash $observer).Hash.ToLowerInvariant();kernel_source=$kernel;kernel_source_sha256=$kernelHash}
}
