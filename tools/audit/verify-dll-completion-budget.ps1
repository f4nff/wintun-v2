param([string]$BaselineDirectory='out\dll-completion-budget-baseline\fcdeb81fb619')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$old=(Resolve-Path -LiteralPath (Join-Path $BaselineDirectory 'api\v2\session_lifecycle.h')).Path
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne 'ca10a6d116035e236af78d238e759d67d51f5307a0f05d772d3fad56884f21dc'){throw 'Unexpected completion-budget baseline'}
$directory=Join-Path $script:RepoRoot "out\dll-completion-budget\$($script:SourceFingerprint.Substring(0,12))"
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
  $lines=@();if($variant -eq 'before'){$lines+='#define PB_CLOSE_COMPLETION_BASELINE 1'}
  if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_CLOSE_COMPLETION_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\close_completion_budget_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects 'completion.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects 'completion.exe'
  Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source)+$support+@('/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 }}
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during completion-budget verification'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_lifecycle=$old;
 baseline_lifecycle_sha256=(Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant();current_lifecycle_sha256=(Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant();
 method='Actual frozen/current lifecycle with identical current DLL/core and hosted native I/O via PB_API_TESTS. Twenty-four cases per process: 127/128/129/256/510 valid completions, ordinary and wrapped cursors, two queues, invalid token and corrupt producer alongside healthy backlog, completions published between empty Poll and STOPPED QUERY, missing/partial delivery, user loan and native reference barriers. Real Windows private thread pool/events; observes at most four Pop calls and 128 descriptors per queue/turn and no fallback timeout. Frozen implementation reproduces premature bookkeeping retirement and subsequent public PB_CORRUPT on valid tokens. Current consumes healthy completion rings before trusted STOPPED reconciliation, preserves damaged/abandoned fallback and physical close barriers. Eight ISO/MS ordinary/guarded processes. No native kernel/NDIS or CPU measurement.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;sessions=$sessions;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
