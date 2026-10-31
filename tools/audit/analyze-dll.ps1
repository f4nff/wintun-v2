# Retain diagnostics for the developer DLL and the embedded-package compilation branch.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\build.ps1') -Stage Api
$analysisDir=Join-Path $outDir 'analysis-dll'; New-Item -ItemType Directory -Path $analysisDir -Force|Out-Null
$units=@(
    @{name='session';source='api\v2\session.c';defines=@()},@{name='adapter';source='api\v2\adapter.c';defines=@()},
    @{name='driver-package';source='api\v2\driver_package.c';defines=@()},@{name='driver-package-embedded';source='api\v2\driver_package.c';defines=@('/DPB_EMBEDDED_DRIVER=1')},
    @{name='core';source='common\v2_core.c';defines=@()},@{name='packet';source='common\packet.c';defines=@()},@{name='flow';source='common\flow.c';defines=@()},
    @{name='legacy-main';source='api\main.c';legacy=$true},@{name='legacy-adapter';source='api\adapter.c';legacy=$true},
    @{name='legacy-driver';source='api\driver.c';legacy=$true},@{name='legacy-rundll32';source='api\rundll32.c';legacy=$true}
)
$diagnostics=@();$prefix=([uri]($script:RepoRoot.TrimEnd('\')+'\')).AbsoluteUri
foreach($unit in $units) {
    $sarif=Join-Path $analysisDir "$($unit.name).sarif"; $log=Join-Path $analysisDir "$($unit.name).log"
    # A single emitted flag otherwise becomes a scalar string; native splatting
    # expands it into characters instead of passing /std:c17 as one argument.
    [string[]]$languageFlags=if($unit.legacy) {
        @('/DMAYBE_WOW64','/DWIN32_LEAN_AND_MEAN','/DUNICODE','/D_UNICODE','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/wd4100','/wd4201',"/I$script:RepoRoot\Release\amd64\api-intermediate")
    } else { @('/std:c17') }
    & cl.exe '/nologo' '/c' '/MT' '/O2' '/W4' '/analyze:only' "/analyze:log$sarif" "/Fo$analysisDir\$($unit.name).obj" @languageFlags @script:VersionArguments @($unit.defines) $unit.source *> $log
    if($LASTEXITCODE -ne 0) {Get-Content $log -Tail 30;throw "DLL analysis compilation failed: $($unit.name)"}
    if(Select-String -LiteralPath $log -Pattern '\bD(9002|9024|9027)\b' -Quiet) {
        Get-Content $log -Tail 30;throw "DLL analysis ignored compiler arguments: $($unit.name)"
    }
    foreach($run in (Get-Content $sarif -Raw|ConvertFrom-Json).runs) {foreach($result in $run.results) {
        $loc=$result.locations[0].physicalLocation;$item=$loc.artifactLocation
        $uri=if($item.uri){$item.uri}else{$run.artifacts[$item.index].location.uri}
        $diagnostics+=[pscustomobject][ordered]@{unit=$unit.name;rule=$result.ruleId;source=$uri;line=$loc.region.startLine;message=$result.message.text;project_source=$uri.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)}
    }}
}
$project=@($diagnostics|Where-Object project_source)
[ordered]@{source_tree_sha256=$script:SourceFingerprint;translation_units=$units.Count;diagnostics=$diagnostics.Count;project_diagnostics=$project.Count;manual_review_required=$true;results=$diagnostics}|ConvertTo-Json -Depth 7|Set-Content (Join-Path $analysisDir 'analysis-summary.json') -Encoding utf8
Get-Item (Join-Path $analysisDir 'analysis-summary.json')|Select-Object FullName,Length
