# Actual full main.c runtime/security initialization. Callers initialize common.ps1.
function Build-RuntimeInitTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-failure-lease-release')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Runtime-init output must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'api\main.c';$hash=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant()
    if($hash -ne '543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'){throw 'Runtime-init requires the accepted complete main.c'}
    $text=[IO.File]::ReadAllText($source);$original=$text;$encoding=[Text.UTF8Encoding]::new($false)
    $before='    DWORD error = GetLastError(); MainRuntimeRelease(); SetLastError(error); return FALSE;'
    $after='    DWORD error = GetLastError(); /* Observer122: omit only failed-acquisition lease release. */ SetLastError(error); return FALSE;'
    if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw 'Runtime-init failure lease tail is not unique'}
    if($Variant -eq 'without-failure-lease-release'){$text=$text.Replace($before,$after)}
    $api=Join-Path $script:RepoRoot 'api';$projected=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $match.Groups[1].Value)).Replace('\','/')+'"'
    })
    $whole=Join-Path $Directory 'main-original.c';$main=Join-Path $Directory 'main-projected.c'
    [IO.File]::WriteAllText($whole,$original,$encoding);[IO.File]::WriteAllText($main,$projected,$encoding)
    $fixtureLines=@();if($Guarded){$fixtureLines+='#define PB_GUARD_TESTS 1'}
    $fixtureLines+='#define PB_RUNTIME_INIT_SOURCE "'+$main.Replace('\','/')+'"'
    $fixtureLines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\runtime_init_fixture.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'fixture-entry.c';[IO.File]::WriteAllLines($entry,$fixtureLines,$encoding)
    $hostLines=@();if($Guarded){$hostLines+='#define PB_GUARD_TESTS 1'}
    $hostLines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\runtime_init_tests.c').Replace('\','/')+'"'
    $hostEntry=Join-Path $Directory 'host-entry.c';[IO.File]::WriteAllLines($hostEntry,$hostLines,$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$Directory\")+$script:VersionArguments
    $fixture=Join-Path $Directory 'runtime-init-fixture.dll';$program=Join-Path $Directory 'runtime-init-tests.exe'
    Invoke-BuildTool cl.exe ($flags+@('/LD',$entry,'/link',"/out:$fixture","/implib:$Directory/runtime-init.lib",'ntdll.lib','advapi32.lib','delayimp.lib','/DELAYLOAD:advapi32.dll','/DELAY:UNLOAD'))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@($hostEntry,'/link',"/out:$program"))|Out-Host
    $parts=@($whole,$main,$entry,$hostEntry)|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    $sources=@('tests\model\runtime_init_fixture.h','tests\safety\runtime_init_fixture.c','tests\model\runtime_init_tests.c','api\main.c')|ForEach-Object {$path=Join-Path $script:RepoRoot $_;@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$fixture;fixture_sha256=(Get-FileHash -LiteralPath $fixture).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;source=$source;source_sha256=$hash;whole=$whole;main=$main;entry=$entry;host_entry=$hostEntry;parts=@($parts);sources=@($sources);
        adaptation=@{kind='single failure-tail release omission plus local include normalization';before=$before;after=$after;count=$(if($Variant -eq 'current'){0}else{1})};
        boundary_macros=@('HeapCreate/Destroy ledger','CreateWellKnownSid/OpenProcessToken/GetTokenInformation failure boundaries and real success','both SDDL conversions/ConvertSidToStringSid/LocalFree ledger','CloseHandle token ledger','InitOnce entry/failed-return gates around actual SDK','real delay import unload observer','DllMain naming')}
}
