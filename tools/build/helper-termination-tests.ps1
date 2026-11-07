# Hosted process/thread lifetime regression; no installer or device is invoked.
function Invoke-HelperTerminationTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string[]]$BaseFlags,
        [string]$FixtureSource='tests\safety\helper_termination_fixture.c',[switch]$CompileOnly)
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $legacy=@($BaseFlags|Where-Object {$_ -ne '/std:c17'})+@('/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/wd4100','/wd4201')
    $fixture=Join-Path $Directory 'helper-termination-fixture.dll';$test=Join-Path $Directory 'helper-termination-tests.exe'
    Invoke-BuildTool cl.exe ($legacy+@('/LD',$FixtureSource,'api\logger.c','/link',"/out:$fixture",("/implib:"+(Join-Path $Directory 'helper-termination-fixture.lib')),
        'ntdll.lib','advapi32.lib','setupapi.lib','shlwapi.lib','shell32.lib','delayimp.lib','/DELAY:UNLOAD',
        '/DELAYLOAD:advapi32.dll','/DELAYLOAD:setupapi.dll','/DELAYLOAD:shlwapi.dll','/DELAYLOAD:shell32.dll'))
    Invoke-BuildTool cl.exe ($BaseFlags+@('tests\safety\helper_termination_tests.c','/link',"/out:$test"))
    if(!$CompileOnly){Invoke-TestTool $test @($fixture)}
}
