param([string]$BaselineDirectory='out\return-stop-baseline\e6c767571bed',[string]$OutputDirectory='')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {$failure=$_;if($script:TestResultFile){Write-TestRecording 'failed'};throw $failure}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Logger ownership requires version 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$logger=Join-Path $script:RepoRoot 'api\logger.c'
$accepted='e4e355f19691248274f36d831142a77266ff1ff876a039243ad33872e32ca525'
if((Get-FileHash (Join-Path $baseline 'api\logger.c')).Hash.ToLowerInvariant() -ne $accepted){throw 'Unexpected accepted logger source'}
$dependencies=@('api\logger.c','api\logger.h','api\adapter.h','api\wintun.h','api\main.h','api\operation.h','api\registry.h','api\ntdll.h',
    'include\packetbridge_identity.h','include\packetbridge_version.h','tests\safety\guard_memory.h')
foreach($path in $dependencies){if((Get-FileHash (Join-Path $baseline $path)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $path)).Hash){throw "Unchanged logger dependency changed:$path"}}
$directory=if($OutputDirectory){[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $OutputDirectory))}else{Join-Path $script:RepoRoot "out\logger-owner\$($script:SourceFingerprint.Substring(0,12))"}
New-Item -ItemType Directory -Path $directory -Force|Out-Null
$encoding=[Text.UTF8Encoding]::new($false)
Start-TestRecording $directory 'operation-results.json'
function Project-Logger([string]$body){
    return [regex]::Replace($body,'(?m)^#include "([^"]+)"',{
        param($match);'#include "'+[IO.Path]::GetFullPath((Join-Path (Split-Path $logger) $match.Groups[1].Value)).Replace('\','/')+'"'
    })
}
function Invoke-LoggerOwnerProcess([string]$Program,[string[]]$Arguments,[int]$Expected=0){
    $info=[Diagnostics.ProcessStartInfo]::new($Program);$info.UseShellExecute=$false;$info.CreateNoWindow=$true;$info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    foreach($argument in $Arguments){$info.ArgumentList.Add($argument)};$process=[Diagnostics.Process]::new();$process.StartInfo=$info
    try{
        if(!$process.Start()){throw 'Cannot start logger test'};$stdout=$process.StandardOutput.ReadToEndAsync();$stderr=$process.StandardError.ReadToEndAsync()
        if(!$process.WaitForExit(30000)){$process.Kill($true);$process.WaitForExit();throw 'Logger ownership watchdog expired30s'}
        $output=$stdout.GetAwaiter().GetResult()+$stderr.GetAwaiter().GetResult();$code=$process.ExitCode
        $log=Join-Path $script:TestLogDirectory ('{0:d2}-logger-owner.log' -f $script:TestExecutions.Count);[IO.File]::WriteAllText($log,$output,$encoding);Write-Output $output.TrimEnd()
        $summary=[regex]::Match($output,'PASS: (\d+) logger ownership cases plus (\d+) checked warmup; native FormatMessage/LocalFree, active CHECK with NDEBUG, bounded callback reentry, return/GLE contracts, zero Local owners; handles (\d+)/(\d+); compensation(\d+)/(\d+)')
        $cases=if($Arguments.Count){1}else{1405};$warm=if($Arguments.Count){2}else{1405};$comp=if($Expected -eq 133){1}else{0};$warmComp=if($Expected -eq 133){2}else{0}
        $marker='LOGGER_SYSTEM_MESSAGE_NOT_FREED: complete actual LoggerError returned with one native SystemMessage allocation; one real LocalFree compensation per case, all callbacks returned and zero Local owners before133'
        $correct=$code -eq $Expected -and $summary.Success -and ($Expected -ne 133 -or $output.Contains($marker))
        if($summary.Success){$correct=$correct -and [int]$summary.Groups[1].Value -eq $cases -and [int]$summary.Groups[2].Value -eq $warm -and
            $summary.Groups[3].Value -eq $summary.Groups[4].Value -and [int]$summary.Groups[5].Value -eq $comp -and [int]$summary.Groups[6].Value -eq $warmComp}
        $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;expected_exit_code=$Expected;expected_observer_failure=($Expected -eq 133 -and $correct);
            log=$log;watchdog_ms=30000;measured_schedules=$cases;checked_warmup_schedules=$warm;schedules=$cases+$warm;negative_compensations=$comp+$warmComp;
            reported_pass_messages=@($output -split '\r?\n'|Where-Object {$_.StartsWith('PASS:')})})
        Write-TestRecording $(if($correct){'running'}else{'failed'});if(!$correct){throw "Unexpected logger ownership result:$code expected$Expected"}
    }finally{$process.Dispose()}
}
$executables=@();$original=[IO.File]::ReadAllText((Join-Path $baseline 'api\logger.c'))
$before='    LocalFree(SystemMessage);';$after='    /* Test mutation: SystemMessage LocalFree omitted. */'
if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'System-message cleanup is not unique'}
$offset=$original.IndexOf($before)
foreach($mode in @('iso','ms')){foreach($guarded in @($false,$true)){foreach($variant in @('current','without-system-message-free')){
    $memory=if($guarded){'guarded'}else{'ordinary'};$objects=Join-Path $directory "$mode-$memory\$variant";New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $raw=Join-Path $objects 'logger-original.c';[IO.File]::WriteAllBytes($raw,[IO.File]::ReadAllBytes($logger))
    if((Get-FileHash $raw).Hash.ToLowerInvariant() -ne $accepted){throw 'Whole logger copy differs from accepted baseline'}
    $selected=$original;$mutation=$null
    if($variant -ne 'current'){$selected=$selected.Replace($before,$after);$mutation=@{original=$before;changed=$after;offset=$offset;length=$before.Length}}
    $selectedPath=Join-Path $objects 'logger-selected.c';[IO.File]::WriteAllText($selectedPath,(Project-Logger $selected),$encoding)
    # Independent whole-body projection check; one uniquely anchored omission
    # is allowed only inside LoggerError before the next function declaration.
    $expected=$original
    if($variant -ne 'current'){
        $start=$original.IndexOf('LoggerError(DWORD Error, LPCWSTR Prefix)');$end=$original.IndexOf('LoggerErrorV(DWORD Error, LPCWSTR Format, va_list Args)')
        if($start -lt 0 -or $offset -le $start -or $offset -ge $end){throw 'Negative cleanup is outside LoggerError'}
        $expected=$original.Substring(0,$offset)+$after+$original.Substring($offset+$before.Length)
    }
    if([IO.File]::ReadAllText($selectedPath) -cne (Project-Logger $expected)){throw 'Logger projection changed beyond selected single omission'}
    $lines=@();if($guarded){$lines+='#define PB_GUARD_TESTS 1'};if($variant -ne 'current'){$lines+='#define PB_LOGGER_MISSING_SYSTEM_FREE 1'}
    $lines+='#define PB_LOGGER_SOURCE "'+$selectedPath.Replace('\','/')+'"'
    $test=Join-Path $script:RepoRoot 'tests\safety\logger_owner_tests.c';$lines+='#include "'+$test.Replace('\','/')+'"'
    $entry=Join-Path $objects 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    $exe=Join-Path $objects 'logger-owner.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'/link',"/out:$exe",'ntdll.lib'))|Out-Host
    if($variant -eq 'current'){Invoke-LoggerOwnerProcess $exe @();Invoke-LoggerOwnerProcess $exe @('133')}else{Invoke-LoggerOwnerProcess $exe @('133') 133}
    foreach($dependency in $dependencies){if((Get-FileHash (Join-Path $baseline $dependency)).Hash -ne (Get-FileHash (Join-Path $script:RepoRoot $dependency)).Hash){throw 'Logger dependency changed during build'}}
    $executables+=,[ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();mode=$mode;guarded=$guarded;variant=$variant;ndebug=$true;pb_api_tests=$false;
        original=$raw;original_sha256=(Get-FileHash $raw).Hash.ToLowerInvariant();selected=$selectedPath;selected_sha256=(Get-FileHash $selectedPath).Hash.ToLowerInvariant();
        entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();mutation=$mutation;arguments=$flags}
}}}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during logger ownership audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;logger_accepted_sha256=$accepted;unchanged_dependencies=$dependencies.Count;
    operations=$script:TestResultFile;executables=$executables;production_changed=$false;kernel_runtime_tested=$false;production_ready=$false;
    audit_sources=@('tests\safety\logger_owner_tests.c','tools\audit\verify-logger-owner.ps1'|ForEach-Object {@{path=$_;sha256=(Get-FileHash (Join-Path $script:RepoRoot $_)).Hash.ToLowerInvariant()}});
    method='Coverage-only small host includes the entire accepted production logger.c. Audit snapshots its exact whole-source hash, checks11 dependencies byte-equal accepted baseline, and independently checks only absolute quoted includes plus the unique negative SystemMessage LocalFree omission. Actual native FormatMessageW allocates every successful message and actual LocalFree retires every owned block; wrappers observe these pointers and inject failures at only the first/system and second/render FormatMessage boundaries, separately and together. No fake successful output or malloc replacement is supplied. The one-layer callback reentry calls actual LoggerError while outer allocations remain owned, and each case checks its own Local ledger zero, created/freed equality and peak at most4. Seven input lengths0/1/2/1022/1023/1024/4096, four error encodings, three ambient errors, four failure masks and direct/log/formatted/LastError wrapper methods form1405 cases per full matrix; direct LoggerError uses only short prefix cases while real CRT formatting tests long/truncated Unicode prefixes. The native system-message success is observed rather than assumed for unknown error values. Direct LoggerError/ErrorV are tested for returned error only; LoggerLog/LogV and the actual logger.h LAST_ERROR wrapper additionally preserve ambient GLE despite callbacks and the explicit LocalFree clobber fault model. No claim is made that native LocalFree changes GLE, and the private SAL annotation is not used to invent a production bug. Setting loggerNULL exercises the actual no-op logger. Current full processes warm1405 and measure1405; selected133 warms2 and measures1. Negative133 omits only the system-message release after the render release remains intact. After the complete actual LoggerError returns, both native message allocations are proven to have succeeded and exactly one Local block remains, with no callback active. The host then calls real LocalFree exactly once through the same ledger per case; all3 cases reach zero Local owners before strict process-handle equality and133. Normal never compensates. Guarded mode guards only caller-owned Unicode prefix inputs at the NOACCESS boundary; SDK-owned Local blocks remain native and unguarded. No DLL, kernel, thread, registry, device or driver operation is exercised; OS FormatMessage and time functions remain native. NDEBUG is mandatory with always-active SAFETY_CHECK, PB_API_TESTS is rejected, and shipping65-byte fingerprint is statically required. Version remains2.0.0.'}|
    ConvertTo-Json -Depth 14|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
