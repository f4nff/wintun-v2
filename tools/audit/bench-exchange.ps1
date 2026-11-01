# SPDX-License-Identifier: GPL-2.0
param(
    [ValidateSet('Smoke','Matrix','Confirm')][string]$Stage='Matrix',
    [ValidateRange(1,21)][int]$Rounds=3,
    [ValidateRange(16384,100000000)][int]$Objects=262144,
    [ValidateSet(64,128,256)][int]$RegionMiB=64,
    [ValidateSet('Filter','Bitmap')][string]$TokenValidation='Filter',
    [string]$Candidates,
    [string]$OutputDirectory
)
# Test-only /FI overrides compile wider batches without editing the production ABI.
# No driver is installed or loaded. Timing covers hosted descriptor exchange.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
if(-not $OutputDirectory){$OutputDirectory=Join-Path $script:RepoRoot "out\exchange\$($script:SourceFingerprint.Substring(0,12))\region-$RegionMiB-$($TokenValidation.ToLowerInvariant())"}
$directory=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory "$($Stage.ToLowerInvariant())-tests.json"
$programs=@{};$artifacts=@()
$affinity=[Diagnostics.Process]::GetCurrentProcess().ProcessorAffinity.ToInt64()
$availableCpus=@([Convert]::ToString($affinity,2).ToCharArray()|Where-Object{$_ -eq '1'}).Count
$maxQueues=[Math]::Min(8,[Math]::Floor($availableCpus/2))
if($maxQueues -lt 1){throw 'Two available logical processors are required for a pinned user/peer pair.'}
$core=Join-Path $script:RepoRoot 'common\v2_core.c'
if($TokenValidation -eq 'Bitmap') {
    # An isolated O(n) duplicate detector, retaining every existing ownership and
    # error-order check. Production source is never edited by this experiment.
    $body=[IO.File]::ReadAllText($core)
    if(([regex]::Matches($body,'uint64_t seen = 0;')).Count -ne 2){throw 'Unexpected validator source.'}
    $body=$body.Replace('uint64_t seen = 0;','uint64_t seen[(PB_MAX_SLOTS + 63u) / 64u] = { 0 };')
    foreach($slot in @('t->Slot','tokens[i].Slot')) {
        $pattern='if \(ShouldScanEarlierSlots\(&seen, '+[regex]::Escape($slot)+', count\)\)\s*for \(uint32_t j = 0; j < i; \+\+j\) if \([^\r\n]+\) return PB_INVALID;'
        if(([regex]::Matches($body,$pattern)).Count -ne 1){throw "Unexpected duplicate-check body for $slot"}
        $replacement="uint64_t bit = (uint64_t)1 << ($slot & 63u);`n        if (seen[$slot >> 6] & bit) return PB_INVALID;`n        seen[$slot >> 6] |= bit;"
        $body=[regex]::Replace($body,$pattern,$replacement)
    }
    $core=Join-Path $directory 'test-only-bitmap-core.c'
    [IO.File]::WriteAllText($core,$body,[Text.UTF8Encoding]::new($false))
}
foreach($width in @(32,64,128,256)) {
    $objectsDir=Join-Path $directory "batch-$width"
    New-Item -ItemType Directory -Path $objectsDir -Force|Out-Null
    $header=Join-Path $objectsDir 'test-only-width.h'
    $include='#include "'+(Join-Path $script:RepoRoot 'include\packetbridge_v2_abi.h').Replace('\','/')+'"'
    @($include,'#undef PB_MAX_BATCH',"#define PB_MAX_BATCH ${width}u",'#undef PB_MAX_REGION',
        ('#define PB_MAX_REGION '+($RegionMiB*1048576)+'u')) | Set-Content -LiteralPath $header -Encoding ascii
    $flags=@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX','/wd4505',"/I$script:RepoRoot\common","/FI$header","/Fo$objectsDir\")
    foreach($kind in @('tests','bench')) {
        $source=if($kind -eq 'tests'){'tests\model\exchange_tests.c'}else{'tests\bench\exchange_bench.c'}
        $exe=Join-Path $objectsDir "$kind.exe"
        Invoke-BuildTool 'cl.exe' ($flags+@($core,$source,'/link',"/out:$exe"))
        if($kind -eq 'tests'){Invoke-TestTool $exe @()}
        else{$programs[$width]=$exe}
        $artifacts+=@{width=$width;kind=$kind;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
    if($width -eq 32 -and $TokenValidation -eq 'Bitmap') {
        $wrapper=Join-Path $objectsDir 'validation-model.c'
        @('#define PB_VALIDATION_CORE_SOURCE "'+$core.Replace('\','/')+'"')|Set-Content -LiteralPath $wrapper -Encoding ascii
        Add-Content -LiteralPath $wrapper -Value ('#include "'+(Join-Path $script:RepoRoot 'tests\model\token_validation_tests.c').Replace('\','/')+'"') -Encoding ascii
        $exe=Join-Path $objectsDir 'validation-model.exe'
        Invoke-BuildTool 'cl.exe' ($flags+@($wrapper,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
    }
}
if($Stage -eq 'Confirm') {
    if(-not $Candidates){throw 'Confirm requires a JSON array of candidate configurations.'}
    $cases=@(Get-Content -LiteralPath $Candidates -Raw|ConvertFrom-Json)
} elseif($Stage -eq 'Smoke') {
    $cases=@(
        @{direction='rx';max_batch=32;batch=32;queues=1;slots=32;ring=64;byte_budget=131072;object_bytes=1500},
        @{direction='tx';max_batch=256;batch=256;queues=1;slots=256;ring=512;byte_budget=524288;object_bytes=1500}
    )
} else {
    $cases=@()
    foreach($width in @(32,64,128,256)) {foreach($queues in (@(1,2,4,8)|Where-Object{$_ -le $maxQueues})) {foreach($depth in @(1,2,4)) {
        $slots=$width*$depth
        if($slots -gt 512 -or [int64]$queues*$slots*131072 -ge [int64]$RegionMiB*1048576){continue}
        foreach($ringFactor in @(1,2)) {
            $ring=$slots*$ringFactor
            foreach($budget in @(131072,524288)) {foreach($length in @(64,1500,32768)) {foreach($direction in @('rx','tx')) {
                $cases+=@{direction=$direction;max_batch=$width;batch=$width;queues=$queues;slots=$slots;ring=$ring;byte_budget=$budget;object_bytes=$length}
            }}}
        }
    }}}
    # Current supported small-batch alternatives at the existing default depth.
    foreach($batch in @(8,16)) {foreach($direction in @('rx','tx')) {foreach($length in @(64,1500,32768)) {
        $cases+=@{direction=$direction;max_batch=32;batch=$batch;queues=1;slots=32;ring=64;byte_budget=131072;object_bytes=$length}
    }}}
    # Include non-power-of-two slot maxima; power-of-two depths alone miss
    # useful in-flight capacity just below the shared-region limit.
    $edges=switch($RegionMiB) {
        64 { @(@(1,510,512),@(2,255,256),@(4,127,128),@(8,63,64)) }
        128 { @(@(1,512,512),@(2,510,512),@(4,255,256),@(8,127,128)) }
        256 { @(@(1,512,512),@(2,512,512),@(4,510,512),@(8,255,256)) }
    }
    foreach($edge in $edges) {foreach($width in @(32,64,128,256)) {
        if($width -gt $edge[1] -or $edge[0] -gt $maxQueues){continue}
        foreach($budget in @(131072,524288)) {foreach($length in @(64,1500,32768)) {foreach($direction in @('rx','tx')) {
            $cases+=@{direction=$direction;max_batch=$width;batch=$width;queues=$edge[0];slots=$edge[1];ring=$edge[2];byte_budget=$budget;object_bytes=$length}
        }}}
    }}
    $cases=@($cases|Group-Object direction,max_batch,batch,queues,slots,ring,byte_budget,object_bytes|ForEach-Object{$_.Group[0]})
}
if(-not $cases.Count){throw 'No benchmark configurations were selected.'}
foreach($case in $cases) {
    if($case.queues -gt $maxQueues){throw "Configuration needs $($case.queues*2) available CPUs; found $availableCpus."}
    if($case.region_limit_bytes -and $case.region_limit_bytes -ne $RegionMiB*1048576){throw 'Candidate region limit does not match -RegionMiB.'}
    if($case.token_validation -and $case.token_validation -ne $TokenValidation.ToLowerInvariant()){throw 'Candidate validator does not match -TokenValidation.'}
}
$cases|ConvertTo-Json -Depth 4|Set-Content -LiteralPath (Join-Path $directory "$($Stage.ToLowerInvariant())-cases.json") -Encoding utf8
$samples=[Collections.Generic.List[object]]::new()
$raw=Join-Path $directory "$($Stage.ToLowerInvariant())-samples.jsonl"
[IO.File]::WriteAllText($raw,'',[Text.UTF8Encoding]::new($false))
$done=0;$taskCount=$Rounds*$cases.Count
foreach($round in 0..($Rounds-1)) {
    # Alternate ascending/descending order to reduce systematic thermal/order bias.
    $ordered=@($cases);if($round%2){[Array]::Reverse($ordered)}
    foreach($case in $ordered) {
        $arguments=@($case.direction,[string]$case.queues,[string]$case.slots,[string]$case.ring,
            [string]$case.byte_budget,[string]$case.object_bytes,[string]$Objects,[string]$case.batch)
        $exe=$programs[[int]$case.max_batch]
        $lines=@(& $exe @arguments 2>&1);$code=$LASTEXITCODE
        if($code -ne 0){$lines|Write-Output;throw "Exchange benchmark failed: $($case|ConvertTo-Json -Compress), exit=$code"}
        $json=@($lines|Where-Object{$_.ToString().StartsWith('{')})
        if($json.Count -ne 1){throw 'Expected exactly one benchmark JSON result.'}
        $sample=$json[0]|ConvertFrom-Json;$sample|Add-Member round $round;$sample|Add-Member exit_code $code
        $sample|Add-Member token_validation $TokenValidation.ToLowerInvariant()
        $samples.Add($sample)
        [IO.File]::AppendAllText($raw,($sample|ConvertTo-Json -Compress)+[Environment]::NewLine,[Text.UTF8Encoding]::new($false))
        $done++;if($done%24 -eq 0 -or $done -eq $taskCount){Write-Output "Progress: $done/$taskCount exchange samples"}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during exchange measurement.'}
Write-TestRecording 'passed'
$processor=Get-CimInstance Win32_Processor|Select-Object Name,NumberOfCores,NumberOfLogicalProcessors
$os=Get-CimInstance Win32_OperatingSystem|Select-Object Caption,Version,BuildNumber
[ordered]@{stage=$Stage;source_tree_sha256=$script:SourceFingerprint;rounds=$Rounds;objects_per_sample=$Objects;region_limit_mib=$RegionMiB;token_validation=$TokenValidation;
    cases=$cases.Count;samples=$samples.Count;processor=$processor;os=$os;artifacts=$artifacts;
    method='Actual DLL session_queue.h and v2_core.c, real SRW locks/auto-reset events/pinned Windows threads, one user and one hosted-peer thread per queue. Immediate completion/recycle, exact ordered 16-byte payload markers, pre-touched buffers. No full-payload copy, NDIS callbacks, real kernel worker/driver, crypto or network. Wider PB_MAX_BATCH values are test-only compile overrides; byte budget is a hosted-peer parameter. SetEvent calls are counted separately from waits; neither is a context-switch counter.';
    kernel_runtime_tested=$false;production_defaults_changed=$false;core_source=$core;core_sha256=(Get-FileHash -LiteralPath $core).Hash.ToLowerInvariant();raw_samples=$raw}|
    ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory "$($Stage.ToLowerInvariant())-evidence.json") -Encoding utf8
Write-Output "Evidence: $directory"
