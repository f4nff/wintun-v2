function Build-CoalescePrefilter32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Coalesce prefilter output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Coalesce prefilter requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
    $test=Join-Path $script:RepoRoot 'tests/model/coalesce_prefilter32_tests.c';$header=Join-Path $script:RepoRoot 'driver/v2/session_coalesce.h';$packet=Join-Path $script:RepoRoot 'common/packet.c'
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    $lines+='#define PB_PREFILTER_HEADER "'+$header.Replace('\','/')+'"';$lines+='#define PB_PREFILTER_PACKET "'+$packet.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $dependencies=@('driver/v2/session_coalesce.h','driver/v2/session_state.h','common/v2_core.c','common/v2_core.h','common/packet.c','common/packet.h','common/coalesce.c','common/coalesce.h','common/flow.h','include/packetbridge_probe.h','include/packetbridge_v2_abi.h','include/packetbridge_types.h','include/packetbridge_version.h','tests/safety/guard_memory.h','wintun.props')
    $sources=@();foreach($relative in $dependencies){$source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source/'+$relative);New-Item -ItemType Directory -Path (Split-Path $copy) -Force|Out-Null;[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source));$sources+=@{relative=$relative;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'coalesce-prefilter32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/coalesce.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;sources=$sources;method='Actual whole helper/packet implementation, linked unchanged core/coalesce; lexical copy and core call observers forward real operations. Byte hits prohibit rereading a prefix within one attempt; repeated bounded flow lookahead is accounted separately. No production branch/owner repair, real driver/NDIS/old-machine execution or CPU timing. Guards cover declared private buffers and optional mapped region only.'}
}
