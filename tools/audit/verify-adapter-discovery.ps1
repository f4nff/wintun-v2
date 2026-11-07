param([string]$BaselineDirectory='out\interface-guard-baseline\b208f76ac8e3')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'adapter-open-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Adapter discovery audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$baselineFile=Join-Path $baseline 'api\adapter.c'
$baselineHash='36d2297197f55158d1c14b5cd01bb1455d54c52a5d3381853f3a9dc6b62a36d2'
if((Get-FileHash -LiteralPath $baselineFile).Hash.ToLowerInvariant() -ne $baselineHash){throw 'Unexpected frozen adapter.c'}
foreach($name in @('logger.h','adapter.h','main.h','registry.h','wintun.h','operation.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne
       (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Discovery dependency changed: $name"}
}
$body=[IO.File]::ReadAllText($baselineFile)
$signature=$body.IndexOf('AdapterGetDeviceObjectFileName(LPCWSTR InstanceId)')
if($signature -lt 0){throw 'Frozen discovery function missing'}
$start=$body.LastIndexOf('_Use_decl_annotations_',$signature)
$end=$body.IndexOf('typedef struct _WAIT_FOR_INTERFACE_CTX',$signature)
if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen discovery function'}
$function=$body.Substring($start,$end-$start)
if(!$function.Contains('AdapterGetDeviceObjectFileName(') -or $function.Contains('WaitForInterface(')){throw 'Unexpected discovery extraction'}
$expected=($body.Substring(0,$start)+"#include `"adapter_discovery.h`"`n`n"+$body.Substring($end)).Replace("`r`n","`n")
# The independent rename review extracted only the existing naming functions.
# Preserve the whole aggregate comparison after that finite transformation.
$renameStart=$expected.IndexOf("static _Return_type_success_(return != FALSE)`nBOOL`nRenameByNetGUID(")
$renameEnd=$expected.IndexOf("_Use_decl_annotations_`nVOID WINAPI`nWintunGetAdapterLUID(",$renameStart)
if($renameStart -lt 0 -or $renameEnd -le $renameStart){throw 'Cannot isolate independent frozen rename functions'}
$expected=$expected.Substring(0,$renameStart)+"#include `"adapter_rename.h`"`n`n"+$expected.Substring($renameEnd)
$expected=Update-AdapterOpenFailureLedger $expected
if([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\adapter.c')).Replace("`r`n","`n") -cne $expected){throw 'Adapter aggregate changed beyond discovery/rename extractions and reviewed Open enum failure'}
$directory=Join-Path $script:RepoRoot "out\adapter-discovery\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$frozen=Join-Path $directory 'discovery-before.h'
[IO.File]::WriteAllText($frozen,$function)
$functionHash=(Get-FileHash -LiteralPath $frozen).Hash.ToLowerInvariant()
if($functionHash -ne 'c5cbea7a4e710d95579a8d51645d1752e6fee65a53a0dbd8ba094a60c64ee551'){throw 'Frozen discovery function hash mismatch'}
$test=Join-Path $script:RepoRoot 'tests\safety\adapter_discovery_tests.c'
$executables=@()
foreach($mode in @('default','iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=$test
    if($variant -eq 'before') {
        $source=Join-Path $objects 'entry.c'
        @(('#define PB_ADAPTER_DISCOVERY_SOURCE "'+$frozen.Replace('\','/')+'"'),
          ('#include "'+$test.Replace('\','/')+'"'))|Set-Content -LiteralPath $source -Encoding utf8
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$objects\")
    if($mode -ne 'default'){$flags+="/volatile:$mode"}
    $exe=Join-Path $objects 'discovery.exe'
    Invoke-BuildTool cl.exe ($flags+@($source)+$script:VersionArguments+@('/link',"/out:$exe",'cfgmgr32.lib'))
    if($variant -eq 'after') {
        Invoke-TestTool $exe @()
        Invoke-TestTool $exe @('1')
    } else {
        $output=@(& $exe '1' 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects 'control-1.log';$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq 95 -and ($output -join "`n").Contains('INTERFACE_LIST_GROWTH_NOT_RETRIED')
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@('1');exit_code=$code;expected_exit_code=95;expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected discovery baseline control: exit=$code"}
    }
    $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during adapter discovery audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    frozen_adapter_sha256=$baselineHash;frozen_function_sha256=$functionHash;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    method='Frozen whole adapter.c and verbatim extracted discovery function hashes checked; unchanged dependencies. Exact aggregate ledger also permits the independently audited rename extraction and Open enum failure block. Actual production header in current tests. Default/ISO/MS Win7 targets. Old CLI1 must exit 95 with INTERFACE_LIST_GROWTH_NOT_RETRIED after a valid CM size/list growth race, with heap reclaimed before exit. Current full matrix and CLI1 must exit zero. CM size/list, monotonic clock and cancellable waits are hosted; heap allocation/free, CM error mapping and CRT formatting are native. Tests cover complete MULTI_SZ data, empty lists, Unicode/large paths, changing snapshots, each allocation failure, original CM errors, capped backoff, local retry budget and earlier parent deadline/cancellation, including tick wrap. SDK calls themselves are synchronous and cannot be preempted; elapsed checks bound retry work, not an in-progress OS call. No real device/registry mutations, driver loading, old-OS or native kernel runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
