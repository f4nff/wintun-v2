param([string]$BaselineDirectory = 'out\audit2-baseline\a32a2ad7160e',
    [ValidateRange(3,15)][int]$Runs = 5)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
Initialize-BuildEnvironment
$baseline = [IO.Path]::GetFullPath((Join-Path $script:RepoRoot $BaselineDirectory))
if (-not (Test-Path -LiteralPath "$baseline\common\v2_core.c")) { throw 'Missing preserved pre-audit source snapshot.' }
$script:SourceFingerprint = Get-SourceFingerprint
$script:VersionArguments += "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$benchDir = Join-Path $script:RepoRoot "out\audit2-bench\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $benchDir -Force | Out-Null
Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include", "/fo$benchDir\version.res") + $script:VersionArguments + @('tools\build\artifact.rc'))
$variants = @('before','after')
foreach ($variant in $variants) {
    $objects = Join-Path $benchDir $variant
    New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $core = if ($variant -eq 'before') { "$baseline\common\v2_core.c" } else { 'common\v2_core.c' }
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\",'tests\bench\v2_core_bench.c',$core) + $script:VersionArguments + @('/link',"/out:$benchDir\$variant.exe","$benchDir\version.res"))
}
$wrap = @()
foreach ($variant in $variants) {
    $raw = & "$benchDir\$variant.exe" '--wrap'
    $code = $LASTEXITCODE
    $expected = if ($variant -eq 'before') { 2 } else { 0 }
    if ($code -ne $expected) { throw "Unexpected wrap reproduction result: $variant exit $code" }
    $wrap += [ordered]@{ variant=$variant; exit_code=$code; observation=($raw | ConvertFrom-Json) }
}
$measurements = @()
for ($run=0; $run -lt $Runs; ++$run) {
    $order = if ($run % 2) { @('after','before') } else { $variants }
    foreach ($variant in $order) {
        $lines = & "$benchDir\$variant.exe"
        if ($LASTEXITCODE -ne 0) { throw "Benchmark failed: $variant" }
        foreach ($line in $lines) {
            $sample = $line | ConvertFrom-Json
            $measurements += [pscustomobject][ordered]@{ run=$run; variant=$variant; slots=$sample.slots; case=$sample.case;
                iterations=$sample.iterations; ns_per_attempt=$sample.ns_per_attempt; cycles_per_attempt=$sample.cycles_per_attempt }
        }
    }
}
function Get-Median([object[]]$Values) {
    $sorted = @($Values | Sort-Object)
    $middle = [int][Math]::Floor($sorted.Count/2)
    if ($sorted.Count % 2) { return $sorted[$middle] }
    return ($sorted[$middle-1]+$sorted[$middle])/2
}
$medians = @()
foreach ($slots in @(32,31)) { foreach ($case in @('reserve-release','full')) {
    $pair = @{}
    foreach ($variant in $variants) {
        $samples = @($measurements | Where-Object { $_.variant -eq $variant -and $_.slots -eq $slots -and $_.case -eq $case })
        $pair[$variant] = [ordered]@{ ns=Get-Median @($samples.ns_per_attempt); cycles=Get-Median @($samples.cycles_per_attempt) }
    }
    $medians += [ordered]@{ slots=$slots; case=$case; before=$pair.before; after=$pair.after;
        ns_reduction_percent=[Math]::Round(100*(1-$pair.after.ns/$pair.before.ns),2) }
} }
[ordered]@{ source_tree_sha256=$script:SourceFingerprint; baseline_directory=$baseline;
    baseline_core_sha256=(Get-FileHash -LiteralPath "$baseline\common\v2_core.c").Hash.ToLowerInvariant();
    current_core_sha256=(Get-FileHash -LiteralPath 'common\v2_core.c').Hash.ToLowerInvariant();
    compiler='MSVC x64 /O2 /MT /W4 /WX, separate translation units, no LTO'; runs=$Runs;
    scope='User-mode PB_POOL only; not kernel CPU, NDIS or VPN throughput. Alternating order, each thread pinned to the lowest eligible CPU in its process affinity mask; system noise remains possible.';
    kernel_runtime_tested=$false; wrap_reproduction=$wrap; medians=$medians; samples=$measurements } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath "$benchDir\benchmark-results.json" -Encoding utf8
$medians | ConvertTo-Json -Depth 5
Get-Item -LiteralPath "$benchDir\benchmark-results.json" | Select-Object FullName,Length
