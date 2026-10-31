param([string]$BaselineDirectory='out\sys-event-policy-baseline\d052ec1415db')
# Frozen/current event/reference/Describe bodies, real hosted waits and guarded packets.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-event-policy\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
Start-TestRecording $directory 'operation-results.json'
$cases=@(
    @{name='reference';fixture='receive_reference_tests.c';macro='PB_REFERENCE_SOURCE';header='session_references.h'},
    @{name='indication';fixture='indication_gate_tests.c';macro='PB_INDICATION_SOURCE';header='indication_gate.h'},
    @{name='describe';fixture='describe_checksum_tests.c';macro='PB_DESCRIBE_SOURCE';header='session_describe.h'}
)
$executables=@()
foreach($variant in @('before','after')) {
    $objects=Join-Path $directory $variant;New-Item -ItemType Directory -Path $objects -Force | Out-Null
    foreach($case in $cases) {
        $lines=@()
        if($variant -eq 'before') {
            $lines+=@('#define PB_EVENT_POLICY_BASELINE 1',('#define '+$case.macro+' "'+(Join-Path $baseline ('driver\v2\'+$case.header)).Replace('\','/')+'"'))
        }
        $lines+='#include "'+(Join-Path $script:RepoRoot ('tests\model\'+$case.fixture)).Replace('\','/')+'"'
        $source=Join-Path $objects "$($case.name)-fixture.c";$lines | Set-Content -LiteralPath $source -Encoding utf8
        $sources=if($case.name -eq 'describe') {@('common\packet.c',$source)} else {@($source)}
        $exe=Join-Path $objects "$($case.name).exe"
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objects\")+$script:VersionArguments+$sources+@('/link',"/out:$exe"))
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case=$case.name;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
$hashes=foreach($variant in @('before','after')) {
    $root=if($variant -eq 'before') {$baseline} else {$script:RepoRoot}
    foreach($case in $cases) {
        $file='driver\v2\'+$case.header
        @{variant=$variant;source=$file;sha256=(Get-FileHash -LiteralPath (Join-Path $root $file)).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) {throw 'Sources changed during event/policy A/B checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    method='Frozen/current actual reference, indication gate and Describe functions. Same real hosted locks/events/waiters, 200000 concurrent retain cycles, 100000 concurrent indications plus 10000 rotations, guarded packet/core and policy-disable injection. Operation counts and progress; not kernel CPU/latency/throughput.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables;source_hashes=$hashes} | ConvertTo-Json -Depth 7 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
