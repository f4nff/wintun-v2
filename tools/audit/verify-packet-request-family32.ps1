param([string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
. (Join-Path $PSScriptRoot '..\build\packet-request-family32-tests.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -cne '2.0.0'){throw 'Packet request family requires2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$accepted=[ordered]@{
    'common\packet.c'='bf3731c3cc5712d7b2cd409777302971118ca99e4fea00047d9583263a1e37c1'
    'common\packet.h'='f72971019b709f8d86b5289312595b345dbf928722e06aa8c4d9c3aa0dd1868e'
    'include\packetbridge_types.h'='406666ab99bba5b4168dae2ebed0bd6234fe90945ce4447e2ec895b039a67e2e'
    'include\packetbridge_version.h'='837c6a14288b113acf8d7eec1e0c855d70fda441e16fe7770718338a458d1a66'
    'tests\safety\guard_memory.h'='ca169cf1a4b11506dc45572b60d61d8b05cf8a73bf7ba424b04dca228637906b'
}
function Assert-RequestDependencies {
    foreach($path in $accepted.Keys){if((Get-FileHash -LiteralPath (Join-Path $script:RepoRoot $path)).Hash.ToLowerInvariant() -cne $accepted[$path]){throw "Accepted request dependency changed:$path"}}
}
Assert-RequestDependencies
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\packet-request-family32\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
function Assert-RequestBytes([string]$path,[string]$text) {
    $expectedHash=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($encoding.GetBytes($text))).ToLowerInvariant()
    if((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $expectedHash){throw "Generated request bytes differ:$path"}
}
function Assert-RequestBuild($build) {
    Assert-RequestDependencies
    $paths=@($accepted.Keys)+@('tests\safety\packet_request_family32_tests.c')
    if(@($build.sources).Count -ne $paths.Count -or @($build.sources.relative|Select-Object -Unique).Count -ne $paths.Count -or
       @(Compare-Object ($build.sources.relative|Sort-Object) ($paths|Sort-Object)).Count){throw 'Request source set changed'}
    foreach($item in $build.sources){if($item.path -cne (Join-Path $script:RepoRoot $item.relative) -or $item.sha256 -cne (Get-FileHash -LiteralPath $item.path).Hash.ToLowerInvariant()){throw 'Request source binding changed'}}
    $work=Split-Path $build.path;$raw=Join-Path $work 'packet-actual.c';$header=Join-Path $work 'packet.h';$entry=Join-Path $work 'entry.c'
    if($build.source -cne (Join-Path $script:RepoRoot 'common\packet.c') -or $build.accepted_sha256 -cne $accepted['common\packet.c'] -or
        $build.raw -cne $raw -or $build.raw_sha256 -cne $accepted['common\packet.c'] -or (Get-FileHash $raw).Hash.ToLowerInvariant() -cne $accepted['common\packet.c']){throw 'Whole packet source copy changed'}
    $productionHeader=Join-Path $script:RepoRoot 'common\packet.h'
    Assert-RequestBytes $header ('#include "'+$productionHeader.Replace('\','/')+'"'+"`n")
    $test=Join-Path $script:RepoRoot 'tests\safety\packet_request_family32_tests.c'
    $lines=@(('#define PB_REQUEST_PACKET_SOURCE "'+$raw.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    Assert-RequestBytes $entry (($lines -join "`n")+"`n")
    if($build.header -cne $header -or $build.entry -cne $entry -or $build.header_sha256 -cne (Get-FileHash $header).Hash.ToLowerInvariant() -or
        $build.entry_sha256 -cne (Get-FileHash $entry).Hash.ToLowerInvariant() -or $build.sha256 -cne (Get-FileHash $build.path).Hash.ToLowerInvariant()){throw 'Request output binding changed'}
    if($build.ip_version -notin @(4,6) -or $build.mode -notin @('iso','ms') -or !$build.ndebug -or $build.pb_api_tests){throw 'Request build branch changed'}
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',
        "/DPB_TEST_IP_VERSION=$($build.ip_version)","/volatile:$($build.mode)","/Fo$work\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($build.guarded){$flags+='/DPB_GUARD_TESTS=1'}
    if(($build.flags -join "`n") -cne ($flags -join "`n")){throw 'Request compiler flags changed'}
}
function Assert-RequestRows([string]$text,[int]$family) {
    $witness=@([regex]::Matches($text,'(?m)^FAMILY: IPv([46])\r?$'))
    if($witness.Count -ne 1 -or [int]$witness[0].Groups[1].Value -ne $family){throw 'Missing or mixed request family'}
    $rows=@($text -split '\r?\n'|Where-Object {$_.StartsWith('REQUEST32 ')})
    if($rows.Count -ne 24){throw 'Wrong request matrix length'}
    $rowPattern='^REQUEST32 phase=(?<phase>warm|measure) ip=(?<ip>[46]) protocol=(?<protocol>6|17) shape=(?<shape>ascending|descending|interleaved|overlap31|short31|short0) calls=(?<calls>\d+) first-status=(?<status>\d+) first-written=(?<written>\d+) metadata=(?<meta>\d+) metadata-bytes=(?<metabytes>\d+) headers=(?<headers>\d+) payloads=(?<payloads>\d+) payload-bytes=(?<bytes>\d+) byte-hits=(?<hits>\d+) source-plan-sentinels=preserved owners=0$'
    $shapes=@('ascending','descending','interleaved','overlap31','short31','short0');$index=0
    foreach($phase in @('warm','measure')){foreach($protocol in @(6,17)){foreach($shape in $shapes){
        $match=[regex]::Match($rows[$index++],$rowPattern);if(!$match.Success){throw 'Malformed request row'}
        $g=$match.Groups;$invalid=$shape -ceq 'overlap31';$short=$shape.StartsWith('short');$prefix=if($shape -ceq 'short31'){31}else{0}
        $calls=if($short){2}else{1};$written=if($invalid){[uint32]::MaxValue}elseif($short){$prefix}else{32}
        $status=if($invalid){1}elseif($short){3}else{0};$copied=if($invalid){0}else{32};$hits=if($invalid){0}else{1}
        $metabytes=(32+$(if($short){32-$prefix}else{0}))*16
        foreach($pair in @(@('ip',$family),@('protocol',$protocol),@('calls',$calls),@('status',$status),@('written',$written),@('meta',$calls),@('metabytes',$metabytes),@('headers',$copied),@('payloads',$copied),@('bytes',($copied*64)),@('hits',$hits))){if([uint64]$g[$pair[0]].Value -ne [uint64]$pair[1]){throw "Wrong request row field:$($pair[0])"}}
        if($g['phase'].Value -cne $phase -or $g['shape'].Value -cne $shape){throw 'Request row ordering changed'}
    }}}
    $summary=[regex]::Match($text,'(?m)^PASS: (\d+) packet request geometry cases \((\d+) warm/(\d+) measured\), IPv([46]), exact32 maximum disjoint/overlap/prefix calls, explicit production memcpy ownership, zero caller allocations; handles(\d+)/(\d+)\.\r?$')
    if(!$summary.Success -or [int]$summary.Groups[1].Value -ne 24 -or [int]$summary.Groups[2].Value -ne 12 -or [int]$summary.Groups[3].Value -ne 12 -or
        [int]$summary.Groups[4].Value -ne $family -or $summary.Groups[5].Value -cne $summary.Groups[6].Value){throw 'Request summary/equality changed'}
}
function Invoke-RequestProcess($build) {
    $info=[Diagnostics.ProcessStartInfo]::new($build.path);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try {
        if(!$process.Start()){throw 'Cannot start request test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Request watchdog expired30s'}
        $text=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-ipv{1}-{2}-{3}.log' -f $script:TestExecutions.Count,$build.ip_version,$build.mode,$build.guarded)
        [IO.File]::WriteAllText($log,$text,$encoding)
        $script:TestExecutions.Add([ordered]@{program=$build.path;arguments=@();exit_code=$code;log=$log;ip_version=$build.ip_version;mode=$build.mode;guarded=$build.guarded;
            watchdog_ms=30000;checked_warmup_schedules=12;measured_schedules=12;schedules=24;reported_pass_messages=@($text -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording 'running';if($code -ne 0){throw "Request test failed:$code"};Assert-RequestRows $text $build.ip_version
        Write-Output ($text -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})
        return $text
    }finally{$process.Dispose()}
}
Start-TestRecording $directory 'operation-results.json'
$executables=@();$logs=@()
foreach($family in @(4,6)){foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){
    $memory=if($guarded){'guarded'}else{'ordinary'};$work=Join-Path $directory "ipv$family-$mode-$memory"
    $build=Build-PacketRequestFamily32Tests -Directory $work -IpVersion $family -Mode $mode -Guarded:$guarded
    Assert-RequestBuild $build;$captured=@(Invoke-RequestProcess $build);$logs+=,$captured[-1];Assert-RequestBuild $build;$executables+=,$build
}}}
# Feed actual logs to the opposite family and one deliberately wrong copy row;
# each must be rejected, rather than merely changing the evidence label.
$parserChecks=0
foreach($case in @(@($logs[0],6),@($logs[4],4),@($logs[0].Replace('metadata=1 metadata-bytes=512','metadata=2 metadata-bytes=512'),4))){
    $rejected=$false;try{Assert-RequestRows $case[0] $case[1]}catch{$rejected=$true};if(!$rejected){throw 'Request row negative was accepted'};++$parserChecks
}
# Compile guard failures in separate out objects. Match the intended guard
# diagnostic so unrelated build errors cannot satisfy these controls.
$compileChecks=@();$base=$executables[0];$guardDir=Join-Path $directory 'compile-guards';New-Item -ItemType Directory -Path $guardDir -Force|Out-Null
foreach($kind in @('pb-api-tests','without-ndebug','wrong-fingerprint','wrong-family')){
    $flags=@($base.flags|ForEach-Object {if($_ -like '/Fo*'){"/Fo$guardDir\"}else{$_}})
    $marker=switch($kind){
        'pb-api-tests' {$flags+='/DPB_API_TESTS=1';'This test uses the actual shared packet engine'}
        'without-ndebug' {$flags=@($flags|Where-Object {$_ -cne '/DNDEBUG'});'NDEBUG is required'}
        'wrong-fingerprint' {$flags=@($flags|ForEach-Object {if($_ -like '/DPB_SOURCE_FINGERPRINT=*'){'/DPB_SOURCE_FINGERPRINT=unknown'}else{$_}});'Shipping fingerprint required'}
        'wrong-family' {$flags=@($flags|ForEach-Object {if($_ -like '/DPB_TEST_IP_VERSION=*'){'/DPB_TEST_IP_VERSION=0'}else{$_}});'Select one IP family'}
    }
    $output=(& cl.exe @flags /c $base.entry 2>&1|ForEach-Object {$_.ToString()}) -join "`n";$code=$LASTEXITCODE
    $log=Join-Path $guardDir "$kind.log";[IO.File]::WriteAllText($log,$output,$encoding)
    if($code -eq 0 -or !$output.Contains($marker)){throw "Intended request compile guard did not reject:$kind"}
    $compileChecks+=,@{kind=$kind;exit_code=$code;marker=$marker;log=$log}
}
Assert-RequestDependencies
if((Get-SourceFingerprint) -cne $script:SourceFingerprint){throw 'Sources changed during request audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;accepted_dependency_sources=@($accepted.Keys|ForEach-Object {@{path=$_;sha256=$accepted[$_]}});
    operations=$script:TestResultFile;executables=$executables;parser_negative_checks=$parserChecks;compile_guard_checks=$compileChecks;production_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests\safety\packet_request_family32_tests.c','tools\build\packet-request-family32-tests.ps1','tools\audit\verify-packet-request-family32.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only eight independent executables/processes selectIPv4 orIPv6, ISO/MS and ordinary/guarded. Audit locks five accepted dependency raw hashes; independently reconstructs the exact whole C copy, header forwarding include, entry and shipping compiler flags. Every actual output row is checked in warm/measure order for TCP/UDP and six fixed shapes: ascending, descending worst-case insertion, interleaved32 disjoint128-byte capacities, output31 overlapping output0, zero-capacity31 and zero-capacity0 followed by exactly one suffix retry. Invalid geometry preserves written UINT32_MAX, all output metadata, source, Plan and sentinels before any packet copy. Valid aggregate copies contain one header and one payload per segment; actual explicit packet.c memcpy calls classify metadata/header/payload only, with each source payload byte and each output header/payload hit exactly once across prefix+retry. Setup and independent wire/checksum oracle copies are outside the macro scope; nativeCPU/performance and compiler-generated struct moves are not inferred. Each case frees five actual caller allocations. Guarded source/output/Plan/metadata/written ends meet NOACCESS; private production stack and SDK/CRT internals remain unguarded. No thread/device/driver/registry/session/module/timer is created. Handle equality is the warmed process observation, not real kernel leak proof. Four compile guards and three deliberately wrong log controls are independent acceptance checks. No negative production mutation, production edit or compensation. Version2.0.0.'}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
