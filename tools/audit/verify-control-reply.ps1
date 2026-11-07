param([string]$BaselineDirectory='out\retirement-retry-baseline\03bf04fcdbb3',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\control-reply-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Control reply audit requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate kernel dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\kernel_nbl_fixture.h','tests\safety\guard_memory.h','tests\model\indication_pause_tests.c','tests\model\send_completion_tests.c',
    'tools\build\indication-pause-tests.ps1','tools\build\send-completion-tests.ps1')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\control-reply\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-ControlReplyProjection($build) {
    $e=$build.extractions;$b=$e.base;$n=$b.base
    foreach($part in @($e.parts)+@($b.parts)+@($n.parts)){
        $original=[IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length)
        if([IO.File]::ReadAllText($part.path) -cne $original -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -ne $part.sha256 -or
            (Get-FileHash $part.source).Hash.ToLowerInvariant() -ne $part.source_sha256){throw "Actual source extraction/hash changed:$($part.name)"}
    }
    if($n.host_source_sha256 -ne $n.host_original_sha256 -or $b.reused_source_sha256 -ne $b.reused_original_sha256){throw 'Prior original host snapshots changed'}
    if((Get-FileHash $n.host_original).Hash.ToLowerInvariant() -ne $n.host_source_sha256 -or (Get-FileHash $b.reused_original).Hash.ToLowerInvariant() -ne $b.reused_source_sha256){throw 'Prior snapshot bytes changed'}
    $expectedNbl=[IO.File]::ReadAllText($n.host_original).Replace($n.field_marker,$n.field_marker+"`r`n"+$n.field_inserted)
    foreach($change in $n.include_changes){$expectedNbl=$expectedNbl.Replace($change.original,$change.adapted)}
    if($expectedNbl -cne [IO.File]::ReadAllText($n.host)){throw 'Prior bank-field/include adaptation changed'}
    if($expectedNbl.Replace($b.device_original,$b.device_adapted) -cne [IO.File]::ReadAllText($b.host)){throw 'Prior Device owner-field adaptation changed'}
    $prior=[IO.File]::ReadAllText($b.reused_original)
    foreach($change in @($b.include_changes)+@($b.observer_changes)){$prior=$prior.Replace($change.original,$change.adapted)}
    if($prior -cne [IO.File]::ReadAllText($b.reused)){throw 'Prior indication test adaptation changed'}
    $bankBefore='CHECK(!Session->Active&&Session->Returns.References==1&&SetEvent(BankWait));';$bankAfter='CHECK(SetEvent(BankWait));'
    if($e.bank_original -cne $bankBefore -or $e.bank_adapted -cne $bankAfter -or [regex]::Matches($prior,[regex]::Escape($bankBefore)).Count -ne 1 -or
        $prior.Replace($bankBefore,$bankAfter) -cne [IO.File]::ReadAllText($e.indication_host)){throw 'Bank observer changed beyond old receive-specific ownership assertion'}
    $priorCleanup=$b.parts|Where-Object name -eq 'cleanup';$priorPause=$n.parts|Where-Object name -eq 'pause'
    if($b.variant -ne 'current' -or $n.variant -ne 'current' -or [IO.File]::ReadAllText($b.cleanup) -cne [IO.File]::ReadAllText($priorCleanup.path) -or
        [IO.File]::ReadAllText($n.pause) -cne [IO.File]::ReadAllText($priorPause.path)){throw 'Actual full Free/Pause barriers changed'}
    $hostSource=Join-Path $script:RepoRoot 'tests\model\send_completion_tests.c'
    $expectedHash='cb019555263616c217a0f84d415808af30004266ef1fe3dfb1d28c82b4dc2938'
    if($e.host_source -cne $hostSource -or $e.host_source_sha256 -ne $expectedHash -or $e.host_original_sha256 -ne $expectedHash -or
        (Get-FileHash $e.host_original).Hash.ToLowerInvariant() -ne $expectedHash -or (Get-FileHash (Join-Path $baseline 'tests\model\send_completion_tests.c')).Hash.ToLowerInvariant() -ne $expectedHash){throw 'Frozen completion host mismatch'}
    $body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\send_completion_tests.c'))
    $mainBefore='int main(int argc,char **argv)';$mainAfter='int ReusedCompletionMain(int argc,char **argv)'
    if($e.main_original -cne $mainBefore -or $e.main_adapted -cne $mainAfter -or [regex]::Matches($body,[regex]::Escape($mainBefore)).Count -ne 1){throw 'Unused main rename changed'}
    $body=$body.Replace($mainBefore,$mainAfter);$changes=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{
        param($match);$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $match.Groups[1].Value))
        $adapted='#include "'+$path.Replace('\','/')+'"';$changes.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    if($body -cne [IO.File]::ReadAllText($e.host) -or ($changes|ConvertTo-Json -Depth 4 -Compress) -cne ($e.include_changes|ConvertTo-Json -Depth 4 -Compress)){throw 'Completion host changed beyond unused main and absolute includes'}
    $replyPart=$e.parts|Where-Object name -eq 'reply';$source=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session.h'))
    $startMarker='static NDIS_STATUS PbV2ControlReply(';$endMarker='#include "session_describe.h"';$start=$source.IndexOf($startMarker);$end=$source.IndexOf($endMarker,$start)
    if($replyPart.start_marker -cne $startMarker -or $replyPart.end_marker -cne $endMarker -or $replyPart.offset -ne $start -or $replyPart.length -ne $end-$start){throw 'Actual ControlReply source boundary changed'}
    $original=$source.Substring($start,$end-$start)
    $before='    IoFreeMdl(mdl); ExFreePoolWithTag(data, TUN_MEMORY_TAG);';$after='    if (nbl) IoFreeMdl(mdl); ExFreePoolWithTag(data, TUN_MEMORY_TAG);'
    if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'Frozen reply MDL cleanup not unique'}
    if($build.variant -eq 'without-failed-nbl-mdl-release'){
        if($e.mutation.original -cne $before -or $e.mutation.changed -cne $after -or $e.mutation.offset -ne $original.IndexOf($before) -or $e.mutation.length -ne $before.Length){throw 'MDL release mutation ledger changed'}
        $original=$original.Replace($before,$after)
    }elseif($null -ne $e.mutation){throw 'Current reply unexpectedly mutated'}
    if($original -cne [IO.File]::ReadAllText($e.reply)){throw 'ControlReply changed beyond the unique failed-NBL MDL release guard'}
    $declarations=@'
