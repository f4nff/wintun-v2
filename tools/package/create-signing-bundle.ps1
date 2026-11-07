param([string]$BuildDirectory)
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'catalog-hash.ps1')
$script:SourceFingerprint = Get-SourceFingerprint
if (-not $BuildDirectory) {
    $BuildDirectory = Join-Path $script:RepoRoot "out\packet-path-v2\$script:Version\$($script:SourceFingerprint.Substring(0,12))"
}
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$driverPath = Join-Path $buildPath 'driver'
$driverManifest = Get-Content -LiteralPath "$driverPath\manifest.json" -Raw | ConvertFrom-Json
$toolManifest = Get-Content -LiteralPath "$buildPath\manifest.json" -Raw | ConvertFrom-Json
$tests = Get-Content -LiteralPath "$buildPath\test-results.json" -Raw | ConvertFrom-Json
foreach ($record in @($driverManifest, $toolManifest, $tests)) {
    if ($record.version -ne $script:Version -or $record.source_tree_sha256 -ne $script:SourceFingerprint) {
        throw 'Build sources changed, or artifacts are from different builds. Rebuild Tests, Driver, Probe and Api first.'
    }
}
if ($tests.result -ne 'passed' -or $tests.kernel_runtime_tested -ne $false) { throw 'Missing successful offline test evidence' }
foreach ($name in @('packetbridge.sys', 'packetbridge.inf', 'packetbridge.cat', 'packetbridge.pdb')) {
    $entry = @($driverManifest.files | Where-Object name -eq $name)
    if ($entry.Count -ne 1 -or (Get-FileHash -LiteralPath "$driverPath\$name" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry[0].sha256) {
        throw "Driver artifact changed since build: $name"
    }
}
foreach ($name in @('pbprobe.exe','wintun.dll')) {
    $entry = @($toolManifest.files | Where-Object name -eq $name)
    if ($entry.Count -ne 1 -or (Get-FileHash -LiteralPath "$buildPath\$name" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry[0].sha256) {
        throw "Development binary changed since build: $name"
    }
}
foreach ($name in @('packetbridge.sys', 'packetbridge.cat')) {
    if ((Get-AuthenticodeSignature -LiteralPath "$driverPath\$name").SignerCertificate) { throw 'Package already contains signatures; preserve it as signed input' }
}
$bundleName = "PacketBridgeV2-$script:Version-amd64-v2-$($script:SourceFingerprint.Substring(0,12))-for-signing"
$bundlePath = Join-Path $script:RepoRoot "out\signing\$bundleName"
$zipPath = "$bundlePath.zip"
if ((Test-Path -LiteralPath $bundlePath) -or (Test-Path -LiteralPath $zipPath)) { throw 'Signing bundle already exists. Preserve the frozen signing inputs.' }
New-Item -ItemType Directory -Path $bundlePath | Out-Null
Copy-Item -LiteralPath "$driverPath\packetbridge.sys", "$driverPath\packetbridge.inf", "$driverPath\packetbridge.cat", "$driverPath\packetbridge.pdb" -Destination $bundlePath
Copy-Item -LiteralPath "$buildPath\pbprobe.exe", "$buildPath\wintun.dll", "$buildPath\test-results.json", "$script:RepoRoot\COPYING", "$script:RepoRoot\docs\offload-capability-probe.md" -Destination $bundlePath
Copy-Item -LiteralPath "$PSScriptRoot\verify-signed-driver.ps1", "$PSScriptRoot\catalog-hash.ps1", "$PSScriptRoot\verify-embedded-driver.ps1" -Destination $bundlePath
Copy-Item -LiteralPath "$driverPath\manifest.json" -Destination "$bundlePath\build-manifest.json"
$files = @(Get-ChildItem -LiteralPath $bundlePath -File | Sort-Object Name | ForEach-Object {
    [ordered]@{ name = $_.Name; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[ordered]@{ product = 'PacketBridgeV2'; stage = 'v2-signing-input'; version = $script:Version; file_version = $script:FileVersion;
    source_commit = $driverManifest.source_commit; source_tree_sha256 = $script:SourceFingerprint;
    driver_image_catalog_sha256 = (Get-CatalogMemberHash "$bundlePath\packetbridge.sys");
    kernel_runtime_tested = $false; production_ready = $false; files = $files } |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath "$bundlePath\signing-input.json" -Encoding utf8
Compress-Archive -LiteralPath $bundlePath -DestinationPath $zipPath -CompressionLevel Optimal
Get-Item -LiteralPath $zipPath | Select-Object FullName, Length
Get-FileHash -LiteralPath $zipPath -Algorithm SHA256
