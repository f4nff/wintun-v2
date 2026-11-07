param([string]$BaselineDirectory='out\registration-owner-baseline\74362d336bb1',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\callback-handoff-tests.ps1')
. (Join-Path $PSScriptRoot 'native-start-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'
 'api\v2\session_lifecycle.h'='33806814b12535da7304b63d544aa0b31c9862a8c7b465eb004d374a9455392d'
}
foreach($relative in $frozen.Keys){
 $old=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative
 if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne $frozen[$relative]){throw "Unexpected frozen callback source: $relative"}
 Assert-NativeStartDependency -BaselineDirectory $baseline -Relative $relative
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\callback-handoff\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
function Invoke-ObserverControl {
 param([string]$Program,[string]$Fixture)
 $lines=@(& $Program $Fixture 101 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE;$output=$lines -join "`n"
 $log=Join-Path $script:TestLogDirectory ('{0:d2}-missing-work-pin.log' -f $script:TestExecutions.Count)
 $lines|Set-Content -LiteralPath $log -Encoding utf8;$lines|Write-Output
 $script:TestExecutions.Add([ordered]@{program=$Program;arguments=@($Fixture,'101');exit_code=$code;expected_exit_code=101;log=$log;reported_pass_messages=@($lines|Where-Object {$_.StartsWith('PASS:')})})
 Write-TestRecording 'running'
 if($code -ne 101 -or $output -notmatch 'CALLBACK_WORK_PIN_MISSING:.*negative-only repair=1, shutdown/join/unload completed with zero owned residue'){throw "Callback observer control failed ($code)"}
}
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
 $memory=if($guarded){'guarded'}else{'ordinary'}
 foreach($variant in @('current','missing-first-work-pin')){
  $objects=Join-Path $directory "$mode-$memory\$variant"
  $built=Build-CallbackHandoffTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
  if($variant -eq 'current'){Invoke-TestTool $built.path @($built.fixture);Invoke-TestTool $built.path @($built.fixture,'101')}
  else{Invoke-ObserverControl $built.path $built.fixture}
  $executables+=$built
 }
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during callback handoff verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline=$baseline;baseline_sources=$frozen;
 native_start_ledger=@{path='tools/audit/native-start-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'native-start-ledger.ps1')).Hash.ToLowerInvariant()};
 method='Only the independently reviewed cold Allocate failure contract differs; all remaining session/lifecycle sources are unchanged. Actual session/lifecycle in unloadable DLLs; wrapper tests idle predicate under real State and gates only after unlocking, before the old work reference is dropped. New policy or stop work is queued at that precise boundary. Real private max-one pools, callback-active counter, module references/deferred tail release, SRW/condition variables/events and explicit callback-thread handle joins. Hosted device I/O only; one/two queues, first and second policy success/failure, consume/forget, queued abandon, late stop/public transfer, zero-budget PrepareClose and synchronous failed wait after a joined caller. Every detach requires zero tracked allocations/regions/events/pools/work/pins/runtime quota; a full warmup precedes measured handle convergence. Negative generated variant omits exactly the first policy work Retain: witness refs=2, negative-only repair=1 then actual shutdown/join/unload, final exit101; current control exits0. This is an observer fault injection, not a confirmed historical production bug.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables}|
 ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
