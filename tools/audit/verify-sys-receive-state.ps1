param([string]$BaselineDirectory='out\sys-receive-state-baseline\7e16a8d49ffd')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$oldReceive=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'driver\v2\session_receive.h')).Path
$oldCoalesce=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'driver\v2\session_coalesce.h')).Path
if((Get-FileHash -LiteralPath $oldReceive).Hash.ToLowerInvariant() -ne '40f43a3b8310445224e03513a3152f6fb26d5dcae1d72f4eb0eecd26e97b8c85' -or (Get-FileHash -LiteralPath $oldCoalesce).Hash.ToLowerInvariant() -ne '5e4f762a8dfa3c1968b699b3e7249efda6a8355050bf77a3356b2d347e0cb426'){throw 'Unexpected receive-state baseline'}
$currentReceive=Join-Path $script:RepoRoot 'driver\v2\session_receive.h';$currentCoalesce=Join-Path $script:RepoRoot 'driver\v2\session_coalesce.h'
$directory=Join-Path $script:RepoRoot "out\sys-receive-state\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$candidateCoalesce=Join-Path $directory 'coalesce-candidate.h'
$candidateReceive=Join-Path $directory 'receive-candidate.h'
$declaration='    ULONG group = 1, copied = MAXULONG; PB_COALESCE coal = { 0 };'
$body=[IO.File]::ReadAllText($currentCoalesce)
if([regex]::Matches($body,[regex]::Escape($declaration)).Count -ne 1){throw 'Cannot isolate coalescing-state candidate'}
$body=$body.Replace($declaration,'    ULONG group = 1, copied = MAXULONG; PB_COALESCE *coal = outCoal;').Replace('&coal,','coal,').Replace('&coal)','coal)').Replace('coal.','coal->').Replace('*outCopied = copied; *outCoal = coal; return group;','*outCopied = copied; return group;')
[IO.File]::WriteAllText($candidateCoalesce,$body,[Text.UTF8Encoding]::new($false))
$body=[IO.File]::ReadAllText($currentReceive)
$declaration='        ULONG group = 1, copied = MAXULONG; PB_COALESCE coal = { 0 };'
if([regex]::Matches($body,[regex]::Escape($declaration)).Count -ne 1){throw 'Cannot isolate receive-state candidate'}
[IO.File]::WriteAllText($candidateReceive,$body.Replace($declaration,'        ULONG group = 1, copied = MAXULONG; PB_COALESCE coal;'),[Text.UTF8Encoding]::new($false))
$headers=@();$executables=@();$assembly=@();$benchmarks=@{};$samples=@()
foreach($variant in @('before','after','poisoned')) {
 $coalesce=if($variant -eq 'before'){$oldCoalesce}elseif($variant -eq 'after'){$candidateCoalesce}else{$currentCoalesce}
 $receive=if($variant -eq 'before'){$oldReceive}elseif($variant -eq 'after'){$candidateReceive}else{$currentReceive}
 $body=[IO.File]::ReadAllText($receive).Replace('#include "session_coalesce.h"','#include "'+$coalesce.Replace('\','/')+'"')
 if($variant -eq 'poisoned') {
  $declaration='        ULONG group = 1, copied = MAXULONG; PB_COALESCE coal = { 0 };'
  if([regex]::Matches($body,[regex]::Escape($declaration)).Count -ne 1){throw 'Missing caller-state poison boundary'}
  $body=$body.Replace($declaration,$declaration+' memset(&coal, 0xa5, sizeof(coal));')
 }
 $header=Join-Path $directory "receive-$variant.h";[IO.File]::WriteAllText($header,$body,[Text.UTF8Encoding]::new($false))
 $headers+=@{variant=$variant;path=$header;sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
  $memory=if($guarded){'guarded'}else{'ordinary'}
  foreach($case in @('receive','state')) {
   if($variant -eq 'poisoned' -and $case -eq 'state'){continue}
   $objects=Join-Path $directory "$variant-$mode-$memory-$case";New-Item -ItemType Directory -Path $objects -Force|Out-Null
   $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
   if($case -eq 'receive') {$lines+='#define PB_RECEIVE_SOURCE "'+$header.Replace('\','/')+'"';$test='tests\model\drain_boundary_tests.c';$support=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c')}
   else {
    $lines+='#define PB_COALESCE_STATE_ACTUAL_SOURCE "'+$coalesce.Replace('\','/')+'"';$test='tests\model\coalesce_state_tests.c';$support=@('common\v2_core.c','common\coalesce.c')
   }
   $lines+='#include "'+(Join-Path $script:RepoRoot $test).Replace('\','/')+'"'
   $source=Join-Path $objects "$case.c";$lines|Set-Content -LiteralPath $source -Encoding utf8
   $exe=Join-Path $objects "$case.exe"
   $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\driver\v2","/Fo$objects\")+$script:VersionArguments
   Invoke-BuildTool cl.exe ($flags+@($source)+$support+@('/link',"/out:$exe"));Invoke-TestTool $exe @()
   $executables+=@{variant=$variant;case=$case;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
   if($case -eq 'receive' -and $variant -ne 'poisoned' -and $mode -eq 'iso' -and -not $guarded) {
    $source=Join-Path $objects 'benchmark.c'
    @(('#define PB_RECEIVE_SOURCE "'+$header.Replace('\','/')+'"'),('#include "'+(Join-Path $script:RepoRoot 'tests\bench\sys_receive_state_bench.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $objects 'benchmark.exe'
    Invoke-BuildTool cl.exe ($flags+@('/FAs',"/Fa$objects\",$source)+$support+@('/link',"/out:$exe"))
    $benchmarks[$variant]=$exe;$listing=Join-Path $objects 'benchmark.asm'
    $executables+=@{variant=$variant;case='benchmark';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    $assembly+=@{variant=$variant;path=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant()}
   }
  }
 }}
}
for($round=0;$round -lt 7;++$round) {foreach($variant in $(if($round%2){@('after','before')}else{@('before','after')})) {
 foreach($line in @(Invoke-TestTool $benchmarks[$variant] @())) {if($line.StartsWith('{')){$sample=$line|ConvertFrom-Json;$sample|Add-Member variant $variant;$sample|Add-Member round $round;$samples+=$sample}}
}}
$summaries=foreach($group in ($samples|Group-Object variant,case,batch)) {
 if($group.Count -ne 7){throw 'Incomplete receive-state benchmark samples'}
 $row=$group.Group[0];$ns=@($group.Group.ns_per_receive|Sort-Object);$cycles=@($group.Group.cycles_per_receive|Sort-Object)
 [pscustomobject]@{variant=$row.variant;case=$row.case;batch=$row.batch;samples=7;median_ns=$ns[3];median_cycles=$cycles[3]}
}
if($samples.Count -ne 42 -or (Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Receive-state evidence incomplete or stale'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_receive=$oldReceive;baseline_coalesce=$oldCoalesce;
 baseline_receive_sha256=(Get-FileHash -LiteralPath $oldReceive).Hash.ToLowerInvariant();baseline_coalesce_sha256=(Get-FileHash -LiteralPath $oldCoalesce).Hash.ToLowerInvariant();current_receive_sha256=(Get-FileHash -LiteralPath $currentReceive).Hash.ToLowerInvariant();current_coalesce_sha256=(Get-FileHash -LiteralPath $currentCoalesce).Hash.ToLowerInvariant();
 production_implementation_changed=$false;decision='candidate-not-adopted';candidate_receive=$candidateReceive;candidate_receive_sha256=(Get-FileHash -LiteralPath $candidateReceive).Hash.ToLowerInvariant();candidate_coalesce=$candidateCoalesce;candidate_coalesce_sha256=(Get-FileHash -LiteralPath $candidateCoalesce).Hash.ToLowerInvariant();
 method='Retained production/frozen receive versus an out-only direct-state candidate, identical core and hosted DDIs. Twenty regression processes: before/after ISO/MS ordinary/guarded actual receive and poisoned caller-state/Begin-address packet oracle, plus four poisoned-retained-production actual receive cases. Same NBL/MDL/payload/checksum/slot/return/wake/fault/allocation assertions. Candidate removes caller/helper state clears and uses caller output directly; it is not adopted. Resolved receive headers change only selected coalescing include and declared 0xa5 caller-state fill. Core Begin remains the successful-state initializer. Fourteen benchmark processes use actual receive/return/core, identical hosted allocation/DDI checks, fixed allowed CPU, 5000 warmups and 100000 iterations, seven alternating rounds for plain ACK batches 1/32 and RSC-enabled pure ACK batch 32. Earlier short and longer comparisons show mixed/no stable gains; preserve production initialization/output behavior. Assembly proves removed operations only, not native kernel CPU/IRQL, throughput or stack-size evidence.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;headers=$headers;assembly=$assembly;samples=$samples;summaries=@($summaries)}|
 ConvertTo-Json -Depth 8|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries|Sort-Object case,batch,variant|Format-Table
Write-Output (Join-Path $directory 'evidence.json')
