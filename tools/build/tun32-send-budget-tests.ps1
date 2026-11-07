function Build-Tun32SendBudgetTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,[ValidateSet(0,4,6)][int]$IpVersion=0)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'TUN32 send budget output must be absolute'}
    . (Join-Path $PSScriptRoot 'tun32-sys-tests.ps1')
    # Compile the existing32 host as a construction control, then reuse its
    # exact production bindings. Its executable is not run or counted as a case.
    $base=Build-Tun32SysTests -Directory (Join-Path $Directory 'base') -Direction send -Mode $Mode -Guarded:$Guarded -IpVersion $IpVersion
    $original=Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c';$test=Join-Path $script:RepoRoot 'tests\model\tun32_send_budget_tests.c'
    $before='#include "'+$original.Replace('\','/')+'"';$after='#include "'+$test.Replace('\','/')+'"'
    $body=[IO.File]::ReadAllText($base.entry)
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'TUN32 send budget entry binding not unique'}
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,$body.Replace($before,$after),[Text.UTF8Encoding]::new($false))
    $flags=@($base.arguments|Where-Object {$_ -notlike '/Fo*'})+"/Fo$Directory\"
    $exe=Join-Path $Directory 'tun32-send-budget.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common\packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;ip_version=$IpVersion;arguments=$flags;base=$base}
}
