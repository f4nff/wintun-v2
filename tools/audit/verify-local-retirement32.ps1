param([string]$BaselineDirectory='',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '../build/common.ps1')
. (Join-Path $PSScriptRoot '../build/local-retirement32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Local-retirement audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$historical=-not [string]::IsNullOrWhiteSpace($BaselineDirectory);$hashes=@{}
$accepted='02e1234fcd0916b5eccbd8ff67f5382b2825c0d4bcac66c9cd9562b7e5156568'
if($historical){
    $baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
    [string[]]$paths=@($manifest.source_paths);[string[]]$sorted=$paths.Clone();[Array]::Sort($sorted,[StringComparer]::Ordinal)
    if($manifest.version -cne '2.0.0' -or $manifest.source_tree_sha256 -cne $accepted -or $manifest.order -cne 'typed string[] Ordinal' -or $paths.Count -ne 694 -or @($manifest.files.PSObject.Properties).Count -ne 694 -or ($paths -join "`n") -cne ($sorted -join "`n")){throw 'Unexpected local-retirement historical manifest'}
    $set=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal);$entries=@()
    foreach($path in $paths){if(!$set.Add($path)){throw 'Duplicate local-retirement baseline path'};$hash=(Get-FileHash -LiteralPath (Join-Path $baseline $path)).Hash.ToLowerInvariant();if($hash -cne $manifest.files.$path){throw "Baseline raw bytes changed:$path"};$hashes[$path]=$hash;$entries+=$path+':'+$hash}
    foreach($name in $manifest.files.PSObject.Properties.Name){if(!$set.Contains($name)){throw 'Historical source order/file map differ'}}
    if([Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes(($entries -join "`n")))).ToLowerInvariant() -cne $accepted){throw 'Historical local-retirement fingerprint differs'}
}
$dependencies=@(rg --files api/v2 common include);if($LASTEXITCODE){throw 'Cannot enumerate local-retirement dependencies'}
$dependencies+=@('wintun.props','tests/safety/native_policy_fixture.c','tests/model/native_policy_fixture.h','tests/safety/guard_memory.h')
$dependencies=@($dependencies|ForEach-Object {$_.Replace('\','/')}|Select-Object -Unique)
$directory=if($OutputDirectory){if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}}else{Join-Path $script:RepoRoot "out/local-retirement32/$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$utf8=[Text.UTF8Encoding]::new($false)
if(!$historical){
    $baseline=Join-Path $directory 'current-source'
    foreach($relative in $dependencies){$current=Join-Path $script:RepoRoot $relative;$snapshot=Join-Path $baseline $relative;New-Item -ItemType Directory -Path (Split-Path $snapshot) -Force|Out-Null;$bytes=[IO.File]::ReadAllBytes($current);[IO.File]::WriteAllBytes($snapshot,$bytes);$hashes[$relative]=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()}
    [ordered]@{source_tree_sha256=$script:SourceFingerprint;files=$hashes}|ConvertTo-Json -Depth 4|Set-Content (Join-Path $baseline 'snapshot.json') -Encoding utf8
}
function Assert-LrDependencies {
    foreach($relative in $dependencies){$before=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative;if((Get-FileHash $before).Hash.ToLowerInvariant() -cne $hashes[$relative] -or (Get-FileHash $before).Hash -cne (Get-FileHash $current).Hash){throw "Local-retirement raw dependency differs:$relative"}}
}
Assert-LrDependencies
function Assert-LrBuild($build){
    $e=$build.extraction;$api=Join-Path $script:RepoRoot 'api/v2';$sessionSource=Join-Path $api 'session.c';$hostSource=Join-Path $script:RepoRoot 'tests/safety/native_policy_fixture.c';$test=Join-Path $script:RepoRoot 'tests/model/local_retirement32_tests.c'
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or !$build.ndebug -or $build.pb_api_tests){throw 'Local-retirement shipping mode differs'}
    $body=[regex]::Replace([IO.File]::ReadAllText((Join-Path $baseline 'api/v2/session.c')),'(?m)^#include "([^"]+)"',{param($m)'#include "'+[IO.Path]::GetFullPath((Join-Path $api $m.Groups[1].Value)).Replace('\','/')+'"'})
    if($e.session_source -cne $sessionSource -or [IO.File]::ReadAllText($e.session) -cne $body){throw 'Complete session exceeded exact quoted-include projection'}
    if($e.host -cne $hostSource -or $e.host_sha256 -cne '6508bf85aeef129b83e3dd9651fc9d12a4a42cfddde1ec023dadbe561c41ef83' -or (Get-FileHash $e.host_original).Hash.ToLowerInvariant() -cne $e.host_sha256 -or (Get-FileHash $hostSource).Hash.ToLowerInvariant() -cne $e.host_sha256){throw 'Complete frozen native-policy fixture differs'}
    $entry=@'
#define WaitForMultipleObjects LocalWait
#undef WaitForSingleObject
#define WaitForSingleObject LocalProgress
#include PB_LOCAL_RETIREMENT_SESSION_SOURCE
#undef WaitForMultipleObjects
'@
    if([IO.File]::ReadAllText($e.entry) -cne $entry){throw 'Actual SDK wait/CV observer binding differs'}
    $lines=@('#define LR_FIXTURE 1');if($build.guarded){$lines+='#define PB_GUARD_TESTS 1'};$lines+='#define PB_LOCAL_RETIREMENT_SESSION_SOURCE "'+$e.session.Replace('\','/')+'"';$lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$e.entry.Replace('\','/')+'"';$lines+='#include "'+$test.Replace('\','/')+'"'
    if($e.test -cne $test -or [IO.File]::ReadAllText($e.fixture_entry) -cne (($lines -join "`n")+"`n")){throw 'Complete local-retirement entry differs'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/DNDEBUG',"/volatile:$($build.mode)",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/DPB_TEST_IP_VERSION=$($build.ip_version)")+@($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Local-retirement exact compiler flags differ'}
    if((Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256 -or (Get-FileHash $build.fixture).Hash.ToLowerInvariant() -cne $build.fixture_sha256){throw 'Local-retirement binary hash differs'}
    $expectedSources=@($sessionSource,$e.session,$hostSource,$e.host_original,$e.entry,$e.fixture_entry,$test,(Join-Path $script:RepoRoot 'tests/model/native_policy_fixture.h'))
    if(@($build.sources).Count -ne 8){throw 'Local-retirement source metadata count must be8'}
    $sourcePaths=[Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach($source in $build.sources){if(!$sourcePaths.Add([string]$source.path)){throw 'Local-retirement source metadata duplicate path'}}
    $expectedPaths=[Collections.Generic.HashSet[string]]::new([string[]]$expectedSources,[StringComparer]::Ordinal)
    if($expectedPaths.Count -ne 8 -or !$sourcePaths.SetEquals($expectedPaths)){throw 'Local-retirement source metadata path set differs'}
    foreach($source in $build.sources){if((Get-FileHash $source.path).Hash.ToLowerInvariant() -cne $source.sha256){throw 'Local-retirement input/generated source hash differs'}}
}
function Test-LrSourceMetadata($build){
    $json=$build|ConvertTo-Json -Depth 20;$valid=$json|ConvertFrom-Json;Assert-LrBuild $valid
    $contracts=@(@{contract='real build JSON clone';result='passed';sources=@($valid.sources).Count})
    foreach($kind in @('deleted-source','duplicate-source')){
        $candidate=$json|ConvertFrom-Json
        if($kind -ceq 'deleted-source'){$candidate.sources=@($candidate.sources)[0..6];$expected='Local-retirement source metadata count must be8'}
        else{$candidate.sources[7]=$candidate.sources[0];$expected='Local-retirement source metadata duplicate path'}
        $reason=$null;try{Assert-LrBuild $candidate}catch{$reason=$_.Exception.Message}
        if($reason -cne $expected){throw "Local-retirement metadata contract accepted/wrong rejection:$kind ($reason)"}
        $contracts+=@{contract=$kind;result='rejected';reason=$reason}
    }
    return $contracts
}
function Get-LrRows([string]$text,[int]$family){
    $rows=[regex]::Matches($text,'(?m)^CASE family=([46]) protocol=(tcp|udp) wrap=([01]) loan=([01]) fault=([01]) measured=([01]) release=1/7/24 loans=31/24/0 producer=(1/8/32|0/0/0) progress=1/1/1 native_barrier=1 waits=5 timeout=0 public_joins=2 callback_joins=(\d+)\r?$')
    if($rows.Count -ne 32 -or [regex]::Matches($text,'(?m)^CASE ').Count -ne 32){throw 'Local-retirement case rows missing/extra'};$callbacks=0;$localTokens=0
    for($i=0;$i -lt 32;++$i){$shape=$i%16;$protocol=if($shape -ge 8){'udp'}else{'tcp'};$wrap=[int][Math]::Floor($shape/4)%2;$loan=[int][Math]::Floor($shape/2)%2;$fault=$shape%2;$g=$rows[$i].Groups;$joined=[int]$g[8].Value
        if([int]$g[1].Value -ne $family -or $g[2].Value -cne $protocol -or [int]$g[3].Value -ne $wrap -or [int]$g[4].Value -ne $loan -or [int]$g[5].Value -ne $fault -or [int]$g[6].Value -ne [int]($i -ge 16) -or $g[7].Value -cne $(if($fault){'0/0/0'}else{'1/8/32'}) -or $joined -lt 2 -or $joined -gt 8){throw "Local-retirement exact shape/ownership row differs:$i"};$callbacks+=$joined;if($fault){$localTokens+=32}
    }
    $summary=[regex]::Matches($text,'(?m)^PASS: LOCALRETIRE32 family([46]) cases(\d+) warm16 measured16 publicJoins(\d+) callbackJoins(\d+) handles(\d+)/(\d+) owners=0\r?$')
    if($summary.Count -ne 1 -or [regex]::Matches($text,'(?m)^PASS:').Count -ne 1){throw 'Nonunique local-retirement summary'};$g=$summary[0].Groups
    if([int]$g[1].Value -ne $family -or [int]$g[2].Value -ne 32 -or [int]$g[3].Value -ne 64 -or [int]$g[4].Value -ne $callbacks -or $g[5].Value -cne $g[6].Value){throw 'Local-retirement summary/actual join sums/handle equality differ'}
    return @{cases=32;checked_warmup_schedules=16;measured_schedules=16;public_owner_joins=64;callback_os_thread_joins=$callbacks;actual_lifecycle_waits=160;actual_lifecycle_timeouts=0;locally_retired_tokens=$localTokens;normally_recycled_tokens=(1024-$localTokens);process_handles_before=[int]$g[5].Value;process_handles_after=[int]$g[6].Value}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-LrProcess($build){
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.ArgumentList.Add($build.fixture);$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start local-retirement process'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync();if(!$process.WaitForExit(60000)){$process.Kill($true);$process.WaitForExit();throw 'Local-retirement watchdog expired60s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$log=Join-Path $script:TestLogDirectory ('{0:d2}-ip{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded);[IO.File]::WriteAllText($log,$output,$utf8)
        if($process.ExitCode){throw "Local-retirement exit$($process.ExitCode):$log"};$metrics=Get-LrRows $output $build.ip_version;$script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($build.fixture);exit_code=$process.ExitCode;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;metrics=$metrics;log=$log;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})});Write-TestRecording 'running';Write-Output ($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})
    }finally{$process.Dispose()}
}
$builds=@();foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){$memory=if($guarded){'guarded'}else{'ordinary'};$build=Build-LocalRetirement32Tests -Directory (Join-Path $directory "ipv$family/$mode-$memory") -IpVersion $family -Mode $mode -Guarded:$guarded;Assert-LrBuild $build;$builds+=,$build;Invoke-LrProcess $build}}}
$sourceMetadataContracts=Test-LrSourceMetadata $builds[0]
$wrongFamilyRejected=$false;try{Get-LrRows ([IO.File]::ReadAllText($script:TestExecutions[0].log)) 6|Out-Null}catch{$wrongFamilyRejected=$true};if(!$wrongFamilyRejected){throw 'Local-retirement wrong-family log accepted'}
$guards=@();$reference=$builds[0];foreach($contract in @('pb-api-tests','missing-ndebug','missing-fingerprint','invalid-family')){
    $guardDir=Join-Path $directory "compile-contracts/$contract";New-Item -ItemType Directory -Path $guardDir -Force|Out-Null;$flags=@($reference.flags)
    if($contract -ceq 'pb-api-tests'){$flags+='/DPB_API_TESTS=1'}elseif($contract -ceq 'missing-ndebug'){$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'})}elseif($contract -ceq 'missing-fingerprint'){$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})}else{$flags=@($flags|Where-Object {$_ -notlike '/DPB_TEST_IP_VERSION=*'})+'/DPB_TEST_IP_VERSION=0'}
    $output=@(& cl.exe @flags /c $reference.extraction.test "/Fo$guardDir/contract.obj" 2>&1);$code=$LASTEXITCODE;$output|Set-Content (Join-Path $guardDir 'compiler.log') -Encoding utf8
    $diagnostic=switch($contract){'pb-api-tests'{'Shipping local retirement required'}'missing-ndebug'{'Shipping NDEBUG required'}'missing-fingerprint'{'shipping fingerprint required'}'invalid-family'{'Select one IP family'}}
    if(!$code -or ($output -join "`n").IndexOf($diagnostic,[StringComparison]::Ordinal) -lt 0){throw "Wrong local-retirement compile rejection:$contract"};$guards+=@{contract=$contract;exit_code=$code;expected_diagnostic=$diagnostic;log=(Join-Path $guardDir 'compiler.log')}
}
Assert-LrDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during local-retirement audit'}
Write-TestRecording 'passed'
$families=@(foreach($family in @(4,6)){$runs=@($script:TestExecutions|Where-Object {$_.ip_version -eq $family});@{ip_version=$family;executed_processes=$runs.Count;cases=($runs|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;measured_schedules=($runs|ForEach-Object {$_.metrics.measured_schedules}|Measure-Object -Sum).Sum;callback_os_thread_joins=($runs|ForEach-Object {$_.metrics.callback_os_thread_joins}|Measure-Object -Sum).Sum}})
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;source_mode=$(if($historical){'explicit-historical'}else{'current-source'});baseline=$baseline;baseline_source_tree_sha256=$(if($historical){$accepted}else{$null});current_snapshot_source_tree_sha256=$(if($historical){$null}else{$script:SourceFingerprint});operations=$script:TestResultFile;builds=$builds;family_summaries=$families;
    executed_processes=$script:TestExecutions.Count;cases=($script:TestExecutions|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;compile_contracts=$guards;source_metadata_contracts=$sourceMetadataContracts;wrong_family_log_rejected=$wrongFamilyRejected;reviewed_dependencies=$dependencies.Count;unchanged_dependencies=$dependencies.Count;reviewed_production_repairs=@();production_closure_changed=$false;kernel_runtime_tested=$false;production_ready=$false;ndebug=$true;pb_api_tests=$false;
    method='Coverage only, no production defect/fix/before variant. Complete shipping session, queue and lifecycle bodies execute unchanged in a real unloadable DLL with the original whole native-policy allocation/quota/module/private-pool/callback-thread ledger. Two real owners use separate queues; one imports32 valid1500B IPv4/IPv6 TCP/UDP loans, another reaches the actual queue Wait SDK boundary with valid private handles. Normal mode models only shared Terminal/Sequence/progress/user hints; it is not a full SYS Fault/Quiesce. Defensive mode injects exactly WAIT_FAILED with ERROR_NOT_ENOUGH_MEMORY at that boundary, without exhausting memory or closing handles; production QueueLeave latches Fatal/CloseRequested, and this is not an observed natural OS failure or production bug. Loan owner calls actual ReleaseBatch in caller-selected1/7/24 batches with full acceptance. Terminal-only has Fatal0 and publishes real TX_RECYCLE Producer1/8/32. Fatal1 branch retires private ownership with Producer0/0/0. Both keep legal gen1 tokens, explicit Loans31/24/0 and matching FREE/PUBLISHED slot states. No fake FULL, partial API result, slot/refcount/counter repair, payload access after transfer or manual event reset. After consuming stale stop hints, each success sets the actual auto-reset Progress hint0-to1; controller PrepareClose0 repeats a coalesced stop hint, returns BUSY while loans remain and QUARANTINED when zero loans still face independent NativeActive1. Actual SDK lifecycle waits consume each hint with no timeout fallback. All public owners join before nativeActive0 and finalClose; actual non-final Prepare confirms pub+registry refs2, module pins2, Region1/Closed0 and retains real resources until physical close. Final Close consumes public ownership only after joins; captured callbacks are counted dynamically, exact final count equals completed preparation callbacks plus1, and every OS handle joins/closes before true DLL detach/resource convergence. Default audit snapshots complete current declared raw dependencies and source fingerprint, with no historical out dependency. Explicit694-file02e123 baseline is fully typed-Ordinal authenticated and raw-compared. No actual device/driver/registry/SYS/NDIS/IRQL, old-OS runtime, CPU/throughput or internal-copy proof. Guards cover original native allocations/map, not ordinary caller/config storage. Windows7 SDK target and version2.0.0 remain unchanged.';
    audit_sources=@('tests/model/local_retirement32_tests.c','tools/build/local-retirement32-tests.ps1','tools/audit/verify-local-retirement32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}})}|ConvertTo-Json -Depth 18|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
