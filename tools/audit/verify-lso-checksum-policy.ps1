param([string]$BaselineDirectory='out\packet-edge-baseline\6fb17007a923',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\lso-checksum-policy-tests.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'LSO checksum policy requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='6fb17007a9234543f89dea26d3c705639e73b01c2fd2762943862149368a4826'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected accepted LSO policy baseline'}
$dependencies=@(rg --files driver common api include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate LSO policy production dependencies'}
$dependencies+=@('wintun.props','tests\model\offload_handler_tests.c','tests\safety\guard_memory.h','tools\build\offload-handler-tests.ps1')
function Assert-LcpDependencies {
    foreach($relative in $dependencies){$old=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant();if($manifest.files.($relative.Replace('\','/')) -cne $old){throw "Frozen LSO policy manifest changed:$relative"};Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative}
}
Assert-LcpDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\lso-checksum-policy\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Get-LcpPart($build,[string]$name){$found=@($build.base.extractions.parts|Where-Object {$_.name -ceq $name});if($found.Count -ne 1){throw "Nonunique LSO policy part:$name"};return $found[0]}
function Assert-LcpBuild($build){
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or $build.base.variant -cne 'current' -or $build.base.guarded -ne $build.guarded -or $build.base.mode -cne $build.mode){throw 'LSO policy build mode/family changed'}
    $e=$build.base.extractions;if(@($e.parts).Count -ne 5 -or @($e.parts.name|Select-Object -Unique).Count -ne 5){throw 'LSO policy extraction set changed'}
    $source=Join-Path $script:RepoRoot 'driver\v2\offload_probe.h';$body=[IO.File]::ReadAllText($source)
    $markers=[ordered]@{effective=@('static ULONG PbEffectiveMask(TUN_CTX *ctx)','#include "offload_format.h"');report=@('static VOID PbReportOffload(TUN_CTX *ctx)','static BOOLEAN PbUpdateParameter(');handlers=@('static BOOLEAN PbUpdateParameter(','static VOID PbSetLsoCompletion(')}
    foreach($name in $markers.Keys){$part=Get-LcpPart $build $name;$start=$body.IndexOf($markers[$name][0]);$end=$body.IndexOf($markers[$name][1],$start)
        if($start -lt 0 -or $end -le $start -or $part.source -cne $source -or $part.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or $part.offset -ne $start -or $part.length -ne $end-$start -or $part.start_marker -cne $markers[$name][0] -or $part.end_marker -cne $markers[$name][1] -or [IO.File]::ReadAllText($part.path) -cne $body.Substring($start,$end-$start) -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw "Actual LSO policy span changed:$name"}}
    $ndis=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis.h";$text=[IO.File]::ReadAllText($ndis);$type=Get-LcpPart $build 'status-type';$code=Get-LcpPart $build 'status-code'
    $start=$text.IndexOf('#define  NDIS_STATUS_INDICATION_REVISION_1');$end=$text.IndexOf('// Generic Timer support',$start);$match=[regex]::Match($text,'(?m)^#define NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG[^\r\n]*\r?\n')
    foreach($part in @($type,$code)){if($part.source -cne $ndis -or $part.source_sha256 -cne (Get-FileHash $ndis).Hash.ToLowerInvariant() -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -cne $part.sha256){throw 'WDK status source hash changed'}}
    if($start -lt 0 -or $end -le $start -or !$match.Success -or $type.offset -ne $start -or $type.length -ne $end-$start -or [IO.File]::ReadAllText($type.path) -cne $text.Substring($start,$end-$start) -or $code.offset -ne $match.Index -or $code.length -ne $match.Length -or [IO.File]::ReadAllText($code.path) -cne $match.Value){throw 'WDK status type/code spans changed'}
    $handlers=Get-LcpPart $build 'handlers'
    if([IO.File]::ReadAllText($e.selected) -cne [IO.File]::ReadAllText($handlers.path) -or (Get-FileHash $e.selected).Hash.ToLowerInvariant() -cne $e.selected_sha256 -or [IO.File]::ReadAllText($e.types) -cne ([IO.File]::ReadAllText($type.path)+[IO.File]::ReadAllText($code.path)) -or (Get-FileHash $e.types).Hash.ToLowerInvariant() -cne $e.types_sha256){throw 'Actual current handlers or WDK aggregate changed'}
    $sdkPaths=@((Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\oidrequest.h"),(Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\encapsulationconfig.h"),(Join-Path $script:SdkRoot "Include\$script:KitVersion\shared\ntddndis.h"),(Join-Path $script:SdkRoot "Include\$script:KitVersion\shared\ndis\objectheader.h"))
    if($e.kit_version -cne $script:KitVersion -or @($e.sdk_wdk_types).Count -ne 4 -or @(Compare-Object ($e.sdk_wdk_types.path|Sort-Object) ($sdkPaths|Sort-Object)).Count){throw 'Native WDK type set changed'}
    foreach($record in $e.sdk_wdk_types){if((Get-FileHash $record.path).Hash.ToLowerInvariant() -cne $record.sha256){throw 'Native WDK type input hash changed'}}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_OFFLOAD_STATUS_TYPE "'+$e.types.Replace('\','/')+'"';$lines+='#define PB_OFFLOAD_HANDLERS_SOURCE "'+$e.selected.Replace('\','/')+'"'
    foreach($name in @('effective','report')){$part=Get-LcpPart $build $name;$lines+='#define PB_OFFLOAD_'+$name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    $oldTest=Join-Path $script:RepoRoot 'tests\model\offload_handler_tests.c';$lines+='#include "'+$oldTest.Replace('\','/')+'"';$baseEntry=($lines -join [Environment]::NewLine)+[Environment]::NewLine
    if([IO.File]::ReadAllText($build.base.entry) -cne $baseEntry -or (Get-FileHash $build.base.entry).Hash.ToLowerInvariant() -cne $build.base.entry_sha256 -or (Get-FileHash $build.base.path).Hash.ToLowerInvariant() -cne $build.base.sha256){throw 'Complete base compile control binding changed'}
    $report=Get-LcpPart $build 'report';$observer='static VOID ObserveLsoReport(VOID *,NDIS_STATUS_INDICATION *);'+"`n"+'#define NdisMIndicateStatusEx ObserveLsoReport'+"`n"+'#include "'+$report.path.Replace('\','/')+'"'+"`n"+'#undef NdisMIndicateStatusEx'+"`n"
    if([IO.File]::ReadAllText($build.observer) -cne $observer -or (Get-FileHash $build.observer).Hash.ToLowerInvariant() -cne $build.observer_sha256){throw 'Actual status-buffer observer forwarding changed'}
    $test=Join-Path $script:RepoRoot 'tests\model\lso_checksum_policy_tests.c';$expected='#define PB_TEST_IP_VERSION '+$build.ip_version+"`n"+$baseEntry.Replace('#include "'+$oldTest.Replace('\','/')+'"','#include "'+$test.Replace('\','/')+'"').Replace('#define PB_OFFLOAD_REPORT_SOURCE "'+$report.path.Replace('\','/')+'"','#define PB_OFFLOAD_REPORT_SOURCE "'+$build.observer.Replace('\','/')+'"')
    if([IO.File]::ReadAllText($build.entry) -cne $expected -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256 -or $build.test -cne $test -or (Get-FileHash $test).Hash.ToLowerInvariant() -cne $build.test_sha256 -or (Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256){throw 'Complete LSO policy test/source/executable changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$($build.mode)",('/I'+(Join-Path $script:WdkRoot "Include\$script:KitVersion\km")),('/Fo'+(Split-Path $build.path)+'\'))+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.arguments -join "`n") -cne ($flags -join "`n")){throw 'Exact LSO policy shipping flags changed'}
}
function Assert-LcpRows([string]$text,[ValidateSet(4,6)][int]$family){
    $rows=[regex]::Matches($text,'(?m)^OID_LSO ip([46]) ndis(20|30|83|89) independent-checksums checked owners=0\r?$')
    if($rows.Count -ne 4 -or [regex]::Matches($text,'(?m)^OID_LSO ').Count -ne 4){throw 'LSO policy four executed OID versions missing'}
    for($i=0;$i -lt 4;++$i){if([int]$rows[$i].Groups[1].Value -ne $family -or [int]$rows[$i].Groups[2].Value -ne @(20,30,83,89)[$i]){throw 'LSO policy selected-family OID rows mismatch'}}
    $summary=[regex]::Match($text,'(?m)^LSO_POLICY ip([46]) checks(\d+) violations(\d+) handles(\d+)/(\d+) owners=0\r?$');$checks=if($family -eq 4){8212}else{1044}
    if(!$summary.Success -or [regex]::Matches($text,'(?m)^LSO_POLICY ').Count -ne 1 -or [int]$summary.Groups[1].Value -ne $family -or [int]$summary.Groups[2].Value -ne $checks -or $summary.Groups[3].Value -cne '0' -or $summary.Groups[4].Value -cne $summary.Groups[5].Value -or [regex]::Matches($text,'(?m)^PASS: LSO policy follows its own session/allowed/requested/encapsulation gates independently of TX checksum configuration\r?$').Count -ne 1 -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1 -or $text.Contains('LSO_CHECKSUM_CONFIG_DEPENDENCY')){throw 'LSO policy exact checks/violations/owner/handle summary changed'}
    return @{checks=$checks;gate_matrix_checks=$checks-20;executed_oid_versions=$rows.Count;encoded_status_assertions=5*$rows.Count;violations=0;process_handles_before=[int]$summary.Groups[4].Value;process_handles_after=[int]$summary.Groups[5].Value;test_threads=0}
}
function Invoke-LcpProcess($build,[string]$work){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start LSO policy regression'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'LSO policy30s watchdog'}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $work 'results.log';[IO.File]::WriteAllText($log,$text,$encoding);$metrics=$null;if(!$code){$metrics=Assert-LcpRows $text $build.ip_version}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;exit_code=$code;expected_exit_code=0;log=$log;watchdog_ms=30000;metrics=$metrics});Write-TestRecording $(if(!$code -and $metrics){'running'}else{'failed'});if($code -or !$metrics){throw "LSO policy process failed:$code"};Write-Output $text.TrimEnd();return @{log=$log;metrics=$metrics}
    }finally{$process.Dispose()}
}
function Test-LcpCompileContracts($build){
    $work=Join-Path $directory 'compile-contracts';New-Item -ItemType Directory -Path $work -Force|Out-Null;$results=@()
    $cases=@(@{name='no-ndebug';marker='C1189.*LSO checksum policy requires NDEBUG configuration'},@{name='pb-api-tests';marker='C1189.*LSO checksum policy requires actual production paths'},@{name='invalid-family';marker='C1189.*Select independent IPv4 or IPv6'},@{name='no-fingerprint';marker='C2338.*offload handler requires shipping source fingerprint'})
    foreach($case in $cases){$flags=@($build.arguments|Where-Object {$_ -notlike '/Fo*'});$entry=$build.entry
        if($case.name -ceq 'no-ndebug'){$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'})}
        elseif($case.name -ceq 'pb-api-tests'){$flags+='/DPB_API_TESTS=1'}
        elseif($case.name -ceq 'no-fingerprint'){$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})}
        else{$body=[IO.File]::ReadAllText($entry);$before='#define PB_TEST_IP_VERSION '+$build.ip_version;if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'Invalid-family negative source marker not unique'};$entry=Join-Path $work 'invalid-family.c';[IO.File]::WriteAllText($entry,$body.Replace($before,'#define PB_TEST_IP_VERSION 7'),$encoding)}
        $object=Join-Path $work ($case.name+'.obj');$log=Join-Path $work ($case.name+'.log');$flags+=('/Fo'+$object);& cl.exe @flags /c $entry *> $log;$code=$LASTEXITCODE;$output=[IO.File]::ReadAllText($log)
        if(!$code -or $output -notmatch $case.marker){throw "LSO policy compiler accepted/missed precise guard:$($case.name)"};$results+=@{name=$case.name;exit_code=$code;expected_rejection=$true;required_error=$case.marker;log=$log;source=$entry;source_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();arguments=$flags}
    }
    $global:LASTEXITCODE=0;return $results
}
Start-TestRecording $directory 'operation-results.json'
$ledger=Test-LsoChecksumLedger -Original ([IO.File]::ReadAllText((Join-Path $baseline 'common\offload_policy.c'))) -Repaired ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'common\offload_policy.c'))) -Directory (Join-Path $directory 'ledger-contracts')
if($ledger.checks.Count -ne 16 -or $ledger.files.Count -ne 6){throw 'LSO ledger contract set changed'}
$executables=@();$observations=@();$compilerContracts=@()
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "ipv$family\$mode-$memory";$build=Build-LsoChecksumPolicyTests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-LcpDependencies;Assert-LcpBuild $build;$record=Invoke-LcpProcess $build $work;$observations+=@{ip_version=$family;mode=$mode;guarded=$guarded;log=$record.log;metrics=$record.metrics};$executables+=,$build
    if($family -eq 4 -and $mode -ceq 'iso' -and !$guarded){$compilerContracts=@(Test-LcpCompileContracts $build)}
}}}
if($compilerContracts.Count -ne 4){throw 'LSO policy compiler contract matrix incomplete'}
$wrongFamilyLog=($observations|Where-Object {$_.ip_version -eq 4 -and $_.mode -ceq 'iso' -and !$_.guarded}).log;$wrongFamilyRejected=$false
try{$null=Assert-LcpRows ([IO.File]::ReadAllText($wrongFamilyLog)) 6}catch{if($_.ToString() -notmatch 'LSO policy selected-family OID rows mismatch'){throw};$wrongFamilyRejected=$true}
if(!$wrongFamilyRejected){throw 'LSO policy audit accepted the other compiled family'}
Assert-LcpDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Source changed during LSO policy audit'}
Write-TestRecording 'passed'
$ledgerPath=Join-Path $script:RepoRoot 'tools\audit\lso-checksum-ledger.ps1'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline_source_tree_sha256=$accepted;baseline=$baseline;production_changed=$true;permitted_production_change='common/offload_policy.c exact reviewed two-branch repair; driver/v2/session_describe.h exact reviewed checksum-family selection repair';kernel_runtime_tested=$false;production_ready=$false;unchanged_dependencies=$dependencies.Count-2;operations=$script:TestResultFile;executables=$executables;observations=$observations;compiler_contracts=$compilerContracts;
    parser_contracts=@{wrong_family_rejected=$wrongFamilyRejected;source_log=$wrongFamilyLog;encoded_marker_negative='not applicable: encoded fields are asserted against actual copied StatusBuffer in C; no encoded marker is printed'};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    ledger=@{path=$ledgerPath;sha256=(Get-FileHash $ledgerPath).Hash.ToLowerInvariant();contracts=$ledger};audit_sources=@('tests\model\lso_checksum_policy_tests.c','tools\build\lso-checksum-policy-tests.ps1','tools\audit\verify-lso-checksum-policy.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Eight separate IPv4/IPv6 ISO/MS ordinary/guarded current processes execute actual effective-mask, OID/POLICY handlers, report, indication gate and native SDK/WDK formats. Every original function span/status-type/status-code/SDK header, selected unmutated handler, aggregate, complete original entry and new two-include binding is independently reconstructed with offsets/hashes; observer only forwards the actual Report NdisMIndicateStatusEx to a private copy before the old hosted DDI validation. New C EncodedLso checks real copied LsoV2 MaxOffLoadSize/MinSegmentCount/Encapsulation and IPv6 extension/TCP-option fields, with TX checksum fields remaining disabled; no LastReport-only circular proof. Twenty encoded-status assertions per process are enforced at five actual reports for each executed6.20/6.30/6.83/6.89 OID version, each row prints only after context/events are reclaimed. Ordinary checksum disable must preserve requested/effective/reportedLSO; explicit disable/re-enable and own POLICY allowed/zero gates remain effective. Pure own-gate matrix checks active/allowed/requested/encapsulation and independent checksum subsets,8192checks forIPv4 or1024 forIPv6 plus20actualOIDchecks, zero violations and strict handles/owned memory. RX coalescing validation dependencies remain checked. Exactly the reviewed common policy and checksum-family Describe repairs may differ from accepted6fb, each admitted only by its strict complete-file text/raw ledger; other driver/API/common/include production and frozen host/build/guard/version inputs are byte-identical. Old140 evidence and actual32NBL139 safe negative are retained separately, not replayed as native kernel requests here. Base executable compiles as a control only, not an executed case. Four permanent compiler negatives require precise NDEBUG/PB_API/fingerprint/family guard errors; wrong-family parsed log must reject. Actual StatusBuffer bytes are asserted in C without a printed encoded marker, so no fictitious missing-marker test. Guard mode protects original exact-sized context/parameter allocations; hosted locks/events and modeledIRQL are not nativeNDIS/DDI/driver/device/network/registry/Win7 execution orCPU/timing evidence. NDEBUG/nonPB/FP65/version2.0.0 remain required, originalNT_ASSERT=CHECK stays stronger, no test threads/asynchronous callers are created and all resources close before success.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
