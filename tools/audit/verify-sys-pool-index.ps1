param([string]$BaselineDirectory='out\sys-pool-index-baseline\eabc3c674a79')
# Actual pool bodies, guarded independent model, deterministic search probes and
# separate uninstrumented user-mode profiles. Never loads a SYS.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-pool-index\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
function Replace-Once([string]$source,[string]$from,[string]$to) {
    if([regex]::Matches($source,[regex]::Escape($from)).Count -ne 1){throw "Expected one instrumentation anchor: $from"}
    $source.Replace($from,$to)
}
Start-TestRecording $directory 'operation-results.json'
$executables=@();$benchmarks=@{}
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    $sourcePath=if($variant -eq 'before'){Join-Path $baseline 'common\v2_core.c'}else{Join-Path $script:RepoRoot 'common\v2_core.c'}
    $body=[IO.File]::ReadAllText($sourcePath)
    $body=Replace-Once $body '#include "v2_core.h"' ('#include "'+(Join-Path $script:RepoRoot 'common\v2_core.h').Replace('\','/')+'"')
    $plain=Join-Path $objects 'core-plain.c';[IO.File]::WriteAllText($plain,$body,[Text.UTF8Encoding]::new($false))
    if($variant -eq 'before') {
        $body=Replace-Once $body 'if (s->State != PB_SLOT_FREE) continue;' 'if ((SlotProbes++, s->State) != PB_SLOT_FREE) continue;'
    } else {
        $body=Replace-Once $body 'if (pool->Slots[cursor].State == PB_SLOT_FREE)' 'if ((SlotProbes++, pool->Slots[cursor].State) == PB_SLOT_FREE)'
        $body=Replace-Once $body 'slot >= pool->Count || pool->Slots[slot].State != PB_SLOT_FREE' 'slot >= pool->Count || (SlotProbes++, pool->Slots[slot].State) != PB_SLOT_FREE'
        $body=Replace-Once $body 'uint32_t candidates = pool->FreeBits[word] & ~low;' 'uint32_t candidates = (WordProbes++, pool->FreeBits[word]) & ~low;'
        $body=Replace-Once $body 'candidates = pool->FreeBits[word];' 'candidates = (WordProbes++, pool->FreeBits[word]);'
    }
    $instrumented=Join-Path $objects 'core-probes.c';[IO.File]::WriteAllText($instrumented,$body,[Text.UTF8Encoding]::new($false))
    $lines=@('#define PB_POOL_PROBES 1',('#define PB_POOL_CORE_SOURCE "'+$instrumented.Replace('\','/')+'"'))
    if($variant -eq 'before'){$lines+='#define PB_POOL_INDEX_BASELINE 1'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\pool_index_tests.c').Replace('\','/')+'"'
    $fixture=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $fixture -Encoding utf8
    $exe=Join-Path $objects 'pool-tests.exe'
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($fixture,'/link',"/out:$exe"))
    Invoke-TestTool $exe @()
    $executables+=@{variant=$variant;case='guarded-model-and-probes';path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    $bench=Join-Path $objects 'bench.exe'
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
        @($plain,'tests\bench\v2_core_bench.c','/link',"/out:$bench"))
    Invoke-TestTool $bench @('--wrap');$benchmarks[$variant]=$bench
    $executables+=@{variant=$variant;case='uninstrumented-profile';path=$bench;sha256=(Get-FileHash -LiteralPath $bench).Hash.ToLowerInvariant()}
}
$samples=@()
for($run=0;$run -lt 7;++$run) {
    $order=if($run%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $raw=@(& $benchmarks[$variant]);if($LASTEXITCODE -ne 0){throw "Pool profile failed: $variant"}
        foreach($line in $raw) {
            $row=$line | ConvertFrom-Json
            $samples+=[pscustomobject]@{run=$run;variant=$variant;slots=$row.slots;case=$row.case;iterations=$row.iterations;ns=$row.ns_per_attempt;cycles=$row.cycles_per_attempt}
        }
    }
}
$summary=foreach($group in ($samples | Group-Object variant,slots,case)) {
    $rows=@($group.Group);if($rows.Count -ne 7){throw 'Incomplete pool profile'}
    [ordered]@{variant=$rows[0].variant;slots=$rows[0].slots;case=$rows[0].case;median_ns=@($rows.ns | Sort-Object)[3];median_cycles=@($rows.cycles | Sort-Object)[3]}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during pool-index checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_core_sha256=(Get-FileHash (Join-Path $baseline 'common\v2_core.c')).Hash.ToLowerInvariant();
    current_core_sha256=(Get-FileHash 'common\v2_core.c').Hash.ToLowerInvariant();
    method='Frozen/current pool core, same current private type declarations and guarded independent round-robin model. Instrumented copies add counters only at exact slot-state tests and index-search word reads. Separate uninstrumented profiles use fixed allowed CPU, seven alternating runs, free-Next/full/sole-free cases. Constructor, ownership transitions and token generations remain the tested functions. Neither probe counts nor user-mode timings measure kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;profile_summary=@($summary);profile_samples=$samples} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summary | Format-Table;Write-Output (Join-Path $directory 'evidence.json')
