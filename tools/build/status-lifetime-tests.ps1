# Build helper only: caller initializes common.ps1 and owns execution recording.
function Build-StatusLifetimeTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-status-pin')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Status lifetime output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$apiDirectory=Join-Path $script:RepoRoot 'api\v2'
    $originalSession=Join-Path $apiDirectory 'session.c';$originalStatus=Join-Path $apiDirectory 'session_status.h'
    $status=Join-Path $Directory 'session_status.h';[IO.File]::WriteAllBytes($status,[IO.File]::ReadAllBytes($originalStatus))
    $mutation=$null
    if($Variant -eq 'without-status-pin') {
        $text=[IO.File]::ReadAllText($originalStatus).Replace("`r`n","`n")
        $pattern='(?s)static PB_RESULT WINAPI GetStatus\(PB_SESSION \*s, PB_V2_STATUS \*status\).*?(?=static PB_RESULT WINAPI GetCapabilities\()'
        $matches=[regex]::Matches($text,$pattern);if($matches.Count -ne 1){throw 'GetStatus function is not unique'}
        $original=$matches[0].Value
        $acquire='    if (!TryAcquireSRWLockShared(&s->Lifetime)) return PB_BUSY;'
        $release='PB_RESULT result = StatusInternal(s, status); ReleaseSRWLockShared(&s->Lifetime); return result;'
        foreach($part in @($acquire,$release)){if([regex]::Matches($original,[regex]::Escape($part)).Count -ne 1){throw 'GetStatus Lifetime ownership pair is not unique'}}
        $changed=$original.Replace($acquire,'    /* Observer mutation: GetStatus Lifetime acquire/release pair omitted. */').Replace($release,'PB_RESULT result = StatusInternal(s, status); return result;')
        [IO.File]::WriteAllText($status,$text.Replace($original,$changed),$encoding)
        $originalFunction=Join-Path $Directory 'getstatus-original.c';$changedFunction=Join-Path $Directory 'getstatus-without-pin.c'
        [IO.File]::WriteAllText($originalFunction,$original,$encoding);[IO.File]::WriteAllText($changedFunction,$changed,$encoding)
        $mutation=[ordered]@{function='GetStatus';removed=@('if (!TryAcquireSRWLockShared(&s->Lifetime)) return PB_BUSY;','ReleaseSRWLockShared(&s->Lifetime);');
            original=$originalFunction;original_sha256=(Get-FileHash -LiteralPath $originalFunction).Hash.ToLowerInvariant();
            changed=$changedFunction;changed_sha256=(Get-FileHash -LiteralPath $changedFunction).Hash.ToLowerInvariant();
            teardown='Worker holds real Lifetime EX before ReleaseResources; release/join the unpinned reader before letting worker close resources; normal final Close/unload precedes113.'}
    }
    $text=[IO.File]::ReadAllText($originalSession)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match)
        $include=if($match.Groups[1].Value -eq 'session_status.h'){$status}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#undef DeviceIoControl
#define DeviceIoControl ReaderIo
#undef AcquireSRWLockExclusive
#define AcquireSRWLockExclusive ReaderAcquire
#include PB_STATUS_LIFETIME_SESSION_SOURCE
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_STATUS_LIFETIME_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\status_lifetime_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($source,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'status-lifetime.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\status_lifetime_tests.c','/link',"/out:$program")) | Out-Host
    $dll=Join-Path $Directory "status-lifetime-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "status-lifetime-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib')) | Out-Host
    $bound=@($originalSession,$originalStatus,$status,$session,$entry,$source,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\status_lifetime_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\status_lifetime_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\status_lifetime_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;session=$session;entry=$entry;shim=$source;sources=@($bound);mutation=$mutation;
        extraction=[ordered]@{source=$originalStatus;source_sha256=(Get-FileHash -LiteralPath $originalStatus).Hash.ToLowerInvariant();header=$status;header_sha256=(Get-FileHash -LiteralPath $status).Hash.ToLowerInvariant()}}
}
