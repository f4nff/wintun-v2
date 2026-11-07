# Complete production host/transport/client sources; all SetupAPI calls hosted.
# Caller initializes common.ps1 and records execution. No installer is started.
function Build-HelperResponseTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('before','current')][string]$Variant='current')
    $Directory=[IO.Path]::GetFullPath($Directory)
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $selected=$script:RepoRoot
    if($Variant -eq 'before'){$selected=Join-Path $script:RepoRoot 'out\terminal-wake-baseline\e16627eaddcc'}
    $encoding=[Text.UTF8Encoding]::new($false)
    $expected=@{
        'api\rundll32.c'='6f0593fe8468ae5029fc4ef880677904679d50ab3b56584e15efd8574eb442c7'
        'api\rundll32_process.h'='fc92acf52d2c30c20b708310c1707d633efd0c7ba88257532990f8a72b7646be'
        'setupapihost\host.c'='a213312a95b56be4bbe527824f27d838200ab90bb66feaf2cd4a3cf9ed8283ff'
        'setupapihost\host_win7.h'='151150e2ea360f5e87137f99750229e6fb70aaf5770c3454b6c18b9fcefd8027'
    }
    $sources=[ordered]@{}
    foreach($relative in $expected.Keys){
        $path=Join-Path $selected $relative;$hash=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
        if($Variant -eq 'before' -and $hash -ne $expected[$relative]){throw "Unexpected frozen helper source: $relative"}
        $sources[$relative]=[ordered]@{path=$path;sha256=$hash}
    }
    # Only local include paths are normalized. The production bodies remain
    # byte-for-byte equivalent after newline normalization; none are reimplemented.
    function Write-HelperResponseSource([string]$Relative,[string]$Destination,[hashtable]$Overrides) {
        $source=Join-Path $selected $Relative;$currentDirectory=Split-Path (Join-Path $script:RepoRoot $Relative)
        $text=[IO.File]::ReadAllText($source)
        $text=[regex]::Replace($text,'(?m)^#\s*include "([^"]+)"',{
            param($match)
            $name=$match.Groups[1].Value
            $include=if($Overrides.ContainsKey($name)){$Overrides[$name]}else{[IO.Path]::GetFullPath((Join-Path $currentDirectory $name))}
            '#include "'+$include.Replace('\','/')+'"'
        })
        [IO.File]::WriteAllText($Destination,$text,$encoding)
    }
    $process=Join-Path $Directory 'process.h';Write-HelperResponseSource 'api\rundll32_process.h' $process @{}
    $shim=Join-Path $Directory 'process-shim.h'
    [IO.File]::WriteAllLines($shim,@('#define ExecuteRunDll32 ActualExecuteRunDll32',
        ('#include "'+$process.Replace('\','/')+'"'),'#undef ExecuteRunDll32','#define ExecuteRunDll32 TestExecuteRunDll32'),$encoding)
    $parent=Join-Path $Directory 'parent.c';Write-HelperResponseSource 'api\rundll32.c' $parent @{'rundll32_process.h'=$shim}
    $win7=Join-Path $Directory 'host-win7.h';Write-HelperResponseSource 'setupapihost\host_win7.h' $win7 @{}
    $hostSource=Join-Path $Directory 'host.c';Write-HelperResponseSource 'setupapihost\host.c' $hostSource @{'host_win7.h'=$win7}
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_HELPER_RESPONSE_PARENT_SOURCE "'+$parent.Replace('\','/')+'"'
    $lines+='#define PB_HELPER_RESPONSE_HOST_SOURCE "'+$hostSource.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\helper_response_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllLines($entry,$lines,$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'helper-response.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$entry,'/link',"/out:$program",("/implib:"+(Join-Path $Directory 'helper-response.lib')),'shell32.lib','shlwapi.lib','delayimp.lib'))|Out-Host
    $generated=[ordered]@{}
    foreach($path in @($process,$parent,$win7,$hostSource,$shim)){$generated[[IO.Path]::GetFileName($path)]=[ordered]@{path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();volatile=$Mode;
        guarded=[bool]$Guarded;variant=$Variant;sources=$sources;generated=$generated;entry=$entry;entry_sha256=(Get-FileHash -LiteralPath $entry).Hash.ToLowerInvariant()}
}
