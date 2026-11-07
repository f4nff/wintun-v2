# Build helper only; common.ps1 initializes the compiler and version arguments.
function Build-FacadeStopTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-pending-return')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Facade output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api';$v2=Join-Path $api 'v2'
    $workerOriginal=Join-Path $api 'compat_worker.h';$worker=Join-Path $Directory 'compat_worker.h'
    [IO.File]::WriteAllBytes($worker,[IO.File]::ReadAllBytes($workerOriginal));$mutation=$null
    if($Variant -eq 'without-pending-return') {
        $text=[IO.File]::ReadAllText($workerOriginal);$line='        CompatReturnPending(s, q, &pending);'
        if([regex]::Matches($text,[regex]::Escape($line)).Count -ne 1){throw 'Worker pending-return call is not unique'}
        $replacement='        (void)&CompatReturnPending; /* Observer mutation: pending return omitted. */'
        [IO.File]::WriteAllText($worker,$text.Replace($line,$replacement),$encoding)
        $mutation=[ordered]@{function='CompatWorker';removed=$line;replacement=$replacement;
            teardown='Close boundary witnesses63 remaining loans; the same queue owner uses actual ReleaseBatch with producer-known tokens before actual Close5000/ReleaseSession;116 only after cleanup and unload.'}
    }
    $legacyOriginal=Join-Path $api 'session.c';$nativeOriginal=Join-Path $v2 'session.c'
    $legacy=Join-Path $Directory 'legacy-session.c';$native=Join-Path $Directory 'native-session.c'
    $text=[IO.File]::ReadAllText($legacyOriginal)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($m);$include=if($m.Groups[1].Value -eq 'compat_worker.h'){$worker}else{[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    });[IO.File]::WriteAllText($legacy,$generated,$encoding)
    $text=[IO.File]::ReadAllText($nativeOriginal)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $v2 $m.Groups[1].Value)).Replace('\','/')+'"'})
    [IO.File]::WriteAllText($native,$generated,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#undef DeviceIoControl
#define DeviceIoControl FacadeIo
#define WaitForMultipleObjects FacadeWait
#include PB_FACADE_NATIVE_SOURCE
#undef WaitForMultipleObjects
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_FACADE_NATIVE_SOURCE "'+$native.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#define PB_FACADE_LEGACY_SOURCE "'+$legacy.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\facade_stop_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($source,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/I$api")+$script:VersionArguments
    $exe=Join-Path $Directory 'facade-stop.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\facade_stop_tests.c','/link',"/out:$exe"))|Out-Host
    $dll=Join-Path $Directory "facade-stop-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "facade-stop-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $bound=@($legacyOriginal,$nativeOriginal,$workerOriginal,$legacy,$native,$worker,$entry,$source,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\facade_stop_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\facade_stop_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\facade_stop_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;sources=@($bound);mutation=$mutation;
        extraction=[ordered]@{legacy_source=$legacyOriginal;legacy=$legacy;native_source=$nativeOriginal;native=$native;
            worker_source=$workerOriginal;worker_source_sha256=(Get-FileHash $workerOriginal).Hash.ToLowerInvariant();worker=$worker;worker_sha256=(Get-FileHash $worker).Hash.ToLowerInvariant();
            entry=$entry;shim=$source;adaptation='Only relative include paths become absolute; legacy compat_worker include selects the recorded byte-copy/mutation. PB_COMPAT_OPEN remains its production default. Factory table wrappers retain actual Open/Close and BeginStop; a post-BeginStop gate forces the early loan63 park, and CloseEntered gates final observations.'}}
}
