function Build-LsoChecksumPolicyTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'LSO checksum policy output must be absolute'}
    . (Join-Path $PSScriptRoot 'offload-handler-tests.ps1')
    $base=Build-OffloadHandlerTests -Directory (Join-Path $Directory 'base') -Mode $Mode -Guarded:$Guarded
    $test=Join-Path $script:RepoRoot 'tests\model\lso_checksum_policy_tests.c';$oldTest=Join-Path $script:RepoRoot 'tests\model\offload_handler_tests.c'
    $before='#include "'+$oldTest.Replace('\','/')+'"';$after='#include "'+$test.Replace('\','/')+'"';$body=[IO.File]::ReadAllText($base.entry)
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'LSO checksum policy source binding changed'}
    $report=@($base.extractions.parts|Where-Object name -eq 'report');if($report.Count -ne 1){throw 'Unique LSO report span required'}
    $observer=Join-Path $Directory 'report-observer.h'
    $observerBody='static VOID ObserveLsoReport(VOID *,NDIS_STATUS_INDICATION *);'+"`n"+'#define NdisMIndicateStatusEx ObserveLsoReport'+"`n"+'#include "'+$report[0].path.Replace('\','/')+'"'+"`n"+'#undef NdisMIndicateStatusEx'+"`n"
    [IO.File]::WriteAllText($observer,$observerBody,[Text.UTF8Encoding]::new($false))
    $original='#define PB_OFFLOAD_REPORT_SOURCE "'+$report[0].path.Replace('\','/')+'"';$replacement='#define PB_OFFLOAD_REPORT_SOURCE "'+$observer.Replace('\','/')+'"'
    if([regex]::Matches($body,[regex]::Escape($original)).Count -ne 1){throw 'LSO report source binding changed'}
    $body='#define PB_TEST_IP_VERSION '+$IpVersion+"`n"+$body.Replace($before,$after).Replace($original,$replacement)
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,$body,[Text.UTF8Encoding]::new($false))
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",
        ('/I'+(Join-Path $script:WdkRoot "Include\$script:KitVersion\km")),"/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'lso-checksum-policy.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common\offload_policy.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();observer=$observer;observer_sha256=(Get-FileHash $observer).Hash.ToLowerInvariant();base=$base}
}
