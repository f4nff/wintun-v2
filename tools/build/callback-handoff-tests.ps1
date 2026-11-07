# Build helper only: caller initializes common.ps1 and owns execution/recording.
function Build-CallbackHandoffTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','missing-first-work-pin')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $apiDirectory=Join-Path $script:RepoRoot 'api\v2'
    $lifecycle=Join-Path $apiDirectory 'session_lifecycle.h'
    $selected=$lifecycle;$encoding=[Text.UTF8Encoding]::new($false)
    if($Variant -eq 'missing-first-work-pin') {
        $body=[IO.File]::ReadAllText($lifecycle)
        $anchor='s->Scheduled = TRUE; Retain(s); SubmitThreadpoolWork(s->Work);'
        if(([regex]::Matches($body,[regex]::Escape($anchor))).Count -ne 1){throw 'Missing unique ScheduleLocked work owner'}
        # Delete exactly the first policy work pin, not every queued owner.
        # Negative-only repair after observing refs=2 permits safe teardown.
        $mutation='s->Scheduled = TRUE; if (!(s->Progress.PolicyRequested == 1 && s->Progress.Phase == PB_LIFE_RUNNING)) Retain(s); SubmitThreadpoolWork(s->Work);'
        $selected=Join-Path $Directory 'lifecycle-missing-first-work-pin.h'
        [IO.File]::WriteAllText($selected,$body.Replace($anchor,$mutation),$encoding)
    }
    $sessionBody=[IO.File]::ReadAllText((Join-Path $apiDirectory 'session.c'))
    $generated=[regex]::Replace($sessionBody,'(?m)^#include "([^"]+)"',{
        param($match)
        $include=if($match.Groups[1].Value -eq 'session_lifecycle.h'){$selected}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($Variant -ne 'current'){$lines+='#define PB_CALLBACK_NEGATIVE 1'}
    $lines+='#define PB_CALLBACK_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\callback_handoff_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'callback-handoff.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\callback_handoff_tests.c','/link',"/out:$program")) | Out-Host
    $dll=Join-Path $Directory "callback-$Variant.dll"
    $support=@('common\v2_core.c','common\packet.c','common\flow.c')
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source)+$support+@('/link',"/out:$dll",("/implib:"+(Join-Path $Directory "callback-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','/EXPORT:PbTestOpen','/EXPORT:PbTestBlock','/EXPORT:PbTestFail','bcrypt.lib')) | Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();
        fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        session=$session;session_sha256=(Get-FileHash -LiteralPath $session).Hash.ToLowerInvariant();
        lifecycle=$selected;lifecycle_sha256=(Get-FileHash -LiteralPath $selected).Hash.ToLowerInvariant();
        entry=$source;entry_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()}
}
