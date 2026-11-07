function Build-Tun32ChecksumMetadataTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','tasks-before-family')][string]$Variant='current')
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Checksum metadata output must be absolute'}
    . (Join-Path $PSScriptRoot 'tun32-offload-send-tests.ps1')
    $base=Build-Tun32OffloadSendTests -Directory (Join-Path $Directory 'base') -IpVersion $IpVersion -Mode $Mode -Guarded:$Guarded
    $old=Join-Path $script:RepoRoot 'tests\model\tun32_offload_send_tests.c';$test=Join-Path $script:RepoRoot 'tests\model\tun32_checksum_metadata_tests.c'
    $hostBody=[IO.File]::ReadAllText($old);$main='int main(int argc,char **argv)'
    if([regex]::Matches($hostBody,[regex]::Escape($main)).Count -ne 1){throw 'Checksum original main binding changed'}
    $hostBody=$hostBody.Replace($main,'int UnusedOffload32Main(int argc,char **argv)')
    foreach($relative in @('../../include/packetbridge_version.h','../../common/platform.h')){
        $include='#include "'+$relative+'"';if([regex]::Matches($hostBody,[regex]::Escape($include)).Count -ne 1){throw 'Checksum original relative include changed'}
        $absolute=[IO.Path]::GetFullPath((Join-Path (Split-Path $old) $relative)).Replace('\','/');$hostBody=$hostBody.Replace($include,('#include "'+$absolute+'"'))
    }
    $hostPath=Join-Path $Directory 'offload32-host.h';[IO.File]::WriteAllText($hostPath,$hostBody,[Text.UTF8Encoding]::new($false))
    $before='#include "'+$old.Replace('\','/')+'"';$after='#include "'+$test.Replace('\','/')+'"';$body=[IO.File]::ReadAllText($base.entry)
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'Checksum metadata include binding changed'}
    $body='#define PB_CHECKSUM_OFFLOAD_HOST "'+$hostPath.Replace('\','/')+'"'+"`n"+$body.Replace($before,$after)
    $describe=Join-Path $script:RepoRoot 'driver\v2\session_describe.h';$compiledDescribe=$describe
    if($Variant -ceq 'tasks-before-family'){
        $describe=Join-Path $script:RepoRoot 'out\return-wake-baseline\4fd68e95547d\driver\v2\session_describe.h'
        if((Get-FileHash $describe).Hash.ToLowerInvariant() -cne '79aa2380c77311ef9bf5353cdc2ee0b75bac2126a858d40bbddbd2e37fe6f694'){throw 'Checksum metadata original raw Describe changed'}
        $compiledDescribe=Join-Path $Directory 'describe-before.h';[IO.File]::WriteAllBytes($compiledDescribe,[IO.File]::ReadAllBytes($describe))
        $binding='#define PB_SEND_DESCRIBE_ACTUAL "'+(Join-Path $script:RepoRoot 'driver\v2\session_describe.h').Replace('\','/')+'"'
        if([regex]::Matches($body,[regex]::Escape($binding)).Count -ne 1){throw 'Checksum Describe source binding changed'}
        $body=$body.Replace($binding,('#define PB_SEND_DESCRIBE_ACTUAL "'+$compiledDescribe.Replace('\','/')+'"'))
    }
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,$body,[Text.UTF8Encoding]::new($false))
    $flags=@($base.arguments|Where-Object {$_ -notlike '/Fo*'})+"/Fo$Directory\";$exe=Join-Path $Directory 'checksum-metadata.exe'
    Invoke-BuildTool cl.exe ($flags+@($entry,'common\packet.c','common\offload_policy.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();
        ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;arguments=$flags;test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();base=$base;
        describe=@{source=$describe;source_sha256=(Get-FileHash $describe).Hash.ToLowerInvariant();path=$compiledDescribe;sha256=(Get-FileHash $compiledDescribe).Hash.ToLowerInvariant();shipping=($Variant -ceq 'current')};
        host=@{source=$old;source_sha256=(Get-FileHash $old).Hash.ToLowerInvariant();path=$hostPath;sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant()}}
}
