# Build helper only: caller initializes common.ps1 and owns execution recording.
function Build-UnpublishedCloseTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-owner-handoff')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Unpublished close output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api\v2';$original=Join-Path $api 'session.c'
    $text=[IO.File]::ReadAllText($original);$mutation=$null
    if($Variant -eq 'without-owner-handoff') {
        $matches=[regex]::Matches($text,'(?s)static void Destroy\(PB_SESSION \*s\).*?(?=static PB_RESULT Allocate\()')
        if($matches.Count -ne 1){throw 'Destroy function is not unique'}
        $before=$matches[0].Value;$line='ReleaseSession(s); return;'
        if([regex]::Matches($before,[regex]::Escape($line)).Count -ne 1){throw 'Failed-close owner transfer is not unique'}
        $after=$before.Replace($line,'return; /* Observer mutation: ownership handoff omitted. */')
        $text=$text.Replace($before,$after)
        $beforePath=Join-Path $Directory 'destroy-original.c';$afterPath=Join-Path $Directory 'destroy-without-handoff.c'
        [IO.File]::WriteAllText($beforePath,$before,$encoding);[IO.File]::WriteAllText($afterPath,$after,$encoding)
        $mutation=[ordered]@{function='Destroy';removed=$line;replacement='return; /* Observer mutation: ownership handoff omitted. */';
            original=$beforePath;original_sha256=(Get-FileHash $beforePath).Hash.ToLowerInvariant();changed=$afterPath;changed_sha256=(Get-FileHash $afterPath).Hash.ToLowerInvariant();
            teardown='With construction+registry refs2 and no callback, ordinary loader owned: invoke actual ReleaseSession once solely for negative cleanup, then identical real close/tail/thread joins/unload before118.'}
    }
    $selected=Join-Path $Directory 'session-selected.c';[IO.File]::WriteAllText($selected,$text,$encoding)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value)).Replace('\','/')+'"'})
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#undef CreateFileW
#define CreateFileW UnpublishedFile
#undef CloseHandle
#define CloseHandle UnpublishedClose
#undef DeviceIoControl
#define DeviceIoControl UnpublishedIo
#undef CreateThreadpoolWork
#define CreateThreadpoolWork UnpublishedWork
#include PB_UNPUBLISHED_SESSION_SOURCE
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_UNPUBLISHED_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\unpublished_close_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($source,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $exe=Join-Path $Directory 'unpublished-close.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\unpublished_close_tests.c','/link',"/out:$exe"))|Out-Host
    $dll=Join-Path $Directory "unpublished-close-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "unpublished-close-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $bound=@($original,$selected,$session,$entry,$source,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\unpublished_close_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\unpublished_close_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\unpublished_close_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;source=$original;selected=$selected;session=$session;entry=$entry;shim=$source;sources=@($bound);mutation=$mutation;
        extraction=[ordered]@{source=$original;source_sha256=(Get-FileHash $original).Hash.ToLowerInvariant();selected=$selected;selected_sha256=(Get-FileHash $selected).Hash.ToLowerInvariant();
            adaptation='Current source stays byte-identical before only quoted relative includes become absolute. Native session uses four hosted SDK/accounting boundaries; actual Open and lifecycle are unchanged in current builds.'}}
}
