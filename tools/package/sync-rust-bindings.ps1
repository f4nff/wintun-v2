param([Parameter(Mandatory)][string]$ApplicationRoot)
. (Join-Path $PSScriptRoot '..\build\common.ps1')
$applicationPath = (Resolve-Path -LiteralPath $ApplicationRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $applicationPath 'vpn-client\Cargo.toml'))) { throw 'Expected the rust-quiche-vpn application root' }
$destinationRoot = Join-Path $applicationPath 'vendor\packetbridge'
$markerPath = Join-Path $destinationRoot 'packetbridge-source.json'
$oldFiles = @{}
if (Test-Path -LiteralPath $destinationRoot) {
    if (-not (Test-Path -LiteralPath $markerPath)) { throw 'An unmanaged vendor/packetbridge already exists; refusing to overwrite it' }
    $previous = Get-Content -LiteralPath $markerPath -Raw | ConvertFrom-Json
    if ($previous.product -ne 'PacketBridgeV2') { throw 'Unexpected vendor ownership marker' }
    foreach ($file in $previous.files) { $oldFiles[$file.path] = $file.sha256 }
}
$paths = @(git -C $script:RepoRoot ls-files --cached --others --exclude-standard -- rust)
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate Rust binding sources' }
$files = foreach ($path in $paths) {
    if (-not $path.StartsWith('rust/')) { throw 'Unexpected source path' }
    $relative = $path.Substring(5)
    $source = Join-Path $script:RepoRoot $path
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { continue }
    $destination = Join-Path $destinationRoot $relative
    if (Test-Path -LiteralPath $destination) {
        if (-not $oldFiles.ContainsKey($relative) -or
            (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant() -ne $oldFiles[$relative]) {
            throw "Application-side binding was modified: $relative"
        }
    }
    [ordered]@{ path = $relative; source = $source; destination = $destination; sha256 = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash.ToLowerInvariant() }
}
foreach ($file in $files) {
    New-Item -ItemType Directory -Path (Split-Path $file.destination) -Force | Out-Null
    Copy-Item -LiteralPath $file.source -Destination $file.destination -Force
}
Copy-Item -LiteralPath (Join-Path $script:RepoRoot 'COPYING') -Destination (Join-Path $destinationRoot 'COPYING') -Force
[ordered]@{ product = 'PacketBridgeV2'; version = $script:Version; source_root = $script:RepoRoot;
    files = @($files | ForEach-Object { [ordered]@{ path = $_.path; sha256 = $_.sha256 } }) } |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $markerPath -Encoding utf8
Write-Output "Updated $($files.Count) binding sources in $destinationRoot"
