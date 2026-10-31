param([string]$BaselineDirectory='out\dll-remediation-baseline')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
Initialize-BuildEnvironment
$fingerprint=Get-SourceFingerprint
$directory=Join-Path $script:RepoRoot "out\dll-bench\$($fingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib')
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $root=if($variant -eq 'before'){$baseline}else{$script:RepoRoot}
    $sources=@('api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c') | ForEach-Object { Join-Path $root $_ }
    $exports=Join-Path $root 'tests\model\api_test_exports.def'
    Invoke-BuildTool 'cl.exe' (@('/nologo','/LD','/std:c17','/MT','/O2','/W4','/WX','/DPB_API_TESTS=1',"/Fo$objects\")+$script:VersionArguments+$sources+@('/link',"/out:$objects\pbapi-testshim.dll","/def:$exports","/implib:$objects\test.lib")+$libs)
}
Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$directory\",'tests\bench\dll_wait_bench.c')+$script:VersionArguments+@('/link',"/out:$directory\bench.exe"))
$samples=@()
for($run=0;$run -lt 7;++$run) {
    $order=if($run%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $raw=& "$directory\bench.exe" "$directory\$variant\pbapi-testshim.dll"
        if($LASTEXITCODE -ne 0){throw "Benchmark failed: $variant"}
        $samples += [pscustomobject]@{run=$run;variant=$variant;measurement=($raw|ConvertFrom-Json)}
    }
}
$summary=foreach($variant in @('before','after')) {
    $rows=@($samples|Where-Object variant -eq $variant)
    $ns=@($rows.measurement.ns_per_call|Sort-Object);$cycles=@($rows.measurement.cycles_per_call|Sort-Object)
    [ordered]@{variant=$variant;median_ns=$ns[3];median_cycles=$cycles[3]}
}
$hashes=foreach($variant in @('before','after')) {
    $root=if($variant -eq 'before'){$baseline}else{$script:RepoRoot}
    [ordered]@{variant=$variant;session_sha256=(Get-FileHash (Join-Path $root 'api\v2\session.c')).Hash;core_sha256=(Get-FileHash (Join-Path $root 'common\v2_core.c')).Hash}
}
[ordered]@{source_tree_sha256=$fingerprint;method='Hosted empty Wait(0), fake status, 200000 calls, seven alternating runs, fixed allowed CPU; no driver I/O or throughput measurement';summary=$summary;samples=$samples;source_hashes=$hashes}|ConvertTo-Json -Depth 8|Set-Content "$directory\results.json" -Encoding utf8
$summary|Format-Table;Write-Output "$directory\results.json"
