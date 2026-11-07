param([string]$BaselineDirectory='out\ownership-handoff-baseline\9334ef9d25d1',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\adapter-runtime-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Adapter/runtime audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\ownership-handoff-baseline\9334ef9d25d1')){throw 'Unexpected adapter/runtime whole-source baseline'}
$frozen=@{
    'api\adapter.c'='e604c130f502dd453332236f7e02a95c54fbe3e3ceaa3df3e6092e4bed79215f'
    'api\main.c'='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'
    'api\v2\session.c'='9d06e10637c4291c0479735b8ab0e54d41706dbfc0b1e923b4c3e42e00c8b459'
    'api\v2\adapter_unified.c'='ac00917ce0afa0650bc73b1ab2f279e1eb9778a73ca2efe6ab80496ba38bf7f8'
}
foreach($relative in $frozen.Keys){
    if((Get-FileHash -LiteralPath (Join-Path $baseline $relative)).Hash.ToLowerInvariant() -ne $frozen[$relative]){throw "Unexpected frozen adapter/runtime source: $relative"}
}
$dependencies=@(rg --files api common include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate adapter/runtime dependencies'}
$dependencies+=@('tests\safety\adapter_open_tests.c','tests\safety\native_policy_fixture.c','tests\model\native_policy_fixture.h','tests\safety\guard_memory.h')
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{Assert-HelperDrainDependency -BaselineDirectory $baseline -Relative $relative}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\adapter-runtime\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Normalize-ExpectedIncludes([string]$text,[string]$relative){
    $base=Split-Path (Join-Path $script:RepoRoot $relative)
    return [regex]::Replace($text,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})
}
function Remove-ExpectedRegion([string]$text,[string]$first,[string]$next){
    $start=$text.IndexOf($first);$end=$text.IndexOf($next,$start);if($start -lt 0 -or $end -le $start){throw "Missing independent host region: $first"}
    return $text.Substring(0,$start)+$text.Substring($end)
}
function Assert-ArtifactProjection($build){
    foreach($source in $build.sources){if((Get-FileHash -LiteralPath $source.path).Hash.ToLowerInvariant() -ne $source.sha256){throw 'Artifact source hash changed'}}
    foreach($part in $build.parts){if((Get-FileHash -LiteralPath $part.path).Hash.ToLowerInvariant() -ne $part.sha256){throw 'Generated part hash changed'}}
    $adapter=[IO.File]::ReadAllText((Join-Path $baseline 'api\adapter.c'));$name=$adapter.IndexOf('OpenAdapterInternal(LPCWSTR Name)')
    $start=$adapter.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$name);$end=$adapter.IndexOf('typedef struct ADAPTER_OPERATION_INPUT',$name)
    $parts=@{};foreach($part in $build.parts){if($parts.ContainsKey($part.name)){throw 'Duplicate generated part'};$parts[$part.name]=$part}
    $expectedOpen=$adapter.Substring($start,$end-$start)
    if($parts['open-actual.h'].offset -ne $start -or $parts['open-actual.h'].length -ne $end-$start -or [IO.File]::ReadAllText($parts['open-actual.h'].path) -cne $expectedOpen){throw 'Open is not the exact complete frozen function'}
    $name=[regex]::Match($adapter,'VOID WINAPI\r?\nWintunCloseAdapter\(').Index;$start=$adapter.LastIndexOf('_Use_decl_annotations_',$name);$end=$adapter.IndexOf('#include "adapter_rename.h"',$name)
    $expectedClose=$adapter.Substring($start,$end-$start)
    if($parts['close-original.h'].offset -ne $start -or $parts['close-original.h'].length -ne $end-$start -or [IO.File]::ReadAllText($parts['close-original.h'].path) -cne $expectedClose){throw 'Close is not the exact complete frozen function'}
    if($build.variant -eq 'without-runtime-release'){
        $tail='    if (runtimeOwned) MainRuntimeRelease();';if([regex]::Matches($expectedClose,[regex]::Escape($tail)).Count -ne 1){throw 'Mutation tail not unique'}
        $expectedClose=$expectedClose.Replace($tail,'    (void)runtimeOwned; /* Observer control: omit only the actual lease release. */')
    }
    if([IO.File]::ReadAllText($parts['close-actual.h'].path) -cne $expectedClose){throw 'Close mutation changed more than the runtime release'}
    foreach($pair in @(@('main-actual.h','api\main.c'),@('session-actual.h','api\v2\session.c'))){
        $expected=Normalize-ExpectedIncludes ([IO.File]::ReadAllText((Join-Path $baseline $pair[1]))) $pair[1]
        if([IO.File]::ReadAllText($parts[$pair[0]].path) -cne $expected){throw 'Production source changed beyond local include normalization'}
    }
    # Independently replay the finite adapter-open host projection. None of the
    # source's MainRuntime or Close count substitutes survive in the fixture.
    $expected=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\adapter_open_tests.c'))
    $expected=$expected.Replace('HANDLE ModuleHeap;','/* Actual main.c owns ModuleHeap. */').Replace('BOOL IsWindows7;','/* Actual EnvInit owns IsWindows7. */').Replace('Closes,Leases;','Closes;')
    $expected=Remove-ExpectedRegion $expected 'BOOL MainRuntimeAcquire(VOID)' 'HANDLE NamespaceTakeDeviceInstallationMutex(VOID)'
    $expected=Remove-ExpectedRegion $expected 'BOOL MainOperationSleep(DWORD delay)' 'static HDEVINFO WINAPI TestSet('
    $expected=Remove-ExpectedRegion $expected 'VOID WINAPI WintunCloseAdapter(' 'static VOID RenderLog('
    $expected=$expected.Replace('Blocks[i]=(BLOCK){0};--Live;++Frees;SetLastError(ERROR_INVALID_HANDLE);return TRUE;','Blocks[i]=(BLOCK){0};--Live;++Frees;if(memory==LiveAdapter){LiveAdapter=NULL;++Closes;}SetLastError(ERROR_INVALID_HANDLE);return TRUE;')
    $expected=[regex]::Replace($expected,'\bAllocations\b','LegacyAllocations');$expected=[regex]::Replace($expected,'\bFrees\b','LegacyFrees')
    $expected=Normalize-ExpectedIncludes $expected 'tests\safety\adapter_open_tests.c'
    if([IO.File]::ReadAllText($parts['open-host.h'].path) -cne $expected){throw 'Adapter-open host changed outside finite adaptation'}
    $expected=[IO.File]::ReadAllText((Join-Path $baseline 'tests\safety\native_policy_fixture.c'))
    $expected=Remove-ExpectedRegion $expected 'PB_RESULT WINAPI PbAdapterCreate(' 'BOOL WINAPI DllMain('
    $expected=$expected.Replace('NativeRegistered && NativeDrained && !NativeActive','(NativeRegistered || UnpublishedCleanup) && (NativeDrained || UnpublishedCleanup) && !NativeActive')
    $expected=Normalize-ExpectedIncludes $expected 'tests\safety\native_policy_fixture.c'
    if([IO.File]::ReadAllText($parts['native-host.h'].path) -cne $expected){throw 'Native-policy host changed outside finite adaptation'}
    $expected=@'
#undef calloc
#define calloc CombinedCalloc
#undef CreateFileW
#define CreateFileW CombinedFile
#undef CloseHandle
#define CloseHandle CombinedClose
#undef DeviceIoControl
#define DeviceIoControl CombinedIo
'@+"`n#include `""+$parts['session-actual.h'].path.Replace('\','/')+"`"`n"
    if([IO.File]::ReadAllText($parts['session-entry.h'].path) -cne $expected){throw 'Native entry added an unapproved override'}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-AdapterRuntimeProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start adapter/runtime test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Adapter/runtime external watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-adapter-runtime.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='ADAPTER_RUNTIME_LEASE_NOT_RELEASED: native owners closed but actual DependencyUsers remained one; one real release compensation, dependency unload, all joins and DLL detach completed'
        $correct=$code -eq $Expected -and ($Expected -ne 119 -or $output.Contains($marker))
        $schedules=[regex]::Match($output,'PASS: (\d+) adapter/runtime schedules, (\d+) shutdown joins, (\d+) callback-thread joins, (\d+) compensation joins;')
        if(!$schedules.Success){$correct=$false}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 119 -and $correct);
            watchdog_ms=30000;log=$log;schedules=$(if($schedules.Success){[int]$schedules.Groups[1].Value}else{0});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unexpected adapter/runtime exit=$code expected=$Expected"}
    }finally{$process.Dispose()}
}
$artifacts=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-runtime-release')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $build=Build-AdapterRuntimeTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-ArtifactProjection $build;$artifacts+=$build
    if($variant -eq 'current'){Invoke-AdapterRuntimeProcess $build.path @($build.fixture);Invoke-AdapterRuntimeProcess $build.path @($build.fixture,'119')}
    else{Invoke-AdapterRuntimeProcess $build.path @($build.fixture,'119') 119}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during adapter/runtime audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_sources=$frozen;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=($dependencies.Count-2);operations=$script:TestResultFile;artifacts=$artifacts;
    audit_sources=@('tests\model\adapter_runtime_fixture.h','tests\safety\adapter_runtime_fixture.c','tests\model\adapter_runtime_tests.c','tools\build\adapter-runtime-tests.ps1','tools\audit\verify-adapter-runtime.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;
    reviewed_dependency_repairs=@('api\rundll32_process.h','common/offload_policy.c');helper_drain_ledger=@{path='tools/audit/helper-drain-ledger.ps1';sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    method='Coverage-only regression. Whole api/common/include dependencies and old fixture sources preserve their accepted9334 byte contracts except the reviewed api/rundll32_process.h post-exit drain repair and common/offload_policy.c LSO/checksum repair, each admitted only by its separate strict complete-file ledger. Historical controls and existing finite projections are retained. Complete OpenAdapterInternal and WintunCloseAdapter are exact extracted source with offsets/lengths/whole hashes; actual MainRuntime/main.c and non-PB_API_TESTS native session.c preserve all bodies, only local include paths normalized. Hash-locked adapter-open SDK host has only duplicate globals/runtime stubs/operation-sleep/Close removed, counter names isolated and actual heap-free observation added; Leases assertions alias real DependencyUsers. Five native-policy adapter stubs are removed and its Close assertion permits unpublished registration/drain absence without manufacturing state. Actual shipping adapter_unified.c retains/releases the real discovered adapter. Creation factory hosts discovery via actual OpenAdapterInternal, not actual device creation/install; its real MainRuntimeRetain and RuntimeOwned assignment establish the lease, never a manual Retain or counter substitute. Actual StartAdapterSession PROBE/register/policy failure holds its wrapper/core/mapping while real event-backed Device Close fails via HANDLE_FLAG_PROTECT_FROM_CLOSE. Caller CloseAdapter leaves only retained native ownership; actual shutdown0 times out and real CV waiter blocks on DependencyUsers1. Removing protection resumes actual cleanup; real WintunCloseAdapter/MainRuntimeRelease gates before genuine delay-import unload. At this gate legacy/device/mapping are closed but wrapper/core/runtime budget remain independently live. Shutdown success alone does not establish core destruction: final tail gate, joined callback-thread handles, zero native/legacy/wrapper ledgers, ordinary LoadLibrary-held public/shutdown joins and actual aggregate DLL detach separately converge. Current full workload warms and repeats every failure with exact process handle equality. Observer119 omits only actual Close runtime-release tail, waits for nativecore/wrapper/legacy zero with actual Users1 and shutdown still blocked, then compensates exactly once through real MainRuntimeRelease while ordinary DLL ref remains held; unload, shutdown/compensation/callback joins and actual DLL detach all precede119. Current never repairs. This does not access real devices/registry/installer or claim Windows7 runtime, kernel/NDIS/IRQL/security-device behavior or CPU measurement. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.'}|
    ConvertTo-Json -Depth 16|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
