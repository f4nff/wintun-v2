# Invoked after common.ps1 and Initialize-BuildEnvironment by build.ps1.
$driverDir = Join-Path $outDir 'driver'
New-Item -ItemType Directory -Path $driverDir -Force | Out-Null
if ((Test-Path -LiteralPath "$driverDir\packetbridge.sys") -and
    (Get-AuthenticodeSignature -LiteralPath "$driverDir\packetbridge.sys").SignerCertificate) {
    throw 'This output contains a signed driver. Use a new output directory instead of rebuilding it.'
}
if ((Test-Path -LiteralPath "$driverDir\packetbridge.cat") -and
    (Get-AuthenticodeSignature -LiteralPath "$driverDir\packetbridge.cat").SignerCertificate) {
    throw 'This output contains a signed catalog. Do not overwrite signed inputs.'
}
$kernelArgs = @('/nologo','/c','/O2','/Oi','/GL','/Gy','/GF','/Zi','/W4','/Zl','/kernel','/GS','/Gz','/Zp8','/Oy-',
    '/volatile:iso','/d2epilogunwind','/D_WIN64','/D_AMD64_','/DAMD64',
    '/D_WIN32_WINNT=0x0A00','/DWINVER=0x0A00','/DNTDDI_VERSION=0x0A000010',
    '/DDBG=0','/DNDEBUG','/DUNICODE','/D_UNICODE',
    '/Dtry=__try','/Dexcept=__except','/Dfinally=__finally','/Dleave=__leave',
    '/DNDIS_MINIPORT_DRIVER=1','/DNDIS689_MINIPORT=1','/DNDIS_WDM=1','/DPACKETBRIDGE_PROBE=1','/DPACKETBRIDGE_KERNEL=1',
    "/I$script:WdkRoot\Include\$script:KitVersion\km\crt", "/I$script:WdkRoot\Include\$script:KitVersion\km",
    "/I$script:SdkRoot\Include\$script:KitVersion\shared", "/I$script:WdkRoot\Include\$script:KitVersion\shared",
    "/I$script:RepoRoot\include") + $script:VersionArguments
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\driver.obj", "/Fd$objectDir\driver-compile.pdb",'driver\wintun.c'))
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\packet-kernel.obj", "/Fd$objectDir\packet-kernel.pdb",'common\packet.c'))
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\core-kernel.obj", "/Fd$objectDir\core-kernel.pdb",'common\v2_core.c'))
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\coalesce-kernel.obj", "/Fd$objectDir\coalesce-kernel.pdb",'common\coalesce.c'))
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\policy-kernel.obj", "/Fd$objectDir\policy-kernel.pdb",'common\offload_policy.c'))
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\flow-kernel.obj", "/Fd$objectDir\flow-kernel.pdb",'common\flow.c'))
Invoke-BuildTool 'cl.exe' ($kernelArgs + @("/Fo$objectDir\ethernet-kernel.obj", "/Fd$objectDir\ethernet-kernel.pdb",'common\ethernet.c'))
Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include", "/fo$objectDir\driver.res") + $script:VersionArguments + @('driver\v2\packetbridge.rc'))
Invoke-BuildTool 'link.exe' @('/nologo','/driver','/kernel','/nodefaultlib','/entry:GsDriverEntry','/machine:x64',
    '/subsystem:native,10.00','/osversion:10.0','/version:1.0','/release','/incremental:no','/ltcg','/opt:ref','/opt:icf',
    '/debug:full','/merge:_TEXT=.text','/merge:_PAGE=PAGE','/section:INIT,d',
    "/out:$driverDir\packetbridge.sys", "/pdb:$driverDir\packetbridge.pdb",
    "/libpath:$script:WdkRoot\Lib\$script:KitVersion\km\x64", "$objectDir\driver.obj", "$objectDir\packet-kernel.obj", "$objectDir\core-kernel.obj", "$objectDir\coalesce-kernel.obj", "$objectDir\policy-kernel.obj", "$objectDir\flow-kernel.obj", "$objectDir\driver.res",
    "$objectDir\ethernet-kernel.obj", 'ndis.lib','wdmsec.lib','BufferOverflowFastFailK.lib','ntoskrnl.lib','hal.lib','wmilib.lib')
Copy-Item -LiteralPath 'driver\v2\packetbridge.inf' -Destination "$driverDir\packetbridge.inf" -Force
$infDate = [TimeZoneInfo]::ConvertTimeBySystemTimeZoneId([DateTimeOffset]::UtcNow, 'Singapore Standard Time').ToString('MM/dd/yyyy')
Invoke-BuildTool "$script:WdkRoot\bin\$script:KitVersion\x64\stampinf.exe" @('-f',"$driverDir\packetbridge.inf",'-a','amd64','-d',$infDate,'-v',$script:FileVersion)
# Product version stays fixed; distinguish frozen packages before catalog generation.
$infPath = Join-Path $driverDir 'packetbridge.inf'
$infReader = [IO.StreamReader]::new($infPath, [Text.UTF8Encoding]::new($false), $true)
try { $null = $infReader.ReadToEnd(); $infEncoding = $infReader.CurrentEncoding }
finally { $infReader.Dispose() }
[IO.File]::AppendAllText($infPath, "`r`n; SourceFingerprint=$script:SourceFingerprint`r`n", $infEncoding)
Invoke-BuildTool "$script:WdkRoot\bin\$script:KitVersion\x86\Inf2Cat.exe" @("/driver:$driverDir",'/os:10_GE_X64','/uselocaltime')
if ((Get-Item -LiteralPath "$driverDir\packetbridge.sys").VersionInfo.FileVersion -ne $script:Version) { throw 'Driver PE version mismatch' }
Write-ArtifactManifest $driverDir 'v2-development-unsigned' @{ data_abi = $script:DataAbiMajor; data_abi_minor = $script:DataAbiMinor; runtime_tested = $false; default_mask = 0; rx_offloads_implemented = $true; production_ready = $false }
Get-Item -LiteralPath "$driverDir\packetbridge.sys", "$driverDir\packetbridge.inf", "$driverDir\packetbridge.cat" | Select-Object FullName,Length
