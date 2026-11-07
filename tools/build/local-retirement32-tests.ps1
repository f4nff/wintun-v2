# Current shipping queue/lifecycle with an unchanged native-policy owner ledger.
function Build-LocalRetirement32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Local-retirement directory must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Local-retirement requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
    $api=Join-Path $script:RepoRoot 'api/v2';$sessionSource=Join-Path $api 'session.c';$session=Join-Path $Directory 'session.c'
    $body=[regex]::Replace([IO.File]::ReadAllText($sessionSource),'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value)).Replace('\','/')+'"'})
    [IO.File]::WriteAllText($session,$body,$utf8)
    $hostSource=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c';$hostHash='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -cne $hostHash){throw 'Frozen native-policy host changed; review reuse'}
    $hostOriginal=Join-Path $Directory 'native-policy-original.c';[IO.File]::WriteAllBytes($hostOriginal,[IO.File]::ReadAllBytes($hostSource))
    $entry=Join-Path $Directory 'session-entry.h';[IO.File]::WriteAllText($entry,@'
#define WaitForMultipleObjects LocalWait
#undef WaitForSingleObject
#define WaitForSingleObject LocalProgress
#include PB_LOCAL_RETIREMENT_SESSION_SOURCE
#undef WaitForMultipleObjects
'@,$utf8)
    $lines=@('#define LR_FIXTURE 1');if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_LOCAL_RETIREMENT_SESSION_SOURCE "'+$session.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $test=Join-Path $script:RepoRoot 'tests/model/local_retirement32_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$IpVersion")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'local-retirement32.exe';$dll=Join-Path $Directory 'local-retirement32.dll'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$test,'/link',"/out:$exe"))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$fixtureEntry,'common/v2_core.c','common/packet.c','common/flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory 'local-retirement32.lib')),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($sessionSource,$session,$hostSource,$hostOriginal,$entry,$fixtureEntry,$test,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'))|
        Select-Object -Unique|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;flags=$flags;ndebug=$true;pb_api_tests=$false;sources=@($sources);
        extraction=@{session_source=$sessionSource;session=$session;host=$hostSource;host_original=$hostOriginal;host_sha256=$hostHash;entry=$entry;fixture_entry=$fixtureEntry;test=$test;
            adaptation='Complete current session changes only quoted includes. Complete native-policy fixture is included unchanged and copied byte-for-byte. Actual SDK wait boundaries forward original helpers and SDK calls; no worker/lifecycle/queue body mutation or test-only API shortcut.'}}
}
