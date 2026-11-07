# Build helper only; caller initializes common.ps1 and records execution.
function Build-PolicyShellTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-occupancy-check')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Policy shell directory must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Policy shell requires the actual source fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api\v2'
    $originalLife=Join-Path $api 'session_lifecycle.h';$selected=Join-Path $Directory 'session_lifecycle.h'
    [IO.File]::WriteAllBytes($selected,[IO.File]::ReadAllBytes($originalLife));$mutation=$null
    $text=[IO.File]::ReadAllText($originalLife)
    $first='static PB_RESULT WINAPI PollPolicy(PB_SESSION *s, uint64_t ticket, uint32_t timeout, PB_RESULT *operation)'
    $next='static PB_RESULT WINAPI GetCloseProgress(PB_SESSION *s, PB_CLOSE_PROGRESS *out)'
    foreach($marker in @($first,$next)){if([regex]::Matches($text,[regex]::Escape($marker)).Count -ne 1){throw 'Policy function extraction marker is not unique'}}
    $offset=$text.IndexOf($first);$end=$text.IndexOf($next,$offset);if($end -le $offset){throw 'Policy extraction bounds missing'}
    $poll=$text.Substring($offset,$end-$offset);$pollOriginal=Join-Path $Directory 'poll-original.c';[IO.File]::WriteAllText($pollOriginal,$poll,$encoding)
    $before='        if (!s->PolicyOccupied || ticket != s->Progress.PolicyRequested) { result = PB_STALE; break; }'
    $after='        if (ticket != s->Progress.PolicyRequested) { result = PB_STALE; break; }'
    if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1 -or !$poll.Contains($before)){throw 'PollPolicy occupied check is not unique'}
    if($Variant -eq 'without-occupancy-check') {
        [IO.File]::WriteAllText($selected,$text.Replace($before,$after),$encoding)
        $mutation=[ordered]@{function='PollPolicy loop occupied-and-ticket validation';original=$before;changed=$after;offset=$text.IndexOf($before);length=$before.Length;
            teardown='Completed ticket is consumed/forgotten only through actual APIs while a public owner sustains a Closed shell. Unexpected duplicate acknowledgements are observed; all pollers join before final successful Close, callback OS-thread joins and actual DLL unload complete before126. No replacement ticket or reference/counter repair.'}
    }
    $originalSession=Join-Path $api 'session.c';$text=[IO.File]::ReadAllText($originalSession)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match);$include=if($match.Groups[1].Value -eq 'session_lifecycle.h'){$selected}else{[IO.Path]::GetFullPath((Join-Path $api $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#define SleepConditionVariableSRW PolicySleep
#include PB_POLICY_SHELL_SESSION_SOURCE
#undef SleepConditionVariableSRW
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_POLICY_SHELL_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\policy_shell_fixture.c').Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Variant -eq 'without-occupancy-check'){$flags+='/DPB_POLICY_SHELL_WITHOUT_OCCUPANCY=1'}
    $exe=Join-Path $Directory 'policy-shell.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\policy_shell_tests.c','/link',"/out:$exe"))|Out-Host
    $dll=Join-Path $Directory "policy-shell-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$fixtureEntry,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "policy-shell-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($originalSession,$originalLife,$selected,$pollOriginal,$session,$entry,$fixtureEntry,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\policy_shell_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\policy_shell_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\policy_shell_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;session=$session;entry=$entry;fixture_entry=$fixtureEntry;sources=@($sources);mutation=$mutation;
        extraction=[ordered]@{source=$originalLife;source_sha256=(Get-FileHash $originalLife).Hash.ToLowerInvariant();selected=$selected;selected_sha256=(Get-FileHash $selected).Hash.ToLowerInvariant();
            poll_original=$pollOriginal;poll_sha256=(Get-FileHash $pollOriginal).Hash.ToLowerInvariant();offset=$offset;length=$poll.Length;first=$first;next=$next;
            adaptation='Current lifecycle is byte-identical; only complete session quoted includes become absolute/select the recorded lifecycle, plus one SDK sleep observation entry. The negative additionally omits only PolicyOccupied from the real PollPolicy condition, retaining ticket comparison.'}}
}
