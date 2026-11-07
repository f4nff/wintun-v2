# Caller initializes common.ps1; all generated headers remain ignored output.
function Write-MiniportInitParts {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-failure-resource-delete')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$parts=[Collections.Generic.List[object]]::new()
    function Copy-InitSpan([string]$Name,[string]$Source,[string]$First,[string]$Next,[bool]$IncludeEnd=$false) {
        $text=[IO.File]::ReadAllText($Source)
        foreach($marker in @($First,$Next)){if([regex]::Matches($text,[regex]::Escape($marker)).Count -ne 1){throw "Miniport span marker not unique:$Name/$marker"}}
        $offset=$text.IndexOf($First);$end=$text.IndexOf($Next,$offset);if($end -le $offset){throw "Miniport span reversed:$Name"};if($IncludeEnd){$end+=$Next.Length}
        $path=Join-Path $Directory "$Name-extracted.h";[IO.File]::WriteAllText($path,$text.Substring($offset,$end-$offset),$encoding)
        $parts.Add([ordered]@{name=$Name;source=$Source;source_sha256=(Get-FileHash $Source).Hash.ToLowerInvariant();offset=$offset;length=$end-$offset;
            first=$First;next=$Next;include_end=$IncludeEnd;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()});return $path
    }
    $driver=Join-Path $script:RepoRoot 'driver\wintun.c'
    $constants=Copy-InitSpan 'constants' $driver '#define TUN_VENDOR_NAME' '/* Legacy userspace byte rings'
    $context=Copy-InitSpan 'context' $driver 'typedef struct _TUN_CTX' 'static UINT NdisVersion;'
    $init=Copy-InitSpan 'init' $driver 'static MINIPORT_INITIALIZE TunInitializeEx;' 'static MINIPORT_HALT TunHaltEx;'
    $halt=Copy-InitSpan 'halt' $driver 'static MINIPORT_HALT TunHaltEx;' 'static MINIPORT_SHUTDOWN TunShutdownEx;'
    $common=Copy-InitSpan 'common' $driver '#define TUN_FORCE_UNREGISTRATION' "_IRQL_requires_max_(PASSIVE_LEVEL)`r`nstatic VOID`r`nTunProcessNotification("
    $offloadSource=Join-Path $script:RepoRoot 'driver\v2\offload_probe.h'
    $offload=Copy-InitSpan 'offload' $offloadSource 'static NDIS_STATUS PbRegisterOffload(TUN_CTX *ctx)' 'static VOID PbReportOffload(TUN_CTX *ctx)'
    $selected=[IO.File]::ReadAllText($init)
    $before='    if (RegistrationLockInitialized) ExDeleteResourceLite(&Ctx->Device.RegistrationLock);'
    $after='    /* Test mutation: failure-path RegistrationLock deletion omitted. */'
    if([regex]::Matches($selected,[regex]::Escape($before)).Count -ne 1){throw 'Init failure resource cleanup not unique'}
    $mutation=$null
    if($Variant -ne 'current'){$mutation=[ordered]@{original=$before;changed=$after;offset=$selected.IndexOf($before);length=$before.Length};$selected=$selected.Replace($before,$after)}
    $selectedPath=Join-Path $Directory 'init-selected.h';[IO.File]::WriteAllText($selectedPath,$selected,$encoding)
    $ndis=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis.h"
    $wdm=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\wdm.h"
    $pool=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\nblapi.h"
    $interface=Copy-InitSpan 'interface' $wdm 'typedef enum _INTERFACE_TYPE {' '}INTERFACE_TYPE, *PINTERFACE_TYPE;' $true
    $ndisInterface=Copy-InitSpan 'ndis-interface' $ndis 'typedef enum _NDIS_INTERFACE_TYPE' '} NDIS_INTERFACE_TYPE, *PNDIS_INTERFACE_TYPE;' $true
    $parameters=Copy-InitSpan 'init-parameters' $ndis '#define NDIS_MINIPORT_INIT_PARAMETERS_REVISION_1' '#define NDIS_MINIPORT_RESTART_PARAMETERS_REVISION_1'
    $registration=Copy-InitSpan 'registration' $ndis '#define NDIS_MINIPORT_ADAPTER_REGISTRATION_ATTRIBUTES_REVISION_1' '#define NDIS_MINIPORT_ADD_DEVICE_REGISTRATION_ATTRIBUTES_REVISION_1'
    $offloadType=Copy-InitSpan 'offload-type' $ndis '#define NDIS_MINIPORT_ADAPTER_OFFLOAD_ATTRIBUTES_REVISION_1' '#include <ndis/wifiattributes.h>'
    $poolType=Copy-InitSpan 'pool-type' $pool '#define NET_BUFFER_LIST_POOL_PARAMETERS_REVISION_1' "_Must_inspect_result_`r`n__drv_allocatesMem(mem)`r`n_IRQL_requires_max_(DISPATCH_LEVEL)`r`nNDIS_EXPORTED_ROUTINE`r`nNDIS_HANDLE`r`nNdisAllocateNetBufferListPool("
    $haltType=Copy-InitSpan 'halt-type' $ndis 'typedef enum _NDIS_HALT_ACTION' '} NDIS_HALT_ACTION, *PNDIS_HALT_ACTION;' $true
    $ndisText=[IO.File]::ReadAllText($ndis);$defines=''
    foreach($name in @('NDIS_RUNTIME_VERSION_630','NDIS_MINIPORT_ATTRIBUTES_SURPRISE_REMOVE_OK','NDIS_MINIPORT_ATTRIBUTES_NO_HALT_ON_SUSPEND')){
        $matches=[regex]::Matches($ndisText,'(?m)^#define '+$name+'[^\r\n]*\r?\n');if($matches.Count -ne 1){throw "NDIS constant not unique:$name"}
        $path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($path,$matches[0].Value,$encoding);$defines+=$matches[0].Value
        $parts.Add([ordered]@{name=$name;source=$ndis;source_sha256=(Get-FileHash $ndis).Hash.ToLowerInvariant();offset=$matches[0].Index;length=$matches[0].Length;first='^#define '+$name;next='[line end]';include_end=$false;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()})
    }
    $types=Join-Path $Directory 'ndis-types.h'
    $typeText=[IO.File]::ReadAllText($interface)+"`r`n"+[IO.File]::ReadAllText($ndisInterface)+"`r`n"+$defines+"`r`n"+
        'typedef struct _CM_PARTIAL_RESOURCE_LIST NDIS_RESOURCE_LIST, *PNDIS_RESOURCE_LIST;'+"`r`n"+[IO.File]::ReadAllText($parameters)+"`r`n"+
        [IO.File]::ReadAllText($registration)+"`r`n"+[IO.File]::ReadAllText($offloadType)+"`r`n"+[IO.File]::ReadAllText($poolType)+"`r`n"+[IO.File]::ReadAllText($haltType)+"`r`n"+
        'typedef union _NDIS_MINIPORT_ADAPTER_ATTRIBUTES NDIS_MINIPORT_ADAPTER_ATTRIBUTES, *PNDIS_MINIPORT_ADAPTER_ATTRIBUTES;'+"`r`n"
    [IO.File]::WriteAllText($types,$typeText,$encoding)
    return [ordered]@{variant=$Variant;parts=@($parts);selected=$selectedPath;selected_sha256=(Get-FileHash $selectedPath).Hash.ToLowerInvariant();mutation=$mutation;
        types=$types;types_sha256=(Get-FileHash $types).Hash.ToLowerInvariant();kit_version=$script:KitVersion;
        native_headers=@('shared\ntddndis.h','shared\ndis\types.h','shared\ndis\status.h'|ForEach-Object {$p=Join-Path $script:SdkRoot "Include\$script:KitVersion\$_";@{path=$p;sha256=(Get-FileHash $p).Hash.ToLowerInvariant()}})+
            @(@{path=(Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\miniportgeneralattributes.h");sha256=(Get-FileHash (Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\miniportgeneralattributes.h")).Hash.ToLowerInvariant()});
        adaptation='Production context/constants/Initialize/Halt/common unregister/offload-registration bodies and required WDK concrete type/size definitions are exact spans. SDK ntddndis and native miniport general-attributes header remain directly included. Only opaque unused resource-list and adapter-union pointer declarations are supplied; no concrete attribute layout is redefined. Current Initialize is verbatim, negative omits only its failure cleanup ExDeleteResourceLite.'}
}
function Build-MiniportInitTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-failure-resource-delete')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Miniport init output must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Miniport init requires shipping source fingerprint'}
    $e=Write-MiniportInitParts -Directory $Directory -Variant $Variant;$lines=@()
    if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};if($Variant -ne 'current'){$lines+='#define PB_INIT_WITHOUT_RESOURCE_DELETE 1'}
    $lines+='#define PB_INIT_TYPES "'+$e.types.Replace('\','/')+'"'
    $lines+='#define PB_INIT_SELECTED "'+$e.selected.Replace('\','/')+'"'
    foreach($name in @('constants','context','halt','common','offload')){$part=$e.parts|Where-Object name -eq $name;$lines+='#define PB_INIT_'+$name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\miniport_init_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`r`n")+"`r`n",[Text.UTF8Encoding]::new($false));$exe=Join-Path $Directory 'miniport-init.exe'
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",('/I'+(Join-Path $script:WdkRoot "Include\$script:KitVersion\km")),"/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe ($flags+@($entry,'/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;extractions=$e}
}
