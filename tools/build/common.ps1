$ErrorActionPreference = 'Stop'
$script:RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
[xml]$script:VersionProps = Get-Content -LiteralPath (Join-Path $script:RepoRoot 'wintun.props') -Raw
$script:VersionGroup = $script:VersionProps.Project.PropertyGroup | Where-Object { $_.WintunVersionMaj -ne $null } | Select-Object -First 1
$script:VersionMajor = [int]$script:VersionGroup.WintunVersionMaj
$script:VersionMinor = [int]$script:VersionGroup.WintunVersionMin
$script:VersionPatch = [int]$script:VersionGroup.WintunVersionRel
$script:Version = "$script:VersionMajor.$script:VersionMinor.$script:VersionPatch"
$script:FileVersion = "$script:Version.0"
$script:VersionArguments = @("/DWINTUN_VERSION_MAJ=$script:VersionMajor", "/DWINTUN_VERSION_MIN=$script:VersionMinor", "/DWINTUN_VERSION_REL=$script:VersionPatch", "/DWINTUN_VERSION=$script:Version")

function Invoke-BuildTool {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}

# Test summaries are derived from executed processes and their output. Never
# publish hard-coded counts or keep a previous "passed" summary after a failure.
function Start-TestRecording {
    param([string]$Directory,[string]$Name='test-results.json')
    $script:TestExecutions=[Collections.Generic.List[object]]::new()
    $script:TestResultFile=Join-Path $Directory $Name
    $script:TestLogDirectory=Join-Path $Directory 'test-logs'
    New-Item -ItemType Directory -Path $script:TestLogDirectory -Force | Out-Null
    Write-TestRecording 'running'
}
function Write-TestRecording {
    param([string]$Status)
    [ordered]@{version=$script:Version;source_tree_sha256=$script:SourceFingerprint;result=$Status;
        kernel_runtime_tested=$false;executed_processes=$script:TestExecutions.Count;executions=@($script:TestExecutions.ToArray())} |
        ConvertTo-Json -Depth 9 | Set-Content -LiteralPath $script:TestResultFile -Encoding utf8
}
function Invoke-TestTool {
    param([string]$Program,[string[]]$Arguments)
    $lines=[Collections.Generic.List[string]]::new()
    & $Program @Arguments 2>&1 | ForEach-Object { $line=$_.ToString();$lines.Add($line);Write-Output $line }
    $code=$LASTEXITCODE
    $name='{0:d2}-{1}.log' -f $script:TestExecutions.Count,[IO.Path]::GetFileNameWithoutExtension($Program)
    $log=Join-Path $script:TestLogDirectory $name
    $lines | Set-Content -LiteralPath $log -Encoding utf8
    $output=$lines -join "`n"
    $python=@([regex]::Matches($output,'(?m)^Ran (\d+) tests? ') | ForEach-Object { [int]$_.Groups[1].Value })
    $rust=@([regex]::Matches($output,'test result: \w+\. (\d+) passed; (\d+) failed;') | ForEach-Object {
        [ordered]@{passed=[int]$_.Groups[1].Value;failed=[int]$_.Groups[2].Value}
    })
    $script:TestExecutions.Add([ordered]@{program=$Program;arguments=$Arguments;exit_code=$code;log=$log;
        python_reported_tests=$python;rust_reported_results=$rust;reported_pass_messages=@($lines | Where-Object { $_.StartsWith('PASS:') })})
    Write-TestRecording $(if($code -eq 0){'running'}else{'failed'})
    if($code -ne 0){throw "$Program failed with exit code $code"}
}

function Get-SourceFingerprint {
    # Use a concrete string array: PowerShell's object[] overload binding does
    # not reliably sort native-command output with the requested comparer.
    # Git quoting must not hide a Unicode path from the subsequent file read.
    [string[]]$paths = @(git -c core.quotepath=false -C $script:RepoRoot ls-files --full-name --cached --others --exclude-standard)
    if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate build sources' }
    [Array]::Sort($paths, [StringComparer]::Ordinal)
    $entries = foreach ($path in $paths) {
        $fullPath = Join-Path $script:RepoRoot $path
        if (Test-Path -LiteralPath $fullPath -PathType Leaf) {
            $path + ':' + (Get-FileHash -LiteralPath $fullPath -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $bytes = [Text.Encoding]::UTF8.GetBytes(($entries -join "`n"))
    $hash = [Security.Cryptography.SHA256]::Create()
    try { return [Convert]::ToHexString($hash.ComputeHash($bytes)).ToLowerInvariant() }
    finally { $hash.Dispose() }
}

function Initialize-BuildEnvironment {
    foreach($entry in @(
        @('DataAbiMajor','include\packetbridge_v2_abi.h','PB_ABI_MAJOR'),
        @('DataAbiMinor','include\packetbridge_v2_abi.h','PB_ABI_MINOR'),
        @('ApiAbiMajor','api\v2\packetbridge.h','PB_INTERFACE_V2_MAJOR'),
        @('ApiAbiMinor','api\v2\packetbridge.h','PB_INTERFACE_V2_MINOR'),
        @('PacketAbiMajor','api\v2\packetbridge.h','PB_PACKET_ABI_MAJOR'),
        @('PacketAbiMinor','api\v2\packetbridge.h','PB_PACKET_ABI_MINOR'),
        @('LifecycleAbiMajor','api\v2\lifecycle.h','PB_LIFECYCLE_ABI'))) {
        $contents=Get-Content -LiteralPath (Join-Path $script:RepoRoot $entry[1]) -Raw
        $match=[regex]::Match($contents,'(?m)^#define\s+'+[regex]::Escape($entry[2])+'\s+(\d+)u\s*$')
        if(-not $match.Success){throw "Cannot read ABI definition: $($entry[2])"}
        Set-Variable -Name $entry[0] -Scope Script -Value ([uint32]$match.Groups[1].Value)
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $script:VsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $script:VsRoot) { throw 'Visual Studio C++ tools were not found.' }
    & (Join-Path $script:VsRoot 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    $script:KitVersion = '10.0.26100.0'
    $script:SdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
    $script:WdkRoot = Join-Path $script:RepoRoot 'Release\build-tools\wdk\c'
    if (-not (Test-Path -LiteralPath "$script:WdkRoot\Include\$script:KitVersion\km\ntddk.h")) { throw 'WDK 10.0.26100 headers are required.' }
    $script:SdkBin = Join-Path $script:SdkRoot "bin\$script:KitVersion\x64"
    Set-Location -LiteralPath $script:RepoRoot
}

function Write-ArtifactManifest {
    param([string]$Directory, [string]$Kind, [hashtable]$Details = @{})
    $directoryPath = [IO.Path]::GetFullPath($Directory)
    $files = @(Get-ChildItem -LiteralPath $directoryPath -File | Where-Object { $_.Name -ne 'manifest.json' } | Sort-Object Name | ForEach-Object {
        [ordered]@{ name = $_.Name; size = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
    })
    [ordered]@{ product = 'PacketBridgeV2'; version = $script:Version; file_version = $script:FileVersion; kind = $Kind; source_commit = (git -C $script:RepoRoot rev-parse HEAD); source_tree_sha256 = $script:SourceFingerprint; working_tree = (git -C $script:RepoRoot status --porcelain | Out-String).Trim(); details = $Details; files = $files } |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directoryPath 'manifest.json') -Encoding utf8
}
