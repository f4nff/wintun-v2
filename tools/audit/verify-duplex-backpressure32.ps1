param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/duplex-backpressure32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Duplex audit requires version2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$baselineHashes=@{}
$accepted=if($historical){'dc6f9aef0663074ccdccf4413f0d1f6cdeae993b0daac5fa96900ecf54f598c1'}else{$script:SourceFingerprint}
if($historical){
    # Explicit historical mode fails closed before creating any audit output.
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
    $manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    $set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.order -cne 'typed string[] Ordinal' -or $paths.Count -ne 678 -or @($manifest.files.PSObject.Properties).Count -ne 678 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected duplex accepted baseline'}
    foreach($path in $paths){if(!$set.Add($path)){throw 'Duplicate duplex baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Baseline raw bytes changed:$path"};$baselineHashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($name in $manifest.files.PSObject.Properties.Name){if(!$set.Contains($name)){throw 'Duplex baseline file map differs'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Duplex baseline whole fingerprint differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate duplex source dependencies'}
$dependencies+=@('api/session.c','api/compat_worker.h','api/compat_pump.h','api/termination_wait.h','api/adapter.h','api/logger.h','api/main.h','api/wintun.h','api/operation.h','wintun.props',
    'tests/safety/facade_stop_fixture.c','tests/safety/native_policy_fixture.c','tests/model/facade_stop_fixture.h','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/duplex-backpressure32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
if(!$historical){
    # The default needs no old out artifact. Authenticate complete current raw
    # dependencies before compilation, retain their byte snapshots, and bind
    # the unchanged declared closure and complete tree fingerprint at the end.
    $baseline=Join-Path $directory 'current-source'
    foreach($relative in $dependencies){$current=Join-Path $script:RepoRoot $relative;$snapshot=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $snapshot) -Force|Out-Null
        $bytes=[IO.File]::ReadAllBytes($current);[IO.File]::WriteAllBytes($snapshot,$bytes);$baselineHashes[$relative]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
    }
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$baselineHashes}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-DpDependencies {
    foreach($relative in $dependencies){$before=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $baselineHashes[$relative] -or (Get-FileHash $current).Hash -cne (Get-FileHash $before).Hash){throw "Duplex unchanged dependency differs:$relative"}}
}
Assert-DpDependencies
function Convert-DpExpected([string]$body,[string]$base){return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $base $m.Groups[1].Value)).Replace('\','/')+'"'})}
function Assert-DpBuild($build){
    $e=$build.extraction;$api=Join-Path $script:RepoRoot 'api';$v2=Join-Path $api 'v2';$test=Join-Path $script:RepoRoot 'tests/model/duplex_backpressure32_tests.c'
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or !$build.ndebug -or $build.pb_api_tests){throw 'Duplex shipping mode differs'}
    $expected="#define memcpy DuplexCopy`n"+(Convert-DpExpected ([IO.File]::ReadAllText((Join-Path $baseline 'api/session.c'))) $api)+"`n#undef memcpy`n"
    if($e.legacy_source -cne (Join-Path $api 'session.c') -or [IO.File]::ReadAllText($e.legacy) -cne $expected){throw 'Duplex legacy body exceeded include/copy observer binding'}
    if($e.native_source -cne (Join-Path $v2 'session.c') -or [IO.File]::ReadAllText($e.native) -cne (Convert-DpExpected ([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c'))) $v2)){throw 'Duplex complete native body differs'}
    $baseSource=Join-Path $script:RepoRoot 'tests/safety/facade_stop_fixture.c';$nativeHost=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c'
    if($e.base_source -cne $baseSource -or $e.native_host -cne $nativeHost -or $e.base_source_sha256 -cne '42ed2b2b099bca8b6d5be8c8d6fb843ee529ad8be5549b0643899d813bc9fcc3' -or $e.native_host_sha256 -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83' -or (Get-FileHash $e.base_original).Hash -cne (Get-FileHash $baseSource).Hash -or [IO.File]::ReadAllText($e.base) -cne (Convert-DpExpected ([IO.File]::ReadAllText((Join-Path $baseline 'tests/safety/facade_stop_fixture.c'))) (Split-Path $baseSource))){throw 'Frozen duplex ownership host exceeded include projection'}
    $entry=@'
#undef DeviceIoControl
#define DeviceIoControl FacadeIo
#define WaitForMultipleObjects BackWait
#define PbQueueArm BackArm
#include PB_FACADE_NATIVE_SOURCE
#undef WaitForMultipleObjects
#undef PbQueueArm
'@
    if([IO.File]::ReadAllText($e.entry) -cne $entry){throw 'Duplex native SDK entry binding differs'}
    $lines=@('#define BP_FIXTURE 1');if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_FACADE_NATIVE_SOURCE "'+$e.native.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"'
    $lines+='#define PB_FACADE_LEGACY_SOURCE "'+$e.legacy.Replace('\','/')+'"';$lines+='#define PB_BACKPRESSURE_BASE "'+$e.base.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    if($e.test -cne $test -or [IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Duplex final source entry differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$($build.mode)",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/I$api")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Exact duplex shipping compiler flags differ'}
    if((Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256 -or (Get-FileHash $build.fixture).Hash.ToLowerInvariant() -cne $build.fixture_sha256){throw 'Duplex binary binding changed'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -cne $source.sha256){throw 'Duplex source/artifact binding changed'}}
}
function Get-DpRows([string]$text,[int]$family){
    $rows=[regex]::Matches($text,'(?m)^CASE family=([46]) protocol=(tcp|udp) wrap=([01]) stop=([01]) reverse=([01]) measured=([01]) in=(\d+) out=(\d+) pending_retired=(\d+) reserve=1 commit=1 release=2 poll=([12]) copied_bytes=(\d+) waits=(\d+) joins=(\d+)\r?$')
    if($rows.Count -ne 48 -or [regex]::Matches($text,'(?m)^CASE ').Count -ne 48){throw 'Missing/extra duplex case row'};$callbacks=0;$waits=0
    for($i=0;$i -lt 48;++$i){$shape=$i%12;$stop=$shape%3 -eq 2;$reverse=$shape%3 -eq 1;$wrap=[int][Math]::Floor($shape/3)%2;$protocol=if($shape -ge 6){'udp'}else{'tcp'};$measured=[int]($i -ge 24);$row=$rows[$i];$g=$row.Groups
        if([int]$g[1].Value -ne $family -or $g[2].Value -cne $protocol -or [int]$g[3].Value -ne $wrap -or [int]$g[4].Value -ne [int]$stop -or [int]$g[5].Value -ne [int]$reverse -or [int]$g[6].Value -ne $measured -or [int]$g[7].Value -ne $(if($stop){6}else{32}) -or [int]$g[8].Value -ne $(if($stop){7}else{32}) -or [int]$g[9].Value -ne $(if($stop){26}else{0}) -or [int]$g[10].Value -ne $(if($stop){1}else{2}) -or [int]$g[11].Value -ne $(if($stop){19500}else{96000}) -or [int]$g[12].Value -lt 4 -or [int]$g[13].Value -lt 1 -or [int]$g[13].Value -gt 8){throw "Duplex exact family/protocol/ownership row differs:$i"}
        $callbacks+=[int]$g[13].Value;$waits+=[int]$g[12].Value
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: family([46]) duplex backpressure32 cases(\d+) warm24 measured24 endjoins(\d+) callbackjoins(\d+) handles(\d+)/(\d+)\r?$')
    if($summary.Count -ne 1 -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1){throw 'Nonunique duplex summary'};$g=$summary[0].Groups
    if([int]$g[1].Value -ne $family -or [int]$g[2].Value -ne 48 -or [int]$g[3].Value -ne 48 -or [int]$g[4].Value -ne $callbacks -or $g[5].Value -cne $g[6].Value){throw 'Duplex actual join sum/handle equality differs'}
    return @{cases=48;checked_warmup_schedules=24;measured_schedules=24;end_caller_joins=48;callback_os_thread_joins=$callbacks;actual_sdk_wait_entries=$waits;process_handles_before=[int]$g[5].Value;process_handles_after=[int]$g[6].Value}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-DpProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.ArgumentList.Add($build.fixture)
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start duplex process'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'Duplex process watchdog expired60s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ip{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        if($process.ExitCode){throw "Duplex process exit$($process.ExitCode):$log"};$metrics=Get-DpRows $output $build.ip_version
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;log=$log;metrics=$metrics;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})});Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})
    }finally{$process.Dispose()}
}
$builds=@();foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){$memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-DuplexBackpressure32Tests -Directory (Join-Path $directory "ipv$family/$mode-$memory") -IpVersion $family -Mode $mode -Guarded:$guarded;Assert-DpBuild $build;$builds+=,$build;Invoke-DpProcess $build}}}
$wrongFamilyRejected=$false;try{Get-DpRows ([IO.File]::ReadAllText($script:TestExecutions[0].log)) 6|Out-Null}catch{$wrongFamilyRejected=$true};if(!$wrongFamilyRejected){throw 'Wrong-family log accepted'}
$guards=@();$reference=$builds[0];foreach($contract in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $guardDir=Join-Path $directory "compile-contracts/$contract";New-Item -ItemType Directory -Path $guardDir -Force|Out-Null;$flags=@($reference.flags)
    if($contract -ceq 'pb-api-tests'){$flags+='/DPB_API_TESTS=1'}elseif($contract -ceq 'missing-ndebug'){$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'})}elseif($contract -ceq 'missing-fingerprint'){$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})}else{$flags=@($flags|Where-Object {$_ -notlike '/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0'}
    $compilerOutput=@(& cl.exe @flags /c $reference.extraction.test "/Fo$guardDir/contract.obj" 2>&1);$exitCode=$LASTEXITCODE;$compilerOutput|Set-Content (Join-Path $guardDir 'compiler.log') -Encoding utf8
    $diagnostic=switch($contract){'pb-api-tests'{'Shipping native paths required'}'missing-ndebug'{'Shipping facade branch required'}'missing-fingerprint'{'facade stop tests require shipping fingerprint layout'}'invalid-family'{'Select one IP family'}}
    if(!$exitCode -or ($compilerOutput -join "`n").IndexOf($diagnostic,[StringComparison]::Ordinal) -lt 0){throw "Duplex compile contract did not reject the expected condition:$contract"};$guards+=@{contract=$contract;exit_code=$exitCode;expected_diagnostic=$diagnostic;log=(Join-Path $guardDir 'compiler.log')}
}
Assert-DpDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during duplex audit'}
Write-TestRecording 'passed'
$familySummaries=@(foreach($family in @(4,6)){$executions=@($script:TestExecutions|Where-Object {$_.ip_version -eq $family});@{ip_version=$family;executed_processes=$executions.Count;cases=($executions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;measured_schedules=($executions|ForEach-Object {$_.metrics.measured_schedules}|Measure-Object -Sum).Sum;callback_os_thread_joins=($executions|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$familySummaries;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$guards;wrong_family_log_rejected=$wrongFamilyRejected;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;
    kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only, no production defect or before variant. Complete unchanged facade-stop/native-policy ledgers execute actual legacy worker/default native Open, real non-PB_API_TESTS queue/lifecycle, real SDK waits and unloadable DLL. Natural geometry:64/128 native slots/ring; outgoing64 published1500B seed packets consume all RX slots; incoming2x61000B seed plus32x1500B target fills the128KiB private ring with seed2+target6 and holds26 actual native loans. Outgoing target32 encounters actual pool FULL. Returning32 or7 seed completions gives one nonempty actual Reserve32/7 generation2 and one Commit32/7. Recovery tests both completion-before-private-Wake and private-Wake-before-completion under a pre-SDK entry gate, preserves FIFO and copies each incoming/outgoing target exactly once. Stop keeps incoming26 uncopied, discards outgoing25 private bytes, and actual ReturnPending retires26 without copying. Per-sequence copy hits are scoped to explicit legacy memcpy calls crossing native/private ranges; seed/injection/metadata copies excluded, not a CPU benchmark. Release batches are8 then26; actual nonempty Poll batches are32+64 or7. Empty polls/reserve attempts and legal callback counts are not fixed. Ready-arm counts remain unchanged while unconsumable pending exists. Terminal is only a hosted hint; independent NativeActive1 blocks physical close until actual callback park and host release. All packet calls/pointers complete before End; actual worker5000 close and retained native cleanup use original code; dynamic callback OS handles and End caller join before true DLL unload and exact original resource ledgers/strict post-warm process handle equality. No manual loan/refcount/event repair, actual driver/device/registry/NDIS/IRQL or old-system runtime is claimed. SDK markers identify selected API entry, not kernel scheduler park. Guards cover inherited native allocations/map; legacy rings retain VirtualAlloc and host/test state is ordinary live storage. Windows7 SDK target and version2.0.0 preserved.';
    audit_sources=@('tests/model/duplex_backpressure32_tests.c','tools/build/duplex-backpressure32-tests.ps1','tools/audit/verify-duplex-backpressure32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
