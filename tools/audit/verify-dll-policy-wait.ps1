param([string]$BaselineDirectory='out\dll-policy-wait-baseline\3c1e122a4b00')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'api\v2\session_lifecycle.h')).Path
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne 'a3f965c276124ba99d5ecd64de8ea51d81967ac56d6f8c70f30259849740029c'){throw 'Unexpected policy-wait baseline'}
$directory=Join-Path $script:RepoRoot "out\dll-policy-wait\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$apiDirectory=Join-Path $script:RepoRoot 'api\v2';$current=Join-Path $apiDirectory 'session_lifecycle.h'
$sessionBody=[IO.File]::ReadAllText((Join-Path $apiDirectory 'session.c'))
$executables=@();$sessions=@()
$support=@('api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')){
 $header=if($variant -eq 'before'){$old}else{$current}
 $body=[regex]::Replace($sessionBody,'(?m)^#include "([^"]+)"',{
  param($match)
  $path=if($match.Groups[1].Value -eq 'session_lifecycle.h'){$header}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
  '#include "'+$path.Replace('\','/')+'"'
 })
 $session=Join-Path $directory "session-$variant.c";[IO.File]::WriteAllText($session,$body,[Text.UTF8Encoding]::new($false))
 $sessions+=@{variant=$variant;path=$session;sha256=(Get-FileHash -LiteralPath $session).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
  $memory=if($guarded){'guarded'}else{'ordinary'}
  $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
  $lines=@();if($variant -eq 'before'){$lines+='#define PB_POLICY_WAIT_BASELINE 1'}
  if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_POLICY_WAIT_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\policy_wait_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'policy.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'policy.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source)+$support+@('/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 }}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during policy wait verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_lifecycle=$old;
 baseline_lifecycle_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_lifecycle_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 method='Actual frozen/current lifecycle with identical current session and hosted policy I/O via PB_API_TESTS. Real Windows SRW locks, condition-variable waits, threads and one-thread pools. A host-only wrapper briefly releases/reacquires State after native sleep returns, before production observes any state, to schedule the equivalent stolen-reacquisition window; it never mutates session state. Ten cases/process: peer Poll/Forget retires old ticket x no/pending/completed replacement, unready and BeginStop wakes with user loan/native pin, finite timeout and injected sleep failure. Old double-ack/slot theft/third admission and obsolete-ticket timeout remain reproduced; current stale waiters preserve output/new ownership and do not sleep again. Completed-replacement old cases use a finite timeout to bound the unreachable predicate; INFINITE nontermination is an inference, not a separate unbounded runtime test. Eight ISO/MS ordinary/guarded processes. Raw C lifecycle extension concurrency, not a demonstrated safe-Rust caller race or kernel NDIS/CPU defect.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;sessions=$sessions;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
