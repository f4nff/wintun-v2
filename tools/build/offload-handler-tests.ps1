# Build helper only: caller initializes common.ps1 and owns process recording.
function Write-OffloadHandlerParts {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-policy-bank-wait')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Offload handler output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$parts=@()
    $source=Join-Path $script:RepoRoot 'driver\v2\offload_probe.h';$text=[IO.File]::ReadAllText($source)
    $markers=[ordered]@{
        effective=@('static ULONG PbEffectiveMask(TUN_CTX *ctx)','#include "offload_format.h"')
        report=@('static VOID PbReportOffload(TUN_CTX *ctx)','static BOOLEAN PbUpdateParameter(')
        handlers=@('static BOOLEAN PbUpdateParameter(','static VOID PbSetLsoCompletion(')
    }
    foreach($name in $markers.Keys){
        $first=$markers[$name][0];$last=$markers[$name][1]
        if([regex]::Matches($text,[regex]::Escape($first)).Count -ne 1 -or [regex]::Matches($text,[regex]::Escape($last)).Count -ne 1){throw "Nonunique $name extraction markers"}
        $start=$text.IndexOf($first);$end=$text.IndexOf($last,$start);if($end -le $start){throw "Invalid $name extraction span"}
        $path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($path,$text.Substring($start,$end-$start),$encoding)
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$start;length=$end-$start;
            start_marker=$first;end_marker=$last;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $handlers=$parts|Where-Object name -eq 'handlers';$selected=[IO.File]::ReadAllText($handlers.path)
    $policy='    if (code == PB_PROBE_IOCTL_POLICY) {';$wait='        PbWaitIndications(ctx, oldBank);';$replacement='        (void)oldBank; /* Test mutation: POLICY prior-bank wait omitted. */'
    if([regex]::Matches($selected,[regex]::Escape($policy)).Count -ne 1){throw 'Policy branch marker is not unique'}
    $policyOffset=$selected.IndexOf($policy);$tail=$selected.Substring($policyOffset)
    if([regex]::Matches($tail,[regex]::Escape($wait)).Count -ne 1){throw 'Policy bank wait is not unique'}
    $waitOffset=$policyOffset+$tail.IndexOf($wait)
    if($Variant -eq 'without-policy-bank-wait'){$selected=$selected.Substring(0,$policyOffset)+$tail.Replace($wait,$replacement)}
    $selectedPath=Join-Path $Directory 'handlers-selected.h';[IO.File]::WriteAllText($selectedPath,$selected,$encoding)
    $ndis=Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis.h";$ndisText=[IO.File]::ReadAllText($ndis)
    $first='#define  NDIS_STATUS_INDICATION_REVISION_1';$last='// Generic Timer support'
    if([regex]::Matches($ndisText,[regex]::Escape($first)).Count -ne 1 -or [regex]::Matches($ndisText,[regex]::Escape($last)).Count -ne 1){throw 'WDK status type markers are not unique'}
    $start=$ndisText.IndexOf($first);$end=$ndisText.IndexOf($last,$start);if($end -le $start){throw 'Invalid WDK status type span'}
    $type=Join-Path $Directory 'status-type-extracted.h';[IO.File]::WriteAllText($type,$ndisText.Substring($start,$end-$start),$encoding)
    $parts+=@{name='status-type';source=$ndis;source_sha256=(Get-FileHash $ndis).Hash.ToLowerInvariant();offset=$start;length=$end-$start;
        start_marker=$first;end_marker=$last;path=$type;sha256=(Get-FileHash $type).Hash.ToLowerInvariant()}
    $matches=[regex]::Matches($ndisText,'(?m)^#define NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG[^\r\n]*\r?\n')
    if($matches.Count -ne 1){throw 'WDK status code definition is not unique'}
    $code=Join-Path $Directory 'status-code-extracted.h';[IO.File]::WriteAllText($code,$matches[0].Value,$encoding)
    $parts+=@{name='status-code';source=$ndis;source_sha256=(Get-FileHash $ndis).Hash.ToLowerInvariant();offset=$matches[0].Index;length=$matches[0].Length;
        start_marker='^#define NDIS_STATUS_TASK_OFFLOAD_CURRENT_CONFIG';end_marker='[line end]';path=$code;sha256=(Get-FileHash $code).Hash.ToLowerInvariant()}
    $types=Join-Path $Directory 'status-types.h';[IO.File]::WriteAllText($types,[IO.File]::ReadAllText($type)+[IO.File]::ReadAllText($code),$encoding)
    $typesHeaders=@((Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\oidrequest.h"),(Join-Path $script:WdkRoot "Include\$script:KitVersion\km\ndis\encapsulationconfig.h"),
        (Join-Path $script:SdkRoot "Include\$script:KitVersion\shared\ntddndis.h"),(Join-Path $script:SdkRoot "Include\$script:KitVersion\shared\ndis\objectheader.h"))
    return [ordered]@{variant=$Variant;parts=$parts;selected=$selectedPath;selected_sha256=(Get-FileHash $selectedPath).Hash.ToLowerInvariant();
        policy_marker=$policy;policy_offset=$policyOffset;wait_offset=$waitOffset;mutation_original=$wait;mutation_replacement=$replacement;
        types=$types;types_sha256=(Get-FileHash $types).Hash.ToLowerInvariant();kit_version=$script:KitVersion;
        sdk_wdk_types=@($typesHeaders|ForEach-Object {@{path=$_;sha256=(Get-FileHash $_).Hash.ToLowerInvariant()}});
        adaptation='Current production function spans and WDK status type/size/code are copied verbatim. Only the unique POLICY branch prior-bank wait is omitted in the selected negative; OID waits, runtime capability checks, mask writes, actual Report and all other function bytes remain unchanged. OID/parameter/encapsulation formats come directly from SDK/WDK headers, not hosted lookalike layouts.'}
}
function Build-OffloadHandlerTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-policy-bank-wait')][string]$Variant='current')
    if($script:SourceFingerprint -cnotmatch '^[0-9a-f]{64}$'){throw 'Offload handler builder requires current shipping source fingerprint'}
    $extraction=Write-OffloadHandlerParts -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($Variant -eq 'without-policy-bank-wait'){$lines+='#define PB_OFFLOAD_WITHOUT_POLICY_WAIT 1'}
    $lines+='#define PB_OFFLOAD_STATUS_TYPE "'+$extraction.types.Replace('\','/')+'"'
    $lines+='#define PB_OFFLOAD_HANDLERS_SOURCE "'+$extraction.selected.Replace('\','/')+'"'
    foreach($part in $extraction.parts|Where-Object name -in 'effective','report'){$lines+='#define PB_OFFLOAD_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\offload_handler_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'offload-handler.exe'
    $versionArguments=@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",
        ('/I'+(Join-Path $script:WdkRoot "Include\$script:KitVersion\km")),"/Fo$Directory\",$entry,'common\offload_policy.c')+$versionArguments+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;extractions=$extraction}
}
