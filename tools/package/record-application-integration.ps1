param([Parameter(Mandatory)][string]$ApplicationRoot,
    [string]$TestLog = 'out/vpn-all-tests-latest.log', [string]$BuildLog = 'out/vpn-build-latest.log')
. (Join-Path $PSScriptRoot '..\build\common.ps1')
$applicationPath = (Resolve-Path -LiteralPath $ApplicationRoot).Path
$source = Get-SourceFingerprint
$destination = Join-Path $script:RepoRoot "out\integration\$($source.Substring(0,12))"
if (Test-Path -LiteralPath $destination) { throw 'Preserve the recorded integration snapshot' }
$vendor = Get-Content -LiteralPath (Join-Path $applicationPath 'vendor/packetbridge/packetbridge-source.json') -Raw | ConvertFrom-Json
foreach ($file in $vendor.files) {
    $canonical = Join-Path $script:RepoRoot "rust/$($file.path)"
    $copy = Join-Path $applicationPath "vendor/packetbridge/$($file.path)"
    foreach ($path in @($canonical,$copy)) {
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) { throw "Rust binding copy mismatch: $path" }
    }
}
$testText = Get-Content -LiteralPath $TestLog -Raw
$match = [regex]::Match($testText, 'test result: ok\. (\d+) passed; (\d+) failed; (\d+) ignored;')
if (-not $match.Success -or $match.Groups[2].Value -ne '0') { throw 'Successful full application test evidence is required' }
if ((Get-Content -LiteralPath $BuildLog -Raw) -notmatch 'Finished `dev` profile') { throw 'Successful application build evidence is required' }
$executable = Join-Path $applicationPath 'vpn-client/target/x86_64-pc-windows-gnu/debug/vpn-client.exe'
$relative = @('vpn-client/Cargo.toml','vpn-client/Cargo.lock','vpn-client/src/main.rs','vpn-client/src/client_device.rs',
    'vpn-client/src/wintun_pump.rs','vpn-client/src/wintun_io.rs','vpn-client/src/wintun_ext.rs',
    'vpn-client/src/packetbridge_backend.rs','vpn-client/src/packetbridge_submit.rs','vpn-client/src/packetbridge_submit_tests.rs')
New-Item -ItemType Directory -Path $destination | Out-Null
$files = foreach ($path in $relative) {
    $full = Join-Path $applicationPath $path
    $copy = Join-Path $destination "source/$path"
    New-Item -ItemType Directory -Path (Split-Path $copy) -Force | Out-Null
    Copy-Item -LiteralPath $full -Destination $copy
    [ordered]@{ path=$path; sha256=(Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash.ToLowerInvariant() }
}
Copy-Item -LiteralPath $TestLog -Destination (Join-Path $destination 'application-tests.log')
Copy-Item -LiteralPath $BuildLog -Destination (Join-Path $destination 'application-build.log')
$legacy = Join-Path $applicationPath 'vpn-client/wintun.dll'
[ordered]@{ product='PacketBridgeV2'; version=$script:Version; source_tree_sha256=$source; application_root=$applicationPath;
    application_git_head=(git -C $applicationPath rev-parse HEAD); application_working_files_preserved=$true;
    test_target='x86_64-pc-windows-gnu'; passed=[int]$match.Groups[1].Value; failed=0; ignored=[int]$match.Groups[3].Value;
    executable=$executable; executable_profile='debug'; executable_sha256=(Get-FileHash -LiteralPath $executable -Algorithm SHA256).Hash.ToLowerInvariant();
    legacy_dll_sha256=(Get-FileHash -LiteralPath $legacy -Algorithm SHA256).Hash.ToLowerInvariant();
    legacy_signature_status=(Get-AuthenticodeSignature -LiteralPath $legacy).Status.ToString();
    default_offloads=0; runtime_kernel_tested=$false; performance_tested=$false; runtime_fallback='stop then next auto startup uses legacy';
    files=@($files); binding_files=@($vendor.files) } | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $destination 'application-integration.json') -Encoding utf8
Get-Item -LiteralPath (Join-Path $destination 'application-integration.json') | Select-Object FullName,Length
