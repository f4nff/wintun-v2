# Test-only owned roots. Never enumerate or delete historical helper-PID roots.
function Get-HelperTestRootItem {
    param([Parameter(Mandatory)][string]$Path,[switch]$AllowMissing)
    try{return Get-Item -LiteralPath $Path -Force -ErrorAction Stop}
    catch{if($AllowMissing -and $_.CategoryInfo.Category -eq 'ObjectNotFound'){return $null};throw}
}
function Assert-HelperTestRootPlainPath {
    param([Parameter(Mandatory)][string]$Path,[ValidateSet('Directory','File')][string]$Kind='Directory')
    $cursor=$Path;$first=$true
    while($cursor){
        $item=Get-HelperTestRootItem -Path $cursor
        if($item.Attributes -band [IO.FileAttributes]::ReparsePoint){throw "Helper root path is a reparse point:$cursor"}
        if(($first -and $Kind -ceq 'File' -and $item.PSIsContainer) -or ((!$first -or $Kind -ceq 'Directory') -and !$item.PSIsContainer)){throw "Unexpected helper root path kind:$cursor"}
        $first=$false;$cursor=[IO.Path]::GetDirectoryName($cursor)
    }
}
function Get-HelperTestRootRecord {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines,[Parameter(Mandatory)][string]$RepoRoot)
    $markers=@($Lines|Where-Object {$_.StartsWith('OWNED_ROOT',[StringComparison]::Ordinal)})
    if($markers.Count -ne 1){throw 'Expected exactly one helper OWNED_ROOT marker'}
    $match=[regex]::Match($markers[0],'^OWNED_ROOT pid=(?<owner>[0-9]{1,10}) path=(?<path>.+)$')
    if(!$match.Success){throw 'Malformed helper OWNED_ROOT marker'}
    [uint32]$ownerProcessId=0
    if(![uint32]::TryParse($match.Groups['owner'].Value,[Globalization.NumberStyles]::None,[Globalization.CultureInfo]::InvariantCulture,[ref]$ownerProcessId) -or
        !$ownerProcessId -or $ownerProcessId.ToString([Globalization.CultureInfo]::InvariantCulture) -cne $match.Groups['owner'].Value){throw 'Helper owner PID is not a canonical uint32'}
    $path=$match.Groups['path'].Value
    if(![IO.Path]::IsPathFullyQualified($path)){throw 'Helper root is not absolute'}
    $canonical=[IO.Path]::GetFullPath($path)
    if($canonical -cne $path){throw 'Helper root path is not canonical'}
    $base=[IO.Path]::GetFullPath((Join-Path $RepoRoot 'out\terminal-helper-tests'))
    if(![StringComparer]::OrdinalIgnoreCase.Equals([IO.Path]::GetDirectoryName($canonical),$base)){throw 'Helper root is not a direct child of the fixed test base'}
    $leaf=[regex]::Match([IO.Path]::GetFileName($canonical),'^run-(?<owner>[0-9A-F]{8})-(?<created>[0-9A-F]{16})-(?<attempt>0[1-9]|[12][0-9]|3[0-2])$')
    if(!$leaf.Success -or [Convert]::ToUInt32($leaf.Groups['owner'].Value,16) -ne $ownerProcessId){throw 'Helper root leaf/PID/attempt does not match its marker'}
    Assert-HelperTestRootPlainPath -Path $base
    return [ordered]@{owner_pid=$ownerProcessId;path=$canonical;base=$base;creation_hex=$leaf.Groups['created'].Value;attempt=[int]$leaf.Groups['attempt'].Value;marker=$markers[0]}
}
function Assert-HelperTestRootCompletion {
    param([Parameter(Mandatory)][AllowEmptyCollection()][string[]]$Lines,[Parameter(Mandatory)][string]$RepoRoot,[switch]$BaselineCleanup)
    $record=Get-HelperTestRootRecord -Lines $Lines -RepoRoot $RepoRoot
    $rootItem=Get-HelperTestRootItem -Path $record.path -AllowMissing
    if(!$BaselineCleanup){
        if($null -ne $rootItem){Assert-HelperTestRootPlainPath -Path $record.path;throw 'Normal helper process left its owned root behind'}
        $record.mode='normal';$record.root_absent=$true;$record.removed_paths=@();return $record
    }
    if($null -eq $rootItem){throw 'Expected45 helper root is missing before finite cleanup'}
    Assert-HelperTestRootPlainPath -Path $record.path
    $child=Join-Path $record.path ('helper-{0}-1' -f $record.owner_pid)
    $file=Join-Path $child 'setupapihost.dll'
    $rootEntries=@(Get-ChildItem -LiteralPath $record.path -Force -ErrorAction Stop)
    if($rootEntries.Count -ne 1 -or $rootEntries[0].Name -cne ('helper-{0}-1' -f $record.owner_pid) -or
        ![StringComparer]::OrdinalIgnoreCase.Equals($rootEntries[0].FullName,$child)){throw 'Unexpected helper root contents; no paths removed'}
    Assert-HelperTestRootPlainPath -Path $child
    $childEntries=@(Get-ChildItem -LiteralPath $child -Force -ErrorAction Stop)
    if($childEntries.Count -ne 1 -or $childEntries[0].Name -cne 'setupapihost.dll' -or $childEntries[0].PSIsContainer -or $childEntries[0].Length -ne 0 -or
        ![StringComparer]::OrdinalIgnoreCase.Equals($childEntries[0].FullName,$file)){throw 'Unexpected helper package contents; no paths removed'}
    Assert-HelperTestRootPlainPath -Path $file -Kind File
    # All names, owners, lengths, canonical parents and reparse attributes have
    # been checked before the first deletion. Only these three literal paths
    # may be removed; no recursion, wildcard or historical44 directory sweep.
    Remove-Item -LiteralPath $file -Force -Confirm:$false -ErrorAction Stop
    if(@(Get-ChildItem -LiteralPath $child -Force -ErrorAction Stop).Count){throw 'Helper child became nonempty during cleanup'}
    Remove-Item -LiteralPath $child -Force -Confirm:$false -ErrorAction Stop
    if(@(Get-ChildItem -LiteralPath $record.path -Force -ErrorAction Stop).Count){throw 'Helper root became nonempty during cleanup'}
    Remove-Item -LiteralPath $record.path -Force -Confirm:$false -ErrorAction Stop
    if($null -ne (Get-HelperTestRootItem -Path $record.path -AllowMissing)){throw 'Finite helper root cleanup did not complete'}
    $record.mode='baseline45';$record.root_absent=$true;$record.removed_paths=@($file,$child,$record.path);return $record
}
function Test-HelperTestRootContracts {
    param([Parameter(Mandatory)][string]$RepoRoot)
    $base=[IO.Path]::GetFullPath((Join-Path $RepoRoot 'out\terminal-helper-tests'))
    if($null -eq (Get-HelperTestRootItem -Path $base -AllowMissing)){
        Assert-HelperTestRootPlainPath -Path ([IO.Path]::GetDirectoryName($base))
        $null=New-Item -ItemType Directory -Path $base -ErrorAction Stop
    }
    Assert-HelperTestRootPlainPath -Path $base
    function New-RootContractCase {
        $owner=[uint32]4027383809;$created=[Guid]::NewGuid().ToString('N').Substring(0,16).ToUpperInvariant()
        $root=Join-Path $base ('run-{0:X8}-{1}-01' -f $owner,$created)
        $null=New-Item -ItemType Directory -Path $root -ErrorAction Stop
        $child=Join-Path $root "helper-$owner-1";$null=New-Item -ItemType Directory -Path $child -ErrorAction Stop
        $file=Join-Path $child 'setupapihost.dll';[IO.File]::WriteAllBytes($file,[byte[]]@())
        return @{root=$root;child=$child;file=$file;owner=$owner;marker="OWNED_ROOT pid=$owner path=$root";extra=@()}
    }
    function Clear-RootContractCase($case){
        # Each path was just created by this unit. Never search the base.
        foreach($file in @($case.extra)+@($case.file)){
            if($null -ne (Get-HelperTestRootItem -Path $file -AllowMissing)){Assert-HelperTestRootPlainPath -Path $file -Kind File;Remove-Item -LiteralPath $file -Force -Confirm:$false -ErrorAction Stop}
        }
        foreach($directory in @($case.child,$case.root)){
            if($null -ne (Get-HelperTestRootItem -Path $directory -AllowMissing)){
                Assert-HelperTestRootPlainPath -Path $directory
                if(@(Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop).Count){throw 'Unit-owned helper directory unexpectedly nonempty'}
                Remove-Item -LiteralPath $directory -Force -Confirm:$false -ErrorAction Stop
            }
        }
    }
    function Root-ContractSnapshot($case){
        return (@($case.root,$case.child)+@($case.file)+@($case.extra)|ForEach-Object {
            $item=Get-HelperTestRootItem -Path $_
            '{0}|{1}|{2}' -f $item.FullName,$item.Attributes,$(if($item.PSIsContainer){@((Get-ChildItem -LiteralPath $_ -Force).Name|Sort-Object) -join ','}else{(Get-FileHash -LiteralPath $_).Hash})
        }) -join "`n"
    }
    $checks=@()
    foreach($name in @('missing-marker','duplicate-marker','pid-mismatch','uint-overflow','outside-base','noncanonical-path','attempt00','attempt33','unexpected-file','nonzero-file','normal-root-remains')){
        $case=New-RootContractCase
        try{
            $lines=@($case.marker);$normal=$false
            switch($name){
                'missing-marker' {$lines=@('no marker')}
                'duplicate-marker' {$lines=@($case.marker,$case.marker)}
                'pid-mismatch' {$lines=@($case.marker.Replace("pid=$($case.owner)","pid=$($case.owner+1)"))}
                'uint-overflow' {$lines=@($case.marker.Replace("pid=$($case.owner)",'pid=4294967296'))}
                'outside-base' {$lines=@('OWNED_ROOT pid='+$case.owner+' path='+(Join-Path ([IO.Path]::GetDirectoryName($base)) ([IO.Path]::GetFileName($case.root))))}
                'noncanonical-path' {$lines=@("OWNED_ROOT pid=$($case.owner) path=$base\..\terminal-helper-tests\$([IO.Path]::GetFileName($case.root))")}
                'attempt00' {$lines=@($case.marker.Substring(0,$case.marker.Length-2)+'00')}
                'attempt33' {$lines=@($case.marker.Substring(0,$case.marker.Length-2)+'33')}
                'unexpected-file' {$extra=Join-Path $case.child 'unexpected';[IO.File]::WriteAllBytes($extra,[byte[]]@(1,2,3));$case.extra=@($extra)}
                'nonzero-file' {[IO.File]::WriteAllBytes($case.file,[byte[]]@(7))}
                'normal-root-remains' {$normal=$true}
            }
            $before=Root-ContractSnapshot $case;$rejected=$false
            try{$null=Assert-HelperTestRootCompletion -Lines $lines -RepoRoot $RepoRoot -BaselineCleanup:(!$normal)}catch{$rejected=$true}
            if(!$rejected -or (Root-ContractSnapshot $case) -cne $before){throw "Helper root rejection modified owned paths:$name"}
            $checks+=@{name=$name;expected_rejection=$true;all_paths_unchanged=$true}
        }finally{Clear-RootContractCase $case}
    }
    $case=New-RootContractCase
    try{
        $record=Assert-HelperTestRootCompletion -Lines @($case.marker) -RepoRoot $RepoRoot -BaselineCleanup
        if(!$record.root_absent -or @($record.removed_paths).Count -ne 3 -or $null -ne (Get-HelperTestRootItem -Path $case.root -AllowMissing)){throw 'Exact45 cleanup contract failed'}
        $checks+=@{name='exact45-cleanup';expected_rejection=$false;removed_literal_paths=3}
    }finally{Clear-RootContractCase $case}
    $case=New-RootContractCase
    try{
        Clear-RootContractCase $case
        $record=Assert-HelperTestRootCompletion -Lines @($case.marker) -RepoRoot $RepoRoot
        if(!$record.root_absent -or @($record.removed_paths).Count){throw 'Normal absent-root contract failed'}
        $checks+=@{name='normal-root-absent';expected_rejection=$false;removed_literal_paths=0}
    }finally{Clear-RootContractCase $case}
    return $checks
}
