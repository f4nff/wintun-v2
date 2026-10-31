param(
    [Parameter(Mandatory)][string]$Directory,
    [Parameter(Mandatory)][string]$ExpectedManifest,
    [string]$SignTool
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'catalog-hash.ps1')
$directoryPath = (Resolve-Path -LiteralPath $Directory).Path
$expected = Get-Content -LiteralPath $ExpectedManifest -Raw | ConvertFrom-Json
if ($expected.product -ne 'PacketBridgeV2' -or $expected.stage -notin @('M1-signing-input','v2-signing-input') -or
    $expected.source_tree_sha256 -notmatch '^[a-f0-9]{64}$') { throw 'Invalid signing input manifest' }
foreach ($name in @('packetbridge.sys', 'packetbridge.inf', 'packetbridge.cat')) {
    if (-not (Test-Path -LiteralPath (Join-Path $directoryPath $name) -PathType Leaf)) { throw "Missing signed input: $name" }
}
$sys = Join-Path $directoryPath 'packetbridge.sys'
$inf = Join-Path $directoryPath 'packetbridge.inf'
$cat = Join-Path $directoryPath 'packetbridge.cat'
$info = (Get-Item -LiteralPath $sys).VersionInfo
if ($info.FileVersion -ne $expected.version -or
    "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)" -ne $expected.file_version -or
    $info.ProductName -ne 'PacketBridgeV2' -or $info.OriginalFilename -ne 'packetbridge.sys' -or
    $info.PrivateBuild -ne $expected.source_tree_sha256) { throw 'Signed SYS version, identity or source build mismatch' }
if ((Get-CatalogMemberHash $sys) -ne $expected.driver_image_catalog_sha256) { throw 'Signed SYS image differs from the signing input' }
$originalInf = @($expected.files | Where-Object name -eq 'packetbridge.inf')
if ($originalInf.Count -ne 1 -or (Get-FileHash -LiteralPath $inf -Algorithm SHA256).Hash.ToLowerInvariant() -ne $originalInf[0].sha256) {
    throw 'INF changed after catalog generation; a new package and signature are required'
}
if (-not $SignTool) {
    $SignTool = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe'
}
if (-not (Test-Path -LiteralPath $SignTool -PathType Leaf)) { throw 'Supply -SignTool with the SDK signtool.exe path' }
& $SignTool verify /v /kp /c $cat $sys
if ($LASTEXITCODE -ne 0) { throw 'Kernel policy / catalog SYS verification failed' }
& $SignTool verify /v /pa /c $cat $inf
if ($LASTEXITCODE -ne 0) { throw 'Signed catalog / INF membership verification failed' }
$files = foreach ($path in @($sys, $inf, $cat)) {
    [ordered]@{ name = [IO.Path]::GetFileName($path); sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() }
}
[ordered]@{ product = 'PacketBridgeV2'; version = $expected.version; source_tree_sha256 = $expected.source_tree_sha256;
    kernel_signature_verified = $true; runtime_loaded = $false; files = @($files) } | ConvertTo-Json -Depth 5
# This verifies signatures on this host. Target-machine Secure Boot/HVCI loading still needs a runtime test.
