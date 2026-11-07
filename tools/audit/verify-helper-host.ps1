param([string]$BaselineDirectory='out\helper-protocol-baseline\1befd7473d86')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Helper host audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$oldHost=Join-Path $baseline 'setupapihost\host.c';$oldWin7=Join-Path $baseline 'setupapihost\host_win7.h'
if((Get-FileHash -LiteralPath $oldHost).Hash.ToLowerInvariant() -ne '787a6c0579f9f8d96f60f4550f89d7af2430eb03446b0a773a0fda376b15630f' -or
   (Get-FileHash -LiteralPath $oldWin7).Hash.ToLowerInvariant() -ne 'ce51d1eb14dd14360f88ae2eba9812ce3a0564eccfc6983f99cb9aa2e137862e'){throw 'Unexpected frozen helper host'}
foreach($name in @('common\system_library.h','setupapihost\resources.rc','include\packetbridge_version.h','driver\v2\packetbridge.inf')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline $name)).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $name)).Hash){throw "Helper dependency changed: $name"}
}
if((Get-FileHash -LiteralPath (Join-Path $script:RepoRoot 'include\packetbridge_identity.h')).Hash.ToLowerInvariant() -ne '61573e1ac413c204421a1707ecab067a764b0a55e50952758de475f045dbd41f'){throw 'Unreviewed shared hardware identity'}
$directory=Join-Path $script:RepoRoot "out\helper-host-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$cases=@(
    @{name='arguments';test='tests\safety\helper_host_tests.c';macro='PB_HELPER_HOST_SOURCE';old=$oldHost;libs=@('shell32.lib','uuid.lib');controls=@(@(1,92,'HELPER_PARSE_FAILURE_REPORTED_SUCCESS'),@(2,93,'HELPER_MISSING_ARGUMENT_REPORTED_SUCCESS'))},
    @{name='identity';test='tests\safety\helper_identity_tests.c';macro='PB_HELPER_IDENTITY_SOURCE';old=$oldWin7;libs=@('uuid.lib');controls=@(,@(1,94,'HELPER_WIN7_USED_LEGACY_HARDWARE_ID'))}
)
$executables=@()
foreach($case in $cases) {foreach($mode in @('iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "$($case.name)-$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=$case.test
    if($variant -eq 'before') {
        $source=Join-Path $objects 'entry.c'
        @(('#define '+$case.macro+' "'+$case.old.Replace('\','/')+'"'),
          ('#include "'+(Join-Path $script:RepoRoot $case.test).Replace('\','/')+'"'))|
          Set-Content -LiteralPath $source -Encoding utf8
    }
    $exe=Join-Path $objects 'helper.exe'
    Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source)+$script:VersionArguments+@('/link',"/out:$exe","/implib:$objects\helper.lib")+$case.libs)
    if($variant -eq 'after') {
        Invoke-TestTool $exe @()
        foreach($control in $case.controls){Invoke-TestTool $exe @([string]$control[0])}
    } else {foreach($control in $case.controls) {
        $arguments=@([string]$control[0]);$output=@(& $exe @arguments 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
        $log=Join-Path $objects ("control-"+$control[0]+'.log');$output|Set-Content -LiteralPath $log -Encoding utf8
        $expected=$code -eq $control[1] -and ($output -join "`n").Contains($control[2])
        $script:TestExecutions.Add([ordered]@{program=$exe;arguments=$arguments;exit_code=$code;expected_exit_code=$control[1];expected_baseline_failure=$expected;log=$log})
        Write-TestRecording $(if($expected){'running'}else{'failed'})
        if(!$expected){throw "Unexpected $($case.name) baseline: $($control[2]), exit=$code"}
    }}
    $executables+=@{case=$case.name;variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
}}}
# Build the real helper as well as the hosted tests; it is not loaded or invoked.
# Frozen/current DLLs retain the actual imports, exports and 2.0.0 version resource.
$native=@();$diagnostics=@()
foreach($mode in @('iso','ms')) {foreach($variant in @('before','after')) {
    $objects=Join-Path $directory "native-$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=if($variant -eq 'before'){$oldHost}else{'setupapihost\host.c'}
    $resource=Join-Path $objects 'version.res';$dll=Join-Path $objects 'setupapihost.dll'
    Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/fo$resource")+$script:VersionArguments+@('setupapihost\resources.rc'))
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/wd4100','/wd4201','/DUNICODE','/D_UNICODE','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\")+$script:VersionArguments
    Invoke-BuildTool cl.exe ($flags+@('/LD',$source,'/link',"/out:$dll","/implib:$objects\host.lib",'/subsystem:windows,6.01','/osversion:6.1','/DELAY:UNLOAD','/DELAYLOAD:setupapi.dll','/DELAYLOAD:shell32.dll',$resource,'setupapi.lib','shell32.lib','cfgmgr32.lib','uuid.lib','delayimp.lib'))
    $nativeVersion=(Get-Item -LiteralPath $dll).VersionInfo
    if($nativeVersion.FileMajorPart -ne 2 -or $nativeVersion.FileMinorPart -ne 0 -or $nativeVersion.FileBuildPart -ne 0 -or $nativeVersion.FilePrivatePart -ne 0){throw 'Native helper version drift'}
    $native+=@{variant=$variant;volatile=$mode;path=$dll;sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();file_version=$script:FileVersion}
    if($variant -eq 'after') {
        $sarif=Join-Path $objects 'host.sarif'
        $analysisFlags=@($flags|Where-Object {$_ -ne '/WX'})
        Invoke-BuildTool cl.exe ($analysisFlags+@('/c','/analyze:only',"/analyze:log$sarif",$source))
        foreach($run in (Get-Content -LiteralPath $sarif -Raw|ConvertFrom-Json).runs) {foreach($result in $run.results) {
            $loc=$result.locations[0].physicalLocation;$uri=$loc.artifactLocation.uri
            if(!$uri){$uri=$run.artifacts[$loc.artifactLocation.index].location.uri}
            $diagnostics+=@{volatile=$mode;rule=$result.ruleId;uri=$uri;line=$loc.region.startLine;message=$result.message.text}
        }}
    }
}}
$own=@($diagnostics|Where-Object {$_.uri -like 'file:///D:/rust/wintun/*'})
if($own.Count){$own|ConvertTo-Json -Depth 5;throw 'Native helper has project analysis diagnostics'}
if($script:Version -cne '2.0.0'){throw 'Audit version changed during helper build'}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during helper audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    native_helpers=$native;native_helper_project_diagnostics=$own.Count;native_helper_diagnostics=$diagnostics;
    method='Frozen full host.c and host_win7.h hashes with unchanged loader/version/INF dependencies. ISO/MS Win7 API target. Exact old controls 92 parser failure reported success, 93 missing argument reported success, 94 Win7 helper uses legacy Wintun hardware ID against actual PacketBridgeV2 INF. Actual handler bodies, native command-line parser/formatter/LocalFree/pipe I/O and event/heap ownership; every device call hosted. Current argument matrix has 612 schedules per mode, including native API warmup with exact per-case ownership then two equal stable process-handle measurements. Identity matrix has 549 schedules per mode, reads real repository INF, validates double-NUL HWID, driver-list/parent ownership and explicit fixture reclamation after failed modeled cleanup. All repaired controls run too. Real production helper DLLs compiled before/after for both modes, not loaded or invoked; current helper analysis requires zero project diagnostics. No actual device changes, installation, native kernel or old-system runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
