# Build helper only. Caller initializes common.ps1 and owns test recording.
function Write-KernelRegister {
    param([Parameter(Mandatory)][string]$Directory,
        [ValidateSet('current','without-alias-check','start-before-publish')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'driver\v2\session.h'
    $body=[IO.File]::ReadAllText($source)
    $start=$body.IndexOf('static NTSTATUS PbV2Register(TUN_CTX *ctx, IRP *irp)')
    if($start -lt 0){throw 'Cannot find actual register function'}
    $end=$body.IndexOf('#include "session_unregister.h"',$start)
    if($end -le $start){throw 'Cannot isolate actual register function'}
    $function=$body.Substring($start,$end-$start)
    $encoding=[Text.UTF8Encoding]::new($false)
    $original=Join-Path $Directory 'register-extracted.h';[IO.File]::WriteAllText($original,$function,$encoding)
    if($Variant -eq 'without-alias-check'){
        $first=$function.IndexOf('        if (q->KernelEvent == q->UserEvent')
        $last=$function.IndexOf('        q->TxSlots =',$first)
        if($first -lt 0 -or $last -le $first){throw 'Cannot isolate alias checks'}
        $function=$function.Substring(0,$first)+"        /* Test mutation: event object alias checks omitted. */`r`n"+$function.Substring($last)
    }elseif($Variant -eq 'start-before-publish'){
        $line='    KeSetEvent(&s->StartEvent, IO_NO_INCREMENT, FALSE);'
        $anchor='    KIRQL irql = ExAcquireSpinLockExclusive(&ctx->TransitionLock);'
        if([regex]::Matches($function,[regex]::Escape($line)).Count -ne 1 -or [regex]::Matches($function,[regex]::Escape($anchor)).Count -ne 1){throw 'Cannot isolate startup publication order'}
        $function=$function.Replace($line,'    /* Test mutation: startup moved before publication. */').Replace($anchor,$line+"`r`n"+$anchor)
    }
    $header=Join-Path $Directory 'register-function.h';[IO.File]::WriteAllText($header,"#pragma once`r`n"+$function,$encoding)
    return [ordered]@{variant=$Variant;source=$source;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();
        extracted=$original;extracted_sha256=(Get-FileHash -LiteralPath $original).Hash.ToLowerInvariant();offset=$start;length=$end-$start;
        header=$header;header_sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant()}
}
function Build-KernelRegisterTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-alias-check','start-before-publish')][string]$Variant='current')
    $extraction=Write-KernelRegister -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_REGISTER_SOURCE "'+$extraction.header.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\register_owner_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'register.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",$entry,'common\v2_core.c')+$script:VersionArguments+@('/link',"/out:$exe")) | Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;extraction=$extraction}
}
