function Build-CompletionArm32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Completion arm output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Completion arm requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
    $frozen=[ordered]@{
        'tests/model/native_reclaim_tests.c'='f1b676903eb875cda76863981e0837593b36c51837fceaa9cecdc60a7258c765'
        'tests/model/late_commit_tests.c'='3ae8a974a8d121f47d28ba19bfb88d80f4e7498f70c83a104787fa216e81c17f'
        'tests/model/kernel_nbl_fixture.h'='c236a169004e4851111c1000353b85685dbe92b32191e8cb28509989f19d0eb3'
        'tests/safety/guard_memory.h'='ca169cf1a4b11506dc45572b60d61d8b05cf8a73bf7ba424b04dca228637906b'
    }
    $inputs=@();foreach($relative in $frozen.Keys){
        $source=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash $source).Hash.ToLowerInvariant();if($hash -cne $frozen[$relative]){throw "Frozen completion arm input differs: $relative"}
        $copy=Join-Path $Directory ('original-'+[IO.Path]::GetFileName($source));[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes($source))
        $inputs+=@{relative=$relative;source=$source;source_sha256=$hash;path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant()}
    }
    $parts=@();foreach($name in 'session','queue','open-close'){
        $relative=switch($name){'session'{'api/v2/session.c'}'queue'{'api/v2/session_queue.h'}'open-close'{'tests/model/late_commit_tests.c'}}
        $source=Join-Path $script:RepoRoot $relative;$body=[IO.File]::ReadAllText($source);$start=0;$end=$body.Length
        if($name -ceq 'open-close'){$start=$body.IndexOf('static VOID OpenPeer(');$end=$body.IndexOf('static VOID RunLate(',$start);if($start -lt 0 -or $end -le $start){throw 'Completion arm Open/Finish span missing'}}
        $path=Join-Path $Directory "$name-original.h";[IO.File]::WriteAllText($path,$body.Substring($start,$end-$start),$utf8)
        $parts+=@{name=$name;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$start;length=$end-$start;path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $queueEntry=Join-Path $Directory 'queue-entry.h';$queue=$parts|Where-Object name -ceq 'queue'
    $lines=@('#define PbQueueArm ArmObserved','#define WaitForMultipleObjects WaitObserved',('#include "'+$queue.path.Replace('\','/')+'"'),'#undef WaitForMultipleObjects','#undef PbQueueArm')
    [IO.File]::WriteAllText($queueEntry,($lines -join "`n")+"`n",$utf8)
    $sessionSource=Join-Path $script:RepoRoot 'api/v2/session.c';$body=[IO.File]::ReadAllText($sessionSource)
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq 'session_queue.h'){$queueEntry}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $sessionSource) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
    $session=Join-Path $Directory 'session-current.c';[IO.File]::WriteAllText($session,$body,$utf8)
    $hostSource=Join-Path $script:RepoRoot 'tests/model/native_reclaim_tests.c';$body=[IO.File]::ReadAllText($hostSource)
    $guard='adapter&&lists&&count==2&&!flags';if([regex]::Matches($body,[regex]::Escape($guard)).Count -ne 1){throw 'Completion indication count guard not unique'}
    $body=$body.Replace($guard,'adapter&&lists&&count==32&&!flags')
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)$path=if($m.Groups[1].Value -ceq '../../api/v2/session.c'){$session}else{[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $m.Groups[1].Value))};'#include "'+$path.Replace('\','/')+'"'})
    $hostPath=Join-Path $Directory 'native-host.h';[IO.File]::WriteAllText($hostPath,$body,$utf8)
    $open=$parts|Where-Object name -ceq 'open-close';$test=Join-Path $script:RepoRoot 'tests/model/completion_arm32_tests.c';$entry=Join-Path $Directory 'entry.c'
    $bindings=[ordered]@{PB_NATIVE_RECLAIM_CLEANUP_SOURCE=(Join-Path $script:RepoRoot 'driver/v2/session_cleanup.h');PB_ARM_HOST=$hostPath;PB_ARM_OPEN_CLOSE=$open.path}
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_TEST_IP_VERSION '+$IpVersion
    foreach($name in $bindings.Keys){$lines+='#define '+$name+' "'+$bindings[$name].Replace('\','/')+'"'}
    $lines+='#include "'+$test.Replace('\','/')+'"';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$utf8)
    $production=@();foreach($relative in @(rg --files api driver common include)){
        $source=Join-Path $script:RepoRoot $relative;$path=Join-Path $Directory ('source-'+$relative.Replace('\','-').Replace('/','-'));[IO.File]::WriteAllBytes($path,[IO.File]::ReadAllBytes($source))
        $production+=@{relative=$relative;source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    };if($LASTEXITCODE){throw 'Cannot enumerate completion arm production closure'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $Directory 'completion-arm32.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'common/v2_core.c','common/ethernet.c','common/coalesce.c','common/packet.c','common/flow.c','/link',"/out:$exe",'bcrypt.lib'))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;arguments=$flags;inputs=$inputs;parts=$parts;production=$production;
        host=$hostPath;host_sha256=(Get-FileHash $hostPath).Hash.ToLowerInvariant();session=$session;session_sha256=(Get-FileHash $session).Hash.ToLowerInvariant();queue_entry=$queueEntry;queue_entry_sha256=(Get-FileHash $queueEntry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant()}
}
