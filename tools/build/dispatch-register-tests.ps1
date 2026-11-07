# Reuse the existing register host with two finite type/storage adaptations.
. (Join-Path $PSScriptRoot 'kernel-register-tests.ps1')
function Write-DispatchRegisterParts {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-control-wait')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Dispatch output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$register=Write-KernelRegister -Directory $Directory
    $source=Join-Path $script:RepoRoot 'driver\wintun.c';$text=[IO.File]::ReadAllText($source)
    $notification=[regex]::Match($text,'(?m)^_IRQL_requires_max_\(PASSIVE_LEVEL\)\r?\nstatic VOID\r?\nTunProcessNotification\(')
    if(!$notification.Success){throw 'Cannot find common unregister end'}
    $anchors=[ordered]@{
        common=@($text.IndexOf('#define TUN_FORCE_UNREGISTRATION'),$notification.Index)
        dispatch=@($text.IndexOf('_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)'),$text.IndexOf('_Dispatch_type_(IRP_MJ_CLOSE)'))
        halt=@($text.IndexOf('static MINIPORT_HALT TunHaltEx;'),$text.IndexOf('static MINIPORT_SHUTDOWN TunShutdownEx;'))
    }
    $parts=@();foreach($name in $anchors.Keys){$start=$anchors[$name][0];$end=$anchors[$name][1];if($start -lt 0 -or $end -le $start){throw "Cannot isolate $name"}
        $path=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($path,$text.Substring($start,$end-$start),$encoding)
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$start;length=$end-$start;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}}
    $legacy=[regex]::Matches($text,'(?m)^#define TUN_IOCTL_REGISTER_RINGS[^\r\n]*\r?\n')
    if($legacy.Count -ne 1){throw 'Legacy IOCTL definition is not unique'}
    $path=Join-Path $Directory 'legacy-extracted.h';[IO.File]::WriteAllText($path,$legacy[0].Value,$encoding)
    $parts+=@{name='legacy';source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$legacy[0].Index;length=$legacy[0].Length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    $sessionSource=Join-Path $script:RepoRoot 'driver\v2\session.h';$session=[IO.File]::ReadAllText($sessionSource)
    $start=$session.IndexOf('static NTSTATUS PbV2Control(');if($start -lt 0){throw 'Cannot find actual control router'}
    $path=Join-Path $Directory 'control-extracted.h';[IO.File]::WriteAllText($path,$session.Substring($start),$encoding)
    $parts+=@{name='control';source=$sessionSource;source_sha256=(Get-FileHash $sessionSource).Hash.ToLowerInvariant();offset=$start;length=$session.Length-$start;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    $gateSource=Join-Path $script:RepoRoot 'driver\context_gate.h';$gate=[IO.File]::ReadAllText($gateSource);$line='    ExWaitForRundownProtectionRelease(&ctx->ControlRundown);'
    if([regex]::Matches($gate,[regex]::Escape($line)).Count -ne 1){throw 'Control rundown wait is not unique'}
    if($Variant -eq 'without-control-wait'){$gate=$gate.Replace($line,'    /* Test mutation: Control wait omitted. */')}
    $gatePath=Join-Path $Directory 'context-gate.h';[IO.File]::WriteAllText($gatePath,$gate,$encoding)
    $hostSource=Join-Path $script:RepoRoot 'tests\model\register_owner_tests.c';$hostBody=[IO.File]::ReadAllText($hostSource)
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -ne '1bfe838ab756839abeb7d100e33018b2ecac1599dcb60ff16bd79879eda7536e'){throw 'Reused register host changed; review finite adaptations'}
    $hostOriginal=Join-Path $Directory 'register-host-original.c';[IO.File]::WriteAllBytes($hostOriginal,[IO.File]::ReadAllBytes($hostSource))
    $typeStart=$hostBody.IndexOf('typedef struct _TUN_CTX {');$typeEnd=$hostBody.IndexOf('typedef UCHAR KIRQL;',$typeStart)
    if($typeStart -lt 0 -or $typeEnd -le $typeStart){throw 'Cannot isolate reused host types'}
    $oldTypes=$hostBody.Substring($typeStart,$typeEnd-$typeStart);$typesPath=Join-Path $Directory 'original-host-types.h';[IO.File]::WriteAllText($typesPath,$oldTypes,$encoding)
    $adapted=$hostBody.Substring(0,$typeStart)+"PB_DISPATCH_HOST_TYPES`r`n"+$hostBody.Substring($typeEnd)
    $declaration='static TUN_CTX Context;';if([regex]::Matches($adapted,[regex]::Escape($declaration)).Count -ne 1){throw 'Context storage declaration is not unique'}
    $adapted=$adapted.Replace($declaration,"static TUN_CTX *HostedCtxPointer;`r`n#define Context (*HostedCtxPointer)")
    $adapted=[regex]::Replace($adapted,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value)).Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'register-host.h';[IO.File]::WriteAllText($hostPath,$adapted,$encoding)
    return [ordered]@{variant=$Variant;register=$register;parts=$parts;gate=$gatePath;gate_source=$gateSource;gate_sha256=(Get-FileHash $gatePath).Hash.ToLowerInvariant();
        host_source=$hostSource;host_source_sha256=(Get-FileHash $hostSource).Hash.ToLowerInvariant();host_original=$hostOriginal;host_original_sha256=(Get-FileHash $hostOriginal).Hash.ToLowerInvariant();host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();
        types_offset=$typeStart;types_length=$typeEnd-$typeStart;types=$typesPath;types_sha256=(Get-FileHash $typesPath).Hash.ToLowerInvariant();
        types_start_marker='typedef struct _TUN_CTX {';types_end_marker='typedef UCHAR KIRQL;';storage_original=$declaration;storage_adapted="static TUN_CTX *HostedCtxPointer;`r`n#define Context (*HostedCtxPointer)";
        adaptation='Only original TUN_CTX/IO_STACK_LOCATION/IRP type region -> PB_DISPATCH_HOST_TYPES, one static Context storage declaration -> pointer/token alias, and relative include paths -> absolute. All reused allocation/MDL/event/worker/failure/cleanup host function bodies remain unchanged.'}
}
function Build-DispatchRegisterTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-control-wait')][string]$Variant='current')
    $extraction=Write-DispatchRegisterParts -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_REGISTER_SOURCE "'+$extraction.register.header.Replace('\','/')+'"'
    $lines+='#define PB_DISPATCH_REGISTER_HOST "'+$extraction.host.Replace('\','/')+'"'
    $lines+='#define PB_DISPATCH_CONTEXT_SOURCE "'+$extraction.gate.Replace('\','/')+'"'
    foreach($part in $extraction.parts){$lines+='#define PB_DISPATCH_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\dispatch_register_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'dispatch-register.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",$entry,'common\v2_core.c')+$script:VersionArguments+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;extractions=$extraction}
}
