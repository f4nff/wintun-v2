# Actual source with finite include/SDK observer binding; no historical out needed.
function Build-PolicyAbandon32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Policy abandonment output directory must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Policy abandonment requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $utf8=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api/v2';$original=Join-Path $api 'session.c'
    $session=Join-Path $Directory 'session.c';$body=[IO.File]::ReadAllText($original)
    $adapted=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value)).Replace('\','/')+'"'})
    [IO.File]::WriteAllText($session,$adapted,$utf8)
    $entry=Join-Path $Directory 'session-entry.h'
    $lines=@('#define SleepConditionVariableSRW AbandonSleep',('#include "'+$session.Replace('\','/')+'"'),'#undef SleepConditionVariableSRW')
    [IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $test=Join-Path $script:RepoRoot 'tests/model/policy_abandon32_tests.c';$fixtureEntry=Join-Path $Directory 'fixture.c'
    $lines=@('#define P_FIXTURE 1',('#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$IpVersion","/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Guarded){$flags+='/DPB_GUARD_TESTS=1'}
    $program=Join-Path $Directory 'policy-abandon32.exe';$fixture=Join-Path $Directory 'policy-abandon32.dll'
    Invoke-BuildTool cl.exe ($flags+@($test,'/link',"/out:$program"))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@('/LD',$fixtureEntry,'common/v2_core.c','common/packet.c','common/flow.c','/link',"/out:$fixture",("/implib:"+(Join-Path $Directory 'fixture.lib')),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','/EXPORT:PbGetPacketInterface','bcrypt.lib'))|Out-Host
    $paths=@($original,$session,$entry,$fixtureEntry,$test,(Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'),
        (Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),(Join-Path $script:RepoRoot 'tests/safety/guard_memory.h'),
        (Join-Path $script:RepoRoot 'include/packetbridge_version.h'),(Join-Path $script:RepoRoot 'common/v2_core.c'),
        (Join-Path $script:RepoRoot 'common/packet.c'),(Join-Path $script:RepoRoot 'common/flow.c'))
    $sources=@($paths|ForEach-Object {[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}})
    return [ordered]@{path=$program;sha256=(Get-FileHash $program).Hash.ToLowerInvariant();fixture=$fixture;fixture_sha256=(Get-FileHash $fixture).Hash.ToLowerInvariant();
        ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;flags=$flags;sources=$sources;
        extraction=[ordered]@{source=$original;source_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();session=$session;session_sha256=(Get-FileHash $session).Hash.ToLowerInvariant();
            entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();fixture_entry=$fixtureEntry;fixture_entry_sha256=(Get-FileHash $fixtureEntry).Hash.ToLowerInvariant()};
        method='Whole unchanged native-policy resource ledger and actual shipping session. One lexical CV observer supplies a single explicit failure after native IO1 enters and gates two real old SDK pollers after wake with State restored on return. Actual synchronous SetPolicy auto-Forget, actual IO1 failure, ticket2 replacement, old Poll/Forget identity, Stop and second IO late success/failure are checked with actual32 Published and32 loans. One owner, sync caller and two pollers all join before final Close; all captured callback OS-thread handles join through real DLL detach/unload. Native IO/Active/capability data and CV failure are hosted defensive inputs, not actual SYS/NDIS/old-Windows execution. Guards retain inherited native core/slot/map coverage only. Counts concern APIs/ownership, not packet copy, native CPU or throughput.'}
}
