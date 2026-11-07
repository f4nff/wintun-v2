# Actual complete runtime/namespace/scheduler and exact cleanup/Shutdown regions.
function Build-CleanupNamespaceTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-pending-boundary-delete')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Cleanup/namespace output must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$sources=[Collections.Generic.List[object]]::new();$parts=[Collections.Generic.List[object]]::new()
    function Read-CleanupNamespace([string]$relative,[string]$hash){
        $path=Join-Path $script:RepoRoot $relative;$actual=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();if($actual -ne $hash){throw "Unreviewed cleanup/namespace source: $relative"}
        $sources.Add(@{relative=$relative;path=$path;sha256=$actual});return [IO.File]::ReadAllText($path)
    }
    function Normalize-CleanupNamespace([string]$text,[string]$relative){
        $base=Split-Path (Join-Path $script:RepoRoot $relative);return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
    }
    function Save-CleanupNamespace([string]$name,[string]$text,[string]$source,[int]$offset=-1,[int]$length=-1){
        $path=Join-Path $Directory $name;[IO.File]::WriteAllText($path,$text,$encoding);$parts.Add(@{name=$name;path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();source=$source;offset=$offset;length=$length});return $path
    }
    $mainText=Read-CleanupNamespace 'api\main.c' '543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'
    $null=Save-CleanupNamespace 'main-original.c' $mainText 'api\main.c' 0 $mainText.Length
    $main=Save-CleanupNamespace 'main-actual.h' (Normalize-CleanupNamespace $mainText 'api\main.c') 'api\main.c'
    $namespaceText=Read-CleanupNamespace 'api\namespace.c' 'c8fe5ff67df4c1ddaebfde58385a7adef8831ff661c082ef8ef7e6f4a1252c97'
    $null=Save-CleanupNamespace 'namespace-original.c' $namespaceText 'api\namespace.c' 0 $namespaceText.Length
    $before='    if (PendingBoundary) DeleteBoundaryDescriptor(PendingBoundary);';$after='    /* Observer128: omit only pending failure boundary deletion. */'
    if([regex]::Matches($namespaceText,[regex]::Escape($before)).Count -ne 1){throw 'Pending boundary delete is not unique'}
    if($Variant -eq 'without-pending-boundary-delete'){$namespaceText=$namespaceText.Replace($before,$after)}
    $namespace=Save-CleanupNamespace 'namespace-actual.h' (Normalize-CleanupNamespace $namespaceText 'api\namespace.c') 'api\namespace.c'
    $schedulerText=Read-CleanupNamespace 'api\cleanup.h' 'b02f11371520e0133726274703bb556fb77a88a23735e72f2b2939a566802421'
    $scheduler=Save-CleanupNamespace 'scheduler-actual.h' (Normalize-CleanupNamespace $schedulerText 'api\cleanup.h') 'api\cleanup.h'
    $adapter=Read-CleanupNamespace 'api\adapter.c' 'e604c130f502dd453332236f7e02a95c54fbe3e3ceaa3df3e6092e4bed79215f'
    $first='static VOID CleanupRun(VOID)';$next='BOOL WINAPI WintunShutdown(DWORD Timeout)';$end='VOID AdapterCleanupDetach(VOID)'
    foreach($marker in @($first,$next,$end)){if([regex]::Matches($adapter,[regex]::Escape($marker)).Count -ne 1){throw 'Cleanup/Shutdown extraction markers are not unique'}}
    $a=$adapter.IndexOf($first);$b=$adapter.IndexOf($next,$a);$c=$adapter.IndexOf($end,$b);if($a -lt 0 -or $b -le $a -or $c -le $b){throw 'Cannot extract complete cleanup/Shutdown regions'}
    $run=Save-CleanupNamespace 'cleanup-run-actual.h' $adapter.Substring($a,$b-$a) 'api\adapter.c' $a ($b-$a)
    $shutdown=Save-CleanupNamespace 'shutdown-actual.h' $adapter.Substring($b,$c-$b) 'api\adapter.c' $b ($c-$b)
    $null=Read-CleanupNamespace 'api\logger.c' 'e4e355f19691248274f36d831142a77266ff1ff876a039243ad33872e32ca525'
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    foreach($definition in @(@('MAIN_SOURCE',$main),@('SOURCE',$namespace),@('SCHEDULER_SOURCE',$scheduler),@('RUN_SOURCE',$run),@('SHUTDOWN_SOURCE',$shutdown))){$lines+='#define PB_CLEANUP_NAMESPACE_'+$definition[0]+' "'+$definition[1].Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\cleanup_namespace_fixture.c').Replace('\','/')+'"'
    $entry=Save-CleanupNamespace 'fixture-entry.c' ($lines -join "`n") 'fixture-entry'
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\cleanup_namespace_tests.c').Replace('\','/')+'"'
    $hostEntry=Save-CleanupNamespace 'host-entry.c' ($lines -join "`n") 'host-entry'
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/wd4100','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$Directory\")+$script:VersionArguments
    $dll=Join-Path $Directory 'cleanup-namespace-fixture.dll';$program=Join-Path $Directory 'cleanup-namespace-tests.exe'
    Invoke-BuildTool cl.exe ($flags+@('/LD',$entry,'api\logger.c','/link',"/out:$dll","/implib:$Directory/cleanup-namespace.lib",'ntdll.lib','advapi32.lib','delayimp.lib','/DELAYLOAD:advapi32.dll','/DELAY:UNLOAD'))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@($hostEntry,'/link',"/out:$program"))|Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;sources=@($sources.ToArray());parts=@($parts.ToArray());
        adaptation=@{before=$before;after=$after;count=$(if($Variant -eq 'current'){0}else{1})};
        boundaries=@('namespace object naming uses unnamed event/mutex SDK host','SID/boundary descriptor and synchronization are real','main only DllMain/unload observation','cleanup scheduler real default SDK pool, callback forwarding and module/event ledger','only extracted CleanupRun maps final release/take observers','legacy/orphan installation scan bodies are hosted','no old fixture/body reused or changed')}
}
