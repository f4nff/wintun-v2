param([string]$BaselineDirectory='out\queue-pop-baseline\5788103200a3')
# Frozen/current shared core, unchanged DLL and hosted kernel boundaries. No SYS load.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('common\v2_core.c','c0ed6bf51fd890b27667254839ac6886f365acf9a198ce406a0a0f8748995914'),
    @('common\v2_core.h','2994f94b4b76f0e08b92474badc8f1d05b5214eb400fb2723c035093bea062a1'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected queue-pop baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\queue-pop\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldCore=Join-Path $directory 'core-before.c'
$header='#include "'+(Join-Path $script:RepoRoot 'common\v2_core.h').Replace('\','/')+'"'
$body=[IO.File]::ReadAllText((Join-Path $baseline 'common\v2_core.c')).Replace('#include "v2_core.h"',$header)
[IO.File]::WriteAllText($oldCore,$body,[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@();$native=@();$benchmarks=@{};$samples=@()
$support=@('api\v2\adapter.c','api\v2\driver_package.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')) {
    $core=if($variant -eq 'before'){$oldCore}else{Join-Path $script:RepoRoot 'common\v2_core.c'}
    foreach($mode in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\common","/Fo$objects\")+$script:VersionArguments
        foreach($test in @('queue_pop','queue_order','completion_progress','drain_boundary','api','completion_retire')) {
            $lines=@();$sources=@()
            if($test -in @('queue_pop','queue_order')) {$lines+='#define PB_CORE_SOURCE "'+$core.Replace('\','/')+'"'}
            else {$sources+=$core}
            $lines+='#include "'+(Join-Path $script:RepoRoot "tests\model\${test}_tests.c").Replace('\','/')+'"'
            if($test -in @('api','completion_retire')) {
                $sources+=$support
                if($test -eq 'api') {$sources+='api\v2\session.c'}
            } elseif($test -eq 'drain_boundary') {$sources+=@('common\ethernet.c','common\coalesce.c','common\packet.c')}
            $source=Join-Path $objects "$test.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
            $exe=Join-Path $objects "$test.exe"
            $link=if($test -in @('api','completion_retire')){$libs}else{@()}
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$source)+$sources+@('/link',"/out:$exe")+$link)
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case=$test;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        Invoke-BuildTool 'cl.exe' ($flags+@('/c','/FAcs',"/Fa$objects\",$core))
        $listing=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($core)+'.cod');$body=[IO.File]::ReadAllText($listing)
        $functions=foreach($function in @('PbQueuePop','PbQueuePopBudget')) {
            $emitted=[regex]::Match($body,'(?ms)^'+$function+' PROC.*?^'+$function+' ENDP').Value
            if(-not $emitted){throw "Cannot locate native $function/$variant/$mode"}
            # Strip source/assembly comments; emitted instructions, not annotated C.
            $instructions=[regex]::Replace($emitted,'(?m);[^\r\n]*','')
            $clamps=[regex]::Matches($instructions,'\b65536\b').Count
            $expected=($variant -eq 'before' -or $function -eq 'PbQueuePopBudget')
            if(($clamps -gt 0) -ne $expected){throw "Unexpected native length accounting: $function/$variant/$mode/$clamps"}
            @{function=$function;block_size_instruction_operands=$clamps}
        }
        $native+=@{variant=$variant;volatile=$mode;path=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant();functions=@($functions)}
        if($mode -eq 'iso') {
            $exe=Join-Path $objects 'benchmark.exe'
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1','api\v2\session.c',$core)+$support+@('tests\bench\dll_completion_bench.c','/link',"/out:$exe")+$libs)
            $benchmarks[$variant]=$exe
        }
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $lines=@(Invoke-TestTool $benchmarks[$variant] @())
        foreach($line in $lines) {if($line.StartsWith('{')) {$sample=$line|ConvertFrom-Json;$sample|Add-Member variant $variant;$sample|Add-Member round $round;$samples+=$sample}}
    }
}
$summaries=foreach($group in ($samples|Group-Object variant,reverse,batch)) {
    if($group.Count -ne 7){throw "Incomplete queue-pop benchmark group: $($group.Name)"}
    $ns=@($group.Group.ns_per_batch|Sort-Object);$cycles=@($group.Group.cycles_per_batch|Sort-Object);$sample=$group.Group[0]
    [pscustomobject]@{variant=$sample.variant;reverse=$sample.reverse;batch=$sample.batch;samples=$group.Count;median_ns_per_batch=$ns[3];median_cycles_per_batch=$cycles[3]}
}
if(-not $samples.Count -or (Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'No samples, or sources changed during queue-pop verification'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_core_sha256=(Get-FileHash -LiteralPath (Join-Path $baseline 'common\v2_core.c')).Hash.ToLowerInvariant();
    current_core_sha256=(Get-FileHash 'common\v2_core.c').Hash.ToLowerInvariant();
    method='Frozen/current actual core, same current DLL and hosted kernel functions. Guarded read-only sources and exact-sized output spans use an independent prefix oracle, malformed lengths, partial/empty/corrupt/wrapped reads and immediate reuse at Consumer publication. ISO/MS runs include real arm/recheck concurrency, completion flush, pause/drain return/credit wake boundaries and actual DLL API/retirement tests. Native ordinary-pop block-size accounting disappears; budgeted-pop retains it. Actual reserve/commit/budgeted-hosted-peer/completion-poll cycles keep real Windows events; six batch sizes, sequential/reverse, seven alternating fixed-affinity runs. Only actual DLL completion uses ordinary pop in the timing loop. Not kernel/NDIS CPU or throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;native=$native;samples=$samples;summaries=@($summaries)} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries|Where-Object batch -in @(1,8,32)|Sort-Object reverse,batch,variant|Format-Table
Write-Output (Join-Path $directory 'evidence.json')
