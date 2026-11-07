function Build-HelperDrainTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('before','current')][string]$Variant='current',[string]$BaselineDirectory='out\retire-edge-baseline\d1dc19684d05')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Helper drain output must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Helper drain requires shipping fingerprint'}
    # Ordinary current builds use current sources only. Historical replay is an
    # explicit contract: never silently substitute current files for its input.
    $historical=$Variant -ceq 'before' -or $PSBoundParameters.ContainsKey('BaselineDirectory')
    $baseline=$null;$sourceMode='current-source'
    if($historical){
        $baselinePath=if([IO.Path]::IsPathFullyQualified($BaselineDirectory)){$BaselineDirectory}else{Join-Path $script:RepoRoot $BaselineDirectory}
        $baseline=(Resolve-Path -LiteralPath $baselinePath).Path;$sourceMode='historical-baseline'
    }
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$parts=@();$sources=@()
    $dependencies=@('api\main.c','api\main.h','api\operation_runtime.h','api\operation.h','api\logger.h','api\adapter.h','api\namespace.h','api\registry.h',
        'api\ntdll.h','api\rundll32.h','api\wintun.h','common\system_library.h','include\packetbridge_identity.h','include\packetbridge_version.h',
        'tests\safety\legacy_runtime_fixture.c','tests\safety\guard_memory.h','api\rundll32.c','api\rundll32_process.h','api\resource.h',
        'api\overlapped_wait.h','api\termination_wait.h','api\event_pair.h','include\packetbridge_helper_protocol.h','api\logger.c')
    $fixed=@{'api\main.c'='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f';
        'api\rundll32.c'='5b5e65d6c84065cff91908dfb8de1c9a07063ac98108154cf8d0627493c4a430';
        'tests\safety\legacy_runtime_fixture.c'='2d60fe668da0d6b5beca986500429181548afcb67db4732a72fdf41d6fa964ed';
        'api\rundll32_process.h'='0fea04423f2e1dd8c87ec50f51334cd49a73fcba9cb854b3023a406dee16303d'}
    foreach($relative in $dependencies){
        $path=Join-Path $script:RepoRoot $relative;$hash=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()
        if($fixed.ContainsKey($relative)-and $hash -cne $fixed[$relative]){throw "Unreviewed complete helper drain source:$relative"}
        if($historical -and $relative -cne 'api\rundll32_process.h' -and $hash -cne (Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant()){throw "Unchanged helper drain dependency changed:$relative"}
        $sources+=@{path=$path;relative=$relative;sha256=$hash}
    }
    if($historical){
        if((Get-FileHash -LiteralPath (Join-Path $baseline 'api\rundll32_process.h')).Hash.ToLowerInvariant() -cne '7f5749ae9a3310f6a4d909dbcc29af78c5424b7268047245aec59073f788a76c'){throw 'Unreviewed complete old helper header'}
    }
    function Project-HelperDrainIncludes([string]$body,[string]$relative){
        $base=Split-Path (Join-Path $script:RepoRoot $relative)
        return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
    }
    function Replace-HelperDrainOnce([string]$body,[string]$before,[string]$after){
        if([regex]::Matches($body,[regex]::Escape($before)).Count -ne 1){throw "Nonunique helper drain fixture boundary:$before"};return $body.Replace($before,$after)
    }
    foreach($pair in @(@('main-original.c','api\main.c'),@('rundll-original.c','api\rundll32.c'),@('fixture-original.c','tests\safety\legacy_runtime_fixture.c'),@('helper-current.h','api\rundll32_process.h'))){
        [IO.File]::WriteAllBytes((Join-Path $Directory $pair[0]),[IO.File]::ReadAllBytes((Join-Path $script:RepoRoot $pair[1])))
    }
    if($historical){[IO.File]::WriteAllBytes((Join-Path $Directory 'helper-before.h'),[IO.File]::ReadAllBytes((Join-Path $baseline 'api\rundll32_process.h')))}
    $selected=if($Variant -eq 'before'){'helper-before.h'}else{'helper-current.h'}
    $helperPath=Join-Path $Directory 'helper-selected.h'
    [IO.File]::WriteAllText($helperPath,(Project-HelperDrainIncludes ([IO.File]::ReadAllText((Join-Path $Directory $selected))) 'api\rundll32_process.h'),$encoding)
    $mainPath=Join-Path $Directory 'main-selected.h'
    [IO.File]::WriteAllText($mainPath,(Project-HelperDrainIncludes ([IO.File]::ReadAllText((Join-Path $Directory 'main-original.c'))) 'api\main.c'),$encoding)
    $rundll=Project-HelperDrainIncludes ([IO.File]::ReadAllText((Join-Path $Directory 'rundll-original.c'))) 'api\rundll32.c'
    $rundll=Replace-HelperDrainOnce $rundll ('#include "'+(Join-Path $script:RepoRoot 'api\rundll32_process.h').Replace('\','/')+'"') ('#include "'+$helperPath.Replace('\','/')+'"')
    $rundllPath=Join-Path $Directory 'rundll-selected.h';[IO.File]::WriteAllText($rundllPath,$rundll,$encoding)
    $fixtureBody=[IO.File]::ReadAllText((Join-Path $Directory 'fixture-original.c'))
    $fixtureBody=Replace-HelperDrainOnce $fixtureBody '#define RUN_DLL32_TIMEOUT 2000u' '/* Retain production120000ms default; no public helper waiter is invoked. */'
    $fixtureBody=Replace-HelperDrainOnce $fixtureBody 'if (Detached) SetEvent(Detached);' 'DrainFinalCheck(); if (Detached) SetEvent(Detached);'
    $asserts=[regex]::Matches($fixtureBody,'\bassert\(').Count;if($asserts -ne 4){throw 'Original helper fixture assertions changed'}
    $fixtureBody=[regex]::Replace($fixtureBody,'\bassert\(','HD_CHECK(')
    $fixturePath=Join-Path $Directory 'fixture-selected.h';[IO.File]::WriteAllText($fixturePath,$fixtureBody,$encoding)
    $test=Join-Path $script:RepoRoot 'tests\safety\helper_drain_tests.c'
    $shared=@();if($Guarded){$shared+='#define PB_GUARD_TESTS 1'};if($Variant -eq 'before'){$shared+='#define PB_HELPER_DRAIN_BEFORE 1'}
    $fixtureLines=$shared+@('#define PB_HELPER_DRAIN_FIXTURE 1')
    foreach($pair in @(@('PB_LEGACY_RUNTIME_MAIN_SOURCE',$mainPath),@('PB_LEGACY_HELPER_SOURCE',$rundllPath),@('PB_HELPER_DRAIN_SOURCE',$fixturePath))){$fixtureLines+='#define '+$pair[0]+' "'+$pair[1].Replace('\','/')+'"'}
    $fixtureLines+='#include "'+$test.Replace('\','/')+'"'
    $fixtureEntry=Join-Path $Directory 'fixture-entry.c';[IO.File]::WriteAllText($fixtureEntry,($fixtureLines -join "`n")+"`n",$encoding)
    $hostLines=$shared+@('#include "'+$test.Replace('\','/')+'"');$hostEntry=Join-Path $Directory 'host-entry.c';[IO.File]::WriteAllText($hostEntry,($hostLines -join "`n")+"`n",$encoding)
    $partNames=@('main-original.c','main-selected.h','rundll-original.c','rundll-selected.h','fixture-original.c','fixture-selected.h')
    if($historical){$partNames+='helper-before.h'}
    $partNames+=@('helper-current.h','helper-selected.h','fixture-entry.c','host-entry.c')
    foreach($name in $partNames){
        $path=Join-Path $Directory $name;$parts+=@{name=$name;path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}
    }
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/wd4100','/wd4200','/DNDEBUG','/DMAYBE_WOW64=1','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $dll=Join-Path $Directory 'helper-drain.dll';$exe=Join-Path $Directory 'helper-drain.exe'
    Invoke-BuildTool cl.exe ($flags+@('/LD',$fixtureEntry,'api\logger.c','/link',"/out:$dll","/implib:$Directory/helper-drain.lib",'ntdll.lib','advapi32.lib','setupapi.lib','shlwapi.lib','shell32.lib','delayimp.lib','/DELAY:UNLOAD','/DELAYLOAD:advapi32.dll','/DELAYLOAD:setupapi.dll','/DELAYLOAD:shlwapi.dll','/DELAYLOAD:shell32.dll'))|Out-Host
    Invoke-BuildTool cl.exe ($flags+@($hostEntry,'/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash $dll).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        ndebug=$true;pb_api_tests=$false;maybe_wow64_source=$true;sources=$sources;parts=$parts;arguments=$flags;baseline=$baseline;source_mode=$sourceMode;fixture_assert_tokens=$asserts;test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant()}
}
