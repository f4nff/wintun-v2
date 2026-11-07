param([string]$BaselineDirectory='out\slot-reclamation-baseline\ba49741c551c')
# Frozen publication-reuse reproduction, production facade prefix tests and
# alternating hosted CPU measurements. No installation or kernel execution.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Reclamation audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('api\compat_worker.h','312987fb744fd18aaf0960e4eff2ca44b4b1d0e1e6ca17034cd535abae454f46'),
    @('api\session.c','6e9165e007401cf9a7f2b0e3a1dc4aab3f4abe9202a17022e4c1fdaed75e030f'),
    @('api\v2\session_queue.h','67f8c0d0dd7c99e16cbe62e4026138742fc113b2051b03d7c5141c46420ff70e'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]){throw "Changed baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\reclamation-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$native=@('api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
$benchmarks=@{};$executables=@();$samples=@()
foreach($variant in @('before','after')) {
    foreach($mode in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX','/wd4200',"/I$script:RepoRoot\api\v2","/Fo$objects\")+$script:VersionArguments
        $lines=@()
        if($variant -eq 'before'){$lines+='#define PB_RETURN_ALIAS_QUEUE_SOURCE "'+(Join-Path $baseline 'api\v2\session_queue.h').Replace('\','/')+'"'}
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\return_alias_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects 'alias.c';[IO.File]::WriteAllLines($source,$lines)
        $exe=Join-Path $objects 'alias.exe'
        Invoke-BuildTool 'cl.exe' ($flags+@($source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$exe"))
        $arguments=if($variant -eq 'before'){@('--expect-baseline')}else{@()}
        Invoke-TestTool $exe $arguments
        $executables+=@{variant=$variant;case='alias';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        $lines=@()
        if($variant -eq 'before') {
            $oldFacade=Join-Path $objects 'facade-before.c'
            $content=[IO.File]::ReadAllText((Join-Path $baseline 'api\session.c'))
            foreach($header in @('adapter.h','logger.h','main.h','wintun.h','v2/compat.h','v2/lifecycle.h','compat_worker.h')) {
                $root=if($header -eq 'compat_worker.h'){$baseline}else{$script:RepoRoot}
                $include='#include "'+(Join-Path $root "api/$header").Replace('\','/')+'"'
                $content=$content.Replace('#include "'+$header+'"',$include)
            }
            [IO.File]::WriteAllText($oldFacade,$content)
            $lines+='#define PB_COMPAT_SESSION_SOURCE "'+$oldFacade.Replace('\','/')+'"'
        }
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\compat_session_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects 'compat.c';[IO.File]::WriteAllLines($source,$lines)
        $exe=Join-Path $objects 'compat.exe'
        # The native v2 implementation is identical for both facade variants.
        Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$source)+$native+@('/link',"/out:$exe")+$libs)
        Invoke-TestTool $exe $arguments
        $executables+=@{variant=$variant;case='facade';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        if($mode -eq 'iso') {
            $benchmarks["$variant-facade"]=$exe
            $session='api\v2\session.c'
            if($variant -eq 'before') {
                $session=Join-Path $objects 'session-before.c'
                $include='#include "'+(Join-Path $baseline 'api\v2\session_queue.h').Replace('\','/')+'"'
                $content=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c')).Replace('#include "session_queue.h"',$include)
                [IO.File]::WriteAllText($session,$content)
            }
            $exe=Join-Path $objects 'return-benchmark.exe'
            $rest=@($native|Where-Object {$_ -ne 'api\v2\session.c'})
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session,'tests\bench\dll_return_bench.c')+$rest+@('/link',"/out:$exe")+$libs)
            $benchmarks["$variant-return"]=$exe
        }
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) { foreach($kind in @('facade','return')) {
        $arguments=if($kind -eq 'facade'){@('--bench')}else{@()}
        $lines=@(Invoke-TestTool $benchmarks["$variant-$kind"] $arguments)
        foreach($line in $lines) {
            if($line.StartsWith('{')) {
                $sample=$line|ConvertFrom-Json;$sample|Add-Member variant $variant;$sample|Add-Member round $round;$samples+=$sample
            }
        }
    } }
}
$summaries=foreach($group in ($samples|Group-Object variant,case,batch)) {
    $first=$group.Group[0];$field=if($first.case -eq 'compat-duplex'){'worker_cycles_per_packet'}else{'cycles_per_batch'}
    $sorted=@($group.Group.$field|Sort-Object)
    [ordered]@{variant=$first.variant;case=$first.case;batch=$first.batch;samples=$sorted.Count;metric=$field;median=$sorted[[int][Math]::Floor($sorted.Count/2)]}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    kernel_runtime_tested=$false;method='Frozen/current real production queue and facade, deterministic ownership reuse and prefix tests in ISO/MS volatile modes. Seven alternating fixed-affinity hosted rounds; facade uses the same current native implementation on both sides, raw API table and no call/event counters. CPU time/cycles belong to the compatibility owner thread, packet denominator includes both directions. Return benchmark isolates the token snapshot. No kernel throughput or old-OS runtime claim.';
    operations=$script:TestResultFile;executables=$executables;samples=$samples;summaries=@($summaries)}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries|ForEach-Object {[pscustomobject]$_}|Format-Table
Write-Output (Join-Path $directory 'evidence.json')
