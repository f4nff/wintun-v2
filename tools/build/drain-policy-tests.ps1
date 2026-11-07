# Build helper only; caller initializes common.ps1 and owns recording.
function Build-DrainPolicyTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-drain-wait-break')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Drain/policy output directory must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Drain/policy requires the actual source fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api\v2'
    $control=Join-Path $api 'session_control.h';$selected=Join-Path $Directory 'session_control.h'
    [IO.File]::WriteAllBytes($selected,[IO.File]::ReadAllBytes($control));$mutation=$null
    $text=[IO.File]::ReadAllText($control);$marker='static PB_RESULT WINAPI Drain(PB_SESSION *s)'
    if([regex]::Matches($text,[regex]::Escape($marker)).Count -ne 1){throw 'Drain function marker is not unique'}
    $start=$text.IndexOf($marker);$drain=$text.Substring($start)
    $originalDrain=Join-Path $Directory 'drain-original.c';[IO.File]::WriteAllText($originalDrain,$drain,$encoding)
    if($Variant -eq 'without-drain-wait-break') {
        $before='        if (!SleepConditionVariableSRW(&s->Changed, &s->State, INFINITE, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; break; }'
        $after='        if (!SleepConditionVariableSRW(&s->Changed, &s->State, INFINITE, 0) && GetLastError() != ERROR_TIMEOUT) { result = PB_CORRUPT; /* Observer123: omit only the failed-wait break. */ }'
        if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1 -or $text.IndexOf($before) -lt $start){throw 'Drain non-timeout wait failure branch is not unique'}
        [IO.File]::WriteAllText($selected,$text.Replace($before,$after),$encoding)
        $mutation=[ordered]@{function='Drain non-timeout condition-variable failure';offset=$text.IndexOf($before);length=$before.Length;original=$before;changed=$after;
            teardown='One failure is injected; erroneous retries call the real SDK and release/reacquire State. Observe retry before late policy release, then actual Drain completes; every public caller/owner joins before finalClose, real callback-thread joins/unload complete before123. No reference or Draining counter repair.'}
    }
    $original=Join-Path $api 'session.c';$text=[IO.File]::ReadAllText($original)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match);$include=if($match.Groups[1].Value -eq 'session_control.h'){$selected}else{[IO.Path]::GetFullPath((Join-Path $api $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#define WaitForMultipleObjects OwnerWait
#define SleepConditionVariableSRW DrainSleep
#include PB_DRAIN_POLICY_SESSION_SOURCE
#undef WaitForMultipleObjects
#undef SleepConditionVariableSRW
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_DRAIN_POLICY_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\drain_policy_fixture.c').Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Variant -eq 'without-drain-wait-break'){$flags+='/DPB_DRAIN_POLICY_WITHOUT_BREAK=1'}
    $exe=Join-Path $Directory 'drain-policy.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\drain_policy_tests.c','/link',"/out:$exe"))|Out-Host
    $dll=Join-Path $Directory "drain-policy-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$fixtureEntry,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "drain-policy-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($original,$control,$selected,$originalDrain,$session,$entry,$fixtureEntry,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\drain_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\drain_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\drain_policy_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;session=$session;entry=$entry;fixture_entry=$fixtureEntry;sources=@($sources);mutation=$mutation;
        extraction=[ordered]@{source=$control;source_sha256=(Get-FileHash $control).Hash.ToLowerInvariant();selected=$selected;selected_sha256=(Get-FileHash $selected).Hash.ToLowerInvariant();
            drain_original=$originalDrain;drain_sha256=(Get-FileHash $originalDrain).Hash.ToLowerInvariant();offset=$start;length=$drain.Length;
            adaptation='Current control header is byte-identical. Session changes only quoted include paths to absolute and selects the recorded control header. SDK wait/sleep observations reuse the unchanged native-policy fixture/ledger; negative additionally omits only the Drain failure break.'}}
}
