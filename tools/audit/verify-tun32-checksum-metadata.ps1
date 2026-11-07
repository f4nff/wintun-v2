param([string]$BaselineDirectory='out\return-wake-baseline\4fd68e95547d',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\tun32-checksum-metadata-tests.ps1')
. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
. (Join-Path $PSScriptRoot 'tun32-projection.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Checksum metadata audit requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$accepted='4fd68e95547d0066742c1014148caa05e7a3fdddaa9b9130ac63a4c602d89a54'
$manifest=Get-Content -LiteralPath (Join-Path $baseline 'baseline.json') -Raw|ConvertFrom-Json
if($manifest.status -cne 'accepted' -or $manifest.source_tree_sha256 -cne $accepted -or @($manifest.files.PSObject.Properties).Count -ne 663){throw 'Unexpected checksum metadata baseline'}
$dependencies=@(rg --files driver common include)
if($LASTEXITCODE){throw 'Cannot enumerate checksum metadata dependencies'}
$dependencies+=@('tests\model\tun32_offload_send_tests.c','tests\model\tun32_sys_tests.c','tests\model\send_describe_tests.c','tests\model\send_window_tests.c','tests\model\drain_boundary_tests.c','tests\model\kernel_nbl_fixture.h','tests\model\send_prefix_cases.h','tests\safety\guard_memory.h','tools\build\tun32-sys-tests.ps1','tools\build\tun32-offload-send-tests.ps1')
function Assert-C32Dependencies {
    foreach($relative in $dependencies){$old=(Get-FileHash (Join-Path $baseline $relative)).Hash.ToLowerInvariant()
        if($manifest.files.($relative.Replace('\','/')) -cne $old){throw "Accepted checksum baseline changed:$relative"}
        Assert-NetworkDependency -BaselineDirectory $baseline -Relative $relative
    }
}
Assert-C32Dependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\tun32-checksum-metadata\$($script:SourceFingerprint.Substring(0,12))"}
$null=New-Item -ItemType Directory -Path $directory -Force
$encoding=[Text.UTF8Encoding]::new($false)
# Reuse the independent complete base projection, not the builder itself.
$projectionSource=Join-Path $PSScriptRoot 'verify-tun32-offload-send.ps1'
$ast=[Management.Automation.Language.Parser]::ParseFile($projectionSource,[ref]$null,[ref]$null)
$loaded=@()
foreach($name in @('Replace-O32Projection','Assert-O32Build')){
    $nodes=@($ast.FindAll({param($node)$node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -ceq $name},$true))
    if($nodes.Count -ne 1){throw 'Nonunique checksum base verifier'}
    . ([ScriptBlock]::Create($nodes[0].Extent.Text))
    $loaded+=@{path=$projectionSource;sha256=(Get-FileHash $projectionSource).Hash.ToLowerInvariant();function=$name;body=$nodes[0].Extent.Text}
}
function Assert-C32Build($build){
    Assert-O32Build $build.base
    if($build.ip_version -notin @(4,6) -or $build.base.ip_version -ne $build.ip_version -or $build.variant -notin @('current','tasks-before-family')){throw 'Checksum metadata family/variant binding changed'}
    $original=Join-Path $script:RepoRoot 'tests\model\tun32_offload_send_tests.c'
    $test=Join-Path $script:RepoRoot 'tests\model\tun32_checksum_metadata_tests.c'
    $expected=[IO.File]::ReadAllText($original)
    $expected=Replace-O32Projection $expected 'int main(int argc,char **argv)' 'int UnusedOffload32Main(int argc,char **argv)'
    foreach($relative in @('../../include/packetbridge_version.h','../../common/platform.h')){
        $absolute=[IO.Path]::GetFullPath((Join-Path (Split-Path $original) $relative)).Replace('\','/')
        $expected=Replace-O32Projection $expected ('#include "'+$relative+'"') ('#include "'+$absolute+'"')
    }
    if($build.host.source -cne $original -or $build.host.source_sha256 -cne (Get-FileHash $original).Hash.ToLowerInvariant() -or
        [IO.File]::ReadAllText($build.host.path) -cne $expected -or (Get-FileHash $build.host.path).Hash.ToLowerInvariant() -cne $build.host.sha256){throw 'Checksum host exceeded main/two-include projection'}
    $expected='#define PB_CHECKSUM_OFFLOAD_HOST "'+$build.host.path.Replace('\','/')+'"'+"`n"+[IO.File]::ReadAllText($build.base.entry)
    $expected=Replace-O32Projection $expected ('#include "'+$original.Replace('\','/')+'"') ('#include "'+$test.Replace('\','/')+'"')
    $current=Join-Path $script:RepoRoot 'driver\v2\session_describe.h'
    $source=if($build.variant -ceq 'current'){$current}else{Join-Path $baseline 'driver\v2\session_describe.h'}
    $compiled=if($build.variant -ceq 'current'){$current}else{Join-Path (Split-Path $build.path) 'describe-before.h'}
    if($build.describe.source -cne $source -or $build.describe.path -cne $compiled -or $build.describe.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant() -or
        $build.describe.sha256 -cne $build.describe.source_sha256 -or (Get-FileHash $compiled).Hash.ToLowerInvariant() -cne $build.describe.sha256 -or $build.describe.shipping -ne ($build.variant -ceq 'current')){throw 'Checksum actual compiled Describe raw source changed'}
    if($build.variant -ceq 'tasks-before-family'){
        if($build.describe.sha256 -cne '79aa2380c77311ef9bf5353cdc2ee0b75bac2126a858d40bbddbd2e37fe6f694'){throw 'Checksum baseline negative was edited'}
        $expected=Replace-O32Projection $expected ('#define PB_SEND_DESCRIBE_ACTUAL "'+$current.Replace('\','/')+'"') ('#define PB_SEND_DESCRIBE_ACTUAL "'+$compiled.Replace('\','/')+'"')
    }
    if([IO.File]::ReadAllText($build.entry) -cne $expected -or (Get-FileHash $build.entry).Hash.ToLowerInvariant() -cne $build.entry_sha256 -or
        $build.test -cne $test -or (Get-FileHash $test).Hash.ToLowerInvariant() -cne $build.test_sha256 -or (Get-FileHash $build.path).Hash.ToLowerInvariant() -cne $build.sha256){throw 'Checksum complete entry/test/executable binding changed'}
    $flags=@($build.base.arguments|Where-Object {$_ -notlike '/Fo*'})+('/Fo'+(Split-Path $build.path)+'\')
    if(($build.arguments -join "`n") -cne ($flags -join "`n")){throw 'Checksum shipping compilation flags changed'}
}
function Assert-C32Rows([string]$text,[int]$family,[string]$variant){
    $rows=[regex]::Matches($text,'(?m)^CHECKSUM32 ip([46]) protocol(6|17) kind([0-4]) enabled([01]) ndis(60014|60059) published(0|32) pushes([01]) prefix1888 payload48000 owners0\r?$')
    if($rows.Count -ne 80 -or [regex]::Matches($text,'(?m)^CHECKSUM32 ').Count -ne 80){throw 'Checksum metadata80 case rows missing'}
    $before=$variant -ceq 'tasks-before-family';$published=0
    for($i=0;$i -lt 80;++$i){$shape=$i%40;$enabled=$shape%2;$kind=[int][Math]::Floor(($shape%10)/2);$protocol=if(($shape%20) -lt 10){6}else{17};$runtime=if($shape -lt 20){'60014'}else{'60059'}
        $okay=$kind -eq 4 -or ($kind -eq 2 -and $enabled) -or (!$before -and $kind -lt 2)
        $expected=@($family,$protocol,$kind,$enabled,$runtime,$(if($okay){32}else{0}),[int][bool]$okay)
        for($field=0;$field -lt $expected.Count;++$field){if($rows[$i].Groups[$field+1].Value -cne [string]$expected[$field]){throw "Checksum metadata row$i field$field mismatch"}}
        $published+=[int]$rows[$i].Groups[6].Value
    }
    $summary=[regex]::Match($text,'(?m)^CHECKSUM_METADATA ip([46]) cases80 warm40 measure40 violations(0|32) rejected(768|1792) handles(\d+)/(\d+) mappings7680/7680 owners0 repairs0 threads0\r?$')
    if(!$summary.Success -or [regex]::Matches($text,'(?m)^CHECKSUM_METADATA ').Count -ne 1 -or [int]$summary.Groups[1].Value -ne $family -or
        [int]$summary.Groups[2].Value -ne $(if($before){32}else{0}) -or [int]$summary.Groups[3].Value -ne $(if($before){1792}else{768}) -or
        $summary.Groups[4].Value -cne $summary.Groups[5].Value -or [regex]::Matches($text,'(?m)^CHECKSUM_FAMILY_ZERO_NOT_IGNORED:').Count -ne [int]$before -or
        [regex]::Matches($text,'(?m)^PASS: NDIS checksum family selectors precede checksum task bits; plain bytes preserved and active-family errors/policy retained\r?$').Count -ne [int](!$before)){throw 'Checksum metadata exact summary/resource/variant mismatch'}
    return @{cases=80;checked_warmup_schedules=40;measured_schedules=40;published=$published;producer_pushes=$published/32;prefix_bytes=80*1888;payload_bytes=80*48000;
        violations=$(if($before){32}else{0});process_handles_before=[int]$summary.Groups[4].Value;process_handles_after=[int]$summary.Groups[5].Value;source_mappings_allocated=7680;source_mappings_freed=7680}
}
Start-TestRecording $directory 'operation-results.json'
$contracts=Test-ChecksumFamilyLedger -Original ([IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session_describe.h'))) -Repaired ([IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_describe.h'))) -Directory (Join-Path $directory 'ledger-contracts')
if($contracts.checks.Count -ne 16 -or $contracts.files.Count -ne 6){throw 'Checksum exact ledger contracts missing'}
$executables=@();$firstLog=$null
foreach($variant in @('current','tasks-before-family')){foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "$variant\ipv$family\$mode-$memory"
    $build=Build-Tun32ChecksumMetadataTests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded -Variant $variant
    Assert-C32Dependencies;Assert-C32Build $build
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{if(!$process.Start()){throw 'Cannot start checksum metadata process'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Checksum metadata30s watchdog'}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode;$log=Join-Path $work 'results.log';[IO.File]::WriteAllText($log,$text,$encoding)
        $expected=if($variant -ceq 'current'){0}else{141};$metrics=$null;if($code -eq $expected){$metrics=Assert-C32Rows $text $family $variant}
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();exit_code=$code;expected_exit_code=$expected;expected_baseline_failure=($variant -cne 'current' -and $code -eq 141);log=$log;ip_version=$family;mode=$mode;guarded=$guarded;variant=$variant;metrics=$metrics})
        Write-TestRecording $(if($code -eq $expected -and $metrics){'running'}else{'failed'});if($code -ne $expected -or !$metrics){throw "Checksum metadata process failed:$code"}
        if(!$firstLog){$firstLog=$text};Write-Output "Checksum metadata IPv$family $mode guarded$guarded $variant exit$code cases80 handles$($metrics.process_handles_before)/$($metrics.process_handles_after)"
    }finally{$process.Dispose()}
    $executables+=,$build
}}}}
$oneCase=[regex]::new('^CHECKSUM32[^\r\n]+\r?\n',[Text.RegularExpressions.RegexOptions]::Multiline)
$parserChecks=@();foreach($case in @(@{name='other-family';text=$firstLog;family=6},@{name='bad-copy-count';text=$firstLog.Replace('payload48000','payload96000');family=4},@{name='missing-case';text=$oneCase.Replace($firstLog,'',1);family=4})){
    $rejected=$false;try{$null=Assert-C32Rows $case.text $case.family 'current'}catch{$rejected=$true};if(!$rejected){throw "Checksum parser accepted:$($case.name)"};$parserChecks+=@{name=$case.name;expected_rejection=$true}
}
$compileChecks=@();$control=$executables[0];$compileDirectory=Join-Path $directory 'compile-contracts';$null=New-Item -ItemType Directory -Path $compileDirectory -Force
foreach($kind in @('without-fingerprint','without-ndebug','with-test-shim','wrong-family')){
    $flags=@($control.arguments|Where-Object {$_ -notlike '/Fo*'});$entry=$control.entry
    $marker=switch($kind){
        'without-fingerprint' {$flags=@($flags|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'});'C2338.*TUN32 offload send requires shipping fingerprint'}
        'without-ndebug' {$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'});'C1189.*TUN32 offload send requires NDEBUG configuration'}
        'with-test-shim' {$flags+='/DPB_API_TESTS=1';'C1189.*TUN32 offload send requires actual production paths'}
        'wrong-family' {
            $entry=Join-Path $compileDirectory 'wrong-family.c';$body=[IO.File]::ReadAllText($control.entry)
            $body=Replace-O32Projection $body '#define PB_TEST_IP_VERSION 4' '#define PB_TEST_IP_VERSION 7'
            [IO.File]::WriteAllText($entry,$body,$encoding);'C1189.*TUN32 offload send requires independent IP version4 or6'
        }
    }
    $flags+=('/Fo'+(Join-Path $compileDirectory ($kind+'.obj')));$log=Join-Path $compileDirectory ($kind+'.log')
    & cl.exe @flags /c $entry *> $log;$code=$LASTEXITCODE;$text=[IO.File]::ReadAllText($log)
    if(!$code -or $text -notmatch $marker){throw "Checksum intended compile guard did not reject:$kind"}
    $compileChecks+=@{name=$kind;expected_rejection=$true;exit_code=$code;required_error=$marker;source=$entry;source_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();log=$log;log_sha256=(Get-FileHash $log).Hash.ToLowerInvariant()}
}
$global:LASTEXITCODE=0
Assert-C32Dependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during checksum metadata audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;baseline_source_tree_sha256=$accepted;production_changed=$true;
    reviewed_production_repairs=@('driver/v2/session_describe.h');kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;ledger=$contracts;parser_checks=$parserChecks;compile_contracts=$compileChecks;base_verifiers=$loaded;
    audit_sources=@('tests\model\tun32_checksum_metadata_tests.c','tools\build\tun32-checksum-metadata-tests.ps1','tools\audit\verify-tun32-checksum-metadata.ps1','tools\audit\network-dependency-ledger.ps1','tools\audit\checksum-family-ledger.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Independent IPv4/IPv6 actual Read/Describe/Send32 requests, real WDK checksum union, native old-field guards and normal6.20/current6.89 runtime gates. Every process executes80 cases: TCP/UDP, disabled/enabled policy, five defined OOB controls, two versions and checked warm/measure phases. Both family flags clear select no checksum operation before task bits; selector single/combined fields and a maximum ignored offset must preserve complete plain input bytes even with policy0. No reserved bits are fabricated; this is a documented control contract, not a claim that Windows naturally sends stale or receive-union metadata. Active correct checksum requests still require policy, both-family active mismatch still rejects. Source headers/options/TCP Timestamp/payload stay unchanged; caller NBL/NB storage is guard-freed in actual completion callback, source MDLs stay read-only until verification/recycle and are later freed. OnePush32 in successful cases, explicit classification1888/full-L3copy48000 per case; metadata-present diagnostic count is preserved and is not checksum work. Current8processes have0 violations; raw frozen whole Describe before8processes reproduce32 case violations each and exit141 only after mappings7680/7680, strict handles, slots/session references0, no repair or test threads. Whole base/WDK spans and all finite host/entry/flag transforms are independently reconstructed, current base EXEs are compile controls only. Exact16-contract raw/text ledger permits this one production file; other dependencies remain frozen. No native kernel/NDIS/IRQL/oldOS/CPU/throughput/latency assertion.'}|
    ConvertTo-Json -Depth 24|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
