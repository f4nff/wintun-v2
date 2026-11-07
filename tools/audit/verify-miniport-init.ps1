param([string]$BaselineDirectory='out\stop-reclaim-baseline\91ccf2ef652c',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\miniport-init-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Miniport init requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$dependencies=@(rg --files driver common);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate production dependencies'}
$dependencies+=@('include\packetbridge_types.h','include\packetbridge_probe.h','include\packetbridge_version.h','include\packetbridge_v2_abi.h',
    'include\packetbridge_return_diagnostics.h','include\packetbridge_send_diagnostics.h','include\packetbridge_pool_pressure.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){Assert-SendDiagnosticDependency -BaselineDirectory $baseline -Relative $path}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\miniport-init\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-MiniportProjection($build) {
    $e=$build.extractions;$driver=Join-Path $script:RepoRoot 'driver\wintun.c';$ndis=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis.h"
    $wdm=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\wdm.h";$pool=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\nblapi.h"
    $anchors=[ordered]@{
        constants=@($driver,'#define TUN_VENDOR_NAME','/* Legacy userspace byte rings',$false)
        context=@($driver,'typedef struct _TUN_CTX','static UINT NdisVersion;',$false)
        init=@($driver,'static MINIPORT_INITIALIZE TunInitializeEx;','static MINIPORT_HALT TunHaltEx;',$false)
        halt=@($driver,'static MINIPORT_HALT TunHaltEx;','static MINIPORT_SHUTDOWN TunShutdownEx;',$false)
        common=@($driver,'#define TUN_FORCE_UNREGISTRATION',"_IRQL_requires_max_(PASSIVE_LEVEL)`r`nstatic VOID`r`nTunProcessNotification(",$false)
        offload=@((Join-Path $script:RepoRoot 'driver\v2\offload_probe.h'),'static NDIS_STATUS PbRegisterOffload(TUN_CTX *ctx)','static VOID PbReportOffload(TUN_CTX *ctx)',$false)
        interface=@($wdm,'typedef enum _INTERFACE_TYPE {','}INTERFACE_TYPE, *PINTERFACE_TYPE;',$true)
        'ndis-interface'=@($ndis,'typedef enum _NDIS_INTERFACE_TYPE','} NDIS_INTERFACE_TYPE, *PNDIS_INTERFACE_TYPE;',$true)
        'init-parameters'=@($ndis,'#define NDIS_MINIPORT_INIT_PARAMETERS_REVISION_1','#define NDIS_MINIPORT_RESTART_PARAMETERS_REVISION_1',$false)
        registration=@($ndis,'#define NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1','#define NDIS_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES_REVISION_1',$false)
        'offload-type'=@($ndis,'#define NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1','#include <ndis/wifiattributes.h>',$false)
        'pool-type'=@($pool,'#define NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1',"_Must_inspect_result_`r`n__drv_allocatesMem(mem)`r`n_IRQL_requires_max_(DISPATCH_LEVEL)`r`nNDIS_EXPORTED_ROUTINE`r`nNDIS_HANDLE`r`nNdisAllocateNetBufferListPool(",$false)
        'halt-type'=@($ndis,'typedef enum _NDIS_HALT_ACTION','} NDIS_HALT_ACTION, *PNDIS_HALT_ACTION;',$true)
    }
    if(@($e.parts).Count -ne 16 -or @($e.parts.name|Select-Object -Unique).Count -ne 16){throw 'Unexpected raw extraction set'}
    $raw=@{}
    foreach($name in $anchors.Keys){
        $definition=$anchors[$name];$source=[IO.File]::ReadAllText($definition[0]);$found=@($e.parts|Where-Object name -eq $name)
        if($found.Count -ne 1){throw "Raw source part not unique:$name"};$part=$found[0]
        foreach($marker in @($definition[1],$definition[2])){if([regex]::Matches($source,[regex]::Escape($marker)).Count -ne 1){throw "Audit source marker not unique:$name"}}
        $offset=$source.IndexOf($definition[1]);$end=$source.IndexOf($definition[2],$offset);if($definition[3]){$end+=$definition[2].Length}
        if($part.source -cne $definition[0] -or $part.first -cne $definition[1] -or $part.next -cne $definition[2] -or $part.include_end -ne $definition[3] -or $part.offset -ne $offset -or $part.length -ne $end-$offset){throw "Exact source span metadata changed:$name"}
        $raw[$name]=$source.Substring($offset,$end-$offset)
        if([IO.File]::ReadAllText($part.path) -cne $raw[$name]){throw "Source extraction bytes changed:$name"}
    }
    $defines='';$ndisText=[IO.File]::ReadAllText($ndis)
    foreach($name in @('NDIS_RUNTIME_VERSION_630','NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK','NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND')){
        $match=[regex]::Matches($ndisText,'(?m)^#define '+$name+'[^\r\n]*\r?\n');$found=@($e.parts|Where-Object name -eq $name)
        if($found.Count -ne 1){throw "NDIS constant part not unique:$name"};$part=$found[0]
        if($match.Count -ne 1 -or $part.source -cne $ndis -or $part.offset -ne $match[0].Index -or $part.length -ne $match[0].Length -or
            [IO.File]::ReadAllText($part.path) -cne $match[0].Value){throw "Exact NDIS constant changed:$name"};$defines+=$match[0].Value
    }
    foreach($part in $e.parts){if((Get-FileHash $part.source).Hash.ToLowerInvariant() -ne $part.source_sha256 -or (Get-FileHash $part.path).Hash.ToLowerInvariant() -ne $part.sha256){throw "Raw extraction hash changed:$($part.name)"}}
    $types=$raw['interface']+"`r`n"+$raw['ndis-interface']+"`r`n"+$defines+"`r`n"+
        'typedef struct _CM_PARTIAL_RESOURCE_LIST NDIS_RESOURCE_LIST, *PNDIS_RESOURCE_LIST;'+"`r`n"+$raw['init-parameters']+"`r`n"+$raw['registration']+"`r`n"+
        $raw['offload-type']+"`r`n"+$raw['pool-type']+"`r`n"+$raw['halt-type']+"`r`n"+
        'typedef union _NDIS_MINIPORT_ADAPTER_ATTRIBUTES NDIS_MINIPORT_ADAPTER_ATTRIBUTES, *PNDIS_MINIPORT_ADAPTER_ATTRIBUTES;'+"`r`n"
    if($types -cne [IO.File]::ReadAllText($e.types) -or (Get-FileHash $e.types).Hash.ToLowerInvariant() -ne $e.types_sha256){throw 'Type aggregate changed beyond exact spans and two opaque pointer declarations'}
    foreach($header in $e.native_headers){if((Get-FileHash $header.path).Hash.ToLowerInvariant() -ne $header.sha256){throw 'Native SDK/WDK header hash changed'}}
    $before='    if (RegistrationLockInitialized) ExDeleteResourceLite(&Ctx->Device.RegistrationLock);';$after='    /* Test mutation: failure-path RegistrationLock deletion omitted. */';$selected=$raw['init']
    if([regex]::Matches($selected,[regex]::Escape($before)).Count -ne 1){throw 'Actual Init cleanup guard not unique'}
    if($build.variant -ne 'current'){
        if($e.mutation.original -cne $before -or $e.mutation.changed -cne $after -or $e.mutation.offset -ne $selected.IndexOf($before) -or $e.mutation.length -ne $before.Length){throw 'Resource-delete mutation metadata changed'}
        $selected=$selected.Replace($before,$after)
    }elseif($null -ne $e.mutation){throw 'Current Initialize unexpectedly mutated'}
    if($selected -cne [IO.File]::ReadAllText($e.selected) -or (Get-FileHash $e.selected).Hash.ToLowerInvariant() -ne $e.selected_sha256){throw 'Initialize selected body changed beyond unique negative cleanup omission'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};if($build.variant -ne 'current'){$lines+='#define PB_INIT_WITHOUT_RESOURCE_DELETE 1'}
    $lines+='#define PB_INIT_TYPES "'+$e.types.Replace('\','/')+'"';$lines+='#define PB_INIT_SELECTED "'+$e.selected.Replace('\','/')+'"'
    foreach($name in @('constants','context','halt','common','offload')){$part=$e.parts|Where-Object name -eq $name;$lines+='#define PB_INIT_'+$name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\miniport_init_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`r`n")+"`r`n") -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -ne $build.entry_sha256){throw 'Compilation entry changed'}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-MiniportProcess {
    param([string]$Program,[string[]]$Arguments,[int]$Expected=0)
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start miniport init test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Miniport init external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $script:TestLogDirectory ('{0:d2}-miniport-init.log' -f $script:TestExecutions.Count)
        [IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) actual Miniport Initialize/initial-Paused Halt cases plus (\d+) checked warmup; required rollback/optional plain fallback, native NDIS formats and publication, (\d+) Halt/(\d+) Init thread joins; zero owned resources; handles (\d+)/(\d+); compensation(\d+)/(\d+)')
        $full=$Arguments.Count -eq 0;$cases=if($full){64}else{1};$warm=if($full){64}else{2};$halts=if($full){16}else{0};$inits=if($Expected -eq 130){1}else{0};$comp=if($Expected -eq 130){1}else{0};$warmComp=if($Expected -eq 130){2}else{0}
        $marker='MINIPORT_INIT_FAILURE_RESOURCE_NOT_DELETED: pre-Ctx-free observer held live registration resource; one actual Delete compensation before original free, initializer joined and all resources reclaimed before130'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 130 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $warm -and [int]$summary.Groups[3].Value -eq $halts -and [int]$summary.Groups[4].Value -eq $inits -and
            $summary.Groups[5].Value -eq $summary.Groups[6].Value -and [int]$summary.Groups[7].Value -eq $comp -and [int]$summary.Groups[8].Value -eq $warmComp}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 130 -and $correct);
            watchdog_ms=30000;log=$log;measured_schedules=$cases;checked_warmup_schedules=$warm;schedules=$cases+$warm;measured_halt_joins=$halts;measured_initializer_joins=$inits;negative_cleanup_compensations=$comp+$warmComp;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Miniport init test failed:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-failure-resource-delete')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant";$built=Build-MiniportInitTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-MiniportProjection $built
    if($variant -eq 'current'){Invoke-MiniportProcess $built.path @();Invoke-MiniportProcess $built.path @('130')}else{Invoke-MiniportProcess $built.path @('130') 130};$executables+=,$built
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during miniport init audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    send_diagnostic_ledger=@{path='tools/audit/send-diagnostic-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'send-diagnostic-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    network_dependency_ledger=@{path='tools/audit/network-dependency-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')).Hash.ToLowerInvariant()};
    checksum_family_ledger=@{path='tools/audit/checksum-family-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c','driver/v2/session_describe.h');
    reviewed_dependency_repairs=@('driver/v2/session_send.h','common/offload_policy.c','driver/v2/session_describe.h');
    unchanged_dependencies=($dependencies.Count-3);production_changed=$true;kernel_runtime_tested=$false;production_ready=$false;
    operations=$script:TestResultFile;executables=$executables;
    audit_sources=@('tests\model\miniport_init_tests.c','tools\build\miniport-init-tests.ps1','tools\audit\verify-miniport-init.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only real production Initialize/Halt with no packet/session path; original tests are unchanged. Each build copies16 exact raw source spans: production constants, TUN_CTX declaration, complete TunInitializeEx/TunHaltEx/common FILE-owner unregister/PbRegisterOffload; native WDK concrete interface/init-parameters/registration/offload/pool/Halt types and three constants. Native SDK NDIS statistics/PM/offload formats and the native general-attributes header are directly included. Only unused opaque resource-list and adapter-union pointers are forward declared; concrete NDIS layouts, revisions and sizes are not modeled. TUN_CTX declaration is verbatim but its kernel synchronization/device member types are hosted, not a native kernel context-layout proof. NDEBUG is mandatory; shipping ASSERT/NT_ASSERT are disabled while test CHECK stays active, PB_API_TESTS is rejected, and a65-byte shipping source fingerprint is statically required. Four runtime versions6.20/6.29/6.30/6.89 cross24-bit LUID index zero/max and eight outcomes: normal success, context NULL allocation, defensive missing FDO, resource-initialization failure, NBL-pool NULL allocation, required registration/general attribute BAD_VERSION, optional offload BAD_VERSION plain fallback. The missing-FDO and zero identity inputs are explicit defensive/identity boundary models, not claims about naturally returned active Windows adapter values. Input InitParameters has a valid native Header/IfIndex/IfType. Every DDI acquisition boundary checks Reserved is unpublished; exact prefix counts, MAC bytes, required attribute ordering, pool parameters, registration rev1 before6.30/rev2 after, general rev2 and PM rev1/rev2, current disabled/modern hardware RSC/URO and optional plain masks are checked. Failed Init performs its own full rollback and never calls Halt. NDIS registration context is recorded, not treated as a public device entry or required to clear on failure, and is never dereferenced after a failed Init frees it. Successful Initialize starts Paused per the NDIS contract; there is no Restart, packet, worker, OID or Halt during initialization. After success a genuine production context lookup holds Control rundown; actual Halt revokes Reserved and blocks on that pin without keeping the global lookup lock. New lookup is rejected; release and real Halt-thread join then permit actual common no-owner early return, outer Return rundown, NBL-pool/resource/Ctx release. Hosted embedded kernel-event handles are closed with their owning context. All resource/context/pool/event/thread counts return to zero per case. Full processes warm64 then measure64, CLI130 warms2 then measures1, all with strict process-handle equality. Negative130 removes only failure cleanup RegistrationLock deletion. A failed general registration reaches the first context physical free with that resource still owned; the free observer parks before freeing memory, the controller invokes the same hosted ExDeleteResourceLite exactly once while Ctx remains live, then original free and initializer join finish before130. Current never compensates and no ref/counter/generation is edited. Both SDK locks/events and OS-thread joins are real; all kernel DDIs, device/FDO, NDIS and IRQL are hosted. No real driver/device/network/registry, Win7 runtime/Verifier or native CPU measurement is claimed. Version remains2.0.0. Historical dependency replay retains the original test paths and checks, with the reviewed per-NB Send diagnostic cursor reset, the separately reviewed LSO/checksum dependency repair and the checksum-family Describe selection repair admitted by strict complete-file ledgers.'}|
    ConvertTo-Json -Depth 18|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