static BOOLEAN ActualControlRead(NET_BUFFER *,ULONG,UCHAR *,ULONG);
static NDIS_STATUS ActualControlReply(TUN_CTX *,const UCHAR *,ULONG);
static VOID ActualControlComplete(NDIS_HANDLE,NET_BUFFER_LIST *);
static VOID ControlReject(TUN_CTX *,uint64_t,NET_BUFFER_LIST *,NET_BUFFER *,NDIS_STATUS,ULONG,ULONG,ULONG,const PB_SEND_FAILURE *,ULONG,ULONG,ULONG,PB_KERNEL_QUEUE *,ULONG);
#define PbV2Read ActualControlRead
#define PbV2ControlReply ActualControlReply
#define PbCompleteSend ActualControlComplete
#define PbV2RejectSend ControlReject
'@
    $suffix=@'
#undef PbV2Read
#undef PbV2ControlReply
#undef PbCompleteSend
#undef PbV2RejectSend
'@
    $send=$declarations+"`r`n#include `""+(Join-Path $script:RepoRoot 'driver\v2\session_send.h').Replace('\','/')+"`"`r`n"+$suffix
    if($send -cne [IO.File]::ReadAllText($e.send_wrapper)){throw 'Selected Send changed beyond typed declarations and four macro bindings'}
    $callouts=$e.parts|Where-Object name -eq 'callouts';$lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($build.variant -ne 'current'){$lines+='#define PB_CONTROL_WITHOUT_FAILED_MDL 1'}
    $lines+='#define PB_CONTROL_REUSED_HOST "'+$e.host.Replace('\','/')+'"'
    $lines+='#define PB_CONTROL_REPLY_SOURCE "'+$e.reply.Replace('\','/')+'"'
    $lines+='#define PB_CONTROL_CALLOUTS_SOURCE "'+$callouts.path.Replace('\','/')+'"'
    $lines+='#define PB_SEND_COMPLETION_HOST "'+$e.indication_host.Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_NBL_HOST "'+$b.host.Replace('\','/')+'"'
    $lines+='#define PB_REFERENCE_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_references.h').Replace('\','/')+'"'
    $lines+='#define PB_NBL_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h').Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_PAUSE_SOURCE "'+$n.pause.Replace('\','/')+'"'
    foreach($part in $n.parts|Where-Object name -ne 'pause'){$lines+='#define PB_INDICATION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    foreach($part in $b.parts|Where-Object name -ne 'cleanup'){$path=if($part.name -eq 'send'){$e.send_wrapper}else{$part.path};$lines+='#define PB_SEND_COMPLETION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\control_reply_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`r`n")+"`r`n")){throw 'Final compilation entry changed'}
    foreach($item in $build.sources){if((Get-FileHash -LiteralPath $item.path).Hash.ToLowerInvariant() -ne $item.sha256){throw "Recorded source/artifact hash changed:$($item.path)"}}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-ControlReplyProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;foreach($argument in $Arguments){$info.ArgumentList.Add($argument)}
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start control reply test'}
        $stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Control reply external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-control-reply.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) actual ControlReply/Send/RESOURCES/Pause/commonFILEUnregister/Free cases plus (\d+) checked warmup; explicit runtime NBL gate, actual packet/parser/read/callout paths, complete thread joins and zero owned resources; handles (\d+)/(\d+); repaired=(\d+)/(\d+)')
        $full=$Arguments.Count -eq 0;$cases=if($full){64}else{1};$warm=if($full){64}else{2};$repairs=if($Expected -eq 127){1}else{0};$warmRepairs=if($Expected -eq 127){2}else{0}
        $marker='CONTROL_REPLY_FAILED_NBL_MDL_LEAK: payload release parked with owned MDL and no constructed NBL or NDIS owner; actual IoFreeMdl compensation once per negative case, then complete normal joins and reclamation before127'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 127 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $warm -and
            $summary.Groups[3].Value -eq $summary.Groups[4].Value -and [int]$summary.Groups[5].Value -eq $repairs -and [int]$summary.Groups[6].Value -eq $warmRepairs}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 127 -and $correct);
            watchdog_ms=30000;log=$log;measured_schedules=$cases;checked_warmup_schedules=$warm;schedules=$cases+$warm;negative_cleanup_compensations=$repairs+$warmRepairs;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Control reply test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-failed-nbl-mdl-release')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $built=Build-ControlReplyTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-ControlReplyProjection $built
    if($variant -eq 'current'){Invoke-ControlReplyProcess $built.path @();Invoke-ControlReplyProcess $built.path @('127')}
    else{Invoke-ControlReplyProcess $built.path @('127') 127}
    $executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during control reply audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');
    unchanged_dependencies=($dependencies.Count-3);
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\control_reply_tests.c','tools\build\control-reply-tests.ps1','tools\audit\verify-control-reply.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only actual SYS combination with hash-frozen original NBL/indication/send fixtures. Current ControlReply is extracted verbatim per build with full callout/header, Send, TunPause, bank, FILE unregister and Free production dependencies. Reuse metadata records all original bytes/hashes, exact prior finite Device/bank field and boundary-observer adaptations, absolute includes and an unused main rename. The sole new inherited-host assertion change removes its receive-specific Active0/Returns1 check at the bank wait; this test independently requires Send Active1/Returns2 there. Typed declarations and macro names bind exact Send to actual MDL Read, ControlReply, exact NDIS callout wrapper and a diagnostic-only rejection observer. Actual common Ethernet parser builds expected ARP/NS replies from valid input frames; source reads execute actual Read and hosted MDL map. Current successful temporary reply acquisition owns data/MDL/NBL through a RESOURCES receive upcall, which does not generate a return callback. Actual Pause waits the bank while common FILE unregister detaches and full Free waits Active. After the upcall returns, actual ControlReply releases its temporary NBL/MDL/data before actual send completion; completion releases original guarded or ordinary NBL/NB/MDL/payload and remains gated while Active1 still blocks Pause/Free. After completion exits Active0 permits Pause to join while the Send Returns tail separately blocks Free. Only then does actual unregister/free finish. Exactly two session rundown acquisitions per case are Send and Pause; no ordinary receive/return or ring object is manufactured. Three actual allocation failure positions independently cover data, MDL and NBL acquisition rollback with original RESOURCES status and CONTROL_REPLY rejection reason. Four explicit runtime versions6.20/6.30/6.83/6.89 cross both packet families, three failures/success and auto/manual events for64 cases. PB_NBL_VERSION is explicitly runtime-bound before any platform include, PB_NBL_HOST_CURRENT is forbidden, unavailable field lvalue access is rejected and unavailable metadata retains poison. This is a full-size modeled NBL, not an old kernel structure-layout proof. Full processes run64 checked warm then64 measured cases; selected CLI127 runs2 checked warm plus1 measured, all with strict process-handle equality. Negative changes only the final MDL release to run when nbl is nonnull; a failed NBL allocation then leaves a live MDL at the subsequent temporary payload release. A boundary wrapper parks before that physical payload free, proving no constructed NBL/NDIS owner exists and Send lifetime still owns the transaction. The controller invokes the same tracked IoFreeMdl boundary exactly once while payload remains live, then releases the parked original data cleanup. All normal send/pause/unregister/worker joins and complete resource reclamation precede127. Current cases never compensate. No reference, credit, generation or other production counter is repaired. The ready factory models registration; worker OS threads are idle-only start/stop hosts, NDIS/DDIs and IRQL are modeled, and real Windows locks/events/joins execute. No driver/device/network/registry, native Win7/NDIS/IRQL/Verifier or actual kernel CPU measurement is claimed. Shared region remains separately owned and native Free only unlocks its MDL. PB_API_TESTS is rejected and the65-byte shipping source fingerprint is required. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 20|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
