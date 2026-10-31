param([string]$BaselineDirectory='out\dll-token-validation-baseline\723356587cb8')
# Actual old/current validators, exact per-slot model, guarded inputs and real
# DLL cycles. Filter/copy operation counts are distinct from hosted timings.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('common\v2_core.c','1aa57419605064ed1422473b439bd183def99045f112c5d708b20074346794f0'),
    @('common\v2_core.h','2994f94b4b76f0e08b92474badc8f1d05b5214eb400fb2723c035093bea062a1'),
    @('api\v2\session_queue.h','de2da8de2fcdac9f6c613a3d8a49391100fb66aa06415e31a21e1c7ed260c3cf'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected token-validation baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\dll-token-validation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$header='#include "'+(Join-Path $script:RepoRoot 'common\v2_core.h').Replace('\','/')+'"'
$cores=@{};$probeCores=@{}
foreach($variant in @('before','after')) {
    $root=if($variant -eq 'before'){$baseline}else{$script:RepoRoot}
    $body=[IO.File]::ReadAllText((Join-Path $root 'common\v2_core.c')).Replace('#include "v2_core.h"',$header)
    $cores[$variant]=Join-Path $directory "core-$variant.c"
    [IO.File]::WriteAllText($cores[$variant],$body,[Text.UTF8Encoding]::new($false))
    foreach($needle in @('if (items[j].Token.Slot == t->Slot)','if (tokens[j].Slot == tokens[i].Slot)')) {
        if(([regex]::Matches($body,[regex]::Escape($needle))).Count -ne 1){throw "Cannot locate exact comparison: $variant"}
    }
    $body=$body.Replace('if (items[j].Token.Slot == t->Slot)','if ((DuplicateComparisons++, items[j].Token.Slot == t->Slot))')
    $body=$body.Replace('if (tokens[j].Slot == tokens[i].Slot)','if ((DuplicateComparisons++, tokens[j].Slot == tokens[i].Slot))')
    if($variant -eq 'after') {
        $needle='int hit = (*seen & bit) != 0;'
        if(([regex]::Matches($body,[regex]::Escape($needle))).Count -ne 1){throw 'Cannot locate filter probe'}
        $body=$body.Replace($needle,'FilterProbes++; '+$needle)
    }
    $probeCores[$variant]=Join-Path $directory "core-$variant-probes.c"
    [IO.File]::WriteAllText($probeCores[$variant],$body,[Text.UTF8Encoding]::new($false))
}
Start-TestRecording $directory 'operation-results.json'
$executables=@();$benchmarks=@{};$samples=@();$native=@()
$apiSources=@('api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\packet.c','common\flow.c')
$apiLibs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')) {
    foreach($mode in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments
        $lines=@('#define PB_VALIDATION_PROBES 1',('#define PB_VALIDATION_CORE_SOURCE "'+$probeCores[$variant].Replace('\','/')+'"'))
        if($variant -eq 'before'){$lines+='#define PB_VALIDATION_BASELINE 1'}
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\token_validation_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects 'model.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects 'model.exe';Invoke-BuildTool 'cl.exe' ($flags+@($source,'/link',"/out:$exe"));Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case='guarded-model-probes';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        foreach($case in @('api','lifecycle')) {
            $exe=Join-Path $objects "$case.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$cores[$variant])+$apiSources+@("tests\model\${case}_tests.c",'/link',"/out:$exe")+$apiLibs)
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case=$case;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        if($mode -eq 'iso') {
            $exe=Join-Path $objects 'benchmark.exe'
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$cores[$variant])+$apiSources+@('tests\bench\dll_return_bench.c','/link',"/out:$exe")+$apiLibs)
            $benchmarks[$variant]=$exe
            Invoke-BuildTool 'cl.exe' ($flags+@('/c','/FAcs',"/Fa$objects\",$cores[$variant]))
            $listing=Join-Path $objects "core-$variant.cod"
            $native+=@{variant=$variant;path=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant()}
        }
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $lines=@(Invoke-TestTool $benchmarks[$variant] @('--slot-patterns'))
        foreach($line in $lines) {
            if($line.StartsWith('{')) {$sample=$line | ConvertFrom-Json;$sample | Add-Member variant $variant;$sample | Add-Member round $round;$samples+=$sample}
        }
    }
}
$summaries=foreach($group in ($samples | Group-Object variant,case,pattern,batch)) {
    if($group.Count -ne 7){throw "Incomplete benchmark sample group: $($group.Name)"}
    $ns=@($group.Group.ns_per_batch | Sort-Object);$cycles=@($group.Group.cycles_per_batch | Sort-Object);$sample=$group.Group[0];$middle=[int][Math]::Floor($group.Count/2)
    [pscustomobject]@{variant=$sample.variant;case=$sample.case;pattern=$sample.pattern;batch=$sample.batch;samples=$group.Count;median_ns_per_batch=$ns[$middle];median_cycles_per_batch=$cycles[$middle]}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during token-validation checks'}
if(-not $samples.Count){throw 'No benchmark samples were recorded'}
$sourceHashes=foreach($variant in @('before','after')) {
    $root=if($variant -eq 'before'){$baseline}else{$script:RepoRoot}
    @{variant=$variant;original_core_sha256=(Get-FileHash -LiteralPath (Join-Path $root 'common\v2_core.c')).Hash.ToLowerInvariant();
        compiled_core=$cores[$variant];compiled_core_sha256=(Get-FileHash -LiteralPath $cores[$variant]).Hash.ToLowerInvariant();
        probe_core=$probeCores[$variant];probe_core_sha256=(Get-FileHash -LiteralPath $probeCores[$variant]).Hash.ToLowerInvariant()}
}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    method='Frozen/current actual core, same current API/lifecycle. Exact per-slot model with guarded pool/source boundaries, all error positions, duplicate pairs and first-error/transactional-state equivalence in ISO/MS volatile modes. Counters instrument only exact comparisons/filter probes. Benchmarks use uninstrumented actual cores and DLL cycles, six batch lengths and sequential/stride/interleaved slots, seven alternating fixed-affinity runs. Collision fallback and small-batch costs retained. No kernel CPU/latency/throughput claim.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;source_hashes=@($sourceHashes);native=$native;samples=$samples;summaries=@($summaries)} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries | Where-Object { $_.case -eq 'dll-cycle' -and $_.batch -in @(1,8,32) } | Sort-Object pattern,batch,variant | Format-Table
Write-Output (Join-Path $directory 'evidence.json')
