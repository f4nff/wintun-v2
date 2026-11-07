function Build-RejectedReuse32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-partial-success')][string]$Variant='current')
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Rejected reuse output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Rejected reuse requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
    $records=[Collections.Generic.List[object]]::new()
    function Write-RrPart([string]$name,[string]$source,[int]$offset,[int]$length){
        $text=[IO.File]::ReadAllText($source);$path=Join-Path $Directory "$name-original.h"
        [IO.File]::WriteAllText($path,$text.Substring($offset,$length),$encoding)
        $r=[ordered]@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$offset;length=$length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()};$records.Add($r);return $r
    }
    $queueSource=Join-Path $script:RepoRoot 'api/v2/session_queue.h';$queueBody=[IO.File]::ReadAllText($queueSource)
    $queue=Write-RrPart 'queue' $queueSource 0 $queueBody.Length
    $start=$queueBody.IndexOf('static PB_RESULT WINAPI ReserveBatch(');$end=$queueBody.IndexOf('static PB_RESULT WINAPI CancelBatch(',$start)
    if($start -lt 0 -or $end -le $start){throw 'ReserveBatch span missing'}
    $reserve=Write-RrPart 'reserve' $queueSource $start ($end-$start)
    $original='    return QueueLeave(q, reserved ? PB_OK : result);';$replacement='    return QueueLeave(q, result);'
    if([regex]::Matches($queueBody,[regex]::Escape($original)).Count -ne 1 -or [regex]::Matches([IO.File]::ReadAllText($reserve.path),[regex]::Escape($original)).Count -ne 1){throw 'Reserve prefix-success tail not unique'}
    $selected=Join-Path $Directory 'queue-selected.h';$selectedBody=if($Variant -ceq 'current'){$queueBody}else{$queueBody.Replace($original,$replacement)}
    [IO.File]::WriteAllText($selected,$selectedBody,$encoding)
    $queueEntry=Join-Path $Directory 'queue-entry.h';$queueEntryBody='#define WaitForMultipleObjects ReuseWait'+"`n"+'#include "'+$selected.Replace('\','/')+'"'+"`n"+'#undef WaitForMultipleObjects'+"`n"
    [IO.File]::WriteAllText($queueEntry,$queueEntryBody,$encoding)
    $sessionSource=Join-Path $script:RepoRoot 'api/v2/session.c';$sessionBody=[IO.File]::ReadAllText($sessionSource)
    $session=Write-RrPart 'session' $sessionSource 0 $sessionBody.Length
    $sessionBody=[regex]::Replace($sessionBody,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq 'session_queue.h'){$queueEntry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $sessionSource) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
    $sessionPath=Join-Path $Directory 'session-current.c';[IO.File]::WriteAllText($sessionPath,$sessionBody,$encoding)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/native_reclaim_tests.c';$hostBody=[IO.File]::ReadAllText($hostSource)
    $hostPart=Write-RrPart 'host' $hostSource 0 $hostBody.Length
    $guard='adapter&&lists&&count==2&&!flags';$newGuard='adapter&&lists&&count>0&&count<=32&&!flags'
    if([regex]::Matches($hostBody,[regex]::Escape($guard)).Count -ne 1){throw 'Original indication guard not unique'}
    $hostBody=$hostBody.Replace($guard,$newGuard)
    $hostBody=[regex]::Replace($hostBody,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq '../../api/v2/session.c'){$sessionPath}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'native-host.h';[IO.File]::WriteAllText($hostPath,$hostBody,$encoding)
    $lateSource=Join-Path $script:RepoRoot 'tests/model/late_commit_tests.c';$lateBody=[IO.File]::ReadAllText($lateSource)
    $first=$lateBody.IndexOf('static VOID OpenPeer(');$last=$lateBody.IndexOf('static VOID RunLate(',$first)
    if($first -lt 0 -or $last -le $first){throw 'Original Open/Finish span missing'}
    $open=Write-RrPart 'open-close' $lateSource $first ($last-$first)
    $methods=[IO.File]::ReadAllText($open.path);$config='PB_CONFIG config={sizeof(config),queues,64,128,0,0};';$configAfter='PB_CONFIG config={sizeof(config),queues,32,32,0,0};'
    if([regex]::Matches($methods,[regex]::Escape($config)).Count -ne 1){throw 'Only original layout may change'}
    $methodsPath=Join-Path $Directory 'open-close.h';[IO.File]::WriteAllText($methodsPath,$methods.Replace($config,$configAfter),$encoding)
    $statusSource=Join-Path $script:SdkRoot "Include/$script:KitVersion/shared/ndis/status.h";$ntSource=Join-Path $script:SdkRoot "Include/$script:KitVersion/shared/ntstatus.h"
    $status=[regex]::Match([IO.File]::ReadAllText($statusSource),'(?m)^#define NDIS_STATUS_RESOURCES[^\r\n]*\r?\n');$nt=[regex]::Match([IO.File]::ReadAllText($ntSource),'(?m)^#define STATUS_INSUFFICIENT_RESOURCES[^\r\n]*\r?\n')
    if(!$status.Success -or !$nt.Success){throw 'Native resource status definitions missing'}
    $ntPart=Write-RrPart 'nt-resource' $ntSource $nt.Index $nt.Length;$statusPart=Write-RrPart 'ndis-resource' $statusSource $status.Index $status.Length
    $statusPath=Join-Path $Directory 'resource-status.h';[IO.File]::WriteAllText($statusPath,'typedef ULONG NDIS_STATUS;'+"`n"+$nt.Value+$status.Value,$encoding)
    $test=Join-Path $script:RepoRoot 'tests/model/rejected_reuse32_tests.c'
    $lines=@(('#define PB_NATIVE_RECLAIM_CLEANUP_SOURCE "'+(Join-Path $script:RepoRoot 'driver/v2/session_cleanup.h').Replace('\','/')+'"'),
        ('#define PB_REUSE_HOST "'+$hostPath.Replace('\','/')+'"'),('#define PB_REUSE_OPEN_CLOSE "'+$methodsPath.Replace('\','/')+'"'),
        ('#define PB_REUSE_RESOURCE_STATUS "'+$statusPath.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$IpVersion","/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Guarded){$flags+='/DPB_GUARD_TESTS=1'};if($Variant -ceq 'without-partial-success'){$flags+='/DPB_REJECTED_WITHOUT_PARTIAL_SUCCESS=1'}
    $exe=Join-Path $Directory 'rejected-reuse32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','common/flow.c','/link',"/out:$exe",'bcrypt.lib'))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;flags=$flags;
        parts=@($records.ToArray());selected=$selected;selected_sha256=(Get-FileHash $selected).Hash.ToLowerInvariant();queue_entry=$queueEntry;session=$sessionPath;host=$hostPath;open_close=$methodsPath;resource_status=$statusPath;
        mutation=@{original=$original;replacement=$replacement;offset=$queueBody.IndexOf($original);length=$original.Length};test=@{path=$test;sha256=(Get-FileHash $test).Hash.ToLowerInvariant()}}
}
