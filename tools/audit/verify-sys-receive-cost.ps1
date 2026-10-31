param([string]$BaselineDirectory='out\sys-empty-coalesce-baseline\1e9c5350e158')
# Compare operation counts and wait progress using frozen function bodies.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-receive-cost\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$oldPath=Join-Path $baseline 'driver\v2\session_receive.h'
$old=[IO.File]::ReadAllText($oldPath)
$receiveStart=$old.IndexOf('static BOOLEAN PbV2ReceiveBatch(')
$blockStart=$old.IndexOf('        ULONG group = 1, copied = MAXULONG;')
$blockEnd=if($blockStart -ge 0) {$old.IndexOf('        NET_BUFFER_LIST *nbl = PbV2Receive(',$blockStart)} else {-1}
if($receiveStart -lt 0 -or $blockStart -lt 0 -or $blockEnd -le $blockStart) {throw 'Expected preserved inline coalescing block and receive function'}
$encoding=[Text.UTF8Encoding]::new($false)
$oldReceive=Join-Path $directory 'receive-before.h'
[IO.File]::WriteAllText($oldReceive,('#include "'+(Join-Path $script:RepoRoot 'driver\v2\session_drain.h').Replace('\','/')+'"')+"`n"+$old.Substring($receiveStart),$encoding)
$block=$old.Substring($blockStart,$blockEnd-$blockStart)
$block=$block.Replace('i + 1 < count','1 < count').Replace('valid[i + 1]','valid[1]').Replace('items[i]','items[0]').Replace('i + group < count','group < count').Replace('valid[i + group]','valid[group]').Replace('items + i + group','items + group').Replace('PB_DESCRIPTOR *next = items + group;','const PB_DESCRIPTOR *next = items + group;')
$oldCoalesce=Join-Path $directory 'coalesce-before.h'
$helper="static ULONG PbV2TryCoalesce(PB_KERNEL_QUEUE *q, const PB_DESCRIPTOR *items, const BOOLEAN *valid, ULONG count, ULONG enabled, ULONG *outCopied, PB_COALESCE *outCoal)`n{`n    PB_KERNEL_SESSION *s=q->Session; TUN_CTX *ctx=s->Ctx;`n"+$block+"    *outCopied=copied; *outCoal=coal; return group;`n}`n"
[IO.File]::WriteAllText($oldCoalesce,$helper,$encoding)
Start-TestRecording $directory 'operation-results.json'
$binaries=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    foreach($case in @('empty','coalesce')) {
        $lines=@()
        if($variant -eq 'before') {
            if($case -eq 'empty') {$lines+=@('#define PB_EMPTY_BASELINE 1',('#define PB_RECEIVE_SOURCE "'+$oldReceive.Replace('\','/')+'"'))}
            else {$lines+=@('#define PB_COALESCE_BASELINE 1',('#define PB_COALESCE_SOURCE "'+$oldCoalesce.Replace('\','/')+'"'))}
        }
        $fixture=if($case -eq 'empty') {'tests\model\drain_boundary_tests.c'} else {'tests\model\coalesce_path_tests.c'}
        $lines+='#include "'+(Join-Path $script:RepoRoot $fixture).Replace('\','/')+'"'
        $source=Join-Path $objects "$case-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
        $sources=if($case -eq 'empty') {@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')} else {@('common\v2_core.c','common\coalesce.c')}
        $exe=Join-Path $objects "$case.exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+$sources+@($source,'/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $binaries+=@{variant=$variant;case=$case;exe=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during receive A/B checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;baseline_receive_sha256=(Get-FileHash -LiteralPath $oldPath).Hash.ToLowerInvariant();
    current_receive_sha256=(Get-FileHash 'driver\v2\session_receive.h').Hash.ToLowerInvariant();current_coalesce_sha256=(Get-FileHash 'driver\v2\session_coalesce.h').Hash.ToLowerInvariant();
    method='Frozen receive/coalescing bodies and current production functions with same hosted DDI and shared coalescer. Byte/call counts and wait progress, not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$binaries} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
