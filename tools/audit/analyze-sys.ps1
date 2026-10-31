# Builds a separate unsigned source-fingerprint output and preserves raw MSVC/SARIF diagnostics.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\build\build.ps1') -Stage Driver
$analysisDir = Join-Path $outDir 'analysis'
New-Item -ItemType Directory -Path $analysisDir -Force | Out-Null
$sources = @('driver\wintun.c','common\v2_core.c','common\packet.c','common\coalesce.c','common\ethernet.c','common\offload_policy.c','common\flow.c')
$diagnostics = @()
$sourcePrefix = ([uri]($script:RepoRoot.TrimEnd('\') + '\')).AbsoluteUri
foreach ($source in $sources) {
    $name = [IO.Path]::GetFileNameWithoutExtension($source)
    $sarif = Join-Path $analysisDir "$name.sarif"
    $log = Join-Path $analysisDir "$name.log"
    & cl.exe @kernelArgs '/analyze:only' "/analyze:log$sarif" "/Fo$analysisDir\$name.obj" $source *> $log
    if ($LASTEXITCODE -ne 0) { Get-Content -LiteralPath $log -Tail 40; throw "Static analysis compilation failed: $source" }
    foreach ($run in (Get-Content -LiteralPath $sarif -Raw | ConvertFrom-Json).runs) {
        foreach ($result in $run.results) {
            $location = $result.locations[0].physicalLocation
            $artifact = $location.artifactLocation
            $uri = if ($artifact.uri) { $artifact.uri } else { $run.artifacts[$artifact.index].location.uri }
            $isProject = $uri.StartsWith($sourcePrefix,[StringComparison]::OrdinalIgnoreCase) -and -not $uri.ToLowerInvariant().Contains('/release/')
            $diagnostics += [pscustomobject][ordered]@{ translation_unit=$source; rule=$result.ruleId; source=$uri;
                line=$location.region.startLine; message=$result.message.text; project_source=$isProject }
        }
    }
}
$project = @($diagnostics | Where-Object project_source)
[ordered]@{ source_tree_sha256=$script:SourceFingerprint; compiler='MSVC /analyze:only'; translation_units=$sources.Count;
    diagnostics=$diagnostics.Count; project_diagnostics=$project.Count; project_rules=@($project | Group-Object rule | ForEach-Object { [ordered]@{ rule=$_.Name; count=$_.Count } });
    manual_review_required=$true; kernel_runtime_tested=$false; results=$diagnostics } | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $analysisDir 'analysis-summary.json') -Encoding utf8
Get-Item -LiteralPath (Join-Path $analysisDir 'analysis-summary.json') | Select-Object FullName,Length
