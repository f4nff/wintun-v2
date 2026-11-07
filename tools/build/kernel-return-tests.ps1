# Build helper only: caller initializes tools/build/common.ps1 and owns recording.
function Write-KernelReturnCallbacks {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-outer-rundown')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $driver=Join-Path $script:RepoRoot 'driver\wintun.c'
    $text=[IO.File]::ReadAllText($driver)
    $returnStart=$text.IndexOf('static MINIPORT_RETURN_NET_BUFFER_LISTS TunReturnNetBufferLists;')
    $returnEnd=$text.IndexOf('#define TUN_FORCE_UNREGISTRATION',$returnStart)
    $haltStart=$text.IndexOf('static MINIPORT_HALT TunHaltEx;')
    $haltEnd=$text.IndexOf('static MINIPORT_SHUTDOWN TunShutdownEx;',$haltStart)
    if($returnStart -lt 0 -or $returnEnd -le $returnStart -or $haltStart -lt 0 -or $haltEnd -le $haltStart){throw 'Cannot isolate real outer Return/Halt callbacks'}
    $return=$text.Substring($returnStart,$returnEnd-$returnStart)
    $halt=$text.Substring($haltStart,$haltEnd-$haltStart)
    $encoding=[Text.UTF8Encoding]::new($false)
    $returnFile=Join-Path $Directory 'return-extracted.h';[IO.File]::WriteAllText($returnFile,$return,$encoding)
    $haltFile=Join-Path $Directory 'halt-extracted.h';[IO.File]::WriteAllText($haltFile,$halt,$encoding)
    $wait='    ExWaitForRundownProtectionRelease(&Ctx->Probe.ReturnRundown);'
    if([regex]::Matches($halt,[regex]::Escape($wait)).Count -ne 1){throw 'Outer Halt barrier is not unique'}
    if($Variant -eq 'without-outer-rundown'){$halt=$halt.Replace($wait,'    /* Test mutation: outer callback rundown wait omitted. */')}
    $header=Join-Path $Directory 'outer-callbacks.h'
    [IO.File]::WriteAllText($header,"#pragma once`r`n"+$return+$halt,$encoding)
    return [ordered]@{variant=$Variant;header=$header;header_sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant();
        driver=$driver;driver_sha256=(Get-FileHash -LiteralPath $driver).Hash.ToLowerInvariant();
        return_extraction=$returnFile;return_sha256=(Get-FileHash -LiteralPath $returnFile).Hash.ToLowerInvariant();
        halt_extraction=$haltFile;halt_sha256=(Get-FileHash -LiteralPath $haltFile).Hash.ToLowerInvariant();
        return_offset=$returnStart;return_length=$return.Length;halt_offset=$haltStart;halt_length=($haltEnd-$haltStart)}
}
function Build-KernelReturnTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-outer-rundown')][string]$Variant='current')
    $extraction=Write-KernelReturnCallbacks -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_OUTER_CALLBACK_SOURCE "'+$extraction.header.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\outer_return_halt_tests.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $source -Encoding utf8
    $exe=Join-Path $Directory 'outer.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",$source,'common\v2_core.c','common\ethernet.c')+$script:VersionArguments+@('/link',"/out:$exe")) | Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();volatile=$Mode;guarded=[bool]$Guarded;extraction=$extraction}
}
