param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string]$ExpectedManifest)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath $Directory).Path
$expected = Get-Content -LiteralPath $ExpectedManifest -Raw | ConvertFrom-Json
if ($expected.product -ne 'PacketBridgeV2' -or $expected.kind -ne 'v2-development-unsigned' -or
    $expected.version -ne '2.0.0' -or $expected.file_version -ne '2.0.0.0' -or
    $expected.source_tree_sha256 -cnotmatch '^[a-f0-9]{64}$') { throw 'Invalid unsigned development manifest' }
$files = foreach ($name in @('packetbridge.sys','packetbridge.inf','packetbridge.cat')) {
    $path = Join-Path $root $name
    $entry = @($expected.files | Where-Object name -CEQ $name)
    if ($entry.Count -ne 1 -or $entry[0].sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        -not (Test-Path -LiteralPath $path -PathType Leaf) -or
        (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $entry[0].sha256) { throw "Unsigned input hash mismatch: $name" }
    # INF has no standalone Authenticode signature. Its exact bytes are checked above.
    if ($name -ne 'packetbridge.inf' -and (Get-AuthenticodeSignature -LiteralPath $path).Status -ne 'NotSigned') {
        throw "UnsignedTestApi accepts only unsigned frozen inputs: $name"
    }
    [ordered]@{name=$name;sha256=$entry[0].sha256}
}
$sys = Join-Path $root 'packetbridge.sys'
$info = (Get-Item -LiteralPath $sys).VersionInfo
if ($info.ProductName -ne 'PacketBridgeV2' -or $info.OriginalFilename -ne 'packetbridge.sys' -or
    $info.FileVersion -ne $expected.version -or $info.PrivateBuild -cne $expected.source_tree_sha256 -or
    "$($info.FileMajorPart).$($info.FileMinorPart).$($info.FileBuildPart).$($info.FilePrivatePart)" -ne $expected.file_version) {
    throw 'Unsigned SYS identity/source mismatch'
}
$image = [IO.File]::ReadAllBytes($sys)
if ($image.Length -lt 64 -or [BitConverter]::ToUInt16($image,0) -ne 0x5a4d) { throw 'Invalid SYS PE image' }
$pe = [BitConverter]::ToUInt32($image,0x3c)
if ($pe -gt $image.Length-24 -or [BitConverter]::ToUInt32($image,$pe) -ne 0x4550 -or
    [BitConverter]::ToUInt16($image,$pe+4) -ne 0x8664) { throw 'Unsigned SYS must be x64' }
[ordered]@{product='PacketBridgeV2';version=$expected.version;source_tree_sha256=$expected.source_tree_sha256;
    unsigned_test_only=$true;kernel_signature_verified=$false;runtime_loaded=$false;files=@($files)} | ConvertTo-Json -Depth 5
