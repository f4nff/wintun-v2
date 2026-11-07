# Caller initializes common.ps1, supplies the shipping fingerprint, owns recording.
function Write-KernelProcessCallbacks {
    param([Parameter(Mandatory)][string]$Directory,
        [ValidateSet('current','without-pid-check','without-file-check','without-control-wait')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'driver\wintun.c';$text=[IO.File]::ReadAllText($source)
    $anchors=[ordered]@{
        common=@('#define TUN_FORCE_UNREGISTRATION','_Dispatch_type_(IRP_MJ_DEVICE_CONTROL)')
        close=@('_Dispatch_type_(IRP_MJ_CLOSE)','_Dispatch_type_(IRP_MJ_PNP)')
        halt=@('static MINIPORT_HALT TunHaltEx;','static MINIPORT_SHUTDOWN TunShutdownEx;')
    }
    $encoding=[Text.UTF8Encoding]::new($false);$parts=@();$metadata=@()
    foreach($name in $anchors.Keys){
        $start=$text.IndexOf($anchors[$name][0]);if($start -lt 0){throw "Cannot find $name callback block"}
        $end=$text.IndexOf($anchors[$name][1],$start);if($end -le $start){throw "Cannot isolate $name callback block"}
        $part=$text.Substring($start,$end-$start);$file=Join-Path $Directory "$name-extracted.h";[IO.File]::WriteAllText($file,$part,$encoding)
        $metadata+=@{name=$name;offset=$start;length=$end-$start;path=$file;sha256=(Get-FileHash -LiteralPath $file).Hash.ToLowerInvariant()};$parts+=,$part
    }
    $callbacks=$parts -join ''
    $removed=if($Variant -eq 'without-pid-check'){'(ProcessId && Ctx->Device.OwningProcessId != ProcessId)'}elseif($Variant -eq 'without-file-check'){'(Owner != TUN_FORCE_UNREGISTRATION && Ctx->Device.OwningFileObject != Owner)'}else{$null}
    if($removed){if([regex]::Matches($callbacks,[regex]::Escape($removed)).Count -ne 1){throw 'Owner guard is not unique'};$callbacks=$callbacks.Replace($removed,'FALSE /* test mutation */')}
    $header=Join-Path $Directory 'process-callbacks.h';[IO.File]::WriteAllText($header,"#pragma once`r`n"+$callbacks,$encoding)
    $gateSource=Join-Path $script:RepoRoot 'driver\context_gate.h';$gate=[IO.File]::ReadAllText($gateSource)
    if($Variant -eq 'without-control-wait'){
        $line='    ExWaitForRundownProtectionRelease(&ctx->ControlRundown);'
        if([regex]::Matches($gate,[regex]::Escape($line)).Count -ne 1){throw 'Control wait is not unique'}
        $gate=$gate.Replace($line,'    /* Test mutation: ControlRundown wait omitted. */')
    }
    $gateHeader=Join-Path $Directory 'context-gate.h';[IO.File]::WriteAllText($gateHeader,$gate,$encoding)
    return [ordered]@{variant=$Variant;driver=$source;driver_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();extractions=$metadata;
        callbacks=$header;callbacks_sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant();
        context_source=$gateSource;context_source_sha256=(Get-FileHash -LiteralPath $gateSource).Hash.ToLowerInvariant();
        context_header=$gateHeader;context_header_sha256=(Get-FileHash -LiteralPath $gateHeader).Hash.ToLowerInvariant()}
}
function Build-KernelProcessTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-pid-check','without-file-check','without-control-wait')][string]$Variant='current')
    $extraction=Write-KernelProcessCallbacks -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_PROCESS_CONTEXT_SOURCE "'+$extraction.context_header.Replace('\','/')+'"'
    $lines+='#define PB_PROCESS_CALLBACK_SOURCE "'+$extraction.callbacks.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\process_owner_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'process-owner.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",$entry,'common\v2_core.c')+$script:VersionArguments+@('/link',"/out:$exe")) | Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;extraction=$extraction}
}
