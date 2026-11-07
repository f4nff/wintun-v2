param([ValidateSet('Api','Driver')][string]$Stage='Api')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\build.ps1') -Stage $Stage
$analysis = Join-Path $outDir "analysis-unified-$Stage"
New-Item -ItemType Directory -Path $analysis -Force | Out-Null
$units = if ($Stage -eq 'Api') { $sources } else {
    @('driver\wintun.c','common\v2_core.c','common\packet.c','common\coalesce.c','common\ethernet.c','common\offload_policy.c','common\flow.c')
}
$arguments = if ($Stage -eq 'Api') { $flags } else { $kernelArgs }
$arguments = @($arguments | Where-Object { $_ -ne '/WX' })
$results = @()
foreach ($source in $units) {
    $name = $source.Replace('\','-').Replace('.c','')
    $sarif = Join-Path $analysis "$name.sarif"
    $log = Join-Path $analysis "$name.log"
    & cl.exe @arguments '/analyze:only' "/analyze:log$sarif" "/Fo$analysis\$name.obj" $source *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 30; throw "Analysis compile failed: $source" }
    foreach ($run in (Get-Content -LiteralPath $sarif -Raw | ConvertFrom-Json).runs) {
        foreach ($result in $run.results) {
            $loc = $result.locations[0].physicalLocation
            $uri = $loc.artifactLocation.uri
            if (-not $uri) { $uri = $run.artifacts[$loc.artifactLocation.index].location.uri }
            $results += [ordered]@{ unit=$source; rule=$result.ruleId; uri=$uri; line=$loc.region.startLine; message=$result.message.text }
        }
    }
}
[ordered]@{ source_tree_sha256=$script:SourceFingerprint; units=$units.Count; kernel_runtime_tested=$false; diagnostics=$results } |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $analysis 'summary.json') -Encoding utf8
Write-Output "Analysis summary: $analysis\summary.json"
