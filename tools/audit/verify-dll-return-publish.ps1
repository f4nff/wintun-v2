param([string]$BaselineDirectory='out\dll-return-publish-baseline\96ef1d64980b')
# Frozen/current production core and ReleaseBatch, exact-wire regression and
# alternating hosted costs. No driver install/load or kernel CPU measurement.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('common\v2_core.c','e3004ae685e406e81152cb093f0fa35e7673459530b8fdc08698842ec81fd951'),
    @('common\v2_core.h','16c787103bdb9698787c4e59f0348d5e34d5d7d5ee96830277ed7b6bc9dc5fcd'),
    @('api\v2\session_queue.h','1f0081178e59db3833ecc99b00eb325e0be83448f5e86fc8f1c4a0597c355dca'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected return-publish baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\dll-return-publish\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldCore=Join-Path $directory 'core-before.c'
$header='#include "'+(Join-Path $script:RepoRoot 'common\v2_core.h').Replace('\','/')+'"'
$contents=[IO.File]::ReadAllText((Join-Path $baseline 'common\v2_core.c')).Replace('#include "v2_core.h"',$header)
[IO.File]::WriteAllText($oldCore,$contents,[Text.UTF8Encoding]::new($false))
$oldSession=Join-Path $directory 'session-before.c'
$oldQueue='#include "'+(Join-Path $baseline 'api\v2\session_queue.h').Replace('\','/')+'"'
$contents=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c')).Replace('#include "session_queue.h"',$oldQueue)
[IO.File]::WriteAllText($oldSession,$contents,[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@();$benchmarks=@{};$samples=@();$native=@()
$apiSources=@('api\v2\adapter.c','api\v2\driver_package.c','common\packet.c','common\flow.c')
$apiLibs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')) {
    $core=if($variant -eq 'before'){$oldCore}else{'common\v2_core.c'}
    $session=if($variant -eq 'before'){$oldSession}else{'api\v2\session.c'}
    foreach($mode in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\api\v2","/Fo$objects\")+$script:VersionArguments
        $lines=@()
        if($variant -eq 'before') {$lines+=@('#define PB_TOKEN_PUBLISH_BASELINE 1',('#define PB_TOKEN_PUBLISH_CORE "'+$oldCore.Replace('\','/')+'"'))}
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\token_publish_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects 'queue.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects 'queue.exe'
        Invoke-BuildTool 'cl.exe' ($flags+@($source,'/link',"/out:$exe"));Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case='queue';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        foreach($case in @('api','lifecycle')) {
            $exe=Join-Path $objects "$case.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session,$core)+$apiSources+@("tests\model\${case}_tests.c",'/link',"/out:$exe")+$apiLibs)
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case=$case;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        if($mode -eq 'iso') {
            $defines=if($variant -eq 'before'){@('/DPB_TOKEN_PUBLISH_BASELINE=1')}else{@()}
            $exe=Join-Path $objects 'benchmark.exe'
            Invoke-BuildTool 'cl.exe' ($flags+$defines+@('/DPB_API_TESTS=1',$session,$core)+$apiSources+@('tests\bench\dll_return_bench.c','/link',"/out:$exe")+$apiLibs)
            $benchmarks[$variant]=$exe
            # Preserve native assembly for the actual queue and DLL bodies.
            Invoke-BuildTool 'cl.exe' ($flags+@('/c','/FAcs',"/Fa$objects\",'/DPB_API_TESTS=1',$session,$core))
            foreach($inputSource in @($session,$core)) {
                $listing=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($inputSource)+'.cod')
                $body=[regex]::Match([IO.File]::ReadAllText($listing),'(?ms)^ReleaseBatch PROC.*?^ReleaseBatch ENDP').Value
                $reservation=[regex]::Match($body,'sub\s+rsp,\s+(\d+)')
                $native+=@{variant=$variant;source=$inputSource;listing=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant();
                    release_batch_rsp_reservation=if($reservation.Success){[int]$reservation.Groups[1].Value}else{$null}}
            }
        }
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $lines=@(Invoke-TestTool $benchmarks[$variant] @())
        foreach($line in $lines) {
            if($line.StartsWith('{')) {$sample=$line | ConvertFrom-Json;$sample | Add-Member variant $variant;$sample | Add-Member round $round;$samples+=$sample}
        }
    }
}
$summaries=foreach($group in ($samples | Group-Object variant,case,batch)) {
    $ordered=@($group.Group | Sort-Object ns_per_batch);$middle=$ordered[[int][Math]::Floor($ordered.Count/2)]
    [pscustomobject][ordered]@{variant=$middle.variant;case=$middle.case;batch=$middle.batch;samples=$ordered.Count;median_ns_per_batch=$middle.ns_per_batch;
        median_cycles_per_batch=@($ordered.cycles_per_batch | Sort-Object)[[int][Math]::Floor($ordered.Count/2)]}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during return-publish verification'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    method='Frozen/current actual shared core and ReleaseBatch; exact-wire differential matrix, guarded boundaries, publication/reuse schedules, native concurrent event waits in MS/ISO volatile modes; actual API/lifecycle regressions. Seven alternating fixed-affinity hosted benchmark rounds, raw samples and native emitted code retained. Ring still writes full zero-metadata 64-byte descriptors. No kernel CPU, latency or throughput claim.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;native=$native;samples=$samples;summaries=@($summaries)} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries | Format-Table
Write-Output (Join-Path $directory 'evidence.json')
