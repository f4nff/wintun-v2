# Reproducible offline acceptance. Does not install drivers or change boot policy.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Set-Location -LiteralPath $script:RepoRoot
$fingerprint = Get-SourceFingerprint
$build = Join-Path $script:RepoRoot "out\packet-path-v2\$script:Version\$($fingerprint.Substring(0,12))"
$validation = Join-Path $script:RepoRoot 'out\unified-validation'
New-Item -ItemType Directory -Path $validation -Force | Out-Null
[ordered]@{source_tree_sha256=$fingerprint;offline_result='running';kernel_runtime_tested=$false;production_ready=$false} |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $validation 'summary.json') -Encoding utf8
$runs = [Collections.Generic.List[object]]::new()
function Run-Validation {
    param([string]$Name, [string]$Program, [string[]]$Arguments)
    $log = Join-Path $validation "$Name.log"
    & $Program @Arguments *> $log
    $code = $LASTEXITCODE
    $runs.Add([ordered]@{name=$Name;exit_code=$code;log=$log})
    if ($code -ne 0) { Get-Content -LiteralPath $log -Tail 25; throw "Validation failed: $Name" }
    Write-Output "PASS: $Name ($log)"
}
Run-Validation 'tests' 'pwsh.exe' @('-NoProfile','-File','tools\build\build.ps1','-Stage','Tests')
Run-Validation 'safety' 'pwsh.exe' @('-NoProfile','-File','tools\build\build.ps1','-Stage','Safety')
Run-Validation 'driver-analysis' 'pwsh.exe' @('-NoProfile','-File','tools\audit\analyze-unified.ps1','-Stage','Driver')
Run-Validation 'api-analysis' 'pwsh.exe' @('-NoProfile','-File','tools\audit\analyze-unified.ps1','-Stage','Api')
Run-Validation 'probe' 'pwsh.exe' @('-NoProfile','-File','tools\build\build.ps1','-Stage','Probe')
Run-Validation 'embedded-api' 'pwsh.exe' @('-NoProfile','-File','tools\build\build.ps1','-Stage','UnsignedTestApi',
    '-UnsignedDriverDirectory',"$build\driver",'-ExpectedManifest',"$build\driver\manifest.json")
Run-Validation 'pe-imports' 'python.exe' @('tools\package\verify-unified-pe.py',
    '--dll',"$build\unsigned-test-api\wintun.dll",'--sys',"$build\driver\packetbridge.sys")
if ((Get-SourceFingerprint) -ne $fingerprint) { throw 'Source changed during validation; rerun the frozen build' }
$tests = Get-Content -LiteralPath "$build\test-results.json" -Raw | ConvertFrom-Json
$safety = Get-Content -LiteralPath "$build\safety\safety-results.json" -Raw | ConvertFrom-Json
$compat = Get-Content -LiteralPath "$script:RepoRoot\out\unified-tests\test-results.json" -Raw | ConvertFrom-Json
if ($tests.result -ne 'passed' -or $safety.result -ne 'passed' -or $compat.result -ne 'passed') { throw 'Test record is not passed' }
[ordered]@{source_tree_sha256=$fingerprint;offline_result='passed';kernel_runtime_tested=$false;production_ready=$false;
    architecture='x64';minimum_os_target='Windows 7 SP1';dll="$build\unsigned-test-api\wintun.dll";
    driver="$build\driver\packetbridge.sys";test_processes=$tests.executed_processes;safety_processes=$safety.executed_processes;
    unified_test_processes=$compat.executed_processes;static_analysis_requires_review=$true;runs=$runs.ToArray();
    pending_runtime_gates=@('signed installation on each target OS','Driver Verifier','connectivity and offload negotiation',
        'suspend/resume and device removal','sustained kernel pool/handle/thread accounting')} |
    ConvertTo-Json -Depth 9 | Set-Content -LiteralPath (Join-Path $validation 'summary.json') -Encoding utf8
Write-Output "Validation record: $validation\summary.json"
