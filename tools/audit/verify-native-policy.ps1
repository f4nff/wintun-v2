param([string]$BaselineDirectory='out\process-owner-baseline\89081d142dbf',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\native-policy-tests.ps1')
. (Join-Path $PSScriptRoot 'native-start-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$frozen=@{
 'api\v2\session.c'='5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'
 'api\v2\session_control.h'='cd327833ea04c09e9e857389766f36ee7f2c3b4d052035fdda1ec13b7b88d85e'
}
foreach($relative in $frozen.Keys){
 if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant() -ne $frozen[$relative]){throw "Unexpected frozen native policy source: $relative"}
 Assert-NativeStartDependency -BaselineDirectory $baseline -Relative $relative
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\native-policy\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
function Invoke-NativePolicyControl {
 param([string]$Program,[string]$Fixture)
 $lines=@(& $Program $Fixture 106 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE;$output=$lines -join "`n"
 $log=Join-Path $script:TestLogDirectory ('{0:d2}-control-stop-observer.log' -f $script:TestExecutions.Count)
 $lines|Set-Content -LiteralPath $log -Encoding utf8;$lines|Write-Output
 $script:TestExecutions.Add([ordered]@{program=$Program;arguments=@($Fixture,'106');exit_code=$code;expected_exit_code=106;log=$log;reported_pass_messages=@($lines|Where-Object {$_.StartsWith('PASS:')})})
 Write-TestRecording 'running'
 if($code -ne 106 -or $output -notmatch 'NATIVE_POLICY_IO_AFTER_STOP: 1 extra policy IOCTL observed after Control gate; actual DLL drain/cancel, modeled native-active release, join/unload completed with zero owned residue'){throw "Native policy observer control failed ($code)"}
}
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
 $memory=if($guarded){'guarded'}else{'ordinary'}
 foreach($variant in @('current','without-control-stop-check')){
  $objects=Join-Path $directory "$mode-$memory\$variant"
  $built=Build-NativePolicyTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
  if($variant -eq 'current'){Invoke-TestTool $built.path @($built.fixture);Invoke-TestTool $built.path @($built.fixture,'106')}
  else{Invoke-NativePolicyControl $built.path $built.fixture}
  $executables+=$built
 }
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during native policy verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline=$baseline;baseline_sources=$frozen;
 native_start_ledger=@{path='tools/audit/native-start-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'native-start-ledger.ps1')).Hash.ToLowerInvariant()};
 method='Only the independently reviewed cold Allocate failure contract differs; all remaining native sources are unchanged. Actual non-PB_API_TESTS Open/session/control/lifecycle/status/queue in a real unloadable DLL; both fixture and host reject PB_API_TESTS at compile time, fingerprint static assertion requires the shipping 65-byte layout. Device handle is a real event, driver IOCTLs are hosted. NDIS6.20 capabilities use actual PbPlatformOffloads and USO returns native ERROR_NOT_SUPPORTED, consuming or forgetting the real ticket permits native mask-zero asynchronous/synchronous retry while Running. A wrapper gates before real Control acquisition after the worker stop=false snapshot; stopping then prevents a nonzero POLICY IOCTL. Real IO gate separately covers late success and access-denied/out-of-memory failure after stop. Actual reserved user loan and hosted native-active status witness block reclamation in both return orders; public callers are joined and loans returned before consuming the handle, with runtime/native ownership retained when native-active remains. Every final callback and module-tail transfer is gated; all callback-thread handles are joined, every DLL detach checks exact heap/region/event/device/pool/work/module/runtime accounting. Full warmup and measured matrices check process handle convergence. The negative generated control omits only the Control CloseRequested check, records one extra valid policy IOCTL, completes actual DLL DRAIN/cancel, modeled native-active release, thread joins and unload, then exits106 without repair or intentional invalid access. This observer mutation is not a demonstrated historical production defect or real Win7/NDIS execution.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables}|
 ConvertTo-Json -Depth 8|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
