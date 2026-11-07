# Same production hook in both DLLs, real modules/threads, no device installation.
function Invoke-SystemLoaderTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string[]]$BaseFlags,
        [string]$FixtureSource='tests\safety\system_loader_fixture.c',[switch]$CompileOnly)
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $legacy=@($BaseFlags|Where-Object {$_ -ne '/std:c17'})+@('/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/wd4100','/wd4201')
    foreach($kind in @('api','host')) {
        $fixture=Join-Path $Directory "system-loader-$kind.dll"
        $args=@($legacy);$sources=@($FixtureSource)
        if($kind -eq 'host'){$args+='/DPB_TEST_SETUP_HOST=1'}else{$sources+='api\logger.c'}
        $delays=@('/DELAYLOAD:setupapi.dll','/DELAYLOAD:shell32.dll')
        if($kind -eq 'api'){$delays+=@('/DELAYLOAD:advapi32.dll','/DELAYLOAD:shlwapi.dll')}
        Invoke-BuildTool cl.exe ($args+@('/LD')+$sources+@('/link',"/out:$fixture",("/implib:"+(Join-Path $Directory "system-loader-$kind.lib")),
            'ntdll.lib','advapi32.lib','setupapi.lib','shlwapi.lib','shell32.lib','delayimp.lib','/DELAY:UNLOAD')+$delays)
    }
    $test=Join-Path $Directory 'system-loader-tests.exe'
    Invoke-BuildTool cl.exe ($BaseFlags+@('tests\safety\system_loader_tests.c','/link',"/out:$test"))
    $decoy=Join-Path $Directory 'decoy';New-Item -ItemType Directory -Path $decoy -Force|Out-Null
    Invoke-BuildTool cl.exe ($BaseFlags+@('/LD','tests\safety\system_loader_decoy.c','/link',"/out:$decoy\version.dll","/implib:$decoy\version.lib"))
    if(!$CompileOnly){foreach($kind in @('api','host')){Invoke-TestTool $test @((Join-Path $Directory "system-loader-$kind.dll"),$decoy)}}
}
