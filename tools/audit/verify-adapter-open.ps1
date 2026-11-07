param([string]$BaselineDirectory='out\diagnostic-owner-baseline\5fd7eb8e2353')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\adapter-open-tests.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Adapter-open audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\diagnostic-owner-baseline\5fd7eb8e2353')){throw 'Builder requires the frozen 5fd7eb8e2353 whole-source baseline'}
$source=Join-Path $baseline 'api\adapter.c';$sourceHash='7e9249b9216d3e144391d04d0a45892570e3a8cc858af66cf8e084f1e2a6660e'
if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $sourceHash){throw 'Unexpected frozen adapter.c'}
$before=[IO.File]::ReadAllText($source).Replace("`r`n","`n")
$signature=$before.IndexOf('OpenAdapterInternal(LPCWSTR Name)')
$start=$before.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$signature);$end=$before.IndexOf('typedef struct ADAPTER_OPERATION_INPUT',$signature)
if($signature -lt 0 -or $start -lt 0 -or $end -le $start){throw 'Cannot isolate the frozen OpenAdapterInternal aggregate region'}
$region=$before.Substring($start,$end-$start)
$old=@'
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            continue;
        }
'@.Replace("`r`n","`n")
$fixed=@'
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            DWORD Error = GetLastError();
            if (Error == ERROR_NO_MORE_ITEMS)
                break;
            LastError = LOG_ERROR(Error ? Error : ERROR_GEN_FAILURE, L"Failed to enumerate adapters");
            goto cleanupDevInfo;
        }
'@.Replace("`r`n","`n")
if(([regex]::Matches($region,[regex]::Escape($old))).Count -ne 1){throw 'Frozen OpenAdapterInternal enum branch is not unique'}
$expected=$before.Substring(0,$start)+$region.Replace($old,$fixed)+$before.Substring($end)
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\adapter.c')).Replace("`r`n","`n") -cne $expected){throw 'Adapter aggregate changed outside the Open enum failure block'}
foreach($relative in @('api\adapter.h','api\logger.h','api\main.h','api\registry.h','api\operation.h','api\namespace.h','include\packetbridge_identity.h')){
    if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $relative)).Hash){throw "Adapter-open dependency changed: $relative"}
}
$directory=Join-Path $script:RepoRoot "out\adapter-open\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json';$artifacts=@()
foreach($mode in @('iso','ms')) {foreach($guarded in @($false,$true)) {foreach($variant in @('before','current')) {
    $objects=Join-Path $directory "$variant-$mode-guard$([int]$guarded)"
    $build=Build-AdapterOpenTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;$artifacts+=$build
    if($variant -eq 'current'){Invoke-TestTool $build.path @();Invoke-TestTool $build.path @('1')}
    else {
        $output=@(& $build.path 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control-1.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expectedFailure=$code -eq 110 -and ($output -join "`n").Contains('ADAPTER_OPEN_ENUMERATION_ERROR_LOST_AND_SPUN')
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@('1');exit_code=$code;expected_exit_code=110;expected_baseline_failure=$expectedFailure;log=$log})
        Write-TestRecording $(if($expectedFailure){'running'}else{'failed'})
        if(!$expectedFailure){throw "Unexpected adapter-open baseline: exit=$code"}
    }
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during adapter-open audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;frozen_adapter_sha256=$sourceHash;
    frozen_region_sha256='10d5be2bb44c99592f2f653c203299f1c70fb9af640af5560e31ba0997987a44';operations=$script:TestResultFile;artifacts=$artifacts;
    kernel_runtime_tested=$false;production_ready=$false;
    method='Whole frozen adapter.c and exact complete OpenAdapterInternal region hashes locked; extraction offsets/lengths and generated region/entry/binary hashes recorded. Finite diff permits only the Open enum failure block. Win7-target ISO/MS normal/guarded allocation tests. Actual function uses hosted SetupAPI/runtime/metadata/wait/admission boundaries, native events/mutexes/heap (normal) or guard-page allocations (guarded), real logger.h wrappers and native CRT formatting/comparison. First/middle/final Enum failures preserve nonzero errors through logger/cleanup; only NO_MORE_ITEMS means normal end. False+zero Enum is a defensive injection normalized to GEN_FAILURE, not an additional natural SDK failure claim. Old CLI1 observes one error8 then NO_MORE_ITEMS being returned as NOT_FOUND1168 and fully closes/frees/balances owners before110. Persistent errors are a bounded fault model with cancellation checkpoint, not native SDK error-order/reachability evidence. Current matrix/control require zero. Per-record property failures and nonstring type continue, no match, first match/no later reads, case/Unicode/maximum lengths, acquisition/ID/wait/populate errors, partial populate/allocation failures and cancellation are checked; allocation/runtime/handle ledgers return to zero each scenario. No real device/registry/driver actions, installer process/threadpool lifetime or old-OS runtime claim.'}|
    ConvertTo-Json -Depth 12|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
