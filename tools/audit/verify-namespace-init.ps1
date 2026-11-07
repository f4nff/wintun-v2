param([string]$BaselineDirectory='out\namespace-init-baseline\df178ffa4fff')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile){Write-TestRecording 'failed'};throw $_}
Initialize-BuildEnvironment
if($script:Version -ne '2.0.0'){throw 'Namespace audit requires 2.0.0'}
$script:SourceFingerprint=Get-SourceFingerprint;$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$old=Join-Path $baseline 'api\namespace.c'
if((Get-FileHash -LiteralPath $old).Hash.ToLowerInvariant() -ne 'f68474d4dafbb6e8ada0f41bf88f0374e647a375ca2cdc4304592e0cfead473c'){throw 'Unexpected frozen namespace source'}
foreach($name in @('logger.h','main.h','namespace.h','operation.h')) {
    if((Get-FileHash -LiteralPath (Join-Path $baseline "api\$name")).Hash -ne (Get-FileHash -LiteralPath (Join-Path $script:RepoRoot "api\$name")).Hash){throw "Namespace dependency changed: $name"}
}
$directory=Join-Path $script:RepoRoot "out\namespace-init-audit\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force|Out-Null
Start-TestRecording $directory 'operation-results.json'
$oldSource=Join-Path $directory 'namespace-before.c'
$text=[IO.File]::ReadAllText($old)
# Only include locations change; dependency bytes are checked above. Reuse the
# current identical headers so pragma-once does not create duplicate definitions.
$text=[regex]::Replace($text,'#include "([^"]+)"',{
    param($m)
    $path=Join-Path $script:RepoRoot ('api\'+$m.Groups[1].Value)
    '#include "'+$path.Replace('\','/')+'"'
})
[IO.File]::WriteAllText($oldSource,$text)
$before=Join-Path $directory 'before.c'
@(('#define PB_NAMESPACE_SOURCE "'+$oldSource.Replace('\','/')+'"'),
  ('#include "'+(Join-Path $script:RepoRoot 'tests\safety\namespace_init_tests.c').Replace('\','/')+'"'))|
  Set-Content -LiteralPath $before -Encoding utf8
$controls=@('NAMESPACE_RETRY_WITHOUT_BACKOFF','EXPLICIT_STOP_IGNORED_DURING_NAMESPACE_RETRY','LOGGER_CALLED_UNDER_NAMESPACE_INIT_LOCK','MUTEX_CREATE_ERROR_PRESERVED','OPEN_ENTERED_AFTER_STOP','OPEN_ENTERED_AFTER_NAMESPACE_DEADLINE')
$executables=@()
foreach($mode in @('iso','ms')) {
    foreach($variant in @('before','after')) {
        $objects=Join-Path $directory "$variant-$mode";New-Item -ItemType Directory -Path $objects -Force|Out-Null
        $source=if($variant -eq 'before'){$before}else{'tests\safety\namespace_init_tests.c'}
        $exe=Join-Path $objects 'namespace.exe'
        Invoke-BuildTool cl.exe (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$mode","/Fo$objects\",$source)+$script:VersionArguments+@('/link',"/out:$exe",'advapi32.lib'))
        if($variant -eq 'after'){Invoke-TestTool $exe @()}
        else {for($kind=1;$kind -le $controls.Count;++$kind) {
            $output=@(& $exe $kind 2>&1|ForEach-Object {$_.ToString()});$code=$LASTEXITCODE
            $log=Join-Path $objects "control-$kind.log";$output|Set-Content -LiteralPath $log -Encoding utf8
            # Control 4 is a positive control: real logger.h already preserved
            # the original nonzero mutex error in the frozen implementation.
            $positive=$kind -eq 4
            $expectedCode=if($positive){0}else{70+$kind}
            $expected=$code -eq $expectedCode -and ($output -join "`n").Contains($controls[$kind-1])
            $script:TestExecutions.Add([ordered]@{program=$exe;arguments=@([string]$kind);exit_code=$code;expected_exit_code=$expectedCode;expected_baseline_failure=(!$positive -and $expected);expected_baseline_positive_control=($positive -and $expected);matched_expected_result=$expected;log=$log})
            Write-TestRecording $(if($expected){'running'}else{'failed'})
            if(!$expected){throw "Unexpected baseline result: $($controls[$kind-1]), exit=$code"}
        }}
        $executables+=@{variant=$variant;volatile=$mode;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Source changed during namespace audit'}
Write-TestRecording 'passed'
[ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;baseline=$baseline;
    kernel_runtime_tested=$false;production_ready=$false;operations=$script:TestResultFile;executables=$executables;
    baseline_negative_controls_per_mode=5;baseline_positive_controls_per_mode=1;
    method='Actual production namespace.c, frozen whole-file hash and identical dependency headers; ISO/MS volatile Win7 API target. Five exact old negative controls require exits 71/72/73/75/76; old control 4 requires exit 0 and MUTEX_CREATE_ERROR_PRESERVED. Real logger.h wrappers invoke the hosted LoggerErrorV callback, which overwrites the thread error; LoggerLastErrorV still restores the original nonzero mutex failure in both old and current code. The previous exit-74 claim came from replacing that wrapper and is withdrawn. Each current process executes 210 checks with real SID/boundary-descriptor APIs, anonymous mutexes/events, critical sections and joined threads. Private namespace create/open/close are hosted stand-ins backed by real event handles; no project namespace or device changed. Scripted operation waits and elapsed clock verify 50ms bounded retry, 15s initialization budget, shorter enclosing operation, 32/64-bit clock wrap, API latency crossing deadline, all acquisition failures including zero errors, first-error restoration, same-instance retry, pre/during/after-create cancellation, no subsequent open after stop/deadline, cached ownership, eight concurrent users, and a real stop interrupting a gated backoff. Logger lock freedom observed from a separate joined thread. All descriptor/namespace/mutex ownership balances and process handles checked after every complete matrix. No native SYS or old-OS runtime execution.'}|
    ConvertTo-Json -Depth 9|Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
