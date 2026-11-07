# Caller initializes common.ps1; no old out artifacts or device/driver access.
function Build-Tun32ReturnReuseTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'T32 return reuse output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'T32 return reuse requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
    $hosts=[ordered]@{'tests\model\return_reclaim_tests.c'='513c0bda45ef4b7ca59cb5e70ebf8e4b2dfcd93a54be3f791b38edfabb3f1653';'tests\model\kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'}
    $hostRecords=@();foreach($relative in $hosts.Keys){
        $source=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $source).Hash.ToLowerInvariant();if($hash -cne $hosts[$relative]){throw "Frozen T32 return host changed:$relative"}
        $snapshot=Join-Path $Directory ('original-'+[IO.Path]::GetFileName($relative));[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source))
        $hostRecords+=@{relative=$relative;source=$source;sha256=$hash;path=$snapshot;snapshot_sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant()}
    }
    $hostSource=Join-Path $script:RepoRoot 'tests\model\return_reclaim_tests.c';$body=[IO.File]::ReadAllText($hostSource)
    $before='CHECK(adapter && nbls && count==1 && !flags && !Indicated);';$after='CHECK(adapter && nbls && count && count<=PB_MAX_BATCH && !flags && !Indicated);'
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'Bounded indication observer adaptation is not unique'}
    $body=$body.Replace($before,$after);$includes=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'"([^"\r\n]*\.[ch])"',{param($m);$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value));$value='"'+$path.Replace('\','/')+'"';$includes.Add(@{original=$m.Value;adapted=$value});$value})
    $hostPath=Join-Path $Directory 'return-host.h';[IO.File]::WriteAllText($hostPath,$body,$encoding)
    $driver=Join-Path $script:RepoRoot 'driver\wintun.c';$text=[IO.File]::ReadAllText($driver);$first='static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;';$last='#define TUN_FORCE_UNREGISTRATION'
    foreach($marker in @($first,$last)){if([regex]::Matches($text,[regex]::Escape($marker)).Count -ne 1){throw 'Actual outer Return markers must be unique'}}
    $start=$text.IndexOf($first);$end=$text.IndexOf($last,$start);if($end -le $start){throw 'Actual outer Return span missing'}
    $outer=Join-Path $Directory 'outer-return.h';[IO.File]::WriteAllText($outer,$text.Substring($start,$end-$start),$encoding)
    $sources=@('driver\v2\session_state.h','driver\v2\session_progress.h','driver\v2\platform_nbl.h','driver\v2\session_fault.h','driver\v2\session_references.h','driver\v2\session_completion.h','driver\v2\session_return.h','driver\v2\session_nbl.h','driver\v2\session_coalesce.h','driver\v2\session_rx_stats.h','driver\v2\session_receive.h','driver\v2\session_drain.h','driver\v2\session_fault_drain.h','driver\v2\session_recycle.h','driver\v2\session_query.h','driver\v2\session_cleanup.h','api\v2\packetbridge.h','api\v2\session_state.h','api\v2\session_queue.h','common\v2_core.h','common\v2_core.c','common\platform.h','common\ethernet.c','common\ethernet.h','common\coalesce.c','common\coalesce.h','common\packet.c','common\packet.h','common\flow.h','common\return_diagnostics.h','include\packetbridge_version.h','include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_v2_abi.h','include\packetbridge_pool_pressure.h','tests\safety\guard_memory.h','wintun.props')
    $parts=@();foreach($relative in $sources){$source=Join-Path $script:RepoRoot $relative;$snapshot=Join-Path $Directory ('source-'+$relative.Replace('\','-'));[IO.File]::WriteAllBytes($snapshot,[IO.File]::ReadAllBytes($source));$parts+=@{relative=$relative;source=$source;compiled_path=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$snapshot;sha256=(Get-FileHash $snapshot).Hash.ToLowerInvariant()}}
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_return_reuse_tests.c';$entry=Join-Path $Directory 'entry.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_TEST_IP_VERSION '+$IpVersion;$lines+='#define PB_T32_RETURN_HOST "'+$hostPath.Replace('\','/')+'"';$lines+='#define PB_T32_OUTER_RETURN "'+$outer.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    [IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'tun32-return-reuse.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();extractions=@{hosts=$hostRecords;parts=$parts;host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();observer_original=$before;observer_adapted=$after;include_changes=@($includes.ToArray());outer=@{source=$driver;source_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();path=$outer;sha256=(Get-FileHash $outer).Hash.ToLowerInvariant();offset=$start;length=$end-$start;start_marker=$first;end_marker=$last}}}
}
