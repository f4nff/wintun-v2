# Actual session shipping branch with unchanged native-policy ownership ledger.
function Build-CompletionTurnTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-budget-continuation')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Completion turn directory must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Completion turn requires shipping source fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api\v2'
    $lifeSource=Join-Path $api 'session_lifecycle.h';$lifeOriginal=Join-Path $Directory 'lifecycle-original.h';$life=Join-Path $Directory 'session_lifecycle.h'
    [IO.File]::WriteAllBytes($lifeOriginal,[IO.File]::ReadAllBytes($lifeSource));[IO.File]::WriteAllBytes($life,[IO.File]::ReadAllBytes($lifeSource))
    $before='                if (batch == 3) SetEvent(s->ProgressEvent);';$after='                /* Observer132: omit budget continuation only. */'
    $text=[IO.File]::ReadAllText($lifeSource);if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw 'Completion budget continuation is not unique'}
    $mutation=$null
    if($Variant -ne 'current'){$mutation=@{function='LifecycleWorker bounded per-queue poll continuation';original=$before;changed=$after;offset=$text.IndexOf($before);length=$before.Length};[IO.File]::WriteAllText($life,$text.Replace($before,$after),$encoding)}
    $sessionSource=Join-Path $api 'session.c';$session=Join-Path $Directory 'session.c';$text=[IO.File]::ReadAllText($sessionSource)
    $text=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($m);$path=if($m.Groups[1].Value -eq 'session_lifecycle.h'){$life}else{[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'
    });[IO.File]::WriteAllText($session,$text,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#define PbQueuePop TurnPop
#undef WaitForSingleObject
#define WaitForSingleObject TurnWait
#undef AcquireSRWLockExclusive
#define AcquireSRWLockExclusive TurnAcquire
#include PB_COMPLETION_TURN_SESSION_SOURCE
#undef PbQueuePop
'@,$encoding)
    $lines=@('#define CT_FIXTURE 1');if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_COMPLETION_TURN_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $testSource=Join-Path $script:RepoRoot 'tests\model\completion_turn_tests.c';$lines+='#include "'+$testSource.Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Variant -ne 'current'){$flags+='/DCT_NEGATIVE=1'}
    $exe=Join-Path $Directory 'completion-turn.exe';$dll=Join-Path $Directory "completion-turn-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$testSource,'/link',"/out:$exe"))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$fixtureEntry,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "completion-turn-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($lifeSource,$lifeOriginal,$life,$sessionSource,$session,$entry,$fixtureEntry,$testSource,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'))|
        Select-Object -Unique|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;ndebug=$true;pb_api_tests=$false;sources=@($sources);mutation=$mutation;
        extraction=@{lifecycle_source=$lifeSource;lifecycle_original=$lifeOriginal;lifecycle=$life;session_source=$sessionSource;session=$session;entry=$entry;fixture_entry=$fixtureEntry;
            adaptation='Complete lifecycle is byte-identical current or omits one selected continuation line. Full session changes only quoted include paths. The unchanged native-policy host uses SDK pop/acquire/wait boundary wrappers; DataGate acquisition resets only test diagnostic counters, never production ownership.'}}
}
