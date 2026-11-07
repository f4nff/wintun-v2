param([string]$BaselineDirectory='out\terminal-wake-baseline\e16627eaddcc')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\helper-response-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Helper response audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$fixedBaseline=Join-Path $script:RepoRoot 'out\terminal-wake-baseline\e16627eaddcc'
if($baseline -ne $fixedBaseline){throw 'Builder uses the frozen e16627eaddcc whole-source baseline'}
function Replace-HelperResponseOnce([string]$Text,[string]$Before,[string]$After) {
    if(([regex]::Matches($Text,[regex]::Escape($Before))).Count -ne 1){throw "Helper response change is not unique: $Before"}
    return $Text.Replace($Before,$After)
}
function Get-HelperResponseText([string]$Root,[string]$Relative) {
    return [IO.File]::ReadAllText((Join-Path $Root $Relative)).Replace("`r`n","`n")
}
# Independently constrain the production diff. No broad source/body mutation is
# accepted by the negative control or the include-normalizing test builder.
$parent=Get-HelperResponseText $baseline 'api\rundll32.c'
$parent=Replace-HelperResponseOnce $parent '#ifdef MAYBE_WOW64' "#ifdef MAYBE_WOW64`n#include `"../include/packetbridge_helper_protocol.h`""
$parent=Replace-HelperResponseOnce $parent 'WCHAR Response[8 + 1];' 'WCHAR Response[PB_HELPER_STATUS_RESPONSE_CAPACITY];'
$parent=Replace-HelperResponseOnce $parent 'WCHAR Response[MAX_DEVICE_ID_LEN + 1];' 'WCHAR Response[PB_HELPER_CREATE_RESPONSE_CAPACITY];'
$parent=Replace-HelperResponseOnce $parent '        wcsncpy_s(InstanceId, MAX_DEVICE_ID_LEN, Argv[1], _TRUNCATE);' @'
    {
        SIZE_T Length = wcslen(Argv[1]);
        if (!Length || Length >= MAX_DEVICE_ID_LEN)
            LastError = LOG_ERROR(ERROR_INVALID_DATA, L"Invalid instance ID in worker response");
        else
            memcpy(InstanceId, Argv[1], (Length + 1) * sizeof(*InstanceId));
    }
'@.Replace("`r`n","`n")
if($parent -cne (Get-HelperResponseText $script:RepoRoot 'api\rundll32.c')){throw 'Client changed beyond the finite framing/length fix'}
$process=Get-HelperResponseText $baseline 'api\rundll32_process.h'
$process=Replace-HelperResponseOnce $process '#pragma once' "#pragma once`n#include `"../include/packetbridge_helper_protocol.h`""
$process=Replace-HelperResponseOnce $process 'WCHAR Response[MAX_DEVICE_ID_LEN + 1];' 'WCHAR Response[PB_HELPER_CREATE_RESPONSE_CAPACITY];'
$process=Replace-HelperResponseOnce $process 'ResponseCapacity > MAX_DEVICE_ID_LEN + 1' 'ResponseCapacity > PB_HELPER_CREATE_RESPONSE_CAPACITY'
# Preserve the three original capacity/include projections above, then admit
# only the independently reviewed complete post-exit drain error propagation.
Assert-HelperDrainRepairedFile -Path (Join-Path $script:RepoRoot 'api\rundll32_process.h')
$process=Update-HelperDrainLedger -Body $process -CurrentBody (Get-HelperResponseText $script:RepoRoot 'api\rundll32_process.h')
if($process -cne (Get-HelperResponseText $script:RepoRoot 'api\rundll32_process.h')){throw 'Transport changed beyond the finite capacity and reviewed drain repairs'}
$win7=Get-HelperResponseText $baseline 'setupapihost\host_win7.h'
$win7=Replace-HelperResponseOnce $win7 '#include "../include/packetbridge_identity.h"' "#include `"../include/packetbridge_identity.h`"`n#include `"../include/packetbridge_helper_protocol.h`""
$win7=Replace-HelperResponseOnce $win7 'L"%1!X! %2!s!"' 'PB_HELPER_CREATE_RESPONSE_FORMAT'
if($win7 -cne (Get-HelperResponseText $script:RepoRoot 'setupapihost\host_win7.h')){throw 'Host changed beyond shared format definition'}
if((Get-HelperResponseText $baseline 'setupapihost\host.c') -cne (Get-HelperResponseText $script:RepoRoot 'setupapihost\host.c')){throw 'Host formatter changed'}
$dependencies=@('api\logger.h','api\adapter.h','api\main.h','api\registry.h','api\resource.h','api\rundll32.h','api\operation.h',
    'api\termination_wait.h','api\overlapped_wait.h','api\event_pair.h','common\system_library.h','include\packetbridge_identity.h')
foreach($relative in $dependencies) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $relative)).Hash){throw "Helper framing dependency changed: $relative"}
}
$directory=Join-Path $script:RepoRoot "out\helper-response\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$artifacts=@()
foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {foreach($variant in @('before','current')) {
    $objects=Join-Path $directory "$variant-$mode-guard$([int]$guarded)"
    $build=Build-HelperResponseTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $artifacts+=$build
    if($variant -eq 'current'){Invoke-TestTool $build.path @();Invoke-TestTool $build.path @('1')}
    else {
        $output=@(& $build.path 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control-1.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 107 -and ($output -join "`n").Contains('HELPER_MAX_ID_SUCCESS_FRAME_EXCEEDS_TRANSPORT_CAPACITY')
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@('1');exit_code=$code;expected_exit_code=107;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected helper frame baseline: exit=$code"}
    }
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during helper response audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;operations=$script:TestResultFile;artifacts=$artifacts;
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;
    reviewed_dependencies=($dependencies.Count+1);unchanged_dependencies=$dependencies.Count;
    reviewed_dependency_repairs=@('api\rundll32_process.h');helper_drain_ledger=@{path='tools/audit/helper-drain-ledger.ps1';sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')).Hash.ToLowerInvariant()};
    method='Complete hash-locked pre-fix host/client/transport sources, include paths normalized only. Original finite framing/capacity/length projections remain intact. After the three transport capacity/include changes, the shared complete-header ledger admits only the separately reviewed post-exit drain error propagation repair; the original positive/negative controls and all other source comparisons are retained. Win7-target ISO/MS, normal/guarded native output buffers. Actual host.c and host_win7.h call hosted SetupAPI; native FormatMessage and WriteFile send through native anonymous pipes, actual HelperConsume accumulates byte fragments, actual CreateInstanceWin7ViaRundll32 parses with native CommandLineToArgvW. Only ExecuteRunDll32 scheduling/submission is replaced by a synchronous bridge; its actual invalid-capacity admission is separately called. SDK-contract ID lengths 1..199; 199-char ID is a legal hosted API boundary, not observed fixed ROOT generator behavior. Unicode and malformed frames are transport fault/robustness models. Old CLI1=107 after pipe/event/heap/local cleanup. Current matrix and CLI1 require zero, no truncated success for 200+ IDs. Error DWORDs, rollback, parser/formatter/write failure, fragmentation and cancellation checkpoints tested; resource counters and warmed process handle balance checked. No real devices, registry mutation, helper process, module-pin/threadpool lifetime, old-OS runtime or released cross-architecture path reachability claim. Current release API pipeline does not define MAYBE_WOW64.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
