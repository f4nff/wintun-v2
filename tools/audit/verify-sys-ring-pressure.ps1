param([string]$BaselineDirectory='out\sys-ring-pressure-baseline\496e59eac84b')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$old=Join-Path $baseline 'driver\v2\session_send.h'
$oldHash=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant()
if($oldHash -ne 'e07403f4d34dce82c9438fb274e750df64f5c08eaee0c0111aae9e27a4b2adc1'){throw 'Unexpected full-ring send baseline'}
$directory=Join-Path $script:RepoRoot "out\sys-ring-pressure\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')){foreach($mode in @('iso','ms')){
 $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $lines=@()
 if($variant -eq 'before'){
  $lines+='#define PB_RING_PRESSURE_BASELINE 1'
  $lines+='#define PB_SEND_SOURCE "'+$old.Replace('\','/')+'"'
 }
 $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\send_window_tests.c').Replace('\','/')+'"'
 $source=Join-Path $objects 'send.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'send.exe'
 Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'/link',"/out:$exe"))
 Invoke-TestTool $exe @('--ring-pressure-locks')
 Invoke-TestTool $exe @()
 $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during ring-pressure checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_directory=$baseline;
 baseline_send_sha256=$oldHash;current_send_sha256=(Get-FileHash 'driver\v2\session_send.h').Hash.ToLowerInvariant();
 method='Frozen/current actual send/recycle/reference/core, MSVC ISO/MS volatile modes. Count actual TxLock DDI pairs separately from recycle polls and ready-space reads. A full ready ring with 4096 small same-queue attempts uses 4096 versus 128 lock pairs, while every attempt still recycles and rechecks peer space. Queue switches, 128-KiB byte budget including oversized input charges, guarded NBL/NB arrays and wrapped cursors remain bounded. Eight late-peer schedules cover ready+return, ready-only after validated early return, corrupt recycle producer and corrupt ready consumer, preserving exact ownership/generations, original outcomes and admission stop. Full existing send regressions retain pool pressure/late return, malformed tokens, immutable snapshots, prefix publication, control reply, pause/detach/fault budgets, callback and reference barriers. Hosted operation counts only, not kernel CPU, spin contention or latency.';
 late_return_review='Two further ordinary/wrapped schedules drip valid returned tokens into a still-full ready ring. Every attempt retains recycling and ready-space observations; returned slots become free with unchanged generations, but no reservation/publication is authorized until actual ready space exists. The optimization reduces only repeated empty-window lock release/reacquisition. Native lock contention and tail latency are not measured.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
