param([string]$BaselineDirectory='out\audit4-baseline\3824cedc372b',[ValidateRange(3,15)][int]$Runs=5)
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
Initialize-BuildEnvironment
$baseline=[IO.Path]::GetFullPath((Join-Path $script:RepoRoot $BaselineDirectory))
if(-not(Test-Path -LiteralPath "$baseline\common\flow.c")) {throw 'Missing preserved pre-audit flow source'}
$script:SourceFingerprint=Get-SourceFingerprint
$script:VersionArguments+="/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$benchDir=Join-Path $script:RepoRoot "out\audit4-bench\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $benchDir -Force|Out-Null
Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include","/fo$benchDir\version.res")+$script:VersionArguments+@('tools\build\artifact.rc'))
foreach($variant in @('before','after')) {
    $objects=Join-Path $benchDir $variant; New-Item -ItemType Directory -Path $objects -Force|Out-Null
    $source=if($variant -eq 'before') {"$baseline\common\flow.c"} else {'common\flow.c'}
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objects\",'tests\bench\flow_bench.c',$source)+$script:VersionArguments+@('/link',"/out:$benchDir\$variant.exe","$benchDir\version.res"))
}
$samples=@()
for($run=0;$run -lt $Runs;++$run) {
    $order=if($run%2) {@('after','before')} else {@('before','after')}
    foreach($variant in $order) {
        $lines=& "$benchDir\$variant.exe"; if($LASTEXITCODE -ne 0) {throw "Failed $variant flow benchmark"}
        foreach($line in $lines) {
            $s=$line|ConvertFrom-Json
            $samples+=[pscustomobject][ordered]@{run=$run;variant=$variant;ip=$s.ip;queues=$s.queues;iterations=$s.iterations;ns_per_call=$s.ns_per_call;cycles_per_call=$s.cycles_per_call;sum=$s.sum}
        }
    }
}
function Median([object[]]$Values) {
    $s=@($Values|Sort-Object); $middle=[int][Math]::Floor($s.Count/2)
    if($s.Count%2) {return $s[$middle]}; return ($s[$middle-1]+$s[$middle])/2
}
$medians=@()
foreach($ip in @(4,6)) {foreach($queues in @(1,4)) {
    $values=@{}
    foreach($variant in @('before','after')) {
        $s=@($samples|Where-Object {$_.variant -eq $variant -and $_.ip -eq $ip -and $_.queues -eq $queues})
        $values[$variant]=[ordered]@{ns=Median @($s.ns_per_call);cycles=Median @($s.cycles_per_call)}
    }
    $sums=@($samples|Where-Object {$_.ip -eq $ip -and $_.queues -eq $queues}|Select-Object -ExpandProperty sum -Unique)
    if($sums.Count -ne 1) {throw 'Before/after queue selection differs'}
    $medians+=[ordered]@{ip=$ip;queues=$queues;before=$values.before;after=$values.after;ns_reduction_percent=[Math]::Round(100*(1-$values.after.ns/$values.before.ns),2)}
}}
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;baseline_flow_sha256=(Get-FileHash "$baseline\common\flow.c").Hash.ToLowerInvariant();current_flow_sha256=(Get-FileHash 'common\flow.c').Hash.ToLowerInvariant();
    compiler='MSVC x64 /O2 /MT /W4 /WX; separate translation units, no LTO';runs=$Runs;scope='User-mode PbSelectQueue only, pinned to lowest eligible process CPU, alternating order. Not kernel CPU/NDIS/VPN throughput; system noise remains.';kernel_runtime_tested=$false;medians=$medians;samples=$samples}|ConvertTo-Json -Depth 8|Set-Content "$benchDir\benchmark-results.json" -Encoding utf8
$medians|ConvertTo-Json -Depth 5
Get-Item "$benchDir\benchmark-results.json"|Select-Object FullName,Length
