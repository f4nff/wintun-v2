param([Parameter(Mandatory)][string]$DriverDirectory)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$verify = Join-Path $repo 'tools\package\verify-unsigned-driver.ps1'
$manifest = Join-Path $DriverDirectory 'manifest.json'
& $verify -Directory $DriverDirectory -ExpectedManifest $manifest | Out-Null
$temporary = Join-Path $repo "out\unsigned-package-validation\$([Guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $temporary | Out-Null
$cases = @('changed-sys','changed-inf','changed-cat','wrong-source','wrong-kind')
foreach ($case in $cases) {
    $directory = Join-Path $temporary $case
    New-Item -ItemType Directory -Path $directory | Out-Null
    foreach ($name in @('packetbridge.sys','packetbridge.inf','packetbridge.cat','manifest.json')) {
        Copy-Item -LiteralPath (Join-Path $DriverDirectory $name) -Destination $directory
    }
    if ($case.StartsWith('changed-')) {
        $path = Join-Path $directory ('packetbridge.' + $case.Substring(8))
        $bytes = [IO.File]::ReadAllBytes($path); $bytes[$bytes.Length-1] = $bytes[$bytes.Length-1] -bxor 1
        [IO.File]::WriteAllBytes($path,$bytes)
    } else {
        $path = Join-Path $directory 'manifest.json'
        $record = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        if ($case -eq 'wrong-source') { $record.source_tree_sha256 = '0' * 64 }
        else { $record.kind = 'v2-signed-driver-api' }
        $record | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $path -Encoding utf8
    }
    $rejected = $false
    try { & $verify -Directory $directory -ExpectedManifest (Join-Path $directory 'manifest.json') | Out-Null }
    catch { $rejected = $true }
    if (-not $rejected) { throw "Unsigned package verification accepted $case" }
}
Write-Output 'PASS: actual unsigned input verifier accepts the frozen package and rejects SYS/INF/CAT tampering, source mismatch and wrong manifest kind; no driver installed.'
