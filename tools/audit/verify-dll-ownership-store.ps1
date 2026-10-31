param([string]$BaselineDirectory='out\dll-ownership-store-baseline\41f98ae3db4e')
# Freeze only DLL ownership bodies; use the same current core and host peer.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('api\v2\session.c','9bdafa88a6bfd4c808403c6fa827d8d329f26b5ff87a26018259382fbf298b2d'),
    @('api\v2\session_queue.h','51cde578d2cbec90ff0e21be0f1f6f77caf90b5f54b27c89662e2a166efb1864'),
    @('api\v2\session_lifecycle.h','c18cffbc0549ca3da0b7e986463fef3aadf9dbc2859ad52795668881b7ff2f44'),
    @('api\v2\session_state.h','4fe41f2873402efbfdf1ba5dfc77a8ca8ba9b7e69c1c9bd4804c2582389bc95f'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected ownership baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\dll-ownership-store\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$oldSession=Join-Path $directory 'session-before.c';$body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c'))
foreach($header in @('session_state.h','session_lifecycle.h','session_queue.h')) {
    $replacement='#include "'+(Join-Path $baseline "api\v2\$header").Replace('\','/')+'"'
    $body=$body.Replace('#include "'+$header+'"',$replacement)
}
[IO.File]::WriteAllText($oldSession,$body,[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$support=@('api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
$executables=@();$native=@();$benchmarks=@{};$samples=@()
foreach($variant in @('before','after')) {
    $session=if($variant -eq 'before'){$oldSession}else{Join-Path $script:RepoRoot 'api\v2\session.c'}
    foreach($mode in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\api\v2","/Fo$objects\")+$script:VersionArguments
        foreach($guarded in @($false,$true)) {
            $memory=if($guarded){'guarded'}else{'ordinary'}
            $lines=@();if($variant -eq 'before') {$lines+='#define PB_OWNERSHIP_RMW_BASELINE 1'}
            if($guarded) {$lines+='#define PB_GUARD_TESTS 1'}
            $lines+='#define PB_OWNERSHIP_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
            $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\ownership_counter_tests.c').Replace('\','/')+'"'
            $source=Join-Path $objects "$memory.c";$lines|Set-Content -LiteralPath $source -Encoding utf8
            $exe=Join-Path $objects "$memory.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@($source)+$support+@('/link',"/out:$exe")+$libs);Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case='ownership';volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        foreach($case in @('api','lifecycle')) {
            $exe=Join-Path $objects "$case.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session)+$support+@("tests\model\${case}_tests.c",'/link',"/out:$exe")+$libs)
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;case=$case;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
        Invoke-BuildTool 'cl.exe' ($flags+@('/c','/FAcs',"/Fa$objects\",'/DPB_API_TESTS=1',$session))
        $listing=Join-Path $objects ([IO.Path]::GetFileNameWithoutExtension($session)+'.cod');$body=[IO.File]::ReadAllText($listing)
        $functions=foreach($entry in @(@('ReceiveBatch',1),@('ReleaseBatch',1),@('ReserveBatch',1),@('CancelBatch',1),@('CommitBatch',2),@('PollInternal',1))) {
            $name=$entry[0];$function=[regex]::Match($body,'(?ms)^'+$name+' PROC.*?^'+$name+' ENDP').Value
            if(-not $function){throw "Cannot locate native function: $name/$variant/$mode"}
            $instructions=[regex]::Replace($function,'(?m);[^\r\n]*','')
            $calls=[regex]::Matches($instructions,'\block\s+(?:add|xadd)\b').Count
            $expected=if($variant -eq 'before'){$entry[1]}else{0}
            if($calls -ne $expected){throw "Unexpected queue counter RMW instructions: $name/$variant/$mode/$calls"}
            @{function=$name;counter_rmw_instruction_sites=$calls}
        }
        $native+=@{variant=$variant;volatile=$mode;path=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant();functions=@($functions)}
        if($mode -eq 'iso') {foreach($case in @('completion','return')) {
            $exe=Join-Path $objects "$case-benchmark.exe"
            Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session)+$support+@("tests\bench\dll_${case}_bench.c",'/link',"/out:$exe")+$libs)
            $benchmarks["$variant-$case"]=$exe
        }}
    }
}
foreach($round in 0..6) {
    $order=if($round%2){@('after','before')}else{@('before','after')}
    foreach($variant in $order) {foreach($case in @('completion','return')) {
        $lines=@(Invoke-TestTool $benchmarks["$variant-$case"] @())
        foreach($line in $lines) {if($line.StartsWith('{')) {
            $sample=$line|ConvertFrom-Json
            if($sample.case -notin @('completion-cycle','dll-cycle')){continue}
            if($case -eq 'return'){$sample|Add-Member reverse 0}
            $sample|Add-Member variant $variant;$sample|Add-Member round $round;$samples+=$sample
        }}
    }}
}
$summaries=foreach($group in ($samples|Group-Object variant,case,reverse,batch)) {
    if($group.Count -ne 7){throw "Incomplete ownership-store sample group: $($group.Name)"}
    $ns=@($group.Group.ns_per_batch|Sort-Object);$cycles=@($group.Group.cycles_per_batch|Sort-Object);$sample=$group.Group[0]
    [pscustomobject]@{variant=$sample.variant;case=$sample.case;reverse=$sample.reverse;batch=$sample.batch;samples=$group.Count;median_ns_per_batch=$ns[3];median_cycles_per_batch=$cycles[3]}
}
if(-not $samples.Count -or (Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'No samples, or sources changed during ownership-store verification'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_queue_sha256=(Get-FileHash -LiteralPath (Join-Path $baseline 'api\v2\session_queue.h')).Hash.ToLowerInvariant();
    current_queue_sha256=(Get-FileHash 'api\v2\session_queue.h').Hash.ToLowerInvariant();
    method='Frozen/current actual DLL state/queue/lifecycle bodies, same current core. Observe real Windows counter RMWs/acquire reads/release stores without replacing synchronization. ISO/MS, ordinary/guarded, four real owners and a progress reader complete 40000 mixed ownership cycles per fixture. Stop is injected after commit Loans=0 but before Published changes: DataGate, unique owner and native barriers must still hold. Damaged completion requires native STOPPED before lifecycle-exclusive counter reconciliation. Actual API/lifecycle regressions, native six-function RMW instruction-site comparison, and real-event DLL completion and TX-return benchmarks use seven alternating fixed-affinity runs. Not kernel/NDIS CPU or throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;native=$native;samples=$samples;summaries=@($summaries)}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries|Where-Object batch -in @(1,8,32)|Sort-Object case,reverse,batch,variant|Format-Table
Write-Output (Join-Path $directory 'evidence.json')
