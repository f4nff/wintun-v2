param([string]$BaselineDirectory='out\dll-sync-policy-baseline\2312cfc0d593')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'api\v2\session_control.h')).Path
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne '27677bc3e2a1560cd39a9dfaf12316262e7f65aabd0fc98f6c7e959088543a70'){throw 'Unexpected synchronous-policy baseline'}
$directory=Join-Path $script:RepoRoot "out\dll-sync-policy\$($script:SourceFingerprint.Substring(0,12))"
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
  $lines=@();if($variant -eq 'before'){$lines+='#define PB_SYNC_POLICY_BASELINE 1'}
  if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_SYNC_POLICY_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\sync_policy_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'policy.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'policy.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source)+$support+@('/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 }}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during synchronous-policy verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_control=$old;
 baseline_control_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_control_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 method='Actual frozen/current synchronous SetPolicy with identical current lifecycle/session and hosted native policy I/O. Inject one permitted non-timeout condition-variable wait failure, retaining exclusive State on return. Thirteen cases/process: queued/pending/completed first I/O and pending/completed replacement x native success/failure, ordinary success/native failure, and invalid admission. Completion/peer schedules release State, complete actual worker I/O and use real Forget/Begin before reacquisition; no session fields are directly edited. Frozen SetPolicy returns PB_CORRUPT but leaves its unexposed ticket occupied after I/O completes; explicit frozen-only Forget recovers teardown. Current uses existing ticket-checked abandonment, keeps queued/in-flight ownership BUSY until completion, releases completed orphan, and cannot abandon a replacement. Original wait Win32 error is preserved; normal applied failures remain acknowledged. Real Windows SRW locks/events/threads/private pool, ISO/MS ordinary/guarded, balanced runtime/guarded budgets. Failure is injected, not an observed naturally occurring Windows/NDIS failure or a demonstrated packet-slot/physical reclamation bug.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;sessions=$sessions;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
