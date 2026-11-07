param([string]$BaselineDirectory='out\sys-return-wake-baseline\b260b7c381cb')
# Frozen/current actual return functions, identical NBL construction and hosted
# receive/worker/query/DDI; includes real waiters and guarded allocations.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-return-wake\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'driver\v2\session_return.h';$old=[IO.File]::ReadAllText($oldPath)
$receive=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_nbl.h'))
$start=$old.IndexOf('typedef struct PB_RETURN_ITEM');$receiveStart=$receive.IndexOf('static NET_BUFFER_LIST *PbV2Receive(')
if($start -lt 0 -or $receiveStart -lt 0){throw 'Cannot locate preserved return/receive functions'}
$oldNbl=Join-Path $directory 'nbl-before.h'
# The extracted receive body still needs the platform macros from its original
# header preamble. Keep the frozen return and current receive bodies verbatim.
$platformPath=Join-Path $script:RepoRoot 'driver\v2\platform_nbl.h'
$platformInclude='#include "'+$platformPath.Replace('\','/')+'"'
if((Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant() -ne '4525e0e63df6d2c48356bdcf92abdc4b6f91e2d99312131704ba6c6073584802'){throw 'Unexpected frozen return-wake baseline'}
$oldFragment=Join-Path $directory 'return-before-fragment.h'
$receiveFragment=Join-Path $directory 'receive-current-fragment.h'
[IO.File]::WriteAllText($oldFragment,$old.Substring($start),[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText($receiveFragment,$receive.Substring($receiveStart),[Text.UTF8Encoding]::new($false))
$composed=$platformInclude+"`n"+$old.Substring($start)+"`n"+$receive.Substring($receiveStart)
[IO.File]::WriteAllText($oldNbl,$composed,[Text.UTF8Encoding]::new($false))
if([IO.File]::ReadAllText($oldNbl) -cne $composed){throw 'Generated return-wake NBL projection changed'}
$nblProjection=[ordered]@{path=$oldNbl;sha256=(Get-FileHash -LiteralPath $oldNbl).Hash.ToLowerInvariant();prefix=$platformInclude+"`n";
    platform_header=@{path=$platformPath;sha256=(Get-FileHash -LiteralPath $platformPath).Hash.ToLowerInvariant()};
    fragments=@(
        @{source=$oldPath;source_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();character_start=$start;character_length=$old.Length-$start;path=$oldFragment;sha256=(Get-FileHash -LiteralPath $oldFragment).Hash.ToLowerInvariant()},
        @{source=(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h');source_sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot 'driver\v2\session_nbl.h')).Hash.ToLowerInvariant();character_start=$receiveStart;character_length=$receive.Length-$receiveStart;path=$receiveFragment;sha256=(Get-FileHash -LiteralPath $receiveFragment).Hash.ToLowerInvariant()});
    method='Absolute actual platform_nbl.h include, then frozen return suffix and current receive suffix with one LF separator; bodies are verbatim UTF-8 text and offsets/lengths are .NET UTF-16 character indices.'}
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    foreach($guarded in @($false,$true)) {
        $mode=if($guarded){'guarded'}else{'ordinary'}
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        foreach($case in @('return','wait')) {
            $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
            if($variant -eq 'before') {$lines+=@('#define PB_RETURN_WAKE_BASELINE 1',('#define PB_NBL_SOURCE "'+$oldNbl.Replace('\','/')+'"'))}
            $test=if($case -eq 'return'){'tests\model\return_batch_tests.c'}else{'tests\model\drain_boundary_tests.c'}
            $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
            $source=Join-Path $objects "$case-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
            $exe=Join-Path $objects "$case.exe"
            $sources=@('common\v2_core.c','common\ethernet.c');if($case -eq 'wait'){$sources+=@('common\coalesce.c','common\packet.c')}
            Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+
                $sources+@($source,'/link',"/out:$exe"))
            Invoke-TestTool $exe @()
            $executables+=@{variant=$variant;guarded=$guarded;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
        }
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during return-wake checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_directory=$baseline;nbl_projection=$nblProjection;
    baseline_return_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();
    current_return_sha256=(Get-FileHash 'driver\v2\session_return.h').Hash.ToLowerInvariant();
    method='Frozen/current actual return functions, same NBL construction, completion/reference/core and actual worker/query. Count KernelEvent writes and idle receive scans. Real host threads/events cover normal/paused idle, full credit, before-wait notification, normal drain, full completion backpressure and two concurrent return chains. Ordinary/guarded runs verify exact tokens/statistics, real native ownership and balanced resources. Not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
