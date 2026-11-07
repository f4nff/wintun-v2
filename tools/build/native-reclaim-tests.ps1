# Actual DLL session.c and SYS headers are included by the standalone fixture.
function Write-NativeReclaimCleanup {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-returns-wait')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'driver\v2\session_cleanup.h';$text=[IO.File]::ReadAllText($source)
    $offset=$text.IndexOf('static VOID PbV2Free(');if($offset -lt 0){throw 'Cannot isolate actual SYS Free'}
    $original=$text.Substring($offset);$encoding=[Text.UTF8Encoding]::new($false)
    $extracted=Join-Path $Directory 'free-extracted.h';[IO.File]::WriteAllText($extracted,$original,$encoding)
    $body=$original;$line='    ExWaitForRundownProtectionRelease(&s->Returns);'
    if([regex]::Matches($body,[regex]::Escape($line)).Count -ne 1){throw 'Returns barrier is not unique'}
    if($Variant -eq 'without-returns-wait'){$body=$body.Replace($line,'    /* test mutation: session Returns wait omitted */')}
    $header=Join-Path $Directory 'native-free.h';[IO.File]::WriteAllText($header,"#pragma once`r`n"+$body,$encoding)
    return [ordered]@{variant=$Variant;source=$source;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();
        offset=$offset;length=$original.Length;extracted=$extracted;extracted_sha256=(Get-FileHash -LiteralPath $extracted).Hash.ToLowerInvariant();
        header=$header;header_sha256=(Get-FileHash -LiteralPath $header).Hash.ToLowerInvariant()}
}
function Build-NativeReclaimTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-returns-wait')][string]$Variant='current')
    $extraction=Write-NativeReclaimCleanup -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_NATIVE_RECLAIM_CLEANUP_SOURCE "'+$extraction.header.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\native_reclaim_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'native-reclaim.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",
        $entry,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','common\flow.c')+$script:VersionArguments+@('/link',"/out:$exe",'bcrypt.lib'))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;extraction=$extraction}
}
