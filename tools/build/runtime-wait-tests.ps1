# Shared hosted DLL fixture build; callers own Initialize-BuildEnvironment and
# test recording. Uses the shipping runtime with real delay imports, no devices.
function Invoke-RuntimeWaitTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string[]]$BaseFlags,
        [string]$FixtureSource='tests\safety\runtime_wait_fixture.c',[switch]$CompileOnly)
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $runtimeFlags=@($BaseFlags|Where-Object {$_ -ne '/std:c17'})+@('/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/wd4100','/wd4201')
    $fixture=Join-Path $Directory 'runtime-wait-fixture.dll'
    $test=Join-Path $Directory 'runtime-wait-tests.exe'
    Invoke-BuildTool cl.exe ($runtimeFlags+@('/LD',$FixtureSource,'api\logger.c','/link',"/out:$fixture",("/implib:"+(Join-Path $Directory 'runtime-wait-fixture.lib')),
        'ntdll.lib','advapi32.lib','setupapi.lib','shlwapi.lib','shell32.lib','delayimp.lib','/DELAY:UNLOAD',
        '/DELAYLOAD:advapi32.dll','/DELAYLOAD:setupapi.dll','/DELAYLOAD:shlwapi.dll','/DELAYLOAD:shell32.dll'))
    Invoke-BuildTool cl.exe ($BaseFlags+@('tests\safety\runtime_wait_tests.c','/link',"/out:$test"))
    if(!$CompileOnly){Invoke-TestTool $test @($fixture)}
}
