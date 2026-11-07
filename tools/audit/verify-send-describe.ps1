param([string]$BaselineDirectory='out\lifetime-edge-baseline\9dc04c7344b3',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\send-describe-tests.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Send Describe requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h',
    'tests\model\send_window_tests.c','tests\model\send_prefix_cases.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$ledgerContracts=@(Test-SendDiagnosticLedger -Original ([IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session_send.h'))) -Repaired ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_send.h'))))
if($ledgerContracts.Count -ne 9){throw 'Send diagnostic ledger contract count changed'}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\send-describe\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
$fileLedger=Test-SendDiagnosticFileLedger -Directory (Join-Path $directory 'ledger-contracts') -Original ([IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session_send.h'))) -Repaired ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_send.h')))
if(@($fileLedger.checks).Count -ne 7 -or @($fileLedger.files).Count -ne 6){throw 'Send diagnostic byte ledger contract count changed'}
function Assert-SendDescribeProjection($build){
    $e=$build.extractions
    if(@($e.parts).Count -ne 6 -or @($e.parts.name|Select-Object -Unique).Count -ne 6){throw 'Unexpected actual part set'}
    foreach($part in $e.parts){
        if([IO.File]::ReadAllText($part.path) -cne [IO.File]::ReadAllText($part.source).Substring($part.offset,$part.length) -or
            (Get-FileHash $part.path).Hash.ToLowerInvariant() -ne $part.sha256 -or (Get-FileHash $part.source).Hash.ToLowerInvariant() -ne $part.source_sha256){throw "Source span changed:$($part.name)"}
    }
    foreach($name in @('read','describe','send')){
        $part=$e.parts|Where-Object name -eq $name;$source=Join-Path $script:RepoRoot "driver\v2\session_$name.h"
        if($part.source -cne $source -or $part.offset -ne 0 -or $part.length -ne [IO.File]::ReadAllText($source).Length){throw 'Complete production header not selected'}
    }
    $types=@();foreach($name in @('nblchecksum','nbllso','nbluso')){
        $part=$e.parts|Where-Object name -eq $name;$source=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\$name.h"
        $text=[IO.File]::ReadAllText($source);$m=[regex]::Match($text,'(?s)typedef struct _NDIS_.*?\n\} NDIS_[^;]+;')
        if(!$m.Success -or $part.source -cne $source -or $part.offset -ne $m.Index -or $part.length -ne $m.Length -or [IO.File]::ReadAllText($part.path) -cne $m.Value){throw 'WDK concrete metadata type changed'}
        $types+=,$m.Value
    }
    if([IO.File]::ReadAllText($e.types) -cne ($types -join "`n") -or (Get-FileHash $e.types).Hash.ToLowerInvariant() -ne $e.types_sha256){throw 'Concrete metadata aggregate changed'}
    $send=$e.parts|Where-Object name -eq 'send';$selected=[IO.File]::ReadAllText($send.path)
    $before='                } else (VOID)PbPoolTransition(&q->TxPool, &d.Token, PB_SLOT_RESERVED, PB_SLOT_FREE);'
    $after='                } else { /* Observer134: omit failed Describe reservation release. */ }'
    if([regex]::Matches($selected,[regex]::Escape($before)).Count -ne 1){throw 'Failed Describe rollback not unique'}
    if($build.variant -eq 'without-describe-rollback'){
        if($e.mutation.original -cne $before -or $e.mutation.changed -cne $after -or $e.mutation.offset -ne $selected.IndexOf($before) -or $e.mutation.length -ne $before.Length){throw 'Rollback-only mutation metadata changed'}
        $selected=$selected.Replace($before,$after)
    }elseif($null -ne $e.mutation){throw 'Unexpected rollback mutation'}
    if($build.variant -eq 'before-current-nb-attribution'){
        $historicalPath=Join-Path $baseline 'driver\v2\session_send.h'
        if($e.historical.path -cne $historicalPath -or $e.historical.sha256 -ne '9ac96a6fbf63c04b0f1cbaf82ee457de4a7dc8644809a9b71dcb216bfd0ac225'){throw 'Historical Send binding changed'}
        $selected=[IO.File]::ReadAllText($historicalPath)
        $null=Update-SendDiagnosticLedger -Body $selected -CurrentBody ([IO.File]::ReadAllText($send.path))
    }elseif($null -ne $e.historical){throw 'Current selected a historical header'}
    if([IO.File]::ReadAllText($e.selected) -cne $selected -or (Get-FileHash $e.selected).Hash.ToLowerInvariant() -ne $e.selected_sha256){throw 'Send changed beyond rollback omission'}
    $hostSource=Join-Path $script:RepoRoot 'tests\model\send_window_tests.c';$hostHash='a6df5a009da56d1617af06a4f8904055c19fc9c06b78301e680d25466bd1dcfe'
    if($e.host_source -cne $hostSource -or $e.host_original_sha256 -ne $hostHash -or (Get-FileHash (Join-Path $baseline 'tests\model\send_window_tests.c')).Hash.ToLowerInvariant() -ne $hostHash){throw 'Frozen send host hash changed'}
    $body=[IO.File]::ReadAllText((Join-Path $baseline 'tests\model\send_window_tests.c'))
    $first='static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)';$last='#ifndef PB_SEND_DIAGNOSTICS_SOURCE'
    $start=$body.IndexOf($first);$end=$body.IndexOf($last,$start)
    if($e.standin_start -cne $first -or $e.standin_end -cne $last -or $e.standin_offset -ne $start -or $e.standin_length -ne $end-$start -or
        [IO.File]::ReadAllText($e.standin) -cne $body.Substring($start,$end-$start) -or (Get-FileHash $e.standin).Hash.ToLowerInvariant() -ne $e.standin_sha256){throw 'Original Describe stand-in changed'}
    $replacement=@'
#define PbV2Describe ActualDescribe
#include PB_SEND_DESCRIBE_ACTUAL
#undef PbV2Describe
static NDIS_STATUS PbV2Describe(TUN_CTX *ctx,NET_BUFFER_LIST *nbl,NET_BUFFER *nb,ULONG offset,PB_DESCRIPTOR *d,UCHAR *bytes,PB_SEND_FAILURE *failure)
{
    Describes++;ReadingPayload=TRUE;ULONG64 before=PayloadBytes;
    NDIS_STATUS status=ActualDescribe(ctx,nbl,nb,offset,d,bytes,failure);ReadingPayload=FALSE;
    if(status==NDIS_STATUS_RESOURCES){FailedBytes=(ULONG)(PayloadBytes-before);FailedOffset=d->DataOffset;FailedToken=d->Token;}
    return status;
}
'@
    $body=$body.Substring(0,$start)+$replacement+"`n"+$body.Substring($end)
    $infoBefore='#define NET_BUFFER_LIST_INFO(n,k) ((n)->Info[k])';$infoAfter=@'
static VOID **ObserveInfo(NET_BUFFER_LIST *nbl,ULONG index){CHECK(index<4 && (index!=1 || HostedNdisVersion>=0x60053));if(index==1)FutureInfoReads++;return nbl->Info+index;}
#define NET_BUFFER_LIST_INFO(n,k) (*ObserveInfo(n,k))
#define PB_NBL_VERSION HostedNdisVersion
'@
    $transforms=@(@($infoBefore,$infoAfter),@('Session->Region=VirtualAlloc(NULL,Session->Layout.RegionSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);','Session->Region=AllocateRegion(Session->Layout.RegionSize);'),
        @('CHECK(VirtualFree(Session->Region,0,MEM_RELEASE));','ReleaseRegion(Session->Region);'))
    foreach($pair in $transforms){if([regex]::Matches($body,[regex]::Escape($pair[0])).Count -ne 1){throw 'Finite host marker not unique'};$body=$body.Replace($pair[0],$pair[1])}
    $body=[regex]::Replace($body,'"(\.\./[^"]+)"',{
        param($m);'"'+[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value)).Replace('\','/')+'"'
    })
    $read=$e.parts|Where-Object name -eq 'read';$readInclude='"'+(Join-Path $script:RepoRoot 'driver\v2\session_read.h').Replace('\','/')+'"'
    if([regex]::Matches($body,[regex]::Escape($readInclude)).Count -ne 1){throw 'Actual Read selection not unique'}
    $body=$body.Replace($readInclude,'"'+$read.path.Replace('\','/')+'"')
    if([IO.File]::ReadAllText($e.host_adapted) -cne $body -or (Get-FileHash $e.host_adapted).Hash.ToLowerInvariant() -ne $e.host_adapted_sha256){throw 'Host changed beyond finite fixture projection'}
    $describe=$e.parts|Where-Object name -eq 'describe';$lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($build.variant -eq 'without-describe-rollback'){$lines+='#define PB_SEND_DESCRIBE_WITHOUT_ROLLBACK 1'}
    if($build.variant -eq 'before-current-nb-attribution'){$lines+='#define PB_SEND_DESCRIBE_OLD_ATTRIBUTION 1'}
    $lines+='#define PB_SEND_DESCRIBE_TYPES "'+$e.types.Replace('\','/')+'"';$lines+='#define PB_SEND_DESCRIBE_HOST "'+$e.host_adapted.Replace('\','/')+'"'
    $lines+='#define PB_SEND_DESCRIBE_ACTUAL "'+$describe.path.Replace('\','/')+'"';$lines+='#define PB_SEND_SOURCE "'+$e.selected.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\send_describe_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`n")+"`n") -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -ne $build.entry_sha256 -or !$build.ndebug -or $build.pb_api_tests){throw 'Shipping compilation entry changed'}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-SendDescribeProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($arg in $Arguments){$info.ArgumentList.Add($arg)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start Send Describe test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Send Describe external watchdog expired'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-send-describe.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding)
        $summary=[regex]::Match($output,'PASS: (\d+) actual Read Describe Send cases; partial(\d+) repair(\d+) attribution(\d+) mappings(\d+)/(\d+) future_reads(\d+) handles(\d+)/(\d+); bounded queue ownership and complete payloads')
        $full=$Arguments.Count -eq 0;$attributionCase=!$full -and $Arguments[0] -eq '135';$cases=if($full){216}else{3};$partial=if($full){48}elseif($attributionCase){0}else{3}
        $repairs=if($Expected -eq 134){3}else{0};$wrong=if($Expected -eq 135){3}else{0};$future=if($full){576}else{0}
        $marker='SEND_DESCRIBE_RESERVATION_NOT_RELEASED: omitted rollback retained the failed token; actual single release and prefix recycle preceded subsequent reuse and complete cleanup before134'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 134 -or $output.Contains($marker))
        $wrongMarker='SEND_DESCRIBE_OLD_NB_ATTRIBUTION: actual frozen Send inherited the preceding NB queue/offset after current prefix mapping failure; complete source/heap/region/reference cleanup before135'
        $correct=$correct -and ($Expected -ne 135 -or $output.Contains($wrongMarker))
        $records=[regex]::Matches($output,'(?m)^CASE: send describe queues(\d+) ip([46]) ndis([a-f0-9]+) kind([0-8]) partial([01]) repair([01]) attribution([01])\r?$')
        $correct=$correct -and $records.Count -eq $cases -and @($output -split '\r?\n'|Where-Object {$_.StartsWith('CASE:')}).Count -eq $cases
        for($i=0;$i -lt $records.Count;++$i){$row=$records[$i];$shape=$i%108
            $q=if($full){@(1,2,8)[[int][Math]::Floor($shape/36)]}else{2};$ip=if($full){4+2*([int][Math]::Floor($shape/18)%2)}else{6}
            $v=if($full -and ([int][Math]::Floor($shape/9)%2)){'60059'}else{'60014'};$kind=if($full){$shape%9}elseif($attributionCase){1}else{3};$part=[int]($kind -eq 3 -or $kind -eq 5)
            $correct=$correct -and [int]$row.Groups[1].Value -eq $q -and [int]$row.Groups[2].Value -eq $ip -and $row.Groups[3].Value -ceq $v -and
                [int]$row.Groups[4].Value -eq $kind -and [int]$row.Groups[5].Value -eq $part -and [int]$row.Groups[6].Value -eq [int]($Expected -eq 134) -and [int]$row.Groups[7].Value -eq [int]($Expected -eq 135)
        }
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $partial -and [int]$summary.Groups[3].Value -eq $repairs -and
            [int]$summary.Groups[4].Value -eq $wrong -and [int]$summary.Groups[5].Value -eq 12*$cases -and $summary.Groups[5].Value -eq $summary.Groups[6].Value -and [int]$summary.Groups[7].Value -eq $future -and $summary.Groups[8].Value -eq $summary.Groups[9].Value}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -in 134,135 -and $correct);watchdog_ms=30000;log=$log;
            schedules=$cases;partial_copy_cases=$partial;compensating_owner_releases=$repairs;old_attribution_witnesses=$wrong;source_mapping_allocations=12*$cases;source_mapping_frees=12*$cases;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Send Describe failed:$code expected$Expected"};Write-Output $summary.Value
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-describe-rollback','before-current-nb-attribution')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-SendDescribeTests -Directory (Join-Path $directory "$mode-$memory\$variant") -Mode $mode -Guarded:$guarded -Variant $variant -BaselineDirectory $baseline
    Assert-SendDescribeProjection $build;$executables+=,$build
    if($variant -eq 'current'){Invoke-SendDescribeProcess $build.path @();Invoke-SendDescribeProcess $build.path @('134');Invoke-SendDescribeProcess $build.path @('135')}
    elseif($variant -eq 'before-current-nb-attribution'){Invoke-SendDescribeProcess $build.path @('135') 135}else{Invoke-SendDescribeProcess $build.path @('134') 134}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during Send Describe audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=([IO.File]::ReadAllText((Join-Path $baseline 'baseline.json'))|ConvertFrom-Json).source_tree_sha256;
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');
    unchanged_dependencies=$dependencies.Count-3;production_changed=$true;permitted_production_change='driver/v2/session_send.h per-NB diagnostic cursor reset, common/offload_policy.c LSO/checksum dependency repair and driver/v2/session_describe.h checksum-family selection repair only';kernel_runtime_tested=$false;production_ready=$false;
    diagnostic_ledger=@{path=(Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1');sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant();contracts=@($ledgerContracts)+@($fileLedger.checks);files=$fileLedger.files};
    operations=$script:TestResultFile;executables=$executables;audit_sources=@('tests\model\send_describe_tests.c','tools\build\send-describe-tests.ps1','tools\audit\verify-send-describe.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}});
    method='Coverage-only actual complete Read/Describe/Send with the reviewed per-NB diagnostic cursor reset, separately reviewed complete-file LSO/checksum dependency repair and checksum-family Describe selection repair in production, original send-window host and original packet/queue/pool/recycle/diagnostic ownership. The old host is reused through one bounded Describe stand-in replacement, OOB evaluation observer/runtime version selection, tracked heap/region allocation, absolute source includes and a token-only unused main rename. Whole production headers and three concrete WDK OOB typedef spans are copied verbatim with offsets and hashes, independently reconstructed; current Send stays byte-identical, negative omits exactly its failed-Describe RESERVED-to-FREE transition. NDEBUG is required; original host NT_ASSERT=CHECK deliberately remains stronger than production assertion behavior. PB_API_TESTS is rejected and the shipping source string must be65 bytes. One/two/eight queues, IPv4/IPv6 and6.20/6.89 runtime gates cover successful four-NB chains, first/middle mapping failures, a tail mapping failure after50 payload bytes, defensive invalid current offset/short MDL chain, current offset exactly at MDL end empty current MDL and an ARP-classified control-span mapping failure. Every chain includes an intermediate zero-length node and nonzero initial offset. Source mappings are independent read-only guarded spans in both modes; guarded mode additionally protects host heap/region allocations. Failed partial bytes are never exposed: a prior complete NB prefix is published once, failed NBL status/diagnostics are preserved, its suffix is skipped, the next original succeeds, and real reservation reuses the failed slot with generation2 and a complete new payload. All ready descriptors are popped once, exact bytes/order/tokens validated and actual recycle returns every published slot before original fixture cleanup. The full hosted NBL information array is retained; an observer rejects evaluation of unavailable USO fields on6.20 before dereference. This proves runtime branch behavior with concrete metadata layouts, not old-kernel object allocation shape or native Win7 execution. Per-case heap/region/source-map counters return to zero. There are no OS workers/threads or async owners in this bounded synchronous model; completion is synchronous and its Active/Returns pin is checked through the original fixture. Full108 checked warm plus108 measured cases, CLI134/135 two checked warm plusone measured, all with strict process-handle equality. Safe134 observes the deliberately leaked failed token still RESERVED and its free bit clear; the next attempted reservation really reports pool-full because both slots remain owned under the mutation, not because a fixture fabricates FULL. Before any physical cleanup it calls the actual pool transition on the exact failed token once, retires the original valid prefix through actual recycle to clear the full cache naturally, then performs another actual send and observes generation2 reuse before complete normal recycle/free. Current performs zero compensating releases. The independent shared diagnostic ledger admits only the exact complete old-to-new Send two-assignment reset/comment projection and runs sixteen text/raw-byte positive/negative contracts, including BOM/UTF16/LF rejection. Before135 executes the complete frozen original Send: after a valid first NB, a current NB prefix mapping failure retains the preceding queue/offset while correctly identifying the new length; the wrong attribution is observed directly and all original resources are normally returned before135. Fixed current executes identical CLI135 with unknown queue/zero offset and exits0. No references, counters, cursors, caches or generations are repaired. All cleanup precedes134. No driver loading, devices, network, registry, native NDIS/DDI/IRQL, kernel CPU or performance result is claimed. Historical network dependency replay additionally admits only the separately reviewed full-file LSO/checksum repair and checksum-family Describe selection repair. Version remains2.0.0.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
