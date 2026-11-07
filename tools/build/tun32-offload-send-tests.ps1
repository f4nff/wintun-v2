function Build-Tun32OffloadSendTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,[ValidateSet('current','checksum-gated-lso')][string]$Variant='current')
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'TUN32 offload send output must be absolute'}
    . (Join-Path $PSScriptRoot 'tun32-sys-tests.ps1')
    # Existing unchanged host/WDK construction control is compiled, not run.
    $base=Build-Tun32SysTests -Directory (Join-Path $Directory 'base') -Direction send -IpVersion $IpVersion -Mode $Mode -Guarded:$Guarded
    $encoding=[Text.UTF8Encoding]::new($false)
    function Replace-O32Once([string]$text,[string]$before,[string]$after){if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw "Nonunique offload send projection:$before"};return $text.Replace($before,$after)}
    $lsoSource=Join-Path $script:RepoRoot 'driver\v2\offload_probe.h';$text=[IO.File]::ReadAllText($lsoSource)
    $lso=[regex]::Match($text,'(?ms)^static VOID PbSetLsoCompletion\(NET_BUFFER_LIST \*nbls\)\r?\n\{.*?^\}')
    if(!$lso.Success -or [regex]::Matches($text,'static VOID PbSetLsoCompletion\(').Count -ne 1){throw 'Actual LSO completion span changed'}
    $lsoPath=Join-Path $Directory 'lso-completion.h';[IO.File]::WriteAllText($lsoPath,$lso.Value,$encoding)
    $completion=@{source=$lsoSource;source_sha256=(Get-FileHash $lsoSource).Hash.ToLowerInvariant();offset=$lso.Index;length=$lso.Length;path=$lsoPath;sha256=(Get-FileHash $lsoPath).Hash.ToLowerInvariant()}
    $hostRecord=@($base.extractions.generated|Where-Object name -eq 'send-window')[0];$body=[IO.File]::ReadAllText($hostRecord.path)
    $body=Replace-O32Once $body 'static VOID PbSetLsoCompletion(NET_BUFFER_LIST *nbls) { (void)nbls; AssertUnlocked(); }' '#include PB_TUN32_LSO_COMPLETION'
    $mask=[regex]::Match($body,'(?ms)^static ULONG PbEffectiveMask\(TUN_CTX \*ctx\)\r?\n\{.*?^\}');if(!$mask.Success){throw 'Frozen effective-mask stand-in changed'}
    $policyHeader=(Join-Path $script:RepoRoot 'common\offload_policy.h').Replace('\','/')
    $policy='#include "'+$policyHeader+'"'+"`n"+'static ULONG PbEffectiveMask(TUN_CTX *ctx){CHECK(ctx==Current);return PbResolveOffloads(O32Allowed,O32Requested,PB_PROBE_SUPPORTED,TRUE);}'
    $body=Replace-O32Once $body $mask.Value $policy
    $body=Replace-O32Once $body "static VOID OnWindowEnd(VOID)`r`n{" "static VOID OnWindowEnd(VOID)`r`n{`r`n    O32Boundary();"
    $hostPath=Join-Path $Directory 'send-offload-host.h';[IO.File]::WriteAllText($hostPath,$body,$encoding)
    $hostProjection=@{before_path=$hostRecord.path;before_sha256=$hostRecord.sha256;path=$hostPath;sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();mask_offset=$mask.Index;mask_length=$mask.Length;
        method='Only replace LSO completion stub with actual function include; effective-mask stand-in with actual policy/platform pure functions over modeled state; add observer at original unlocked OnWindowEnd. Preserve all other frozen-host/base-projection text.'}
    $entry=[IO.File]::ReadAllText($base.entry)
    $entry=Replace-O32Once $entry ('#define PB_SEND_DESCRIBE_HOST "'+$hostRecord.path.Replace('\','/')+'"') ('#define PB_SEND_DESCRIBE_HOST "'+$hostPath.Replace('\','/')+'"')
    $original=Join-Path $script:RepoRoot 'tests\model\tun32_sys_tests.c';$test=Join-Path $script:RepoRoot 'tests\model\tun32_offload_send_tests.c'
    $entry=Replace-O32Once $entry ('#include "'+$original.Replace('\','/')+'"') ('#include "'+$test.Replace('\','/')+'"')
    $entry='#define PB_TUN32_LSO_COMPLETION "'+$lsoPath.Replace('\','/')+'"'+"`n"+$entry
    $entryPath=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entryPath,$entry,$encoding)
    $policySource=Join-Path $script:RepoRoot 'common\offload_policy.c';$policyPath=$policySource
    if($Variant -ceq 'checksum-gated-lso'){
        $policySource=Join-Path $script:RepoRoot 'out\packet-edge-baseline\6fb17007a923\common\offload_policy.c'
        if((Get-FileHash $policySource).Hash.ToLowerInvariant() -cne 'a3a61042c8dc48ea02af9201fccf57c1da88a3cc52abf5de7c7fc56ffbd39374'){throw 'Old checksum-gated LSO policy raw source changed'}
        $policyPath=Join-Path $Directory 'offload-policy-before.c';[IO.File]::WriteAllBytes($policyPath,[IO.File]::ReadAllBytes($policySource))
    }
    $policyRecord=@{variant=$Variant;source=$policySource;source_sha256=(Get-FileHash $policySource).Hash.ToLowerInvariant();compiled_path=$policyPath;sha256=(Get-FileHash $policyPath).Hash.ToLowerInvariant();shipping=($Variant -ceq 'current')}
    $flags=@($base.arguments|Where-Object {$_ -notlike '/Fo*'})+('/I'+(Join-Path $script:RepoRoot 'common'))+"/Fo$Directory\";$exe=Join-Path $Directory 'tun32-offload-send.exe'
    Invoke-BuildTool cl.exe ($flags+@($entryPath,'common\packet.c',$policyPath,'/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entryPath;entry_sha256=(Get-FileHash $entryPath).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;policy=$policyRecord;test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();arguments=$flags;base=$base;lso_completion=$completion;host_projection=$hostProjection}
}
