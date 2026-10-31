param([string]$BaselineDirectory)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
Initialize-BuildEnvironment
$fingerprint=Get-SourceFingerprint
$directory=Join-Path $script:RepoRoot "out\segment-bench\$($fingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$variants=@('after');$baseline=$null
if($BaselineDirectory) { $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$variants=@('before','after') }
$sources=@()
foreach($variant in $variants) {
    $objects=Join-Path $directory $variant
    New-Item -ItemType Directory -Path "$objects\common","$objects\include" -Force | Out-Null
    $root=if($variant -eq 'before'){$baseline}else{Join-Path $script:RepoRoot 'common'}
    foreach($name in @('packet.c','packet.h')) {Copy-Item -LiteralPath (Join-Path $root $name) -Destination "$objects\common\$name"}
    Copy-Item -LiteralPath (Join-Path $script:RepoRoot 'include\packetbridge_types.h') -Destination "$objects\include\packetbridge_types.h"
    Invoke-BuildTool 'cl.exe' @('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\","$objects\common\packet.c",'tests\bench\segment_bench.c','/link',"/out:$objects\bench.exe")
    $sources+=[ordered]@{variant=$variant;packet_sha256=(Get-FileHash "$objects\common\packet.c").Hash.ToLowerInvariant();header_sha256=(Get-FileHash "$objects\common\packet.h").Hash.ToLowerInvariant()}
}
$samples=@()
for($run=0;$run -lt 5;++$run) {
    $order=if($run%2){@($variants[($variants.Count-1)..0])}else{$variants}
    foreach($variant in $order) {
        $rows=& "$directory\$variant\bench.exe"
        if($LASTEXITCODE -ne 0){throw "Segment benchmark failed: $variant"}
        foreach($row in $rows) {$samples+=[pscustomobject]@{run=$run;variant=$variant;measurement=($row | ConvertFrom-Json)}}
    }
}
$summary=foreach($variant in $variants) {foreach($mss in @(16,1024)) {foreach($batch in @(1,8,32)) {
    $rows=@($samples | Where-Object {$_.variant -eq $variant -and $_.measurement.mss -eq $mss -and $_.measurement.batch -eq $batch})
    $ns=@($rows.measurement.ns_per_batch | Sort-Object);$cycles=@($rows.measurement.cycles_per_batch | Sort-Object)
    [pscustomobject][ordered]@{variant=$variant;mss=$mss;batch=$batch;median_ns_per_batch=$ns[2];median_cycles_per_batch=$cycles[2]}
}}}
if((Get-SourceFingerprint) -ne $fingerprint){throw 'Sources changed during segment benchmark'}
[ordered]@{source_tree_sha256=$fingerprint;method='Hosted batch validation plus checksum/copy, five alternating runs, fixed allowed CPU, MSVC /O2 /MT without LTO; no driver throughput measurement';sources=$sources;summary=$summary;samples=$samples} |
    ConvertTo-Json -Depth 7 | Set-Content "$directory\results.json" -Encoding utf8
$summary | Format-Table
Write-Output "$directory\results.json"
