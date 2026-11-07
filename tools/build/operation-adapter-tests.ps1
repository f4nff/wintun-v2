# Full actual Open/public executor/Close, with finite projections of fixed hosts.
function Build-OperationAdapterTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-late-dispose')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Operation/adapter directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$sources=[Collections.Generic.List[object]]::new();$parts=[Collections.Generic.List[object]]::new();$ledger=[Collections.Generic.List[object]]::new()
    function Read-OperationAdapter([string]$relative,[string]$hash){
        $path=Join-Path $script:RepoRoot $relative;$actual=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();if($actual -ne $hash){throw "Unreviewed operation/adapter source $relative"}
        $sources.Add(@{path=$path;relative=$relative;sha256=$actual});return [IO.File]::ReadAllText($path)
    }
    function Normalize-OperationAdapter([string]$text,[string]$relative){$base=Split-Path (Join-Path $script:RepoRoot $relative);return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
    function Save-OperationAdapter([string]$name,[string]$text,[string]$source,[int]$offset=-1,[int]$length=-1){
        $path=Join-Path $Directory $name;[IO.File]::WriteAllText($path,$text,$encoding);$parts.Add(@{name=$name;path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();source=$source;offset=$offset;length=$length});return $path
    }
    function Replace-OperationAdapter([string]$text,[string]$before,[string]$after,[string]$reason){
        if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw "Nonunique operation/adapter replacement: $reason"};$ledger.Add(@{kind='replace-once';before=$before;after=$after;reason=$reason});return $text.Replace($before,$after)
    }
    function Remove-OperationAdapter([string]$text,[string]$first,[string]$next,[string]$reason){
        if([regex]::Matches($text,[regex]::Escape($first)).Count -ne 1){throw "Nonunique region $reason"};$a=$text.IndexOf($first);$b=$text.IndexOf($next,$a);if($a -lt 0 -or $b -le $a){throw "Missing region $reason"}
        $ledger.Add(@{kind='remove-region';first=$first;next=$next;offset=$a;length=$b-$a;removed=$text.Substring($a,$b-$a);reason=$reason});return $text.Substring(0,$a)+$text.Substring($b)
    }
    $adapter=Read-OperationAdapter 'api\adapter.c' 'e604c130f502dd453332236f7e02a95c54fbe3e3ceaa3df3e6092e4bed79215f'
    $name=$adapter.IndexOf('OpenAdapterInternal(LPCWSTR Name)');$a=$adapter.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$name);$b=$adapter.IndexOf('typedef struct ADAPTER_OPERATION_INPUT',$name)
    if($name -lt 0 -or $a -lt 0 -or $b -le $a){throw 'Complete Open function missing'}
    $open=Save-OperationAdapter 'open-actual.h' $adapter.Substring($a,$b-$a) 'api\adapter.c' $a ($b-$a)
    $match=[regex]::Match($adapter,'VOID WINAPI\r?\nWintunCloseAdapter\(');if(!$match.Success){throw 'Actual Close missing'};$a=$adapter.LastIndexOf('_Use_decl_annotations_',$match.Index);$b=$adapter.IndexOf('#include "adapter_rename.h"',$match.Index)
    $close=Save-OperationAdapter 'close-actual.h' $adapter.Substring($a,$b-$a) 'api\adapter.c' $a ($b-$a)
    $a=$adapter.IndexOf('typedef struct ADAPTER_OPERATION_INPUT');$name=$adapter.IndexOf('AdapterRemoveInstance(HDEVINFO DevInfo, SP_DEVINFO_DATA *DevInfoData)',$a);$b=$adapter.LastIndexOf('_Use_decl_annotations_',$name)
    if($a -lt 0 -or $name -lt 0 -or $b -le $a){throw 'Actual public Open/Run/Dispose region missing'}
    $public=Save-OperationAdapter 'public-actual.h' (Normalize-OperationAdapter $adapter.Substring($a,$b-$a) 'api\adapter.c') 'api\adapter.c' $a ($b-$a)
    $op=Read-OperationAdapter 'api\operation_runtime.h' '10a9d55a690c29c4128af45960be07e52ede302af051e6237cd37d267ea3ef8a'
    $null=Save-OperationAdapter 'operation-original.h' $op 'api\operation_runtime.h' 0 $op.Length
    if($Variant -eq 'without-late-dispose'){$op=Replace-OperationAdapter $op '    if (!claimed && result && job->Dispose) job->Dispose(result);' '    /* Observer125: omit only unclaimed successful-result Dispose. */' 'negative125 only actual unclaimed Dispose call'}
    $op=Save-OperationAdapter 'operation-actual.h' $op 'api\operation_runtime.h'
    $entryText="#define MainRuntimeRelease() ObserveOperationRelease(job)`n#include `""+$op.Replace('\','/')+"`"`n#undef MainRuntimeRelease`n"
    $opEntry=Save-OperationAdapter 'operation-entry.h' $entryText 'release-boundary-entry'
    $main=Read-OperationAdapter 'api\main.c' '543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'
    $null=Save-OperationAdapter 'main-original.c' $main 'api\main.c' 0 $main.Length
    $main=Normalize-OperationAdapter $main 'api\main.c'
    $old='#include "'+(Join-Path $script:RepoRoot 'api\operation_runtime.h').Replace('\','/')+'"';$new='#include "'+$opEntry.Replace('\','/')+'"'
    $main=Replace-OperationAdapter $main $old $new 'only operation include routes the tail release observer'
    $main=Save-OperationAdapter 'main-actual.h' $main 'api\main.c'
    $openHost=Read-OperationAdapter 'tests\safety\adapter_open_tests.c' '37b8c06e4152f2b05e7acd6809a07c432a595e7ccd3319d17793f7817b39ce52'
    $openHost=Replace-OperationAdapter $openHost 'HANDLE ModuleHeap;' '/* Actual main owns ModuleHeap. */' 'remove duplicate heap'
    $openHost=Replace-OperationAdapter $openHost 'BOOL IsWindows7;' '/* Actual EnvInit owns IsWindows7. */' 'remove duplicate environment'
    $openHost=Replace-OperationAdapter $openHost 'Closes,Leases;' 'Closes;' 'Leases assertions alias actual DependencyUsers'
    $openHost=Remove-OperationAdapter $openHost 'BOOL MainRuntimeAcquire(VOID)' 'HANDLE NamespaceTakeDeviceInstallationMutex(VOID)' 'remove all fake runtime functions'
    $openHost=Remove-OperationAdapter $openHost 'BOOL MainOperationSleep(DWORD delay)' 'static HDEVINFO WINAPI TestSet(' 'actual operation sleep'
    $openHost=Remove-OperationAdapter $openHost 'VOID WINAPI WintunCloseAdapter(' 'static VOID RenderLog(' 'actual adapter Close'
    $openHost=Replace-OperationAdapter $openHost 'Blocks[i]=(BLOCK){0};--Live;++Frees;SetLastError(ERROR_INVALID_HANDLE);return TRUE;' 'Blocks[i]=(BLOCK){0};--Live;++Frees;if(memory==LiveAdapter){LiveAdapter=NULL;++Closes;}SetLastError(ERROR_INVALID_HANDLE);return TRUE;' 'observe actual adapter free'
    $openHost=Replace-OperationAdapter $openHost 'CHECK(Leases==1&&!InstallMutex);' 'CHECK(Leases==2&&!InstallMutex);' 'include actual executor lease at namespace entry'
    $openHost=Replace-OperationAdapter $openHost '&&Leases==2&&!DeviceSet);' '&&Leases==3&&!DeviceSet);' 'include actual executor lease during discovered adapter ownership'
    foreach($rename in @(@('Fail','OpenHostFail'),@('Run','OpenHostRun'),@('Allocations','LegacyAllocations'),@('Frees','LegacyFrees'),@('PopulateAdapterData','HostedPopulate'))){
        $count=[regex]::Matches($openHost,'\b'+$rename[0]+'\b').Count;if(!$count){throw 'Missing host rename'};$ledger.Add(@{kind='token-rename';before=$rename[0];after=$rename[1];count=$count});$openHost=[regex]::Replace($openHost,'\b'+$rename[0]+'\b',$rename[1])
    }
    $openHost=Save-OperationAdapter 'open-host.h' (Normalize-OperationAdapter $openHost 'tests\safety\adapter_open_tests.c') 'tests\safety\adapter_open_tests.c'
    $operationHost=Read-OperationAdapter 'tests\safety\legacy_operation_fixture.c' 'ad43448e89325c21a558a022ffd9b61f85519051dddcad00ea46988200274812'
    $operationHost=Replace-OperationAdapter $operationHost '#define MAIN_OPERATION_TIMEOUT 250u' '/* Production 120000ms default retained; actual stop broadcast cancels. */' 'remove only old fixture shortened deadline'
    $operationHost=Replace-OperationAdapter $operationHost '{ assert(Pins>0); InterlockedDecrement(&Pins); FreeLibraryWhenCallbackReturns(instance,module); }' '{ assert(Pins>0); ObserveCallbackThread(); InterlockedDecrement(&Pins); FreeLibraryWhenCallbackReturns(instance,module); }' 'observe real callback thread before actual tail-pin transfer'
    $operationHost=Replace-OperationAdapter $operationHost 'if(Detached) SetEvent(Detached);' 'OperationAdapterFinalCheck(); if(Detached) SetEvent(Detached);' 'extend actual detach with SDK/adapter/dependency assertions'
    $operationHost=Normalize-OperationAdapter $operationHost 'tests\safety\legacy_operation_fixture.c'
    $old='#include "'+(Join-Path $script:RepoRoot 'api\main.c').Replace('\','/')+'"';$new='#include "'+$main.Replace('\','/')+'"'
    $operationHost=Replace-OperationAdapter $operationHost $old $new 'bind complete actual main projection'
    $operationHost=Save-OperationAdapter 'operation-host.h' $operationHost 'tests\safety\legacy_operation_fixture.c'
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    foreach($definition in @(@('OPEN_SOURCE',$open),@('OPEN_HOST',$openHost),@('OPERATION_HOST',$operationHost),@('CLOSE_SOURCE',$close),@('PUBLIC_SOURCE',$public))){$lines+='#define PB_OPERATION_ADAPTER_'+$definition[0]+' "'+$definition[1].Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\operation_adapter_fixture.c').Replace('\','/')+'"'
    $entry=Save-OperationAdapter 'fixture-entry.c' ($lines -join "`n") 'fixture-entry'
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\operation_adapter_tests.c').Replace('\','/')+'"'
    $hostEntry=Save-OperationAdapter 'host-entry.c' ($lines -join "`n") 'host-entry'
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/Fo$Directory\")+$script:VersionArguments
    $dll=Join-Path $Directory 'operation-adapter-fixture.dll';$program=Join-Path $Directory 'operation-adapter-tests.exe'
    Invoke-BuildTool cl.exe ($flags+@('/LD',$entry,'/link',"/out:$dll","/implib:$Directory/operation-adapter.lib",'ntdll.lib','advapi32.lib','delayimp.lib','/DELAYLOAD:advapi32.dll','/DELAY:UNLOAD'))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@($hostEntry,'/link',"/out:$program"))|Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;sources=@($sources.ToArray());parts=@($parts.ToArray());adaptations=@($ledger.ToArray());
        boundary_macros=@('Leases aliases actual runtime count','operation entry observes one actual tail release','actual main dependency unload observer','Close unreachable SwDevice/remove calls hosted; discovered adapter owns neither','existing allocator/handle/pool/module tracking unchanged','metadata success gate before returning to actual Open')}
}
