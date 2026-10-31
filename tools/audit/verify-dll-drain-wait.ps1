param([string]$BaselineDirectory='out\dll-drain-wait-baseline\e6d177c61ca7')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'api\v2\session_control.h')).Path
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne '9fc613f08e6ef52105ed5fe802c2758756b22d15919cfa97f2ffc77105899108'){throw 'Unexpected synchronous Drain baseline'}
$directory=Join-Path $script:RepoRoot "out\dll-drain-wait\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$apiDirectory=Join-Path $script:RepoRoot 'api\v2';$current=Join-Path $apiDirectory 'session_control.h'
$sessionBody=[IO.File]::ReadAllText((Join-Path $apiDirectory 'session.c'))
$executables=@();$sessions=@()
$support=@('api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')) {
 $header=if($variant -eq 'before'){$old}else{$current}
 $body=[regex]::Replace($sessionBody,'(?m)^#include "([^"]+)"',{
  param($match)
  $path=if($match.Groups[1].Value -eq 'session_control.h'){$header}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
  '#include "'+$path.Replace('\','/')+'"'
 })
 $session=Join-Path $directory "session-$variant.c";[IO.File]::WriteAllText($session,$body,[Text.UTF8Encoding]::new($false))
 $sessions+=@{variant=$variant;path=$session;sha256=(Get-FileHash -LiteralPath $session).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {
  $memory=if($guarded){'guarded'}else{'ordinary'}
  $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
  $lines=@();if($variant -eq 'before'){$lines+='#define PB_DRAIN_WAIT_BASELINE 1'}
  if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_DRAIN_WAIT_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\drain_wait_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'drain.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'drain.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source)+$support+@('/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
  if($variant -eq 'before') {
   $output=@(& $exe --require-error-exit 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
   $log=Join-Path $objects 'expected-regression.log';$output|Set-Content -LiteralPath $log -Encoding utf8
   $condition='DrainResult==PB_CORRUPT && DrainError==ERROR_OPERATION_ABORTED && Failures==1 && Calls==1 && !Recoveries'
   $expected=$code -eq 1 -and ($output -join "`n").Contains($condition)
   $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('--require-error-exit');exit_code=$code;expected_exit_code=1;expected_regression_observed=$expected;log=$log})
   Write-TestRecording $(if($expected){'running'}else{'failed'})
   if(-not $expected){throw 'Frozen Drain did not reproduce the ignored wait failure'}
   Write-Output "PASS: frozen Drain rejects the required immediate error exit ($mode/$memory), exit=$code"
  }
 }}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during Drain wait verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_control=$old;
 baseline_control_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_control_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 method='Frozen/current actual synchronous Drain and identical current session/lifecycle. Real Windows locks/events/caller thread/private worker, hosted native DRAIN and non-timeout wait failure. Nineteen cases/process: queued/pending native I/O x one/eight immediate wait failures x native success/failure (8); I/O completed during failed sleep x native success/failure (2); ordinary/spurious successful wake x queued/pending x native success/failure (8); null argument (1). Frozen-only observer bounds persistent failures by completing actual I/O after the budget, without session-state edits. Before: 38 injected failures, 46 caller wait calls, 8 explicit recoveries across ten failure cases. After: ten failures, ten calls, no recovery; PB_CORRUPT preserves the original ERROR_OPERATION_ABORTED even when native I/O completed while unlocked. Ordinary/spurious wake still rechecks predicate. Reserved user loan and hosted native active reference prevent early prepare/physical close after all caller outcomes; actual final Close and runtime/guarded/owned-event-and-thread handle counts balance. Eight positive processes plus four frozen expected-regression exits. Operation counts do not measure CPU time or show naturally occurring OS/NDIS failures.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;sessions=$sessions;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
