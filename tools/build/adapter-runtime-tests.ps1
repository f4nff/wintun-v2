# Actual discovered legacy lease + shipping unified wrapper + actual runtime.
# Existing hosts are hash-locked and adapted only at the listed boundaries.
function Build-AdapterRuntimeTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-runtime-release')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Adapter/runtime output must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$sources=[Collections.Generic.List[object]]::new();$parts=[Collections.Generic.List[object]]::new();$adaptations=[Collections.Generic.List[object]]::new()
    function Read-AdapterRuntimeSource([string]$relative,[string]$locked=''){
        $path=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
        if($locked -and $hash -ne $locked){throw "Unexpected adapter/runtime source: $relative"}
        $sources.Add(@{path=$path;relative=$relative;sha256=$hash});return [IO.File]::ReadAllText($path)
    }
    function Normalize-AdapterRuntimeIncludes([string]$text,[string]$relative){
        $base=Split-Path (Join-Path $script:RepoRoot $relative)
        return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
    }
    function Save-AdapterRuntimePart([string]$name,[string]$text,[string]$source,[int]$offset=-1,[int]$length=-1){
        $path=Join-Path $Directory $name;[IO.File]::WriteAllText($path,$text,$encoding)
        $parts.Add(@{path=$path;name=$name;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();source=$source;offset=$offset;length=$length});return $path
    }
    function Replace-AdapterRuntimeOnce([string]$text,[string]$from,[string]$to,[string]$reason){
        if([regex]::Matches($text,[regex]::Escape($from)).Count -ne 1){throw "Nonunique adapter/runtime adaptation: $reason"}
        $adaptations.Add(@{kind='replace-once';before=$from;after=$to;reason=$reason});return $text.Replace($from,$to)
    }
    function Remove-AdapterRuntimeRegion([string]$text,[string]$first,[string]$next,[string]$reason){
        if([regex]::Matches($text,[regex]::Escape($first)).Count -ne 1){throw "Nonunique region: $reason"}
        $start=$text.IndexOf($first);$end=$text.IndexOf($next,$start);if($start -lt 0 -or $end -le $start){throw "Missing region: $reason"}
        $adaptations.Add(@{kind='remove-region';first=$first;next=$next;offset=$start;length=$end-$start;removed=$text.Substring($start,$end-$start);reason=$reason})
        return $text.Substring(0,$start)+$text.Substring($end)
    }
    $adapter=Read-AdapterRuntimeSource 'api\adapter.c'
    $name=$adapter.IndexOf('OpenAdapterInternal(LPCWSTR Name)');$start=$adapter.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$name);$end=$adapter.IndexOf('typedef struct ADAPTER_OPERATION_INPUT',$name)
    if($name -lt 0 -or $start -lt 0 -or $end -le $start){throw 'Complete OpenAdapterInternal missing'}
    $open=Save-AdapterRuntimePart 'open-actual.h' $adapter.Substring($start,$end-$start) 'api\adapter.c' $start ($end-$start)
    $match=[regex]::Match($adapter,'VOID WINAPI\r?\nWintunCloseAdapter\(');if(!$match.Success){throw 'Actual Close missing'}
    $start=$adapter.LastIndexOf('_Use_decl_annotations_',$match.Index);$end=$adapter.IndexOf('#include "adapter_rename.h"',$match.Index)
    if($start -lt 0 -or $end -le $start){throw 'Complete WintunCloseAdapter missing'}
    $closeText=$adapter.Substring($start,$end-$start);$originalClose=Save-AdapterRuntimePart 'close-original.h' $closeText 'api\adapter.c' $start ($end-$start)
    if($Variant -eq 'without-runtime-release'){$closeText=Replace-AdapterRuntimeOnce $closeText '    if (runtimeOwned) MainRuntimeRelease();' '    (void)runtimeOwned; /* Observer control: omit only the actual lease release. */' 'negative119 only actual Close runtime-release tail'}
    $close=Save-AdapterRuntimePart 'close-actual.h' $closeText 'api\adapter.c' $start ($end-$start)
    $openHost=Read-AdapterRuntimeSource 'tests\safety\adapter_open_tests.c' '37b8c06e4152f2b05e7acd6809a07c432a595e7ccd3319d17793f7817b39ce52'
    $openHost=Replace-AdapterRuntimeOnce $openHost 'HANDLE ModuleHeap;' '/* Actual main.c owns ModuleHeap. */' 'remove duplicate module heap'
    $openHost=Replace-AdapterRuntimeOnce $openHost 'BOOL IsWindows7;' '/* Actual EnvInit owns IsWindows7. */' 'remove duplicate environment flag'
    $openHost=Replace-AdapterRuntimeOnce $openHost 'Closes,Leases;' 'Closes;' 'Leases assertions alias actual DependencyUsers'
    $openHost=Remove-AdapterRuntimeRegion $openHost 'BOOL MainRuntimeAcquire(VOID)' 'HANDLE NamespaceTakeDeviceInstallationMutex(VOID)' 'remove all three fake runtime functions'
    $openHost=Remove-AdapterRuntimeRegion $openHost 'BOOL MainOperationSleep(DWORD delay)' 'static HDEVINFO WINAPI TestSet(' 'use actual operation sleep'
    $openHost=Remove-AdapterRuntimeRegion $openHost 'VOID WINAPI WintunCloseAdapter(' 'static VOID RenderLog(' 'use actual Close'
    $openHost=Replace-AdapterRuntimeOnce $openHost 'Blocks[i]=(BLOCK){0};--Live;++Frees;SetLastError(ERROR_INVALID_HANDLE);return TRUE;' 'Blocks[i]=(BLOCK){0};--Live;++Frees;if(memory==LiveAdapter){LiveAdapter=NULL;++Closes;}SetLastError(ERROR_INVALID_HANDLE);return TRUE;' 'observe actual legacy object free without replacing Close'
    foreach($rename in @(@('Allocations','LegacyAllocations'),@('Frees','LegacyFrees'))){
        $count=[regex]::Matches($openHost,'\b'+$rename[0]+'\b').Count;if(!$count){throw 'Missing host counter rename'}
        $adaptations.Add(@{kind='token-rename';before=$rename[0];after=$rename[1];count=$count});$openHost=[regex]::Replace($openHost,'\b'+$rename[0]+'\b',$rename[1])
    }
    $openHost=Save-AdapterRuntimePart 'open-host.h' (Normalize-AdapterRuntimeIncludes $openHost 'tests\safety\adapter_open_tests.c') 'tests\safety\adapter_open_tests.c'
    $main=Save-AdapterRuntimePart 'main-actual.h' (Normalize-AdapterRuntimeIncludes (Read-AdapterRuntimeSource 'api\main.c') 'api\main.c') 'api\main.c'
    $native=Read-AdapterRuntimeSource 'tests\safety\native_policy_fixture.c' '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83'
    $native=Remove-AdapterRuntimeRegion $native 'PB_RESULT WINAPI PbAdapterCreate(' 'BOOL WINAPI DllMain(' 'remove five adapter stubs; execute shipping unified implementation'
    $native=Replace-AdapterRuntimeOnce $native 'NativeRegistered && NativeDrained && !NativeActive' '(NativeRegistered || UnpublishedCleanup) && (NativeDrained || UnpublishedCleanup) && !NativeActive' 'assertion only: unpublished Open cleanup need not fake registration/drain'
    $native=Save-AdapterRuntimePart 'native-host.h' (Normalize-AdapterRuntimeIncludes $native 'tests\safety\native_policy_fixture.c') 'tests\safety\native_policy_fixture.c'
    $session=Save-AdapterRuntimePart 'session-actual.h' (Normalize-AdapterRuntimeIncludes (Read-AdapterRuntimeSource 'api\v2\session.c') 'api\v2\session.c') 'api\v2\session.c'
    $sessionEntry=Save-AdapterRuntimePart 'session-entry.h' (@'
#undef calloc
#define calloc CombinedCalloc
#undef CreateFileW
#define CreateFileW CombinedFile
#undef CloseHandle
#define CloseHandle CombinedClose
#undef DeviceIoControl
#define DeviceIoControl CombinedIo
'@+"`n#include `""+$session.Replace('\','/')+"`"`n") 'boundary-entry'
    $null=Read-AdapterRuntimeSource 'api\v2\adapter_unified.c'
    $fixtureLines=@();if($Guarded){$fixtureLines+='#define PB_GUARD_TESTS 1'}
    foreach($definition in @(@('OPEN_SOURCE',$open),@('OPEN_HOST',$openHost),@('MAIN_SOURCE',$main),@('CLOSE_SOURCE',$close),@('NATIVE_HOST',$native),@('SESSION_ENTRY',$sessionEntry))){$fixtureLines+='#define PB_ADAPTER_RUNTIME_'+$definition[0]+' "'+$definition[1].Replace('\','/')+'"'}
    $fixtureLines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\adapter_runtime_fixture.c').Replace('\','/')+'"'
    $entry=Save-AdapterRuntimePart 'fixture-entry.c' ($fixtureLines -join "`n") 'boundary-entry'
    $hostLines=@();if($Guarded){$hostLines+='#define PB_GUARD_TESTS 1'}
    $hostLines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\adapter_runtime_tests.c').Replace('\','/')+'"'
    $hostEntry=Save-AdapterRuntimePart 'host-entry.c' ($hostLines -join "`n") 'host-entry'
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$Directory\")+$script:VersionArguments
    $dll=Join-Path $Directory 'adapter-runtime-fixture.dll';$program=Join-Path $Directory 'adapter-runtime-tests.exe'
    Invoke-BuildTool cl.exe ($flags+@('/LD',$entry,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll","/implib:$Directory/adapter-runtime.lib",'bcrypt.lib','ntdll.lib','advapi32.lib','delayimp.lib','/DELAYLOAD:advapi32.dll','/DELAY:UNLOAD'))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@($hostEntry,'/link',"/out:$program"))|Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;sources=@($sources.ToArray());parts=@($parts.ToArray());adaptations=@($adaptations.ToArray());
        macros=@('DllMain names combine both real detach paths','Leases aliases actual DependencyUsers; no replacement runtime count','main.c only boundary unload/CV redirects','native session only calloc/file/Close/IO boundaries','shipping unified calloc/free observers')}
}
