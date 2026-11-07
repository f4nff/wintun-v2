param([string]$BaselineDirectory='out\native-close-baseline\019ed63327d9')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\win7-create-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Win7 creation owner audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\native-close-baseline\019ed63327d9')){throw 'Builder requires the frozen 019ed63327d9 whole-source baseline'}
$source=Join-Path $baseline 'api\adapter_win7.h'
$sourceHash='524c5ae0818daf6f423043e9258135aa437f4f96f50e1e6fb443dcf5b53d24f0'
if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $sourceHash){throw 'Unexpected frozen Win7 creation source'}
$before=[IO.File]::ReadAllText($source).Replace("`r`n","`n")
$old=@'
        if (!CreateInstanceWin7ViaRundll32(Adapter->DevInstanceID))
        {
            LastError = LOG_LAST_ERROR(L"Failed to create device instance");
            goto cleanup;
        }
'@.Replace("`r`n","`n")
if(([regex]::Matches($before,[regex]::Escape($old))).Count -ne 1){throw 'Old helper-failure branch is not unique'}
$expected=$before.Replace($old,$old.Replace('goto cleanup;','goto cleanupDevInfo;'))
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\adapter_win7.h')).Replace("`r`n","`n") -cne $expected){throw 'Win7 aggregate changed beyond the single failure cleanup label'}
foreach($relative in @('api\adapter.h','api\logger.h','api\main.h','api\registry.h','api\operation.h','api\rundll32.h',
    'api\adapter_wait_win7.h','api\orphan_owner.h','include\packetbridge_identity.h')){
    if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $relative)).Hash){throw "Win7 creation dependency changed: $relative"}
}
$directory=Join-Path $script:RepoRoot "out\win7-create\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$artifacts=@()
foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {foreach($variant in @('before','current')) {
    $objects=Join-Path $directory "$variant-$mode-guard$([int]$guarded)"
    $build=Build-Win7CreateTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant
    $artifacts+=$build
    if($variant -eq 'current'){Invoke-TestTool $build.path @();Invoke-TestTool $build.path @('1')}
    else {
        $output=@(& $build.path 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control-1.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expectedFailure=$code -eq 108 -and ($output -join "`n").Contains('WIN7_HELPER_FAILURE_SKIPS_DEVINFO_RELEASE')
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@('1');exit_code=$code;expected_exit_code=108;expected_baseline_failure=$expectedFailure;log=$log})
        Write-TestRecording $(if($expectedFailure){'running'}else{'failed'})
        if(!$expectedFailure){throw "Unexpected Win7 creation baseline: exit=$code"}
    }
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during Win7 creation audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;frozen_source_sha256=$sourceHash;
    operations=$script:TestResultFile;artifacts=$artifacts;kernel_runtime_tested=$false;production_ready=$false;
    method='Whole frozen adapter_win7.h hash locked, only its local include paths normalized for hosting. Production aggregate differs by exactly one goto cleanupDevInfo in the failed cross-architecture helper branch. Actual CreateAdapterWin7 and cleanup labels run with native event-backed HDEVINFO, native heap-backed driver list, real CRT logging and optional guard page after WINTUN_ADAPTER. SetupAPI, process-time, helper and readiness-wait boundaries hosted; no real devices/registry/driver. MAYBE_WOW64 build exercises matching NativeMachine/native installation and differing NativeMachine/helper paths; current release pipeline does not define MAYBE_WOW64 and no released-path runtime leak is claimed. Every acquisition prefix, helper error/cancellation/timeout, open/property/ID/wait failures, original driver-selection error mapping, best-effort coinstaller/interface failures, cleanup combinations and success tested. Cleanup failure injections are ownership fault models and use explicit rescue, not additional confirmed production defects. Negative control observes empty-set ownership still live and no destroy attempt after helper failure, rescues/closes it, balances handles/driver allocations and destroys heap before 108. Current CLI/matrix require zero; per-scenario process handle count and allocation balance checked. Length/Unicode/property-capacity models include rooted maximum199 ID; no fixed ROOT native generator-length or async callback barrier/threadpool lifetime claim.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
