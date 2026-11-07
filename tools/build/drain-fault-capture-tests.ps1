# Use original production headers directly; archive byte-identical evidence.
function Build-DrainFaultCaptureTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Drain fault capture output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Drain fault capture requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
    $parts=@();$headers=@('drain','return','completion','fault','fault_drain','progress','references','query','worker','nbl','receive')
    foreach($name in $headers){
        $source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$path=Join-Path $Directory "evidence-$name.h";[IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes($source))
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=0;length=[IO.File]::ReadAllText($source).Length;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant();compiled_path=$source}
    }
    $frozen=[ordered]@{
        'tests\model\drain_boundary_tests.c'='4fd8afe2b998c6ca628ff05fa266b689e4602be4e68109a477cc5f3001fa7df5'
        'tests\model\kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
    }
    $hosts=@();foreach($relative in $frozen.Keys){
        $source=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $source).Hash.ToLowerInvariant();if($hash -cne $frozen[$relative]){throw "Frozen capture host changed:$relative"}
        $path=Join-Path $Directory ('original-'+[IO.Path]::GetFileName($relative));[IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes($source))
        $hosts+=@{relative=$relative;source=$source;sha256=$hash;original=$path;original_sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $hostSource=Join-Path $script:RepoRoot 'tests\model\drain_boundary_tests.c';$body=[IO.File]::ReadAllText($hostSource)
    $before='PB_RX_SLOT Slots[2][PB_MAX_SLOTS];PB_DESCRIPTOR Pending[2][512];KEVENT Kernel[2],User[2];'
    $after='PB_RX_SLOT Slots[PB_MAX_QUEUES][PB_MAX_SLOTS];PB_DESCRIPTOR Pending[PB_MAX_QUEUES][512];KEVENT Kernel[PB_MAX_QUEUES],User[PB_MAX_QUEUES];'
    if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw 'Two-queue fixture typedef boundary must be unique'}
    $body=$body.Replace($before,$after);$includes=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'"([^"\r\n]*\.h)"',{
        param($m);$path=$m.Groups[1].Value
        if(![IO.Path]::IsPathFullyQualified($path)){$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $path))}
        $value='"'+$path.Replace('\','/')+'"';$includes.Add(@{original=$m.Value;adapted=$value});$value
    })
    $adaptedHost=Join-Path $Directory 'drain-host.h';[IO.File]::WriteAllText($adaptedHost,$body,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_DRAIN_FAULT_CAPTURE_HOST "'+$adaptedHost.Replace('\','/')+'"';$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\drain_fault_capture_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'drain-fault-capture.exe'
    Invoke-BuildTool cl.exe ($flags+@($entry,'common\v2_core.c','common\coalesce.c','common\packet.c','common\ethernet.c','/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;ndebug=$true;pb_api_tests=$false;arguments=$flags;
        extractions=@{parts=$parts;hosts=$hosts;host=$adaptedHost;host_sha256=(Get-FileHash $adaptedHost).Hash.ToLowerInvariant();typedef_original=$before;typedef_adapted=$after;include_changes=@($includes.ToArray());
            method='Production headers compile directly from their unchanged original paths. Whole byte-identical evidence snapshots are archived, not alternate implementations. Only the frozen drain-boundary host typedef expands its four per-queue arrays to PB_MAX_QUEUES and normalizes quoted header paths; old main is renamed by a token macro in the new test. The complete kernel NBL fixture is unchanged.'}}
}
