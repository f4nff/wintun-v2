# Caller initializes common.ps1. Existing native-reclaim sources remain unchanged.
. (Join-Path $PSScriptRoot 'native-reclaim-tests.ps1')
function Build-LateCommitTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-drain-arm-guard')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Late Commit output must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Late Commit requires shipping source fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false)
    $cleanup=Write-NativeReclaimCleanup -Directory $Directory -Variant current
    $hostSource=Join-Path $script:RepoRoot 'tests\model\native_reclaim_tests.c'
    $hostHash='f1b676903eb875cda76863981e0837593b36c51837fceaa9cecdc60a7258c765'
    if((Get-FileHash -LiteralPath $hostSource).Hash.ToLowerInvariant() -ne $hostHash){throw 'Native reclamation host changed; review finite reuse'}
    $hostOriginal=Join-Path $Directory 'native-host-original.c'
    [IO.File]::WriteAllBytes($hostOriginal,[IO.File]::ReadAllBytes($hostSource))
    $queue=Join-Path $Directory 'queue-entry.h'
    [IO.File]::WriteAllText($queue,@'
#define PbQueuePush LatePush
#define ReleaseSRWLockShared LateRelease
#include PB_LATE_QUEUE_SOURCE
#undef PbQueuePush
#undef ReleaseSRWLockShared
'@,$encoding)
    $sessionSource=Join-Path $script:RepoRoot 'api\v2\session.c'
    $sessionOriginal=Join-Path $Directory 'session-original.c'
    [IO.File]::WriteAllBytes($sessionOriginal,[IO.File]::ReadAllBytes($sessionSource))
    $sessionText=[IO.File]::ReadAllText($sessionSource)
    $sessionChanges=[Collections.Generic.List[object]]::new()
    $sessionText=[regex]::Replace($sessionText,'(?m)^#include "([^"]+)"',{
        param($match)
        $path=if($match.Groups[1].Value -eq 'session_queue.h'){$queue}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $sessionSource) $match.Groups[1].Value))}
        $adapted='#include "'+$path.Replace('\','/')+'"'
        $sessionChanges.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$sessionText,$encoding)
    $sessionEntry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($sessionEntry,@'
#define AcquireSRWLockExclusive LateAcquire
#include PB_LATE_SESSION_SOURCE
#undef AcquireSRWLockExclusive
'@,$encoding)
    $hostChanges=[Collections.Generic.List[object]]::new()
    $hostText=[regex]::Replace([IO.File]::ReadAllText($hostSource),'(?m)^#include "([^"]+)"',{
        param($match)
        $path=if($match.Groups[1].Value -eq '../../api/v2/session.c'){$sessionEntry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $match.Groups[1].Value))}
        $adapted='#include "'+$path.Replace('\','/')+'"'
        $hostChanges.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    $projectedHost=Join-Path $Directory 'native-host.h';[IO.File]::WriteAllText($projectedHost,$hostText,$encoding)
    $drainSource=Join-Path $script:RepoRoot 'driver\v2\session_drain.h'
    $drain=[IO.File]::ReadAllText($drainSource);$marker='static __inline PB_RESULT PbV2ArmSubmissions('
    if([regex]::Matches($drain,[regex]::Escape($marker)).Count -ne 1){throw 'Actual ArmSubmissions marker not unique'}
    $offset=$drain.IndexOf($marker);$armOriginal=$drain.Substring($offset)
    $extracted=Join-Path $Directory 'arm-extracted.h';[IO.File]::WriteAllText($extracted,$armOriginal,$encoding)
    $before='    if (!ReadAcquire(&q->Session->Draining)) return PbQueueArm(&q->Rings[PB_QUEUE_RX_SUBMIT], ready);'
    $after='    if (ReadAcquire(&q->Session->DrainSnapshotReady) || !ReadAcquire(&q->Session->Draining)) return PbQueueArm(&q->Rings[PB_QUEUE_RX_SUBMIT], ready);'
    if([regex]::Matches($armOriginal,[regex]::Escape($before)).Count -ne 1){throw 'Draining arm guard not unique'}
    $selected=$armOriginal;$mutation=$null
    if($Variant -eq 'without-drain-arm-guard'){
        $selected=$selected.Replace($before,$after)
        $mutation=[ordered]@{original=$before;changed=$after;offset=$armOriginal.IndexOf($before);length=$before.Length}
    }
    $selectedArm=Join-Path $Directory 'selected-arm.h'
    [IO.File]::WriteAllText($selectedArm,"#define PbV2ArmSubmissions SelectedArmSubmissions`r`n"+$selected+"`r`n#undef PbV2ArmSubmissions`r`n",$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_NATIVE_RECLAIM_CLEANUP_SOURCE "'+$cleanup.header.Replace('\','/')+'"'
    $lines+='#define PB_LATE_RECLAIM_HOST "'+$projectedHost.Replace('\','/')+'"'
    $lines+='#define PB_LATE_ARM_SOURCE "'+$selectedArm.Replace('\','/')+'"'
    $lines+='#define PB_LATE_QUEUE_SOURCE "'+(Join-Path $script:RepoRoot 'api\v2\session_queue.h').Replace('\','/')+'"'
    $lines+='#define PB_LATE_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\late_commit_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`r`n")+"`r`n",$encoding)
    $exe=Join-Path $Directory 'late-commit.exe'
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Variant -eq 'without-drain-arm-guard'){$flags+='/DPB_LATE_COMMIT_WITHOUT_GUARD=1'}
    Invoke-BuildTool cl.exe ($flags+@($entry,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','common\flow.c','/link',"/out:$exe",'bcrypt.lib'))|Out-Host
    $sources=@($hostSource,$hostOriginal,$projectedHost,$sessionSource,$sessionOriginal,$session,$sessionEntry,$queue,$drainSource,$extracted,$selectedArm,$entry,
        (Join-Path $script:RepoRoot 'tests\model\late_commit_tests.c'),(Join-Path $script:RepoRoot 'tools\build\native-reclaim-tests.ps1'))|Select-Object -Unique|ForEach-Object {
        [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        entry=$entry;entry_sha256=(Get-FileHash -LiteralPath $entry).Hash.ToLowerInvariant();sources=@($sources);
        extractions=[ordered]@{host_source=$hostSource;host_source_sha256=$hostHash;host_original=$hostOriginal;host_original_sha256=(Get-FileHash $hostOriginal).Hash.ToLowerInvariant();
            host=$projectedHost;host_sha256=(Get-FileHash $projectedHost).Hash.ToLowerInvariant();host_include_changes=@($hostChanges);
            session_source=$sessionSource;session_original=$sessionOriginal;session_original_sha256=(Get-FileHash $sessionOriginal).Hash.ToLowerInvariant();
            session=$session;session_sha256=(Get-FileHash $session).Hash.ToLowerInvariant();session_include_changes=@($sessionChanges);session_entry=$sessionEntry;queue_entry=$queue;cleanup=$cleanup;
            arm=[ordered]@{source=$drainSource;source_sha256=(Get-FileHash $drainSource).Hash.ToLowerInvariant();marker=$marker;offset=$offset;length=$armOriginal.Length;
                original=$extracted;original_sha256=(Get-FileHash $extracted).Hash.ToLowerInvariant();selected=$selectedArm;selected_sha256=(Get-FileHash $selectedArm).Hash.ToLowerInvariant();mutation=$mutation};
            adaptation='Original host and full session are byte snapshots; only quoted includes become absolute and select recorded wrappers. Macro observers bind actual PbQueuePush, shared DataGate release, and DataGate exclusive acquire without changing their caller bodies. The exact arm function is separately macro-renamed SelectedArmSubmissions; the selected negative only bypasses the draining routing guard after a complete snapshot. Actual production worker/header and full actual Free remain unchanged.'}}
}
