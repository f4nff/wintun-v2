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
Invoke-BuildTool 'cl.exe' (@('/nologo','/LD','/std:c17','/MT','/O2','/DNDEBUG','/W4','/WX',"/Fo$objectDir\", 'api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c') + $extraArguments + $script:VersionArguments + @('/link',"/out:$apiDir\packetbridge.dll", '/def:api\v2\exports.def', "/implib:$objectDir\packetbridge.lib", "$objectDir\api.res") + $extraResources + @('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
if ((Get-Item -LiteralPath "$apiDir\packetbridge.dll").VersionInfo.FileVersion -ne $script:Version) { throw 'API PE version mismatch' }
if ($embedded) {
    & (Join-Path $PSScriptRoot '..\package\verify-embedded-driver.ps1') -Dll "$apiDir\packetbridge.dll" -DriverDirectory $driverInputPath
}
if ((Get-SourceFingerprint) -ne $script:SourceFingerprint) { throw 'Sources changed during API packaging' }
Write-ArtifactManifest $apiDir $(switch ($Stage) { 'FinalApi' { 'v2-signed-driver-api' }; 'UnsignedTestApi' { 'v2-unsigned-test-embedded-api' }; default { 'v2-development-api' } }) @{
    runtime_driver_tested = $false; driver_resources_embedded = $embedded; production_ready = $false
    adapter_diagnostics_schema = 1; adapter_diagnostics_size = 80; native_driver_recovery = 'one-uninstall-reinstall-retry'
    adapter_diagnostics_extended_schema = 2; adapter_diagnostics_extended_size = 180; driver_recovery_scope = 'configured-adapter-name-and-bound-inf'
    unsigned_test_only = ($Stage -eq 'UnsignedTestApi'); kernel_signature_verified = ($Stage -eq 'FinalApi')
    api_abi = $script:ApiAbiMajor; api_abi_minor = $script:ApiAbiMinor; data_abi = $script:DataAbiMajor; data_abi_minor = $script:DataAbiMinor; lifecycle_abi = $script:LifecycleAbiMajor
    packet_abi = $script:PacketAbiMajor; packet_abi_minor = $script:PacketAbiMinor
}
