# Actual default legacy Open -> native OpenSession/Allocate. Caller initializes
# common.ps1 and owns execution recording. No fake Open result or device access.
function Build-NativeStartErrorTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('before','current')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Native-start output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $apiDirectory=Join-Path $script:RepoRoot 'api\v2';$source=Join-Path $apiDirectory 'session.c'
    if($Variant -eq 'before'){
        $source=Join-Path $script:RepoRoot 'out\queue-stop-baseline\a4e6a58f27c1\api\v2\session.c'
        if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne '5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'){throw 'Unexpected frozen native session source'}
    }
    $encoding=[Text.UTF8Encoding]::new($false);$text=[IO.File]::ReadAllText($source)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match)
        '#include "'+[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value)).Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'native-session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_NATIVE_START_CORE_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\native_start_error_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllLines($entry,$lines,$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'native-start-error.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$entry,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$program",'bcrypt.lib'))|Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        source=$source;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();session=$session;session_sha256=(Get-FileHash -LiteralPath $session).Hash.ToLowerInvariant();
        entry=$entry;entry_sha256=(Get-FileHash -LiteralPath $entry).Hash.ToLowerInvariant()}
}
