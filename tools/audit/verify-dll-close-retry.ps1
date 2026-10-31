param([string]$BaselineDirectory='out\dll-close-retry-baseline\ca77e0eb656a')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
 @('session.c','9bdafa88a6bfd4c808403c6fa827d8d329f26b5ff87a26018259382fbf298b2d'),
 @('session_lifecycle.h','4cf334c1afc70a9a5ec07e9f6ac38af90103f00f7e29b0e59c6fc52cefef252e'),
 @('session_state.h','f57bd5791fa3bf2ed616fde34576d8853f3bbd88c0a8ba15ee24a5876f4c2abc'),
 @('adapter.c','8b1e7fa07d8d8540611d3e7fe1e72761180960feb3f4ca9b14ef3dd3efec2a48'))){
 if((Get-FileHash -LiteralPath (Join-Path $baseline ('api\v2\'+$entry[0]))).Hash.ToLowerInvariant() -ne $entry[1]){throw "Unexpected close-retry baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\dll-close-retry\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
$support=@('api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$executables=@()
foreach($variant in @('before','after')){foreach($mode in @('iso','ms')){
 $session=if($variant -eq 'before'){Join-Path $baseline 'api\v2\session.c'}else{Join-Path $script:RepoRoot 'api\v2\session.c'}
 $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
 $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\api\v2","/Fo$objects\")+$script:VersionArguments
 foreach($guarded in @($false,$true)){
  $name=if($guarded){'guarded'}else{'ordinary'};$lines=@()
  if($variant -eq 'before'){$lines+='#define PB_CLOSE_RETRY_BASELINE 1'}
  if($guarded){$lines+='#define PB_GUARD_TESTS 1'}
  $lines+='#define PB_CLOSE_RETRY_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
  $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\close_retry_tests.c').Replace('\','/')+'"'
  $source=Join-Path $objects "$name.c";$lines|Set-Content -LiteralPath $source -Encoding utf8
  $exe=Join-Path $objects "$name.exe"
  Invoke-BuildTool 'cl.exe' ($flags+@($source)+$support+@('/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;guarded=$guarded;case='close-retry';path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 }
 foreach($case in @('api','lifecycle')){
  $exe=Join-Path $objects "$case.exe"
  Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session,'api\v2\adapter.c')+$support+@("tests\model\${case}_tests.c",'/link',"/out:$exe")+$libs)
  Invoke-TestTool $exe @()
  $executables+=@{variant=$variant;volatile=$mode;case=$case;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
 }
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during close-retry checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;baseline_directory=$baseline;
 current_session_sha256=(Get-FileHash 'api\v2\session.c').Hash.ToLowerInvariant();current_lifecycle_sha256=(Get-FileHash 'api\v2\session_lifecycle.h').Hash.ToLowerInvariant();
 method='Frozen/current actual session plus actual adapter retain/release, real Windows protected event handles, private thread pools and native waits; device IO and software-device close are hosted, no SYS loaded. Each fixture executes 20 scenarios covering pre/post-registration Open failures, ordinary rollback, allocation/file-open failure, failed adapter start, published close and outstanding user loans. A real CloseHandle failure retains the mapping; repairing the handle while DRAIN/QUERY is unavailable reproduces frozen-code stalls with explicit hosted IO recovery for teardown. Current retries the already-entered device-close stage without new IO authority, and retains adapter ownership through successful device closure. Ordinary/guarded ISO/MS runs plus actual API/lifecycle regressions record 16 processes. Explicit allocation/region/event/module/work/runtime budgets balance; gate-only park control preserves the production one-second diagnostic wait.';
 kernel_runtime_tested=$false;production_ready=$false;operation_record=$script:TestResultFile;executables=$executables}|
 ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
