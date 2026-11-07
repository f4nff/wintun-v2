# Current source only. Reuse the frozen complete NBL host with its original
# allocation/free/rundown hooks; production functions remain literal extracts.
function Build-Tun32WorkerPauseTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Worker pause output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Worker pause requires shipping fingerprint'}
    . (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
    $parts=Write-IndicationPauseParts -Directory (Join-Path $Directory 'parts')
    $utf8=[Text.UTF8Encoding]::new($false)
    $driver=Join-Path $script:RepoRoot 'driver/wintun.c';$body=[IO.File]::ReadAllText($driver)
    $first='static MINIPORT_RESTART TunRestart;';$last='static MINIPORT_PAUSE TunPause;'
    foreach($marker in @($first,$last)){if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'Restart markers must be unique'}}
    $start=$body.IndexOf($first);$end=$body.IndexOf($last,$start);if($end -le $start){throw 'Restart source span missing'}
    $restart=Join-Path $Directory 'restart.h';[IO.File]::WriteAllText($restart,$body.Substring($start,$end-$start),$utf8)
    $restartRecord=@{source=$driver;source_sha256=(Get-FileHash $driver).Hash.ToLowerInvariant();path=$restart;sha256=(Get-FileHash $restart).Hash.ToLowerInvariant();offset=$start;length=$end-$start;start_marker=$first;end_marker=$last}
    $production=@();foreach($relative in @(rg --files driver common include)){
        $source=Join-Path $script:RepoRoot $relative;$copy=Join-Path $Directory ('source-'+$relative.Replace('\','-').Replace('/','-'))
        [IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source))
        $production+=@{relative=$relative;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}
    }
    if($LASTEXITCODE){throw 'Cannot enumerate worker production closure'}
    $entry=Join-Path $Directory 'entry.c';$lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    $lines+='#define PB_WP_HOST "'+$parts.host.Replace('\','/')+'"'
    $lines+='#define PB_WP_RESTART "'+$restart.Replace('\','/')+'"'
    $lines+='#define PB_REFERENCE_SOURCE "'+(Join-Path $script:RepoRoot 'driver/v2/session_references.h').Replace('\','/')+'"'
    $lines+='#define PB_NBL_SOURCE "'+(Join-Path $script:RepoRoot 'driver/v2/session_nbl.h').Replace('\','/')+'"'
    foreach($part in $parts.parts){$lines+='#define PB_WP_'+$part.name.ToUpperInvariant()+' "'+$part.path.Replace('\','/')+'"'}
    $test=Join-Path $script:RepoRoot 'tests/model/tun32_worker_pause_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    [IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'tun32-worker-pause.exe'
    Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();
        test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;ip_version=$IpVersion;arguments=$flags;
        production=$production;extractions=$parts;restart=$restartRecord}
}
