# Embedded modes consume immutable inputs without recompiling the SYS.
# FinalApi always retains kernel-signature verification. The explicitly named
# UnsignedTestApi mode accepts only hash-verified unsigned development inputs.
$apiDir = $outDir
$extraArguments = @()
$extraResources = @()
$embedded = $Stage -in @('FinalApi', 'UnsignedTestApi')
if ($Stage -eq 'FinalApi') {
    if (-not $SignedDriverDirectory -or -not $ExpectedManifest) { throw 'FinalApi requires -SignedDriverDirectory and -ExpectedManifest' }
    $driverInputPath = (Resolve-Path -LiteralPath $SignedDriverDirectory).Path
    $expectedPath = (Resolve-Path -LiteralPath $ExpectedManifest).Path
    $expected = Get-Content -LiteralPath $expectedPath -Raw | ConvertFrom-Json
    if ($expected.source_tree_sha256 -ne $script:SourceFingerprint) { throw 'Signed driver sources differ from the current DLL sources' }
    & (Join-Path $PSScriptRoot '..\package\verify-signed-driver.ps1') -Directory $driverInputPath -ExpectedManifest $expectedPath
    $apiDir = Join-Path $outDir 'final-api'
} elseif ($Stage -eq 'UnsignedTestApi') {
    if (-not $UnsignedDriverDirectory -or -not $ExpectedManifest) { throw 'UnsignedTestApi requires -UnsignedDriverDirectory and -ExpectedManifest' }
    $driverInputPath = (Resolve-Path -LiteralPath $UnsignedDriverDirectory).Path
    $expectedPath = (Resolve-Path -LiteralPath $ExpectedManifest).Path
    $expected = Get-Content -LiteralPath $expectedPath -Raw | ConvertFrom-Json
    if ($expected.source_tree_sha256 -ne $script:SourceFingerprint) { throw 'Unsigned test driver sources differ from the current DLL sources' }
    & (Join-Path $PSScriptRoot '..\package\verify-unsigned-driver.ps1') -Directory $driverInputPath -ExpectedManifest $expectedPath
    $apiDir = Join-Path $outDir 'unsigned-test-api'
}
if ($embedded) {
    if (Test-Path -LiteralPath $apiDir) { throw 'Preserve the existing embedded DLL. Choose a clean frozen build.' }
    New-Item -ItemType Directory -Path $apiDir | Out-Null
    $lines = @('#include <windows.h>')
    $index = 101
    foreach ($name in @('packetbridge.sys','packetbridge.inf','packetbridge.cat')) {
        $path = (Join-Path $driverInputPath $name).Replace('\','/')
        if ($path.Contains('"') -or $path.Contains("`n") -or $path.Contains("`r")) { throw 'Invalid embedded resource path' }
        $lines += "$index RCDATA `"$path`""
        $index++
    }
    $resourceSource = Join-Path $objectDir 'embedded-driver.rc'
    $lines | Set-Content -LiteralPath $resourceSource -Encoding utf8
    Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') @('/nologo',"/fo$objectDir\embedded-driver.res", $resourceSource)
    $extraResources += "$objectDir\embedded-driver.res"
    $extraArguments += '/DPB_EMBEDDED_DRIVER=1'
}
Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo', "/fo$objectDir\api.res") + $script:VersionArguments + @('api\v2\resources.rc'))
# One shipping DLL: downlevel installer + Wintun adapter + native v2 API.
$apiObjects = Join-Path $objectDir 'unified-api'
New-Item -ItemType Directory -Path $apiObjects -Force | Out-Null
$inf = if ($embedded) { Join-Path $driverInputPath 'packetbridge.inf' } else { Join-Path $outDir 'driver\packetbridge.inf' }
$infDate = [DateTime]::UtcNow.Date
$infVersion = $script:FileVersion
if (Test-Path -LiteralPath $inf) {
    $match = [regex]::Match((Get-Content -LiteralPath $inf -Raw), '(?m)^DriverVer\s*=\s*([^,]+),([^\r\n]+)')
    if (-not $match.Success) { throw 'DriverVer missing from unified driver input' }
    $infDate = [DateTime]::ParseExact($match.Groups[1].Value.Trim(), 'MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture)
    $infVersion = $match.Groups[2].Value.Trim()
}
$fileTime = $infDate.ToFileTimeUtc()
$parts = $infVersion.Split('.')
@("#define WINTUN_INF_FILETIME { (DWORD)($fileTime`ULL & 0xffffffff), (DWORD)($fileTime`ULL >> 32) }",
  "#define WINTUN_INF_VERSION (($($parts[0])ULL << 48) | ($($parts[1])ULL << 32) | ($($parts[2])ULL << 16) | $($parts[3])ULL)") |
    Set-Content -LiteralPath "$apiObjects\wintun-inf.h" -Encoding ascii
Invoke-BuildTool 'lib.exe' @('/nologo','/def:api\nci.def','/machine:x64',"/out:$apiObjects\nci.lib")
$flags = @('/nologo','/c','/std:c17','/MT','/O2','/DNDEBUG','/W4','/WX','/wd4100','/wd4200','/wd4201','/volatile:iso',
    '/FIwinsock2.h','/DUNICODE','/D_UNICODE','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/I$apiObjects") + $script:VersionArguments
$sources = @('api\main.c','api\adapter.c','api\driver.c','api\logger.c','api\namespace.c','api\registry.c',
    'api\resource.c','api\rundll32.c','api\session.c','api\v2\session.c','api\v2\adapter_unified.c',
    'common\v2_core.c','common\packet.c','common\flow.c')
$objects = @()
foreach ($source in $sources) {
    $object = Join-Path $apiObjects ($source.Replace('\','-').Replace('.c','.obj'))
    Invoke-BuildTool 'cl.exe' ($flags + @("/Fo$object", $source))
    $objects += $object
}
$libs = @('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','cfgmgr32.lib',
    'shlwapi.lib','version.lib','ntdll.lib','uuid.lib','delayimp.lib','onecore.lib',"$apiObjects\nci.lib")
$delays = @('advapi32.dll','api-ms-win-devices-query-l1-1-0.dll',
    'cfgmgr32.dll','iphlpapi.dll','ole32.dll','nci.dll','setupapi.dll','shlwapi.dll','version.dll') | ForEach-Object { "/DELAYLOAD:$_" }
Invoke-BuildTool 'link.exe' (@('/nologo','/dll','/machine:x64','/subsystem:windows,6.01','/osversion:6.1','/DELAY:UNLOAD',
    "/out:$apiDir\wintun.dll",'/def:api\exports.def',"/implib:$apiObjects\wintun.lib","$objectDir\api.res") + $objects + $extraResources + $libs + $delays)
if ((Get-Item -LiteralPath "$apiDir\wintun.dll").VersionInfo.FileVersion -ne $script:Version) { throw 'API PE version mismatch' }
if ($embedded) {
    & (Join-Path $PSScriptRoot '..\package\verify-embedded-driver.ps1') -Dll "$apiDir\wintun.dll" -DriverDirectory $driverInputPath
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$apiObjects\",
        'tests\safety\resource_alias_tests.c','api\resource.c','/link',"/out:$apiObjects\resource-alias-tests.exe",'shlwapi.lib','advapi32.lib'))
    Invoke-BuildTool "$apiObjects\resource-alias-tests.exe" @("$apiDir\wintun.dll")
}
Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$apiObjects\unified-dll-tests.obj",
    'tests\safety\unified_dll_tests.c','/link',"/out:$apiObjects\unified-dll-tests.exe"))
Invoke-BuildTool "$apiObjects\unified-dll-tests.exe" @("$apiDir\wintun.dll")
if ((Get-SourceFingerprint) -ne $script:SourceFingerprint) { throw 'Sources changed during API packaging' }
Write-ArtifactManifest $apiDir $(switch ($Stage) { 'FinalApi' { 'v2-signed-driver-api' }; 'UnsignedTestApi' { 'v2-unsigned-test-embedded-api' }; default { 'v2-development-api' } }) @{
    runtime_driver_tested = $false; driver_resources_embedded = $embedded; production_ready = $false
    unified_dll = 'wintun.dll'; kernel_protocol = 'v2-only'; legacy_api_compatibility = $true; minimum_os_target = 'Windows 7 SP1 x64'
    adapter_diagnostics_schema = 1; adapter_diagnostics_size = 80; native_driver_recovery = 'shared-versioned-installer'
    adapter_diagnostics_extended_schema = 2; adapter_diagnostics_extended_size = 180; driver_recovery_scope = 'PacketBridgeV2-device-identity'
    unsigned_test_only = ($Stage -eq 'UnsignedTestApi'); kernel_signature_verified = ($Stage -eq 'FinalApi')
    api_abi = $script:ApiAbiMajor; api_abi_minor = $script:ApiAbiMinor; data_abi = $script:DataAbiMajor; data_abi_minor = $script:DataAbiMinor; lifecycle_abi = $script:LifecycleAbiMajor
    packet_abi = $script:PacketAbiMajor; packet_abi_minor = $script:PacketAbiMinor
}
