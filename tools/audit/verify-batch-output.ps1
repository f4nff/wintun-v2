param([string]$BaselineDirectory='out\batch-output-baseline\1b2900379a56')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Batch-output audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$oldQueue=Join-Path $baseline 'api\v2\session_queue.h'
if((Get-FileHash -LiteralPath $oldQueue).Hash.ToLowerInvariant() -ne '4f9387b9b7f19e08fd3dc4f92a044a14f3391d67d302efa0c1baed7741aa08d4'){throw 'Unexpected frozen queue source'}
$directory=Join-Path $script:RepoRoot "out\batch-output-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json';$executables=@();$benchmarks=@{};$samples=@();$native=@()
function Normalize-Session([string]$content) {
    [regex]::Replace($content,'#include "([^"]+)"',{
        param($m)
        $path=[IO.Path]::GetFullPath((Join-Path (Join-Path $script:RepoRoot 'api\v2') $m.Groups[1].Value))
        if(!(Test-Path -LiteralPath $path)){throw "Unknown include: $path"}
        '#include "'+$path.Replace('\','/')+'"'
    })
}
function Expect-Control([string]$Program,[string]$Argument,[int]$Code,[string]$Marker,[string]$Log) {
    $output=@(& $Program $Argument 2>&1|ForEach-Object {$_.ToString()});$actual=$LASTEXITCODE
    $output|Set-Content -LiteralPath $Log -Encoding utf8
    $expected=$actual -eq $Code -and ($output -join "`n").Contains($Marker)
    $script:TestExecutions.Add([ordered]@{program=$Program;arguments=@($Argument);exit_code=$actual;expected_exit_code=$Code;expected_baseline_failure=$expected;log=$Log})
    Write-TestRecording $(if($expected){'running'}else{'failed'})
    if(!$expected){throw "Unexpected baseline result: $Marker, exit=$actual"}
}
$apiSources=@('api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libraries=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $source='tests\model\batch_output_tests.c';$session='api\v2\session.c'
        if($variant -eq 'before') {
            $session=Join-Path $objects 'session-before.c'
            $body=Normalize-Session ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c')))
            $currentInclude='#include "'+(Join-Path $script:RepoRoot 'api\v2\session_queue.h').Replace('\','/')+'"'
            $oldInclude='#include "'+$oldQueue.Replace('\','/')+'"'
            if(!$body.Contains($currentInclude)){throw 'Cannot isolate batch queue implementation'}
            [IO.File]::WriteAllText($session,$body.Replace($currentInclude,$oldInclude))
            $source=Join-Path $objects 'batch-before.c'
            @('#define PB_BATCH_OUTPUT_BASELINE 1',('#define PB_OWNERSHIP_SESSION_SOURCE "'+$session.Replace('\','/')+'"'),
              ('#include "'+(Join-Path $script:RepoRoot 'tests\model\batch_output_tests.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
        }
        foreach($guard in @('ordinary','guarded')) {
            $exe=Join-Path $objects "batch-$guard.exe";$caseFlags=@($flags)
            if($guard -eq 'guarded'){$caseFlags+='/DPB_GUARD_TESTS=1'}
            Invoke-BuildTool cl.exe ($caseFlags+@($source)+$apiSources+@('/link',"/out:$exe")+$libraries)
            if($variant -eq 'before') {
                Expect-Control $exe '--baseline-zero' 51 'REUSED_OUTPUT_COUNT_LOST_RETIREMENT' (Join-Path $objects "$guard-zero.log")
                Expect-Control $exe '--baseline-overflow' 52 'PUBLIC_COUNT_ESCAPED_BATCH_BOUND' (Join-Path $objects "$guard-overflow.log")
            } else {Invoke-TestTool $exe @()}
            $executables+=@{variant=$variant;volatile=$mode;memory=$guard;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        if($mode -eq 'iso') {
            $exe=Join-Path $objects 'benchmark.exe'
            Invoke-BuildTool cl.exe ($flags+@('/DPB_API_TESTS=1',$session)+$apiSources+@('tests\bench\dll_return_bench.c','/link',"/out:$exe")+$libraries)
            $benchmarks[$variant]=$exe
            Invoke-BuildTool cl.exe ($flags+@('/c','/FAcs',"/Fa$objects\",'/DPB_API_TESTS=1',$session))
            $listing=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($session)+'.cod')
            $native+=@{variant=$variant;listing=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant()}
        }
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        foreach($line in @(Invoke-TestTool $benchmarks[$variant] @())) {
            if($line.StartsWith('{')){$sample=$line|ConvertFrom-Json;$sample|Add-Member variant $variant;$sample|Add-Member round $round;$samples+=$sample}
        }
    }
}
$summaries=foreach($group in ($samples|Group-Object variant,case,batch)) {
    $g=@($group.Group);$middle=[int][Math]::Floor($g.Count/2)
    [ordered]@{variant=$g[0].variant;case=$g[0].case;batch=$g[0].batch;samples=$g.Count;
        median_ns_per_batch=@($g.ns_per_batch|Sort-Object)[$middle];median_cycles_per_batch=@($g.cycles_per_batch|Sort-Object)[$middle]}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during batch-output audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;kernel_runtime_tested=$false;production_ready=$false;
    method='Actual DLL session/pools/SRW gates/lifecycle, legal model peer recycle/reserve/overwrite/republish after the real Producer store, frozen negative controls 51/52 (stop before uninitialised token read). Reject count outputs in transferred TX/RX blocks before publication, retain held-loan outputs and input-token aliasing, generation import, partial-prefix pressure injection, local stopped/fault retirement, overlap geometry and inaccessible-token fast-path witness. Concurrent final return with stop/native references on both sides of the real data gate, joined owner threads. Every session returns all tracked allocations/events/threads/work/pools; full matrix warmup then two measured no-growth process handle passes. ISO/MS volatile and ordinary/guarded memory. Seven alternating fixed-affinity hosted CPU rounds and native emitted code; no kernel CPU/performance claim.';
    operations=$script:TestResultFile;executables=$executables;native=$native;samples=$samples;summaries=@($summaries)}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
