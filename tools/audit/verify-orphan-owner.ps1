param([string]$BaselineDirectory='out\orphan-owner-baseline\a7d4ae839494')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'adapter-open-ledger.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Owner audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
foreach($entry in @(@('api\adapter.c','6c19979fdb2a983c011f25813d3ecee3eb4991531a284ee0b01972b4e821d109'),
    @('api\adapter_win7.h','21db2fe610d44743a9fd59b167dedc61f43473efdda5fc897991b3cff89c9a71'))) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $entry[0])).Hash.ToLowerInvariant() -ne $entry[1]){throw "Unexpected frozen source: $($entry[0])"}
}
function Read-Normalized([string]$Path){[IO.File]::ReadAllText($Path).Replace("`r`n","`n")}
$oldWin=Read-Normalized (Join-Path $baseline 'api\adapter_win7.h')
$oldAdapter=Read-Normalized (Join-Path $baseline 'api\adapter.c')
$defStart=$oldWin.IndexOf('static const DEVPROPKEY');$defEnd=$oldWin.IndexOf('_Must_inspect_result_')
$ownerStart=$oldWin.LastIndexOf('static BOOL');$modernStart=$oldAdapter.IndexOf('VOID AdapterCleanupOrphanedDevices(VOID)')
$modernEnd=$oldAdapter.IndexOf('_Use_decl_annotations_',$modernStart)
if($defStart -lt 0 -or $defEnd -le $defStart -or $ownerStart -le $defEnd -or $modernStart -lt 0 -or $modernEnd -le $modernStart){throw 'Cannot isolate cleanup functions'}
# Preserve the original whole-file checks and account for later, independently
# audited extractions. Only these four exact regions may leave the aggregates;
# the owner/cleanup bodies below still execute in both frozen/current tests.
$expectedWin=$oldWin.Substring(0,$defStart)+"#include `"orphan_owner.h`"`n`n"+$oldWin.Substring($defEnd,$ownerStart-$defEnd)
$expectedWin=$expectedWin.TrimEnd()+"`n"
$waitStart=$expectedWin.IndexOf('_Must_inspect_result_')
$waitEnd=$expectedWin.IndexOf('_Must_inspect_result_',$waitStart+1)
if($waitStart -lt 0 -or $waitEnd -le $waitStart -or !$expectedWin.Substring($waitStart,$waitEnd-$waitStart).Contains('WaitForInterfaceWin7(')){throw 'Cannot isolate independent Win7 wait helper'}
$expectedWin=$expectedWin.Substring(0,$waitStart).TrimEnd()+"`n#include `"adapter_wait_win7.h`"`n`n"+$expectedWin.Substring($waitEnd)
$helperFailure="LastError = LOG_LAST_ERROR(L`"Failed to create device instance`");`n            goto cleanup;"
if(([regex]::Matches($expectedWin,[regex]::Escape($helperFailure))).Count -ne 1){throw 'Cannot isolate independent Win7 helper failure cleanup'}
# verify-win7-create.ps1 separately reproduces the missing empty-set release.
# Permit only that cleanup label change; all other creation bytes still match.
$expectedWin=$expectedWin.Replace($helperFailure,$helperFailure.Replace('goto cleanup;','goto cleanupDevInfo;'))
if($expectedWin -cne (Read-Normalized (Join-Path $script:RepoRoot 'api\adapter_win7.h'))){throw 'Unexpected changes outside Win7 cleanup extraction'}
$expectedAdapter=$oldAdapter.Substring(0,$modernStart)+$oldAdapter.Substring($modernEnd)
$expectedAdapter=$expectedAdapter.Replace('#include "cleanup.h"',"#include `"cleanup.h`"`n#include `"orphan_cleanup.h`"").Replace('AdapterCleanupLegacyDevices(); LegacyDone = TRUE;','LegacyDone = AdapterCleanupLegacyDevices();')
$dataStart=$expectedAdapter.IndexOf('_Must_inspect_result_');$dataEnd=$expectedAdapter.IndexOf('#include "cleanup.h"',$dataStart)
if($dataStart -lt 0 -or $dataEnd -le $dataStart -or !$expectedAdapter.Substring($dataStart,$dataEnd-$dataStart).Contains('PopulateAdapterData(')){throw 'Cannot isolate independent adapter metadata helper'}
$expectedAdapter=$expectedAdapter.Substring(0,$dataStart)+"#include `"adapter_data.h`"`n`n"+$expectedAdapter.Substring($dataEnd)
$renameStart=$expectedAdapter.IndexOf("static _Return_type_success_(return != FALSE)`nBOOL`nRenameByNetGUID(")
$renameEnd=$expectedAdapter.IndexOf("_Use_decl_annotations_`nVOID WINAPI`nWintunGetAdapterLUID(",$renameStart)
if($renameStart -lt 0 -or $renameEnd -le $renameStart){throw 'Cannot isolate independent adapter rename helpers'}
$expectedAdapter=$expectedAdapter.Substring(0,$renameStart)+"#include `"adapter_rename.h`"`n`n"+$expectedAdapter.Substring($renameEnd)
$discoverySignature=$expectedAdapter.IndexOf('AdapterGetDeviceObjectFileName(LPCWSTR InstanceId)')
$discoveryStart=$expectedAdapter.LastIndexOf('_Use_decl_annotations_',$discoverySignature)
$discoveryEnd=$expectedAdapter.IndexOf('typedef struct _WAIT_FOR_INTERFACE_CTX',$discoverySignature)
if($discoverySignature -lt 0 -or $discoveryStart -lt 0 -or $discoveryEnd -le $discoveryStart){throw 'Cannot isolate independent adapter discovery helper'}
$expectedAdapter=$expectedAdapter.Substring(0,$discoveryStart)+"#include `"adapter_discovery.h`"`n`n"+$expectedAdapter.Substring($discoveryEnd)
$expectedAdapter=Update-AdapterOpenFailureLedger $expectedAdapter
if($expectedAdapter -cne (Read-Normalized (Join-Path $script:RepoRoot 'api\adapter.c'))){throw 'Unexpected changes outside cleanup routing/retry and reviewed Open enum failure'}
$owner=Read-Normalized (Join-Path $script:RepoRoot 'api\orphan_owner.h')
$newDef=$owner.Substring($owner.IndexOf('static const DEVPROPKEY'),$owner.IndexOf('static BOOL')-$owner.IndexOf('static const DEVPROPKEY'))
if($newDef -cne $oldWin.Substring($defStart,$defEnd-$defStart)){throw 'Owner property key/layout declarations changed'}
$directory=Join-Path $script:RepoRoot "out\orphan-owner-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json';$executables=@()
$oldHeader=Join-Path $directory 'cleanup-before.h'
[IO.File]::WriteAllText($oldHeader,$oldWin.Substring($defStart,$defEnd-$defStart)+$oldWin.Substring($ownerStart)+$oldAdapter.Substring($modernStart,$modernEnd-$modernStart))
$before=Join-Path $directory 'before.c'
@('#define PB_ORPHAN_BASELINE 1',
  ('#define PB_ORPHAN_ADAPTER_HEADER_SOURCE "'+(Join-Path $baseline 'api\adapter.h').Replace('\','/')+'"'),
  ('#define PB_ORPHAN_CLEANUP_SOURCE "'+$oldHeader.Replace('\','/')+'"'),
  ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\orphan_owner_tests.c').Replace('\','/')+'"'))|Set-Content -LiteralPath $before -Encoding utf8
$controls=@(@(1,53,'LIVE_OWNER_RECLAIMED_WITHOUT_PROOF'),@(2,54,'EXITED_OWNER_RETAINED_WITH_PROCESS_HANDLE'),
    @(3,53,'LIVE_OWNER_RECLAIMED_WITHOUT_PROOF'),@(4,56,'ENUM_FAILURE_RETRIED_WITHOUT_PROGRESS'),
    @(5,57,'UNTERMINATED_DEVICE_NAME_LOGGED'),@(6,58,'DESTRUCTIVE_CLEANUP_AFTER_CANCELLATION'),@(7,59,'UNKNOWN_DEVNODE_RECLAIMED'))
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
        $source=if($variant -eq 'before'){$before}else{'tests\safety\orphan_owner_tests.c'}
        $exe=Join-Path $objects 'owner.exe';Invoke-BuildTool cl.exe ($flags+@($source,'/link',"/out:$exe"))
        if($variant -eq 'after'){Invoke-TestTool $exe @()}
        else {foreach($case in $controls) {
            $argument=[string]$case[0];$output=@(& $exe $argument 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
            $log=Join-Path $objects ("control-"+$argument+'.log');$output|Set-Content -LiteralPath $log -Encoding utf8
            $expected=$code -eq $case[1] -and ($output -join "`n").Contains($case[2])
            $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@($argument);exit_code=$code;expected_exit_code=$case[1];expected_baseline_failure=$expected;log=$log})
            Write-TestRecording $(if($expected){'running'}else{'failed'})
            if(!$expected){throw "Unexpected baseline result: $($case[2]), exit=$code"}
        }}
        $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during owner audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;kernel_runtime_tested=$false;production_ready=$false;
    method='Actual extracted process identity and all three orphan cleanup paths, Windows 7 API target. Frozen whole-file hashes and exact aggregate ledger including independently audited Win7 wait, metadata, rename and discovery extractions and the single helper-failure empty-set cleanup label and Open enum failure repair; no broad aggregate guard bypass. Actual hidden child processes, retained exited process objects including exit code 259, identity mismatch simulation, real nonblocking process waits and query-handle closure. Hosted SetupAPI only; every remove is an observer. Owner/property/name/multisz/status/enum errors, genuine absent metadata, per-record continuation, failed legacy sweep eligibility, guard-page input/output checks. Actual cleanup threads paused in property reads, real stop event before resume, no delete after that cancellation. Exact seven baseline controls 53/54/56/57/58/59, baseline children signalled and joined before exit. ISO/MS volatile, repeated complete matrices and no process-handle growth after warmup. No real devices changed, SYS loaded, or old OS VM.';
    operations=$script:TestResultFile;executables=$executables}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
