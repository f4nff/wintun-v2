# Build one C source as the host and fixture, reusing unchanged ownership ledgers.
function Build-FacadeBackpressureTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-pending-wait-filter')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Backpressure directory must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Backpressure requires shipping source fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api';$v2=Join-Path $api 'v2'
    function Project-Backpressure([string]$text,[string]$base){return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
    $workerSource=Join-Path $api 'compat_worker.h';$workerOriginal=Join-Path $Directory 'worker-original.h'
    [IO.File]::WriteAllBytes($workerOriginal,[IO.File]::ReadAllBytes($workerSource));$text=[IO.File]::ReadAllText($workerSource)
    $before='            result = PbCompatWait(q, s->Wake, !pending.Count, INFINITE);'
    $after='            result = PbCompatWait(q, s->Wake, TRUE, INFINITE);'
    if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw 'Pending wait filter is not unique'}
    $mutation=$null
    if($Variant -eq 'without-pending-wait-filter'){
        $mutation=@{function='CompatWorker pending wait selection';offset=$text.IndexOf($before);length=$before.Length;original=$before;changed=$after}
        $text=$text.Replace($before,$after)
    }
    $worker=Join-Path $Directory 'compat_worker.h';[IO.File]::WriteAllText($worker,(Project-Backpressure $text $api),$encoding)
    $legacySource=Join-Path $api 'session.c';$legacy=Join-Path $Directory 'legacy-session.c'
    $text=[regex]::Replace([IO.File]::ReadAllText($legacySource),'(?m)^#include "([^"]+)"',{
        param($m);$path=if($m.Groups[1].Value -eq 'compat_worker.h'){$worker}else{[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'
    });[IO.File]::WriteAllText($legacy,$text,$encoding)
    $nativeSource=Join-Path $v2 'session.c';$native=Join-Path $Directory 'native-session.c'
    [IO.File]::WriteAllText($native,(Project-Backpressure ([IO.File]::ReadAllText($nativeSource)) $v2),$encoding)
    $baseSource=Join-Path $script:RepoRoot 'tests\safety\facade_stop_fixture.c'
    $baseHash='42ed2b2b099bca8b6d5be8c8d6fb843ee529ad8be5549b0643899d813bc9fcc3'
    if((Get-FileHash $baseSource).Hash.ToLowerInvariant() -ne $baseHash){throw 'Frozen facade host changed; review reuse'}
    $baseOriginal=Join-Path $Directory 'facade-original.c';[IO.File]::WriteAllBytes($baseOriginal,[IO.File]::ReadAllBytes($baseSource))
    $base=Join-Path $Directory 'facade-host.h';[IO.File]::WriteAllText($base,(Project-Backpressure ([IO.File]::ReadAllText($baseSource)) (Split-Path $baseSource)),$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#undef DeviceIoControl
#define DeviceIoControl FacadeIo
#define WaitForMultipleObjects BackWait
#define PbQueueArm BackArm
#include PB_FACADE_NATIVE_SOURCE
#undef WaitForMultipleObjects
#undef PbQueueArm
'@,$encoding)
    $lines=@('#define BP_FIXTURE 1');if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_FACADE_NATIVE_SOURCE "'+$native.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#define PB_FACADE_LEGACY_SOURCE "'+$legacy.Replace('\','/')+'"'
    $lines+='#define PB_BACKPRESSURE_BASE "'+$base.Replace('\','/')+'"'
    $testSource=Join-Path $script:RepoRoot 'tests\model\facade_backpressure_tests.c';$lines+='#include "'+$testSource.Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/I$api")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Variant -ne 'current'){$flags+='/DBP_NEGATIVE=1'}
    $exe=Join-Path $Directory 'facade-backpressure.exe';$dll=Join-Path $Directory "facade-backpressure-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$testSource,'/link',"/out:$exe"))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$fixtureEntry,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "facade-backpressure-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($workerSource,$workerOriginal,$worker,$legacySource,$legacy,$nativeSource,$native,$baseSource,$baseOriginal,$base,$entry,$fixtureEntry,$testSource,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\facade_stop_fixture.h'))|Select-Object -Unique|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;ndebug=$true;pb_api_tests=$false;sources=@($sources);mutation=$mutation;
        extraction=@{worker_source=$workerSource;worker_original=$workerOriginal;worker=$worker;legacy_source=$legacySource;legacy=$legacy;native_source=$nativeSource;native=$native;
            base_source=$baseSource;base_source_sha256=$baseHash;base_original=$baseOriginal;base=$base;entry=$entry;fixture_entry=$fixtureEntry;
            adaptation='Frozen complete facade/native-policy ledgers are unchanged. Only quoted includes become absolute; legacy selects worker byte body, with one negative receive-filter omission. Native SDK wait/arm entry observes actual functions. Test source compiles as host and DLL fixture with NDEBUG shipping branch; no old fixture body is adapted.'}}
}
