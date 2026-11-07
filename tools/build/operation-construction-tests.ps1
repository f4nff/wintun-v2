# Small real-DLL executor construction/shutdown boundary; original hosts stay frozen.
function Build-OperationConstructionTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-construction-unreserve')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Operation construction directory must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Operation construction requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$sources=@();$parts=@();$adaptations=@()
    $accepted=[ordered]@{
        'api\main.c'='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'
        'api\main.h'='f10e50ce03eb353c5d018233cd49c16df73c467842f8f10b58909ace3a476e28'
        'api\operation_runtime.h'='10a9d55a690c29c4128af45960be07e52ede302af051e6237cd37d267ea3ef8a'
        'api\operation.h'='186457ad6d0aef72fcf69cf1cb98f348384c25971b8f871fb3e41387fde425a8'
        'api\logger.h'='641d12be455a7fb7d9a2502ce88f81057d0279b2791853dcd901b6e54d9937b1'
        'api\adapter.h'='992833496d30973aaa33617c1e8e58e3cb6b08517d0ae42528d60e7713f47134'
        'api\namespace.h'='22c447513c160c525c1f4be92a1df898bff30294e598726f72f6307909015aea'
        'api\registry.h'='3383aeb7cde18ae016888861719024269cf5c8cbd91411145d1106d2c214eb08'
        'api\ntdll.h'='ae52a9a55ab886ef8b72e38b53bee9caea355cfd866bf6bf15695106a76e4526'
        'api\rundll32.h'='9ca18cb5b06a9f93da00b98937a4132d2197b90a469871764b144acfb5600a34'
        'api\wintun.h'='bb4b03aa3c4aa52c9880b9329d4840d38a0a558d120171005d1143647b9e19a5'
        'common\system_library.h'='e776526396ea27f0e2641d07070d8cd678344256b1baabc905768ac2db490e8e'
        'include\packetbridge_identity.h'='61573e1ac413c204421a1707ecab067a764b0a55e50952758de475f045dbd41f'
        'include\packetbridge_version.h'='837c6a14288b113acf8d7eec1e0c855d70fda441e16fe7770718338a458d1a66'
        'tests\safety\legacy_operation_fixture.c'='ad43448e89325c21a558a022ffd9b61f85519051dddcad00ea46988200274812'
        'tests\safety\guard_memory.h'='ca169cf1a4b11506dc45572b60d61d8b05cf8a73bf7ba424b04dca228637906b'
    }
    foreach($relative in $accepted.Keys){
        $path=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
        if($hash -ne $accepted[$relative]){throw "Unreviewed operation construction input:$relative"}
        $sources+=@{path=$path;relative=$relative;sha256=$hash}
    }
    function Project-OperationConstruction([string]$text,[string]$relative){
        $base=Split-Path (Join-Path $script:RepoRoot $relative)
        return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
    }
    function Replace-OperationConstruction([string]$text,[string]$before,[string]$after){
        if([regex]::Matches($text,[regex]::Escape($before)).Count -ne 1){throw "Nonunique construction boundary:$before"}
        return $text.Replace($before,$after)
    }
    foreach($copyPair in @(@('api\main.c','main-original.c'),@('api\operation_runtime.h','operation-original.h'),@('tests\safety\legacy_operation_fixture.c','fixture-original.c'))){
        $copy=Join-Path $Directory $copyPair[1];[IO.File]::WriteAllBytes($copy,[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $copyPair[0])))
        $parts+=@{name=$copyPair[1];path=$copy;sha256=(Get-FileHash $copy).Hash.ToLowerInvariant();source=$copyPair[0]}
    }
    $operation=[IO.File]::ReadAllText((Join-Path $Directory 'operation-original.h'))
    $before='    if (!job) { OperationUnreserve(); SetLastError(ERROR_OUTOFMEMORY); return 0; }'
    $after='    if (!job) { /* Test136: omit only construction reservation return. */ SetLastError(ERROR_OUTOFMEMORY); return 0; }'
    if([regex]::Matches($operation,[regex]::Escape($before)).Count -ne 1){throw 'First job failure branch not unique'}
    $mutation=$null
    if($Variant -ne 'current'){
        $mutation=@{offset=$operation.IndexOf($before);length=$before.Length;before=$before;after=$after}
        $operation=Replace-OperationConstruction $operation $before $after
    }
    $operationPath=Join-Path $Directory 'operation-selected.h';[IO.File]::WriteAllText($operationPath,$operation,$encoding)
    $main=Project-OperationConstruction ([IO.File]::ReadAllText((Join-Path $Directory 'main-original.c'))) 'api\main.c'
    $main=Replace-OperationConstruction $main ('#include "'+(Join-Path $script:RepoRoot 'api\operation_runtime.h').Replace('\','/')+'"') ('#include "'+$operationPath.Replace('\','/')+'"')
    $mainPath=Join-Path $Directory 'main-selected.h';[IO.File]::WriteAllText($mainPath,$main,$encoding)
    $fixtureBody=[IO.File]::ReadAllText((Join-Path $Directory 'fixture-original.c'))
    $replacements=@(
        @('#define MAIN_OPERATION_TIMEOUT 250u','/* Production120000ms default retained; explicit shutdown cancels construction. */'),
        @('#define HeapAlloc Allocate','#define HeapAlloc ConstructionAllocate'),
        @('if(Detached) SetEvent(Detached);','ConstructionFinalCheck(); if(Detached) SetEvent(Detached);'))
    foreach($pair in $replacements){$fixtureBody=Replace-OperationConstruction $fixtureBody $pair[0] $pair[1];$adaptations+=@{kind='replace-once';before=$pair[0];after=$pair[1]}}
    $asserts=[regex]::Matches($fixtureBody,'\bassert\(').Count;if($asserts -ne 6){throw 'Original fixture ownership assertions changed'}
    $fixtureBody=[regex]::Replace($fixtureBody,'\bassert\(','OC_CHECK(');$adaptations+=@{kind='token-rename';before='assert(';after='OC_CHECK(';count=$asserts}
    $fixtureBody=Project-OperationConstruction $fixtureBody 'tests\safety\legacy_operation_fixture.c'
    $fixtureBody=Replace-OperationConstruction $fixtureBody ('#include "'+(Join-Path $script:RepoRoot 'api\main.c').Replace('\','/')+'"') ('#include "'+$mainPath.Replace('\','/')+'"')
    $hostPath=Join-Path $Directory 'fixture-selected.h';[IO.File]::WriteAllText($hostPath,$fixtureBody,$encoding)
    foreach($part in @(@('operation-selected.h',$operationPath),@('main-selected.h',$mainPath),@('fixture-selected.h',$hostPath))){$parts+=@{name=$part[0];path=$part[1];sha256=(Get-FileHash $part[1]).Hash.ToLowerInvariant()}}
    $test=Join-Path $script:RepoRoot 'tests\safety\operation_construction_tests.c'
    $shared=@();if($Guarded){$shared+='#define PB_GUARD_TESTS 1'};if($Variant -ne 'current'){$shared+='#define PB_OPERATION_CONSTRUCTION_NEGATIVE 1'}
    $fixtureEntry=Join-Path $Directory 'fixture-entry.c';$lines=$shared+@('#define PB_OPERATION_CONSTRUCTION_FIXTURE 1')
    $lines+='#define PB_OPERATION_CONSTRUCTION_SOURCE "'+$hostPath.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    [IO.File]::WriteAllText($fixtureEntry,($lines -join "`n")+"`n",$encoding)
    $hostEntry=Join-Path $Directory 'host-entry.c';$lines=$shared+@('#include "'+$test.Replace('\','/')+'"')
    [IO.File]::WriteAllText($hostEntry,($lines -join "`n")+"`n",$encoding)
    foreach($part in @(@('fixture-entry.c',$fixtureEntry),@('host-entry.c',$hostEntry))){$parts+=@{name=$part[0];path=$part[1];sha256=(Get-FileHash $part[1]).Hash.ToLowerInvariant()}}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/wd4100','/wd4200','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $dll=Join-Path $Directory 'operation-construction.dll';$exe=Join-Path $Directory 'operation-construction.exe'
    Invoke-BuildTool cl.exe ($flags+@('/LD',$fixtureEntry,'/link',"/out:$dll","/implib:$Directory/operation-construction.lib",'ntdll.lib','advapi32.lib','delayimp.lib','/DELAY:UNLOAD','/DELAYLOAD:advapi32.dll'))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@($hostEntry,'/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();
        mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;ndebug=$true;pb_api_tests=$false;sources=$sources;parts=$parts;adaptations=$adaptations;mutation=$mutation;arguments=$flags;
        test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant()}
}
