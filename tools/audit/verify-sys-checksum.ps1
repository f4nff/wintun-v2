param([string]$BaselineDirectory='out\sys-checksum-baseline\f32d9edfaf9f')
# Frozen/current scalar checksum core with the same driver orchestration/fixtures.
# Hosted timing is recorded separately from correctness and is never a CI gate.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-checksum\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPacket=Join-Path $directory 'packet-before.c'
$old=[IO.File]::ReadAllText((Join-Path $baseline 'common\packet.c'))
$header='#include "'+(Join-Path $script:RepoRoot 'common\packet.h').Replace('\','/')+'"'
# Preserve one packet-header identity when the counting fixture includes core C.
[IO.File]::WriteAllText($oldPacket,$old.Replace('#include "packet.h"',$header),[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@();$benchmarks=@{}
foreach($variant in @('before','after')) {
    $packet=if($variant -eq 'before'){$oldPacket}else{Join-Path $script:RepoRoot 'common\packet.c'}
    foreach($case in @('checksum','describe','coalesce','receive-guarded','bench')) {
        $objects=Join-Path $directory "$variant-$case";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $sources=@($packet);$flags=@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments
        if($case -eq 'checksum'){$sources+='tests\model\checksum_tests.c'}
        elseif($case -eq 'describe'){$sources+='tests\model\describe_checksum_tests.c'}
        elseif($case -eq 'coalesce') {
            $source=Join-Path $objects 'coalesce-fixture.c'
            $lines=@(('#define PB_PACKET_SOURCE "'+$packet.Replace('\','/')+'"'),
                ('#include "'+(Join-Path $script:RepoRoot 'tests\model\coalesce_path_tests.c').Replace('\','/')+'"'))
            $lines | Set-Content -LiteralPath $source -Encoding utf8
            $sources=@('common\v2_core.c','common\coalesce.c',$source)
        }
        elseif($case -eq 'receive-guarded') {
            $flags+='/DPB_GUARD_TESTS=1'
            $sources+=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','tests\model\drain_boundary_tests.c')
        } else {$flags+='/GL';$sources+='tests\bench\checksum_bench.c'}
        $exe=Join-Path $objects 'fixture.exe';$link=@('/link',"/out:$exe");if($case -eq 'bench'){$link+='/ltcg'}
        Invoke-BuildTool 'cl.exe' ($flags+$sources+$link)
        if($case -eq 'bench'){$benchmarks[$variant]=$exe}else{Invoke-TestTool $exe @()}
        $executables+=@{variant=$variant;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
$samples=@()
for($run=0;$run -lt 7;++$run) {
    $order=if($run%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $raw=@(Invoke-TestTool $benchmarks[$variant] @())
        foreach($line in $raw) {
            $measurement=$line | ConvertFrom-Json
            if($measurement.calls -le 0 -or $measurement.cycles_per_call -le 0){throw 'Invalid checksum benchmark sample'}
            $samples+=[pscustomobject]@{run=$run;variant=$variant;measurement=$measurement}
        }
    }
}
$summary=foreach($length in @($samples | ForEach-Object {$_.measurement.length} | Sort-Object -Unique)) {
    $entry=[ordered]@{length=$length}
    $reference=$null
    foreach($variant in @('before','after')) {
        $rows=@($samples | Where-Object {$_.variant -eq $variant -and $_.measurement.length -eq $length})
        if($rows.Count -ne 7){throw 'Missing checksum benchmark runs'}
        foreach($row in $rows) {
            $m=$row.measurement;$value="$($m.result)|$($m.calls)|$($m.cpu_mask)"
            if($null -eq $reference){$reference=$value}elseif($reference -ne $value){throw 'Checksum benchmark result/CPU mismatch'}
        }
        $cycles=@($rows.measurement.cycles_per_call | Sort-Object);$ns=@($rows.measurement.ns_per_call | Sort-Object)
        $entry[$variant]=@{median_cycles=$cycles[3];median_ns=$ns[3]}
    }
    $entry['after_before_cycles_ratio']=$entry.after.median_cycles/$entry.before.median_cycles
    $entry
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during checksum checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_packet_sha256=(Get-FileHash -LiteralPath (Join-Path $baseline 'common\packet.c')).Hash.ToLowerInvariant();current_packet_sha256=(Get-FileHash 'common\packet.c').Hash.ToLowerInvariant();
    method='Frozen/current packet core, independent uint64 byte oracle for exact uint32 raw sum and folded result, guarded read-only input, exhaustive short lengths and large/alignment/overflow/chaining boundaries. Same actual Describe, coalescing helper and guarded receive/return/worker/query. Scalar core timing uses volatile indirect calls, /O2 /GL /LTCG, one allowed CPU and seven alternating runs; results/CPU/call counts must match. Timing is recorded, not a correctness gate or kernel measurement.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;summary=$summary;samples=$samples} | ConvertTo-Json -Depth 10 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
