param([string]$BaselineDirectory='out\process-owner-baseline\89081d142dbf')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'adapter-open-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Adapter rename audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$source=Join-Path $baseline 'api\adapter.c'
$sourceHash='6be0ef5fa07d6ccd2dd6b0ff337f03be1348df2478e642f4258b49ba530a68f1'
if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne $sourceHash){throw 'Unexpected frozen adapter.c'}
foreach($name in @('logger.h','adapter.h','main.h','registry.h','wintun.h','nci.h','operation.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne
       (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Rename dependency changed: $name"}
}
$body=[IO.File]::ReadAllText($source)
$signature=$body.IndexOf('RenameByNetGUID(_In_')
$start=$body.LastIndexOf('static _Return_type_success_',$signature)
$end=$body.IndexOf("_Use_decl_annotations_`r`nVOID WINAPI`r`nWintunGetAdapterLUID",$signature)
if($signature -lt 0 -or $start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen rename region'}
$functions=$body.Substring($start,$end-$start)
$expected=$body.Substring(0,$start)+"#include `"adapter_rename.h`"`r`n`r`n"+$body.Substring($end)
$expected=Update-AdapterOpenFailureLedger $expected
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\adapter.c')).Replace("`r`n","`n") -cne $expected.Replace("`r`n","`n")){
    throw 'Adapter aggregate changed beyond rename extraction and reviewed Open enum failure'
}
$directory=Join-Path $script:RepoRoot "out\adapter-rename\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$frozen=Join-Path $directory 'rename-before.h';[IO.File]::WriteAllText($frozen,$functions)
$functionHash=(Get-FileHash -LiteralPath $frozen).Hash.ToLowerInvariant()
if($functionHash -ne '59a73411581891ec619d14d29f6797eaac725edf3804854b8fe590c33a1c54c6'){throw 'Frozen rename region hash mismatch'}
$test=Join-Path $script:RepoRoot 'tests\safety\adapter_rename_tests.c'
$executables=@()
foreach($mode in @('default','iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $entry=$test
    if($variant -eq 'before') {
        $entry=Join-Path $objects 'entry.c'
        @(('#define PB_ADAPTER_RENAME_SOURCE "'+$frozen.Replace('\','/')+'"'),
          ('#include "'+$test.Replace('\','/')+'"'))|Set-Content -LiteralPath $entry -Encoding utf8
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$objects\")
    if($mode -ne 'default'){$flags+="/volatile:$mode"}
    $exe=Join-Path $objects 'rename.exe'
    Invoke-BuildTool cl.exe ($flags+@($entry)+$script:VersionArguments+@('/link',"/out:$exe",'ole32.lib'))
    if($variant -eq 'after'){Invoke-TestTool $exe @();Invoke-TestTool $exe @('1')}
    else {
        $output=@(& $exe 1 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control-1.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 103 -and ($output -join "`n").Contains('FOREIGN_METADATA_CHANGED_AFTER_RENAME_FAILURE')
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=103;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected rename baseline: exit=$code"}
    }
    $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during adapter rename audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    frozen_adapter_sha256=$sourceHash;frozen_functions_sha256=$functionHash;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen whole adapter.c and exact RenameByNetGUID/ConvertInterfaceAliasToGuid/NciSetAdapterName region hashes checked. Aggregate comparison permits only rename extraction and the independently verified Open enum failure block. Default/ISO/MS Win7 targets. Old CLI1=103 proves metadata setter after a failed foreign NCI rename; native/key/set resources are reclaimed before exit. Current matrix/control require zero. NCI/IP-helper/SetupAPI/registry boundaries hosted; GUID parser/formatter, CRT, heap strings and event-backed set/key handles native. Tests cover native and metadata failure combinations, alias failures, proposals/Unicode/length boundaries, inner/outer suffix limits, original cancellation checkpoints, first/late allocation failure, match/mismatch/invalid GUID search and cleanup. Best-effort metadata errors, including existing cancellation continuation into native retry, retain their original policy. No real network/device/registry mutation, actual NCI/SetupAPI device execution or old-OS/kernel runtime claim.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
