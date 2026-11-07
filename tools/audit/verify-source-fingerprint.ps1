param([string]$OutputDirectory='out\source-fingerprint')
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
$originalRepo=$script:RepoRoot;$start=Get-SourceFingerprint
$directory=if([IO.Path]::IsPathFullyQualified($OutputDirectory)){[IO.Path]::GetFullPath($OutputDirectory)}else{[IO.Path]::GetFullPath((Join-Path $originalRepo $OutputDirectory))}
$null=New-Item -ItemType Directory -Path $directory -Force
$repository=Join-Path $directory ('repository-'+[Guid]::NewGuid().ToString('N'))
$null=New-Item -ItemType Directory -Path (Join-Path $repository 'api') -Force
$encoding=[Text.UTF8Encoding]::new($false)
$names=@('.gitignore','COPYING','README.md','api/a.c','api/a_data.h','api/b.c','测试.dat')
foreach($name in $names){[IO.File]::WriteAllText((Join-Path $repository $name),$(if($name -ceq '.gitignore'){"/out/`n"}else{"content-$name`n"}),$encoding)}
& git -C $repository init -q
if($LASTEXITCODE){throw 'Cannot initialize isolated fingerprint test repository'}
& git -C $repository config core.autocrlf false
if($LASTEXITCODE){throw 'Cannot set isolated fingerprint test options'}
$checks=@()
function Assert-ExpectedFingerprint([string]$name,[string[]]$orderedNames){
    # Deliberately declared order: no native command, sorting overload or
    # production helper is used to create the independent expected sequence.
    $entries=@(foreach($item in $orderedNames){$item+':'+(Get-FileHash -LiteralPath (Join-Path $repository $item)).Hash.ToLowerInvariant()})
    $expected=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($encoding.GetBytes(($entries -join "`n")))).ToLowerInvariant()
    $actual=Get-SourceFingerprint
    if($actual -cne $expected){throw "Fingerprint is not the independently ordered content hash:$name"}
    $script:checks+=@{name=$name;sha256=$actual;expected_sha256=$expected;passed=$true};return $actual
}
try{
    $script:RepoRoot=$repository
    $first=Assert-ExpectedFingerprint 'all-untracked-and-Unicode' $names
    & git -C $repository add -- COPYING README.md
    if($LASTEXITCODE){throw 'Isolated git add failed'}
    if((Assert-ExpectedFingerprint 'mixed-tracked-untracked' $names) -cne $first){throw 'Index membership changed identical source identity'}
    & git -C $repository add -- api/a_data.h api/b.c
    if($LASTEXITCODE){throw 'Isolated git add failed'}
    if((Assert-ExpectedFingerprint 'different-index-partition' $names) -cne $first){throw 'Index partition changed identical source identity'}
    $null=New-Item -ItemType Directory -Path (Join-Path $repository 'out')
    [IO.File]::WriteAllText((Join-Path $repository 'out/artifact.bin'),'ignored artifact',$encoding)
    if((Assert-ExpectedFingerprint 'ignored-artifact' $names) -cne $first){throw 'Ignored artifact changed source identity'}
    [IO.File]::WriteAllText((Join-Path $repository 'api/a.c'),'changed bytes',$encoding)
    if((Assert-ExpectedFingerprint 'source-byte-change' $names) -ceq $first){throw 'Source byte change was not detected'}
    [IO.File]::WriteAllText((Join-Path $repository 'api/a.c'),"content-api/a.c`n",$encoding)
    if((Assert-ExpectedFingerprint 'restore-exact-bytes' $names) -cne $first){throw 'Restored content did not restore identity'}
    [IO.File]::WriteAllText((Join-Path $repository '空 白.dat'),'space and Unicode',$encoding)
    $withSpace=@('.gitignore','COPYING','README.md','api/a.c','api/a_data.h','api/b.c','测试.dat','空 白.dat')
    $null=Assert-ExpectedFingerprint 'Unicode-and-space-path' $withSpace
    # Delete only this known, exclusively created test file. A missing tracked
    # path must not be hashed as though its stale index contents still exist.
    Remove-Item -LiteralPath (Join-Path $repository 'api/b.c') -Force
    $withoutMissing=@('.gitignore','COPYING','README.md','api/a.c','api/a_data.h','测试.dat','空 白.dat')
    $null=Assert-ExpectedFingerprint 'missing-tracked-path' $withoutMissing
}finally{$script:RepoRoot=$originalRepo}
if($checks.Count -ne 8 -or (Get-SourceFingerprint) -cne $start){throw 'Fingerprint contracts or project source seal changed'}
@{version=$script:Version;source_tree_sha256=$start;result='passed';checks=$checks;repository=$repository;kernel_runtime_tested=$false;production_ready=$false;
    method='Eight checks use an isolated fresh Git repository, fixed independent ordinal sequences, real tracked/untracked transitions, Unicode/space paths, ignored output, actual byte edits/restoration and a missing tracked file. Main project index/config/source are never edited by this test. Legacy baseline identities remain attached to their original recorded path order; no old fingerprint is relabeled as ordinal.'}|
    ConvertTo-Json -Depth 5|Set-Content (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
