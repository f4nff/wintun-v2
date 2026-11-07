# Build helper only: caller initializes common.ps1 and owns recording.
function Build-NativePolicyTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-control-stop-check')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $apiDirectory=Join-Path $script:RepoRoot 'api\v2';$encoding=[Text.UTF8Encoding]::new($false)
    $control=Join-Path $apiDirectory 'session_control.h';$selected=$control
    if($Variant -eq 'without-control-stop-check') {
        $text=[IO.File]::ReadAllText($control).Replace("`r`n","`n")
        $check=@'
    if (mask && InterlockedCompareExchange(&s->CloseRequested, 0, 0)) {
        ReleaseSRWLockExclusive(&s->Control); ReleaseSRWLockShared(&s->Lifetime); return PB_STOPPED;
    }
'@
        $check=$check.Replace("`r`n","`n")
        if(([regex]::Matches($text,[regex]::Escape($check))).Count -ne 1){throw 'Native PolicyIo stop check is not unique'}
        $selected=Join-Path $Directory 'control-without-stop-check.h'
        [IO.File]::WriteAllText($selected,$text.Replace($check,'    /* Test observer mutation: Control stop recheck omitted. */'),$encoding)
    }
    $text=[IO.File]::ReadAllText((Join-Path $apiDirectory 'session.c'))
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match)
        $include=if($match.Groups[1].Value -eq 'session_control.h'){$selected}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'native-policy.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\native_policy_tests.c','/link',"/out:$program")) | Out-Host
    $dll=Join-Path $Directory "native-policy-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "native-policy-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib')) | Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;session=$session;session_sha256=(Get-FileHash -LiteralPath $session).Hash.ToLowerInvariant();
        control=$selected;control_sha256=(Get-FileHash -LiteralPath $selected).Hash.ToLowerInvariant();entry=$source;entry_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()}
}
