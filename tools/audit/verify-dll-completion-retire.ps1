param([string]$BaselineDirectory='out\dll-completion-retire-baseline\40884c0cb155')
# Frozen/current actual DLL queue bodies, same current core/lifecycle. No SYS load.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('api\v2\session_queue.h','de2da8de2fcdac9f6c613a3d8a49391100fb66aa06415e31a21e1c7ed260c3cf'),
    @('api\v2\session.c','9bdafa88a6bfd4c808403c6fa827d8d329f26b5ff87a26018259382fbf298b2d'),
    @('common\v2_core.c','c0ed6bf51fd890b27667254839ac6886f365acf9a198ce406a0a0f8748995914'),
    @('common\v2_core.h','2994f94b4b76f0e08b92474badc8f1d05b5214eb400fb2723c035093bea062a1'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected completion-retirement baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\dll-completion-retire\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldSession=Join-Path $directory 'session-before.c'
$oldQueue='#include "'+(Join-Path $baseline 'api\v2\session_queue.h').Replace('\','/')+'"'
$body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c')).Replace('#include "session_queue.h"',$oldQueue)
[IO.File]::WriteAllText($oldSession,$body,[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$executables=@();$benchmarks=@{};$samples=@();$native=@()
$support=@('api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')) {
    $session=if($variant -eq 'before'){$oldSession}else{Join-Path $script:RepoRoot 'api\v2\session.c'}
    foreach($mode in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\api\v2","/Fo$objects\")+$script:VersionArguments
        foreach($guarded in @($false,$true)) {
            $memory=if($guarded){'guarded'}else{'ordinary'}
            $lines=@();if($variant -eq 'before'){$lines+='#define PB_COMPLETION_RETIRE_BASELINE 1'}
            if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
            $lines+='#define PB_COMPLETION_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
            $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\completion_retire_tests.c').Replace('\','/')+'"'
            $source=Join-Path $objects "$memory.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
            $exe=Join-Path $objects "$memory.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@($source)+$support+@('/link',"/out:$exe")+$libs);Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case='retirement';volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        foreach($case in @('api','lifecycle')) {
            $exe=Join-Path $objects "$case.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session)+$support+@("tests\model\${case}_tests.c",'/link',"/out:$exe")+$libs)
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case=$case;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        if($mode -eq 'iso') {
            $exe=Join-Path $objects 'benchmark.exe'
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session)+$support+@('tests\bench\dll_completion_bench.c','/link',"/out:$exe")+$libs)
            $benchmarks[$variant]=$exe
            Invoke-BuildTool 'cl.exe' ($flags+@('/c','/FAcs',"/Fa$objects\",'/DPB_API_TESTS=1',$session))
            $listing=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($session)+'.cod')
            $function=[regex]::Match([IO.File]::ReadAllText($listing),'(?ms)^PollInternal PROC.*?^PollInternal ENDP').Value
            if(-not $function){throw "Cannot locate emitted PollInternal: $variant"}
            $calls=([regex]::Matches($function,'(?m)\bcall\s+PbPoolTransition\b')).Count
            $expected=if($variant -eq 'before'){2}else{1};if($calls -ne $expected){throw "Unexpected emitted transition calls: $variant/$calls"}
            $native+=@{variant=$variant;path=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant();transition_call_sites_in_poll=$calls}
        }
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {
        $lines=@(Invoke-TestTool $benchmarks[$variant] @())
        foreach($line in $lines) {if($line.StartsWith('{')) {$sample=$line | ConvertFrom-Json;$sample | Add-Member variant $variant;$sample | Add-Member round $round;$samples+=$sample}}
    }
}
$summaries=foreach($group in ($samples | Group-Object variant,reverse,batch)) {
    if($group.Count -ne 7){throw "Incomplete completion benchmark group: $($group.Name)"}
    $ns=@($group.Group.ns_per_batch | Sort-Object);$cycles=@($group.Group.cycles_per_batch | Sort-Object);$sample=$group.Group[0]
    [pscustomobject]@{variant=$sample.variant;reverse=$sample.reverse;batch=$sample.batch;samples=$group.Count;median_ns_per_batch=$ns[3];median_cycles_per_batch=$cycles[3]}
}
if(-not $samples.Count -or (Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'No samples, or sources changed during completion checks'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_queue_sha256=(Get-FileHash -LiteralPath (Join-Path $baseline 'api\v2\session_queue.h')).Hash.ToLowerInvariant();
    current_queue_sha256=(Get-FileHash 'api\v2\session_queue.h').Hash.ToLowerInvariant();
    method='Frozen/current actual DLL queue, same current core/lifecycle; transitions and real kernel-event calls observed without replacing behavior. Ordinary/guarded and ISO/MS tests check byte-exact completions, partial/out-of-order/wrapped consumption, validation failure rollback, user/native barriers, wrong owner, exhausted generations and PrepareClose during partial retirement. Invalid-token batches keep private ownership unchanged: frozen before has zero space notification, current has one because its successful Pop already released producer space (separate close-wake cross-layer regression confirms the old stall). Session-owned device/events, work/pool objects, admission and guarded allocations checked; public-close test shell pin joins callbacks for deterministic harness teardown. Uninstrumented actual DLL reserve/commit/hosted-peer/completion-poll cycles retain real Windows event calls, six batch sizes, sequential/reverse, seven alternating fixed-affinity runs. Not kernel/NDIS CPU or throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;native=$native;samples=$samples;summaries=@($summaries)} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries | Where-Object batch -in @(1,8,32) | Sort-Object reverse,batch,variant | Format-Table
Write-Output (Join-Path $directory 'evidence.json')
