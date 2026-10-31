param([string]$BaselineDirectory='out\sys-send-init-baseline\a659e31c7f2e')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'driver\v2\session_send.h')).Path
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne 'f73f43a6692348553489866ce3d902165eac91f56adc97644fa2a492eefc7d3b'){throw 'Unexpected send-initialization baseline'}
$directory=Join-Path $script:RepoRoot "out\sys-send-init\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$current=Join-Path $script:RepoRoot 'driver\v2\session_send.h'
$body=[IO.File]::ReadAllText($current)
$initializer='    PB_TX_WINDOW w = { .Ctx = ctx, .Session = s, .Epoch = retained ? s->Epoch : 0, .Items = items, .Owners = owners };'
if([regex]::Matches($body,[regex]::Escape($initializer)).Count -ne 1){throw 'Cannot locate batch-storage poison boundary'}
$poison=Join-Path $directory 'send-poisoned.h'
$injected="    memset(items, 0xa5, sizeof(items)); memset(owners, 0xa5, sizeof(owners));`r`n"+$initializer
[IO.File]::WriteAllText($poison,$body.Replace($initializer,$injected),[Text.UTF8Encoding]::new($false))
$executables=@();$benchmarks=@{};$samples=@();$listings=@()
foreach($variant in @('before','after','poisoned')){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
 $memory=if($guarded){'guarded'}else{'ordinary'}
 $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $header=if($variant -eq 'before'){$old}elseif($variant -eq 'poisoned'){$poison}else{$current}
 $lines=@();$lines+='#define PB_SEND_SOURCE "'+$header.Replace('\','/')+'"'
 if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
 $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\send_window_tests.c').Replace('\','/')+'"'
 $source=Join-Path $objects 'model.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
 $exe=Join-Path $objects 'model.exe'
 $flags=@('/nologo','/O2','/std:c17','/MT',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments
 Invoke-BuildTool cl.exe ($flags+@($source,'/link',"/out:$exe"));Invoke-TestTool $exe @()
 $executables+=@{variant=$variant;case='send-regressions';volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 if($variant -ne 'poisoned' -and $mode -eq 'iso' -and -not $guarded){
  $lines=@();$lines+='#define PB_SEND_SOURCE "'+$header.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\bench\sys_send_init_bench.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'benchmark.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'benchmark.exe';$listing=Join-Path $objects 'benchmark.asm'
  Invoke-BuildTool cl.exe ($flags+@('/FA',"/Fa$listing",$source,'/link',"/out:$exe"))
  $benchmarks[$variant]=$exe
  $executables+=@{variant=$variant;case='benchmark';volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
  $listings+=@{variant=$variant;path=$listing;sha256=(Get-FileHash -LiteralPath $listing).Hash.ToLowerInvariant()}
 }
}}}
for($round=0;$round -lt 7;++$round){
 $order=if($round%2){@('after','before')}else{@('before','after')}
 foreach($variant in $order){
  $lines=@(Invoke-TestTool $benchmarks[$variant] @())
  foreach($line in $lines){if($line.StartsWith('{')){$sample=$line|ConvertFrom-Json;$sample|Add-Member variant $variant;$sample|Add-Member round $round;$samples+=$sample}}
 }
}
$summaries=foreach($group in ($samples|Group-Object variant,case,batch)){
 if($group.Count -ne 7){throw "Incomplete sample group: $($group.Name)"}
 $row=$group.Group[0];$ns=@($group.Group.ns_per_send|Sort-Object);$cycles=@($group.Group.cycles_per_send|Sort-Object)
 [pscustomobject]@{variant=$row.variant;case=$row.case;batch=$row.batch;samples=$group.Count;median_ns=$ns[3];median_cycles=$cycles[3]}
}
if($samples.Count -ne 98){throw 'Wrong benchmark sample count'}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during send-initialization verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_source=$old;
 baseline_send_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_send_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 method='Actual frozen/current send paired with the same current recycle/reference/core and hosted NDIS fixture. Twelve complete send regressions cover before/after/poisoned storage x ISO/MS x ordinary/guarded. Poisoned source only adds 0xa5 fills to both call-local batch arrays; production has no poison/test hook. All exits and initialized-prefix publication/callback/ownership/wake budgets are unchanged. Fourteen uninstrumented-with-respect-to-initialization benchmark processes retain identical fixture checks/counters, fixed allowed CPU, 2000 warmups and 200000 calls per case, seven alternating rounds. Includes recycle for admitted sends; synthetic Describe/DDIs. No native kernel CPU/IRQL/NDIS/throughput or reduced stack-size claim.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables;assembly=$listings;summaries=@($summaries);samples=$samples}|
 ConvertTo-Json -Depth 8|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
$summaries|Sort-Object case,batch,variant|Format-Table
Write-Output (Join-Path $directory 'evidence.json')
