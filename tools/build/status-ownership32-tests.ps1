# Actual source with finite include/SDK observer binding; no historical out needed.
function Build-StatusOwnership32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Status ownership output directory must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Status ownership requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $utf8=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api/v2';$original=Join-Path $api 'session.c'
    $session=Join-Path $Directory 'session.c';$body=[IO.File]::ReadAllText($original)
    $adapted=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value)).Replace('\','/')+'"'})
    [IO.File]::WriteAllText($session,$adapted,$utf8)
    $entry=Join-Path $Directory 'session-entry.h'
    $lines=@('#undef DeviceIoControl','#define DeviceIoControl ReplyIo',('#include "'+$session.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $test=Join-Path $script:RepoRoot 'tests/model/status_ownership32_tests.c';$fixtureEntry=Join-Path $Directory 'fixture.c'
    $lines=@('#define O_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$IpVersion","/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Guarded){$flags+='/DPB_GUARD_TESTS=1'}
    $program=Join-Path $Directory 'status-ownership32.exe';$fixture=Join-Path $Directory 'status-ownership32.dll'
    Invoke-BuildTool cl.exe ($flags+@($test,'/link',"/out:$program"))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@('/LD',$fixtureEntry,'common/v2_core.c','common/packet.c','common/flow.c','/link',"/out:$fixture",("/implib:"+(Join-Path $Directory 'fixture.lib')),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $paths=@($original,$session,$entry,$fixtureEntry,$test,(Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'),
        (Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),(Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),
        (Join-Path $script:RepoRoot 'include/packetbridge_version.h'),(Join-Path $script:RepoRoot 'common/v2_core.c'),
        (Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    $sources=@($paths|ForEach-Object {[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}})
    return [ordered]@{path=$program;sha256=(Get-FileHash $program).Hash.ToLowerInvariant();fixture=$fixture;fixture_sha256=(Get-FileHash $fixture).Hash.ToLowerInvariant();
        ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;flags=$flags;sources=$sources;
        extraction=[ordered]@{source=$original;source_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();session=$session;session_sha256=(Get-FileHash $session).Hash.ToLowerInvariant();
            entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();fixture_entry=$fixtureEntry;fixture_entry_sha256=(Get-FileHash $fixtureEntry).Hash.ToLowerInvariant()};
        method='Unmodified whole native-policy fixture/resource ledger includes actual shipping session with absolute includes and one DeviceIoControl observer. Actual 32 Reserve/Commit plus32 TX_READY imports/Receive loans, malformed public capability replies, stop/loan/QUERY/native-active independent barriers, trusted STOPPED reconciliation, dynamic callback/OS-thread joins and real DLL unload. IOCTL/active-reference values are hosted defensive inputs; no real SYS/NDIS or Windows7 runtime execution. Guard only original native core/slot objects and mapping; caller/SDK allocations retain their allocators. Counts are API/ownership observations, not CPU/throughput or facade payload-copy measurements.'}
}
