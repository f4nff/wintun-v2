# Caller initializes common.ps1 and supplies the shipping source fingerprint.
function Write-TerminalWakeHeaders {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('current','without-terminal-broadcast')][string]$Variant='current')
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$metadata=@()
    foreach($name in @('session_progress.h','session_fault.h','session_references.h','session_drain.h','session_recycle.h')){
        $source=Join-Path $script:RepoRoot "driver\v2\$name";$contents=[IO.File]::ReadAllText($source);$changed=$false
        if($name -eq 'session_progress.h' -and $Variant -eq 'without-terminal-broadcast'){
            $anchor='    if (terminal) for (ULONG i = 0; i < s->Layout.QueueCount; ++i)'
            if([regex]::Matches($contents,[regex]::Escape($anchor)).Count -ne 1){throw 'Terminal broadcast is not unique'}
            $contents=$contents.Replace($anchor,'    if (FALSE) for (ULONG i = 0; i < s->Layout.QueueCount; ++i) /* test mutation */')
            $changed=$true
        }
        $target=Join-Path $Directory $name
        if($changed){[IO.File]::WriteAllText($target,$contents,$encoding)}else{[IO.File]::WriteAllBytes($target,[IO.File]::ReadAllBytes($source))}
        $metadata+=@{source=$source;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();header=$target;header_sha256=(Get-FileHash -LiteralPath $target).Hash.ToLowerInvariant()}
    }
    $header=Join-Path $Directory 'terminal-kernel.h'
    @('#pragma once','#include "session_fault.h"','#include "session_references.h"','#include "session_drain.h"','#include "session_recycle.h"')|Set-Content -LiteralPath $header -Encoding utf8
    return [ordered]@{variant=$Variant;header=$header;copies=$metadata}
}
function Build-TerminalWakeTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-terminal-broadcast')][string]$Variant='current')
    $extraction=Write-TerminalWakeHeaders -Directory $Directory -Variant $Variant
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_TERMINAL_KERNEL_SOURCE "'+$extraction.header.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\terminal_wake_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';$lines|Set-Content -LiteralPath $entry -Encoding utf8
    $exe=Join-Path $Directory 'terminal-wake.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\",$entry,'common\v2_core.c')+$script:VersionArguments+@('/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;extraction=$extraction}
}
