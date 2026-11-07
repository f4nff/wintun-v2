# Build only; caller initializes common.ps1 and owns test recording.
function Build-Win7CreateTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('before','current')][string]$Variant='current')
    $Directory=[IO.Path]::GetFullPath($Directory);New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'api\adapter_win7.h'
    if($Variant -eq 'before'){
        $source=Join-Path $script:RepoRoot 'out\native-close-baseline\019ed63327d9\api\adapter_win7.h'
        if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne '524c5ae0818daf6f423043e9258135aa437f4f96f50e1e6fb443dcf5b53d24f0'){throw 'Unexpected frozen Win7 creation source'}
    }
    $encoding=[Text.UTF8Encoding]::new($false);$text=[IO.File]::ReadAllText($source)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match)
        '#include "'+[IO.Path]::GetFullPath((Join-Path (Join-Path $script:RepoRoot 'api') $match.Groups[1].Value)).Replace('\','/')+'"'
    })
    $helper=Join-Path $Directory 'adapter-win7.h';[IO.File]::WriteAllText($helper,$generated,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_WIN7_CREATE_SOURCE "'+$helper.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\win7_create_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllLines($entry,$lines,$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'win7-create.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$entry,'/link',"/out:$program",'setupapi.lib','cfgmgr32.lib','advapi32.lib'))|Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        source=$source;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();generated=$helper;generated_sha256=(Get-FileHash -LiteralPath $helper).Hash.ToLowerInvariant();
        entry=$entry;entry_sha256=(Get-FileHash -LiteralPath $entry).Hash.ToLowerInvariant()}
}
