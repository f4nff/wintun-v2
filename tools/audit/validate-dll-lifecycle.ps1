param([switch]$SkipBenchmark)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
$script:SourceFingerprint=Get-SourceFingerprint
$directory=Join-Path $script:RepoRoot "out\dll-lifecycle-validation\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$checks=[Collections.Generic.List[object]]::new()
function Save-Validation([string]$status) {
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;version=$script:Version;result=$status;
        kernel_runtime_tested=$false;production_ready=$false;checks=@($checks.ToArray())} |
        ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $directory 'validation.json') -Encoding utf8
}
Save-Validation 'running'
$work=@()
foreach($stage in @('Tests','Safety','Driver','Probe','Api')) {
    $work+=@{name=$stage;arguments=@('-NoProfile','-File',(Join-Path $script:RepoRoot 'tools\build\build.ps1'),'-Stage',$stage)}
}
foreach($entry in @(@('Legacy','compile-legacy.ps1'),@('AnalyzeDll','analyze-dll.ps1'),@('AnalyzeSys','analyze-sys.ps1'))) {
    $work+=@{name=$entry[0];arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot $entry[1]))}
}
if(-not $SkipBenchmark) {
    $work+=@{name='DllDrainWait';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-drain-wait.ps1'))}
    $work+=@{name='DllSyncPolicy';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-sync-policy.ps1'))}
    $work+=@{name='SysReceiveState';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-receive-state.ps1'))}
    $work+=@{name='SysPauseWake';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-pause-wake.ps1'))}
    $work+=@{name='SysReturnReclaim';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-return-reclaim.ps1'))}
    $work+=@{name='DllCompletionBudget';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-completion-budget.ps1'))}
    $work+=@{name='DllPolicyWait';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-policy-wait.ps1'))}
    $work+=@{name='DllQueryError';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-query-error.ps1'))}
    $work+=@{name='SysSendInit';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-send-init.ps1'))}
    $work+=@{name='SysFaultQuery';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-fault-query.ps1'))}
    $work+=@{name='DllCloseRetry';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-close-retry.ps1'))}
    $work+=@{name='DllCloseWake';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-close-wake.ps1'))}
    $work+=@{name='SysRingPressure';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-ring-pressure.ps1'))}
    $work+=@{name='SysReceiveTail';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-receive-tail.ps1'))}
    $work+=@{name='DllOwnershipStore';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-ownership-store.ps1'))}
    $work+=@{name='QueuePop';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-queue-pop.ps1'))}
    $work+=@{name='DllCompletionRetire';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-completion-retire.ps1'))}
    $work+=@{name='DllTokenValidation';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-token-validation.ps1'))}
    $work+=@{name='SysCoalesceReuse';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-coalesce-reuse.ps1'))}
    $work+=@{name='DllReturnPublish';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-dll-return-publish.ps1'))}
    $work+=@{name='SysPausedRecycle';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-paused-recycle.ps1'))}
    $work+=@{name='SysHotpath';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-hotpath.ps1'))}
    $work+=@{name='SysReceiveCost';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-receive-cost.ps1'))}
    $work+=@{name='SysParseCancel';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-parse-cancel.ps1'))}
    $work+=@{name='SysCursor';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-cursor.ps1'))}
    $work+=@{name='SysSendFault';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-send-fault.ps1'))}
    $work+=@{name='SysReceiveBatch';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-receive-batch.ps1'))}
    $work+=@{name='SysEventPolicy';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-event-policy.ps1'))}
    $work+=@{name='SysCompletionCopy';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-completion-copy.ps1'))}
    $work+=@{name='SysReturnWake';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-return-wake.ps1'))}
    $work+=@{name='SysPoolIndex';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-pool-index.ps1'))}
    $work+=@{name='SysCoalesceTerminal';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-coalesce-terminal.ps1'))}
    $work+=@{name='SysRecycleToken';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-recycle-token.ps1'))}
    $work+=@{name='SysReferenceStore';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-reference-store.ps1'))}
    $work+=@{name='SysChecksum';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-checksum.ps1'))}
    $work+=@{name='SysSendPrefix';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'verify-sys-send-prefix.ps1'))}
    $work+=@{name='Benchmark';arguments=@('-NoProfile','-File',(Join-Path $PSScriptRoot 'bench-dll.ps1'))}
}
foreach($check in $work) {
    $log=Join-Path $directory "$($check.name).log"
    & (Join-Path $PSHOME 'pwsh.exe') @($check.arguments) *> $log
    $code=$LASTEXITCODE
    $checks.Add([ordered]@{name=$check.name;exit_code=$code;log=$log})
    if($code -ne 0) { Save-Validation 'failed';Get-Content $log -Tail 35;throw "Offline lifecycle validation failed: $($check.name)" }
    Save-Validation 'running';Write-Output "PASS: $($check.name)"
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint) { Save-Validation 'stale';throw 'Sources changed during validation; rebuild one frozen fingerprint.' }
Save-Validation 'passed'
Write-Output (Join-Path $directory 'validation.json')
