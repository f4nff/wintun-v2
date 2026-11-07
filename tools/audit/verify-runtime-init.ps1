param([string]$BaselineDirectory='out\wait-retirement-baseline\9d75adca3c0b',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')
. (Join-Path $PSScriptRoot '..\build\runtime-init-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Runtime-init audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
if($baseline -ne (Join-Path $script:RepoRoot 'out\wait-retirement-baseline\9d75adca3c0b')){throw 'Unexpected runtime-init accepted baseline'}
$mainHash='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'
$baselineMain=Join-Path $baseline 'api\main.c'
if((Get-FileHash -LiteralPath $baselineMain).Hash.ToLowerInvariant() -ne $mainHash){throw 'Frozen complete main.c changed'}
$dependencies=@(rg --files api common include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate runtime-init dependencies'}
$dependencies+='tests\safety\guard_memory.h'
foreach($relative in $dependencies){
    if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumDependency -BaselineDirectory $baseline -Relative $relative}
    else{Assert-HelperDrainDependency -BaselineDirectory $baseline -Relative $relative}
}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\runtime-init\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false);$original=[IO.File]::ReadAllText($baselineMain)
function Assert-RuntimeInitProjection($build){
    if($build.source_sha256 -ne $mainHash -or [IO.File]::ReadAllText($build.whole) -cne $original -or (Get-FileHash -LiteralPath $build.whole).Hash.ToLowerInvariant() -ne $mainHash){throw 'Builder did not preserve the entire accepted main.c'}
    $expected=$original;$tail='    DWORD error = GetLastError(); MainRuntimeRelease(); SetLastError(error); return FALSE;'
    if([regex]::Matches($expected,[regex]::Escape($tail)).Count -ne 1){throw 'Actual failure release tail not unique'}
    if($build.variant -eq 'without-failure-lease-release'){$expected=$expected.Replace($tail,'    DWORD error = GetLastError(); /* Observer122: omit only failed-acquisition lease release. */ SetLastError(error); return FALSE;')}
    $api=Join-Path $script:RepoRoot 'api';$expected=[regex]::Replace($expected,'(?m)^#include "([^"]+)"',{
        param($match)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $match.Groups[1].Value)).Replace('\','/')+'"'
    })
    if([IO.File]::ReadAllText($build.main) -cne $expected){throw 'Projected full main.c changed outside one failure release omission/include paths'}
    foreach($item in @($build.parts)+@($build.sources)){if((Get-FileHash -LiteralPath $item.path).Hash.ToLowerInvariant() -ne $item.sha256){throw 'Runtime-init artifact/source hash changed'}}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_RUNTIME_INIT_SOURCE "'+$build.main.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\runtime_init_fixture.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join [Environment]::NewLine)+[Environment]::NewLine)){throw 'Fixture entry has unapproved source overrides'}
    $lines=@();if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\runtime_init_tests.c').Replace('\','/')+'"'
    if([IO.File]::ReadAllText($build.host_entry) -cne (($lines -join [Environment]::NewLine)+[Environment]::NewLine)){throw 'Host entry has unapproved overrides'}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-RuntimeInitProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start runtime-init test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Runtime-init watchdog expired (30s)'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-runtime-init.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $marker='RUNTIME_INIT_FAILURE_LEASE_NOT_RELEASED: no caller lease remained but real DependencyUsers stayed one; genuine release compensation, dependency unload, all caller joins and DLL detach completed'
        $summary=[regex]::Match($output,'PASS: (\d+) runtime initialization schedules, (\d+) joined acquisition callers, (\d+) real release compensations; prefix mask0x([0-9a-f]+), serialized(\d+)/concurrent(\d+),')
        $full=$Arguments.Count -eq 1;$count=if($full){56}else{2};$mask=if($full){'7f'}else{'40'};$repairs=if($Expected -eq 122){2}else{0}
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 122 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $count -and [int]$summary.Groups[2].Value -eq $count -and [int]$summary.Groups[3].Value -eq $repairs -and $summary.Groups[4].Value -eq $mask -and [int]$summary.Groups[5].Value -eq $count/2 -and [int]$summary.Groups[6].Value -eq $count/2}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 122 -and $correct);watchdog_ms=30000;log=$log;
            schedules=$(if($summary.Success){[int]$summary.Groups[1].Value}else{0});joined_callers=$(if($summary.Success){[int]$summary.Groups[2].Value}else{0});compensations=$(if($summary.Success){[int]$summary.Groups[3].Value}else{0});
            prefix_mask=$(if($summary.Success){$summary.Groups[4].Value}else{''});reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unexpected runtime-init result exit=$code expected=$Expected"}
    }finally{$process.Dispose()}
}
$artifacts=@()
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-failure-lease-release')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant"
    $build=Build-RuntimeInitTests -Directory $objects -Mode $mode -Guarded:$guarded -Variant $variant;Assert-RuntimeInitProjection $build;$artifacts+=$build
    if($variant -eq 'current'){Invoke-RuntimeInitProcess $build.path @($build.fixture);Invoke-RuntimeInitProcess $build.path @($build.fixture,'122')}
    else{Invoke-RuntimeInitProcess $build.path @($build.fixture,'122') 122}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during runtime-init audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_main_sha256=$mainHash;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=($dependencies.Count-2);
    operations=$script:TestResultFile;artifacts=$artifacts;kernel_runtime_tested=$false;production_ready=$false;production_changed=$true;
    reviewed_dependency_repairs=@('api\rundll32_process.h','common/offload_policy.c');helper_drain_ledger=@{path='tools/audit/helper-drain-ledger.ps1';sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'helper-drain-ledger.ps1')).Hash.ToLowerInvariant()};
    lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};
    reviewed_production_repairs=@('common/offload_policy.c');
    method='Coverage-only regression: fixed accepted whole main.c hash and reviewed api/common/include dependencies. The api/rundll32_process.h post-exit drain repair and common/offload_policy.c LSO/checksum repair are each constrained by their separate strict complete-file ledgers; all other dependencies remain byte-identical. Historical controls and existing finite projections are retained. Builder copies the entire main.c; the only current projection is absolute local include paths, and negative122 additionally omits the unique MainRuntimeAcquire InitOnce-failure MainRuntimeRelease while retaining original GLE save/restore. All projected sources, entries, test sources and binaries are hashed and independently checked. Actual InitializeRuntime/InitializeSecurityObjects/MainRuntime/InitOnce and real security acquisition successes execute: HeapCreate/Destroy, caller-buffer SID, process token, TokenUser, file SDDL, SID string, pipe SDDL, LocalFree and token Close. Seven SDK acquisition boundaries inject one nonzero error8, never fabricate successful SDK output. Private heap/token/local-allocation ownership is tracked; namespace/cleanup/helper hooks are hosted no-ops with namespace init/done observations, so no real namespace/device/registry/driver is touched. Serialized failure verifies empty heap/security state, RuntimeReady false, original8 and safe retry. Concurrent A is gated before its one SDK failure; B already acquired a real runtime lease and marks the boundary immediately before calling actual InitOnce. This marker is not proof of its opaque internal/kernel wait. While A remains in initialization, actual Users2/Ready0 and no unload are observed. A is then gated after actual InitOnce FALSE callback return; B completes genuine retry with exactly one private heap and two security descriptors, while A retains its failed-acquire lease. A returns8 and releases only its lease; B retains Users1 with loaded genuine advapi32 delay import and no unload attempt. B release unloads it. Full current workload has28warm/28measured schedules, exact process handle equality, actual aggregate DLL detach assertions and all acquisition caller handles joined. Guarded mode protects caller-owned intermodule configuration/snapshot buffers; OS security/private-heap allocations remain genuine API allocations and are not claimed guard-paged. Current CLI122 covers serialized/concurrent last-prefix cases. Observer122 first establishes joined callers and zero true caller leases but actual Users1, compensates exactly once per fresh DLL through real MainRuntimeRelease while ordinary LoadLibrary still owns it, then retries/finishes, genuine unload and actual DLL detach with zero tracked resources precede122. Current performs no compensation and no runtime counter is edited. This is a fault-model ownership/compatibility-target regression, not evidence of a new natural failure, Windows7 execution, kernel/NDIS/IRQL or CPU measurement. Historical common policy replay admits only oldA3 to reviewed18df full-file LSO/checksum configuration repair; all other frozen sources, original ownership controls and finite projections retain their prior contracts.'}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
