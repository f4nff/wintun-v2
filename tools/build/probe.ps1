Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include", "/fo$objectDir\probe.res") + $script:VersionArguments + @('tools\build\artifact.rc'))
Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DUNICODE','/D_UNICODE','/D_WIN32_WINNT=0x0A00','/DNTDDI_VERSION=0x0A000010',
    "/Fo$objectDir\", 'tools\probe\probe.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbprobe.exe", "$objectDir\probe.res", 'setupapi.lib','swdevice.lib','cfgmgr32.lib','onecore.lib'))
Invoke-BuildTool "$outDir\pbprobe.exe" @('--self-test')
if ((Get-Item -LiteralPath "$outDir\pbprobe.exe").VersionInfo.FileVersion -ne $script:Version) { throw 'Probe PE version mismatch' }
Write-ArtifactManifest $outDir 'development-tools' @{ runtime_driver_tested = $false }
