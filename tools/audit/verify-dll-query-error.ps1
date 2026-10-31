param([string]$BaselineDirectory='out\dll-query-error-baseline\7bbec4504e4b')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'api\v2\session_status.h')).Path
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne 'bff862250846d1b79bae501ae4a639f51634041ac6790afe08d0b717c5f0606d'){throw 'Unexpected query-error baseline'}
$directory=Join-Path $script:RepoRoot "out\dll-query-error\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$apiDirectory=Join-Path $script:RepoRoot 'api\v2'
$current=Join-Path $apiDirectory 'session_status.h'
$sessionBody=[IO.File]::ReadAllText((Join-Path $apiDirectory 'session.c'))
$executables=@();$sessions=@()
$support=@('api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
foreach($variant in @('before','after')){
 $header=if($variant -eq 'before'){$old}else{$current}
 # Use the identical current session for both variants. Resolve its local
 # includes absolutely; only StatusInternal/GetStatus's header is substituted.
 $body=[regex]::Replace($sessionBody,'(?m)^#include "([^"]+)"',{
  param($match)
  $path=if($match.Groups[1].Value -eq 'session_status.h'){$header}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
  '#include "'+$path.Replace('\','/')+'"'
 })
 $session=Join-Path $directory "session-$variant.c";[IO.File]::WriteAllText($session,$body,[Text.UTF8Encoding]::new($false))
 $sessions+=@{variant=$variant;path=$session;sha256=(Get-FileHash -LiteralPath $session).Hash.ToLowerInvariant()}
 foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
  $memory=if($guarded){'guarded'}else{'ordinary'}
  $objects=Join-Path $directory "$variant-$mode-$memory";New-Item -ItemType Directory -Path $objects -Force|Out-Null
  $lines=@()
  if($variant -eq 'before'){$lines+='#define PB_QUERY_ERROR_BASELINE 1'}
  if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_CLOSE_RETRY_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\query_error_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'query.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'query.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source)+$support+@('/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 }}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during QUERY error verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_status=$old;
 baseline_status_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_status_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 method='Identical current actual non-PB_API_TESTS session/lifecycle with frozen/current status header. Real Windows handles, private thread pools and waits; device I/O and SwDeviceClose hosted. Each process runs 24 scenarios: short/long byte count, wrong Size/Epoch, true IO failure or valid control x stale zero/ERROR_BUSY x direct/adapter session. Malformed success is rejected in both implementations; frozen API/progress expose stale errors, current reports ERROR_INVALID_DATA. Native failure preserves its actual error. Invalid acknowledgements never authorize device/mapping/adapter release; corrected replies permit one physical close and balanced budgets. Eight ISO/MS ordinary/guarded processes. No kernel failure/CPU measurement.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;sessions=$sessions;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
