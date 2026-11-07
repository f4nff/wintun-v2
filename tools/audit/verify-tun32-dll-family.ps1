param([string]$BaselineDirectory='out\ip-family-baseline\e9e18f1bacc4',[string]$OutputDirectory='',[switch]$IncludeTerminal32)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-dll-family-tests.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'TUN32 DLL family requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path;$accepted='e9e18f1bacc485b39643e9f4aa1dcf9cf1f5319d467b1bbe3920b93c255c5540'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.source_tree_sha256 -cne $accepted -or $manifest.status -cne 'accepted'){throw 'Unexpected DLL family accepted baseline'}
$production=@(rg --files api common include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate DLL family production closure'}
$immutable=@($production)+@('tests\safety\guard_memory.h')
$currentTests=@('tests\model\compat_session_tests.c','tests\model\compat_tun32_cases.h','tests\model\compat_terminal32_cases.h')
function Assert-DllFamilyDependencies {
    foreach($relative in $immutable){
        $before=Join-Path $baseline $relative;$current=Join-Path $script:RepoRoot $relative;$old=(Get-FileHash -LiteralPath $before).Hash.ToLowerInvariant()
        if($manifest.files.($relative.Replace('\','/')) -cne $old){throw "Frozen DLL family manifest changed:$relative"}
        if($relative.Replace('\','/') -ceq 'common/offload_policy.c'){Assert-LsoChecksumFilePair -Before $before -Current $current -Relative $relative}
        elseif((Get-FileHash -LiteralPath $current).Hash.ToLowerInvariant() -cne $old){throw "Immutable DLL family dependency changed:$relative"}
    }
}
Assert-DllFamilyDependencies
$policyChanged=(Get-FileHash (Join-Path $baseline 'common\offload_policy.c')).Hash -cne (Get-FileHash (Join-Path $script:RepoRoot 'common\offload_policy.c')).Hash
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-dll-family\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
function Assert-DllFamilyBuild($build){
    $expectedSources=@($immutable+$currentTests|Sort-Object -Unique)
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or @($build.sources).Count -ne $expectedSources.Count -or @($build.sources.relative|Select-Object -Unique).Count -ne $expectedSources.Count -or @(Compare-Object ($build.sources.relative|Sort-Object) $expectedSources).Count){throw 'DLL family source set changed'}
    foreach($record in $build.sources){if($record.path -cne (Join-Path $script:RepoRoot $record.relative) -or $record.sha256 -cne (Get-FileHash -LiteralPath $record.path).Hash.ToLowerInvariant()){throw 'DLL family source binding/hash changed'}}
    $version=Join-Path $script:RepoRoot 'include\packetbridge_version.h';$test=Join-Path $script:RepoRoot 'tests\model\compat_session_tests.c'
    $lines=@(('#include "'+$version.Replace('\','/')+'"'),
        '_Static_assert(sizeof(PB_SOURCE_STRING)==65,"TUN32 DLL family requires shipping fingerprint");',
        '_Static_assert(PB_VERSION_MAJOR==2&&PB_VERSION_MINOR==0&&PB_VERSION_PATCH==0,"Version remains2.0.0");',
        '#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6','#error Select an independent IP family','#endif',
        '#define main Tun32FamilyHostMain',('#include "'+$test.Replace('\','/')+'"'),'#undef main',
        'int main(int argc,char **argv){printf("FAMILY: IPv%d\n",PB_TEST_IP_VERSION);return Tun32FamilyHostMain(argc,argv);}')
    if([IO.File]::ReadAllText($build.entry) -cne (($lines -join "`n")+"`n") -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256 -or (Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256){throw 'DLL family entry/executable binding changed'}
    $work=Split-Path $build.path
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/DPB_API_TESTS=1',"/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$work\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'DLL family compiler flags changed'}
}
function Assert-DllFamilyRows([string]$text,[int]$family,[string]$kind){
    $witness=@([regex]::Matches($text,'(?m)^FAMILY: IPv([46])\r?$'))
    if($witness.Count -ne 1 -or [int]$witness[0].Groups[1].Value -ne $family){throw 'Missing or mixed compiled family witness'}
    if($kind -ceq 'full'){
        $rows=@($text -split '\r?\n'|Where-Object {$_.StartsWith('TUN32 ')})
        $vectors=@($text -split '\r?\n'|Where-Object {$_.StartsWith('TUN32_VECTOR ')})
        if($rows.Count -ne 16 -or $vectors.Count -ne 16 -or @($text -split '\r?\n'|Where-Object {$_.StartsWith('TUN32_PARTIAL ') -or $_.StartsWith('TERMINAL32 ')}).Count){throw 'Full DLL family matrix row count changed'}
        for($i=0;$i -lt 16;++$i){$shape=$i%8;$phase=if($i -lt 8){'warm'}else{'measure'};$protocol=if([int][Math]::Floor($shape/2)%2){'UDP'}else{'TCP'};$wrap=$shape%2;$length=if($shape -lt 4){1500}else{65535};$turns=if($length -eq 1500){1}else{4};$bytes=[uint64]32*$length;$batch=32/$turns
            $row="TUN32 phase=$phase ip=$family transport=$protocol wrap=$wrap length=$length reserve=$turns commit=$turns receive=1 release=$turns poll-nonempty=1 copies=32/32 bytes=$bytes/$bytes opaque=preserved"
            $perTurn=(@("$batch/$batch/0")*$turns)-join ','
            $vector="TUN32_VECTOR phase=$phase ip=$family transport=$protocol wrap=$wrap length=$length reserve=$perTurn commit=$perTurn receive=64/32/0 release=$perTurn poll=64/32/0"
            if($rows[$i] -cne $row -or $vectors[$i] -cne $vector){throw "Full DLL family row/vector mismatch:$family/$i"}
        }
        $summary=[regex]::Match($text,'(?m)^PASS: (\d+) actual legacy/v2 32-packet duplex cases \((\d+) warm/(\d+) measured\), valid IPv([46]) TCP/UDP and opaque direction/sequence/64-bit markers; exact nonempty batch vectors and one payload copy per direction/packet; handles(\d+)/(\d+)\.\r?$')
        $count=16;$warm=8
        if(!$summary.Success -or [int]$summary.Groups[4].Value -ne $family -or $summary.Groups[5].Value -cne $summary.Groups[6].Value){throw 'Full DLL family summary/handles changed'}
        $before=[int]$summary.Groups[5].Value;$after=[int]$summary.Groups[6].Value
    }elseif($kind -ceq 'partial'){
        $rows=@($text -split '\r?\n'|Where-Object {$_.StartsWith('TUN32_PARTIAL ')})
        if($rows.Count -ne 48 -or @($text -split '\r?\n'|Where-Object {$_.StartsWith('TUN32 ') -or $_.StartsWith('TUN32_VECTOR ') -or $_.StartsWith('TERMINAL32 ')}).Count){throw 'Partial DLL family matrix row count changed'}
        for($i=0;$i -lt 48;++$i){$shape=$i%24;$phase=if($i -lt 24){'warm'}else{'measure'};$protocol=if($shape -lt 12){'TCP'}else{'UDP'};$free=@(1,7,31)[[int][Math]::Floor($shape/4)%3];$stop=[int][Math]::Floor($shape/2)%2;$wrap=$shape%2;$calls=if($stop){1}else{2};$tail=if($stop){0}else{32-$free};$pollCalls=if($stop){0}else{2};$polled=if($stop){0}else{96-$free};$copies=if($stop){$free}else{32};$bytes=1500*$copies
            $row="TUN32_PARTIAL phase=$phase ip=$family transport=$protocol free=$free stop=$stop wrap=$wrap reserve-first=32/$free reserve-calls=$calls reserve-tail=$tail/$tail commit-first=$free/$free commit-calls=$calls commit-tail=$tail/$tail poll-calls=$pollCalls polled=$polled copies=$copies bytes=$bytes prefix-head=exact wait-code=entered owners=0"
            if($rows[$i] -cne $row){throw "Partial DLL family row mismatch:$family/$i"}
        }
        $summary=[regex]::Match($text,'(?m)^PASS: (\d+) natural partial Reserve32 cases \((\d+) warm/(\d+) measured\), free1/7/31, real completion recovery or stopped tail, wrap, exact batch/copy/head ownership, handles(\d+)/(\d+)\.\r?$')
        $count=48;$warm=24
        if(!$summary.Success -or $summary.Groups[4].Value -cne $summary.Groups[5].Value){throw 'Partial DLL family summary/handles changed'}
        $before=[int]$summary.Groups[4].Value;$after=[int]$summary.Groups[5].Value
    }elseif($kind -ceq 'terminal'){
        $rows=@($text -split '\r?\n'|Where-Object {$_.StartsWith('TERMINAL32 ')})
        if($rows.Count -ne 32 -or @($text -split '\r?\n'|Where-Object {$_.StartsWith('TUN32 ') -or $_.StartsWith('TUN32_VECTOR ') -or $_.StartsWith('TUN32_PARTIAL ')}).Count){throw 'Terminal DLL family matrix row count changed'}
        for($i=0;$i -lt 32;++$i){$shape=$i%16;$phase=if($i -lt 16){'warm'}else{'measure'};$protocol=if($shape -lt 8){'TCP'}else{'UDP'};$receive=[int][Math]::Floor($shape/4)%2;$wrap=[int][Math]::Floor($shape/2)%2;$beforeExit=$shape%2;$copies=if($receive){32}else{0};$bytes=1500*$copies
            $row="TERMINAL32 phase=$phase ip=$family transport=$protocol receive=$receive wrap=$wrap before=$beforeExit pointers=32 publicJoins=5 publicWake=1 copy=$copies bytes=$bytes nativePublish=0 owners=0"
            if($rows[$i] -cne $row){throw "Terminal DLL family row mismatch:$family/$i"}
        }
        $summary=[regex]::Match($text,'(?m)^PASS: (\d+) terminal private32 cases warm(\d+) measure(\d+) publicJoins(\d+) handles(\d+)/(\d+); native worker exited, real Wake and original pointers retire before End\r?$')
        $count=32;$warm=16
        if(!$summary.Success -or [int]$summary.Groups[4].Value -ne 160 -or $summary.Groups[5].Value -cne $summary.Groups[6].Value){throw 'Terminal DLL family caller-join/handle summary changed'}
        $before=[int]$summary.Groups[5].Value;$after=[int]$summary.Groups[6].Value
    }else{throw 'Unexpected family test kind'}
    if([int]$summary.Groups[1].Value -ne $count -or [int]$summary.Groups[2].Value -ne $warm -or [int]$summary.Groups[3].Value -ne $warm -or @($text -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')}).Count -ne 1){throw 'DLL family warm/measured summary changed'}
    return [ordered]@{cases=$count;checked_warmup_schedules=$warm;measured_schedules=$warm;tcp_cases=$count/2;udp_cases=$count/2;public_callers_joined=$(if($kind -ceq 'terminal'){160}else{0});process_handles_before=$before;process_handles_after=$after}
}
Start-TestRecording $directory 'operation-results.json'
function Invoke-DllFamilyProcess($build,[string]$kind,[string]$work){
    $argument=if($kind -ceq 'full'){'--tun32'}elseif($kind -ceq 'partial'){'--tun32-partial'}elseif($kind -ceq 'terminal'){'--tun32-terminal'}else{throw 'Unknown DLL family process kind'}
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true;$info.ArgumentList.Add($argument)
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start DLL family test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'DLL family process exceeded30s watchdog'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $work "$kind.log";[IO.File]::WriteAllText($log,$output,$encoding)
        $counts=$null
        if($code -eq 0){$counts=Assert-DllFamilyRows $output $build.ip_version $kind}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@($argument);ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;kind=$kind;exit_code=$code;expected_exit_code=0;log=$log;watchdog_ms=30000;metrics=$counts;reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($code -eq 0 -and $counts){'running'}else{'failed'});if($code -ne 0 -or !$counts){throw "DLL family test failed:$code"}
        Write-Output "IPv$($build.ip_version)/$($build.mode)/$kind $($counts.cases) cases handles$($counts.process_handles_before)/$($counts.process_handles_after)"
    }finally{$process.Dispose()}
}
$executables=@()
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "ipv$family\$mode-$memory"
    $build=Build-Tun32DllFamilyTests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-DllFamilyDependencies;Assert-DllFamilyBuild $build;Invoke-DllFamilyProcess $build 'full' $work;Invoke-DllFamilyProcess $build 'partial' $work
    if($IncludeTerminal32){Invoke-DllFamilyProcess $build 'terminal' $work};$executables+=,$build
}}}
Assert-DllFamilyDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during DLL family audit'}
$families=@();foreach($family in @(4,6)){
    $runs=@($script:TestExecutions|Where-Object ip_version -eq $family)
    $cases=($runs|ForEach-Object {$_.metrics.cases}|Measure-Object -Sum).Sum;$warm=($runs|ForEach-Object {$_.metrics.checked_warmup_schedules}|Measure-Object -Sum).Sum;$measured=($runs|ForEach-Object {$_.metrics.measured_schedules}|Measure-Object -Sum).Sum
    $expectedProcesses=if($IncludeTerminal32){12}else{8};$expectedCases=if($IncludeTerminal32){384}else{256};$expectedHalf=$expectedCases/2
    $callers=($runs|ForEach-Object {$_.metrics.public_callers_joined}|Measure-Object -Sum).Sum
    if($runs.Count -ne $expectedProcesses -or $cases -ne $expectedCases -or $warm -ne $expectedHalf -or $measured -ne $expectedHalf -or $callers -ne $(if($IncludeTerminal32){640}else{0})){throw 'Incomplete DLL family aggregate'}
    $families+=@{ip_version=$family;processes=$runs.Count;cases=$cases;checked_warmup_schedules=$warm;measured_schedules=$measured;public_callers_joined=$callers}
}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;include_terminal32=[bool]$IncludeTerminal32;production_changed=$policyChanged;reviewed_dependency_repairs=@($(if($policyChanged){'common/offload_policy.c'}));kernel_runtime_tested=$false;production_ready=$false;
    reviewed_dependencies=$immutable.Count;unchanged_dependencies=($immutable.Count-[int]$policyChanged);immutable_dependencies=($immutable.Count-[int]$policyChanged);immutable_sources=@($immutable|Where-Object {!$policyChanged -or $_.Replace('\','/') -cne 'common/offload_policy.c'}|ForEach-Object {@{relative=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});current_tests=@($currentTests|ForEach-Object {@{relative=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    operations=$script:TestResultFile;executables=$executables;families=$families;lso_checksum_ledger=@{path='tools/audit/lso-checksum-ledger.ps1';sha256=(Get-FileHash (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')).Hash.ToLowerInvariant()};audit_sources=@('tools\build\tun32-dll-family-tests.ps1','tools\audit\verify-tun32-dll-family.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Separate compiled IPv4/IPv6 executables, explicit FAMILY witness plus exact per-case IP/transport rows. Default runs full16 and partial48 per family/mode in16 independent processes,512 cases with256 checked warmups. IncludeTerminal32 adds32 cases per family/mode in8 more processes,768 cases with384 checked warmups and1280 actual public caller thread joins. The original legacy worker and actual v2 batch/pool methods execute through the existing PB_API_TESTS native peer/stop acknowledgement. Accepted e9e api/common/include dependencies and original guard helper remain raw-locked except the exact whole-file LSO checksum repair allowed by the shared ledger; all three intentionally current test files are separately hash-bound. New entry only verifies65-byte fingerprint/version, renames test main and emits selected family. Full row/vector reconstruction checks both transports, lengths1500/65535,32 versus4x8 batches, copy counts/bytes and opaque preservation. Partial vectors reflect real seed64-k PUBLISHED slots, reserve32/k,zero-accept FULL without Head advance, actual completion recovery or actual End stop and generation/slot/Head assertions. Optional terminal rows separately reconstruct TCP/UDP, send/receive, wrap and last-original-pointer return before/after actual worker exit. Four public callers retire31 original pointers, join, then a fifth returns the held first pointer through real public APIs; public Wake1 is observed before End, not a reset or native publication. Before-exit uses a forward API-entry gate after actual Wait returns; after-exit inspects only still-owned private rings and counters because native resources may already be freed. Receive32 copies/48000 bytes and release32 occur before terminal; send retirement publishes/copies none. No fake FULL, counter repair, extra Retain or manual event reset. Waiters marks wait code entry, not native scheduler park. All public users join before End; original worker/private callback joins and explicit buffer/event/heap/path/map/module/runtime cleanup precede strict process handle equality. Empty API/SDK turns are unconstrained. Guard protects caller expected buffers only; old core/legacy/native allocators remain unchanged. SDK-default historical adapter link dependency is not invoked and is not a Win7 runtime claim. NDEBUG/PB_API_TESTS are explicit; no real device/driver/registry/network, kernel/IRQL or CPU/latency/throughput result. Default selector0 and historical/64-packet entry semantics remain unchanged.'}|
    ConvertTo-Json -Depth 15|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
