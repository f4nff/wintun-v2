# Build helper only; caller initializes common.ps1 and records execution.
function Build-WaitRetirementTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-snapshot-gate')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Wait retirement output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$api=Join-Path $script:RepoRoot 'api\v2'
    $originalLife=Join-Path $api 'session_lifecycle.h';$selected=Join-Path $Directory 'session_lifecycle.h'
    [IO.File]::WriteAllBytes($selected,[IO.File]::ReadAllBytes($originalLife));$mutation=$null
    if($Variant -eq 'without-snapshot-gate') {
        $text=[IO.File]::ReadAllText($originalLife)
        $first='        uint32_t published = 0, failedQueues = 0; PB_RESULT result = PB_OK; BOOL ownershipValid = TRUE;'
        if([regex]::Matches($text,[regex]::Escape($first)).Count -ne 1){throw 'Lifecycle snapshot marker is not unique'}
        $start=$text.IndexOf($first);$last='        AcquireSRWLockExclusive(&s->State);';$end=$text.IndexOf($last,$start)
        if($end -le $start){throw 'Cannot isolate lifecycle ownership snapshot'}
        $before=$text.Substring($start,$end-$start);$acquire='        AcquireSRWLockExclusive(&s->DataGate);';$release='        ReleaseSRWLockExclusive(&s->DataGate);'
        foreach($part in @($acquire,$release)){if([regex]::Matches($before,[regex]::Escape($part)).Count -ne 1){throw 'Snapshot lock ownership pair is not unique'}}
        $after=$before.Replace($acquire,'        /* Observer mutation: snapshot DataGate acquire omitted. */').Replace($release,'        /* Observer mutation: matching snapshot DataGate release omitted. */')
        [IO.File]::WriteAllText($selected,$text.Substring(0,$start)+$after+$text.Substring($end),$encoding)
        $beforePath=Join-Path $Directory 'snapshot-original.c';$afterPath=Join-Path $Directory 'snapshot-without-gate.c'
        [IO.File]::WriteAllText($beforePath,$before,$encoding);[IO.File]::WriteAllText($afterPath,$after,$encoding)
        $mutation=[ordered]@{function='LifecycleWorker first ownership snapshot';removed=@($acquire,$release);offset=$start;length=$end-$start;
            original=$beforePath;original_sha256=(Get-FileHash $beforePath).Hash.ToLowerInvariant();changed=$afterPath;changed_sha256=(Get-FileHash $afterPath).Hash.ToLowerInvariant();
            teardown='All other owners joined; last owner completed every write before a held shared gate. QUERY observer records early snapshot, nativeActive1 prevents PREPARED. Release/join last owner before native0 and normal finalClose;121 after complete unload, no repair required.'}
    }
    $original=Join-Path $api 'session.c';$text=[IO.File]::ReadAllText($original)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($m);$include=if($m.Groups[1].Value -eq 'session_lifecycle.h'){$selected}else{[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#define WaitForMultipleObjects OwnerWait
#define ReleaseSRWLockShared RetireRelease
#undef AcquireSRWLockExclusive
#define AcquireSRWLockExclusive RetireAcquire
#undef DeviceIoControl
#define DeviceIoControl RetireIo
#include PB_WAIT_RETIREMENT_SESSION_SOURCE
#undef WaitForMultipleObjects
#undef ReleaseSRWLockShared
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_WAIT_RETIREMENT_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\wait_retirement_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($source,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $exe=Join-Path $Directory 'wait-retirement.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\wait_retirement_tests.c','/link',"/out:$exe"))|Out-Host
    $dll=Join-Path $Directory "wait-retirement-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "wait-retirement-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib'))|Out-Host
    $sources=@($original,$originalLife,$selected,$session,$entry,$source,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\wait_retirement_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\wait_retirement_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\wait_retirement_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;session=$session;entry=$entry;shim=$source;sources=@($sources);mutation=$mutation;
        extraction=[ordered]@{source=$originalLife;source_sha256=(Get-FileHash $originalLife).Hash.ToLowerInvariant();selected=$selected;selected_sha256=(Get-FileHash $selected).Hash.ToLowerInvariant();
            adaptation='Current lifecycle is byte-identical; session only quoted include paths become absolute, selecting the recorded lifecycle. Real SDK wait/release/acquire/I/O boundaries bind observations in the unchanged native-policy host.'}}
}
