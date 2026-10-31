param([string]$BaselineDirectory='out\dll-close-wake-baseline\ba18cd057953')
# Frozen/current actual queue completion, same lifecycle/core and real events.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(
    @('api\v2\session.c','9bdafa88a6bfd4c808403c6fa827d8d329f26b5ff87a26018259382fbf298b2d'),
    @('api\v2\session_queue.h','5c5c8bc032b5b52750182c80d98c21259c094ce0322a75599a00ed5fe44eb950'),
    @('api\v2\session_lifecycle.h','4cf334c1afc70a9a5ec07e9f6ac38af90103f00f7e29b0e59c6fc52cefef252e'),
    @('api\v2\session_state.h','f57bd5791fa3bf2ed616fde34576d8853f3bbd88c0a8ba15ee24a5876f4c2abc'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]) {throw "Unexpected close-wake baseline: $($entry[0])"}
}
$directory=Join-Path $script:RepoRoot "out\dll-close-wake\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$oldSession=Join-Path $directory 'session-before.c'
$queue='#include "'+(Join-Path $baseline 'api\v2\session_queue.h').Replace('\','/')+'"'
$body=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c')).Replace('#include "session_queue.h"',$queue)
[IO.File]::WriteAllText($oldSession,$body,[Text.UTF8Encoding]::new($false))
Start-TestRecording $directory 'operation-results.json'
$support=@('api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
$libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
$executables=@()
foreach($variant in @('before','after')) {foreach($mode in @('iso','ms')) {
    $session=if($variant -eq 'before'){$oldSession}else{Join-Path $script:RepoRoot 'api\v2\session.c'}
    $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$mode",'/W4','/WX',"/I$script:RepoRoot\api\v2","/Fo$objects\")+$script:VersionArguments
    foreach($guarded in @($false,$true)) {
        $memory=if($guarded){'guarded'}else{'ordinary'}
        $lines=@();if($variant -eq 'before') {$lines+='#define PB_CLOSE_WAKE_BASELINE 1'}
        if($guarded) {$lines+='#define PB_GUARD_TESTS 1'}
        $lines+='#define PB_CLOSE_WAKE_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\close_wake_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects "$memory.c";$lines|Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects "$memory.exe"
        Invoke-BuildTool 'cl.exe' ($flags+@($source)+$support+@('common\ethernet.c','/link',"/out:$exe")+$libs);Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case='close-wake';volatile=$mode;guarded=$guarded;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
    foreach($case in @('api','lifecycle')) {
        $exe=Join-Path $objects "$case.exe"
        Invoke-BuildTool 'cl.exe' ($flags+@('/DPB_API_TESTS=1',$session)+$support+@("tests\model\${case}_tests.c",'/link',"/out:$exe")+$libs)
        Invoke-TestTool $exe @()
        $executables+=@{variant=$variant;case=$case;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during close-wake checks'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_queue_sha256=(Get-FileHash -LiteralPath (Join-Path $baseline 'api\v2\session_queue.h')).Hash.ToLowerInvariant();
    current_queue_sha256=(Get-FileHash 'api\v2\session_queue.h').Hash.ToLowerInvariant();
    method='Frozen/current actual queue, same current lifecycle/core and actual SYS NBL/return/flush/drain/query helpers with hosted DDI. ISO/MS ordinary/guarded; real Windows event and wait operations. 600 non-progress calls produce 600 versus 0 ProgressEvent calls; seventeen corrupt polls produce 18 versus 1 ProgressEvent calls, while every successful invalid-token Pop now notifies KernelEvent for released space. Actual native QUERY is routed into DLL StatusInternal (TestActive acknowledgement bypass disabled) for the cross-layer case: raw invalid-epoch submissions fill completions, actual NBL return leaves pending/credit=1 after acknowledged DRAIN and native refs=0. Old producer stalls until explicit recovery wake; current producer flushes and native STOPPED authorizes local retirement without manual wake. Native-only terminal with a noncooperating full recycle ring reproduces an old user-loan stall; current stopped-admission fallback locally retires that view without publishing a recycle or freeing the native slot, and native acknowledgement still blocks close. Running PB_FULL backpressure is unchanged. Genuine returns, packet rejection status 4, first fault, and first Wait terminal hint retain their progress notifications. Controlled asynchronous retry snapshots justify waiting for a completed failed snapshot, with retained resources checked throughout. Actual API/lifecycle regressions retain Wait cancellation, failure retries and barriers. Shell pin only joins public-close callbacks for deterministic guarded allocation/admission teardown. Not kernel CPU/latency/throughput.';
    stopped_release_boundary_review='Both queue variants also run eight normal/native-terminal x full/partial recycle x ordinary/wrapped cursor scenarios. Each scenario checks nine guarded invalid token batches for byte-exact pool/ring rollback, a joined real wrong-thread release, actual accepted-prefix publication, full-ring local-view retirement (old expected stall plus explicit teardown recovery), repeated-return rejection, and actual SYS recycling of only the published prefix with exact free-index/full-pool-cache checks. A separately held hosted native acknowledgement prevents physical close. The matrix adds no production flags, protocol hooks or shortened waits.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables}|
    ConvertTo-Json -Depth 7|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
