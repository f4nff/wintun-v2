# Current shipping source only; the frozen facade/native-policy ledgers retain
# their original allocation, module, pool and callback thread accounting.
function Build-DuplexBudget32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Duplex directory must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Duplex requires the shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $utf8=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api';$v2=Join-Path $api 'v2'
    function Convert-DpIncludes([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
    $legacySource=Join-Path $api 'session.c';$legacy=Join-Path $Directory 'legacy-session.c'
    $legacyBody=Convert-DpIncludes ([IO.File]::ReadAllText($legacySource)) $api
    [IO.File]::WriteAllText($legacy,"#define memcpy BudgetCopy`n"+$legacyBody+"`n#undef memcpy`n",$utf8)
    $nativeSource=Join-Path $v2 'session.c';$native=Join-Path $Directory 'native-session.c'
    [IO.File]::WriteAllText($native,(Convert-DpIncludes ([IO.File]::ReadAllText($nativeSource)) $v2),$utf8)
    $baseSource=Join-Path $script:RepoRoot 'tests/safety/facade_stop_fixture.c';$baseHash='42ed2b2b099bca8b6d5be8c8d6fb843ee529ad8be5549b0643899d813bc9fcc3'
    $nativeHost=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c';$nativeHostHash='6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
    if((Get-FileHash $baseSource).Hash.ToLowerInvariant() -cne $baseHash -or (Get-FileHash $nativeHost).Hash.ToLowerInvariant() -cne $nativeHostHash){throw 'Frozen duplex host changed; review reuse'}
    $baseOriginal=Join-Path $Directory 'facade-original.c';[IO.File]::WriteAllBytes($baseOriginal,[IO.File]::ReadAllBytes($baseSource))
    $base=Join-Path $Directory 'facade-host.h';[IO.File]::WriteAllText($base,(Convert-DpIncludes ([IO.File]::ReadAllText($baseSource)) (Split-Path $baseSource)),$utf8)
    $entry=Join-Path $Directory 'session-entry.h';[IO.File]::WriteAllText($entry,@'
#undef DeviceIoControl
#define DeviceIoControl FacadeIo
#define WaitForMultipleObjects BudgetWait
#include PB_FACADE_NATIVE_SOURCE
#undef WaitForMultipleObjects
'@,$utf8)
    $lines=@('#define B_FIXTURE 1');if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_FACADE_NATIVE_SOURCE "'+$native.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#define PB_FACADE_LEGACY_SOURCE "'+$legacy.Replace('\','/')+'"'
    $lines+='#define PB_BUDGET_BASE "'+$base.Replace('\','/')+'"'
    $test=Join-Path $script:RepoRoot 'tests/model/duplex_budget32_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$IpVersion","/I$api")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'duplex-budget32.exe';$dll=Join-Path $Directory 'duplex-budget32.dll'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$test,'/link',"/out:$exe"))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$fixtureEntry,'common/v2_core.c','common/packet.c','common/flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory 'duplex-budget32.lib')),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($legacySource,$legacy,$nativeSource,$native,$baseSource,$baseOriginal,$base,$nativeHost,$entry,$fixtureEntry,$test,
        (Join-Path $api 'compat_worker.h'),(Join-Path $api 'compat_pump.h'),(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests/model/facade_stop_fixture.h'))|Select-Object -Unique|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;flags=$flags;ndebug=$true;pb_api_tests=$false;sources=@($sources);
        extraction=@{legacy_source=$legacySource;legacy=$legacy;native_source=$nativeSource;native=$native;base_source=$baseSource;base_source_sha256=$baseHash;
            base_original=$baseOriginal;base=$base;native_host=$nativeHost;native_host_sha256=$nativeHostHash;entry=$entry;fixture_entry=$fixtureEntry;test=$test;
            adaptation='Complete frozen hosts unchanged; only quoted includes become absolute. Native actual wait observer forwards SDK/body. Legacy-only memcpy wrapper forwards once and counts only marked payloads crossing mapping/private-ring ranges. Actual worker, default Open and native queue/lifecycle are unchanged.'}}
}
