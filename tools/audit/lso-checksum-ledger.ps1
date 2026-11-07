# Historical dependency checks accept exactly the reviewed full-file LSO fix.
function Get-LsoChecksumLedgerHash([string]$Text){
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()
}
function Update-LsoChecksumLedger {
    param([Parameter(Mandatory)][string]$Body,[string]$CurrentBody)
    if((Get-LsoChecksumLedgerHash $Body) -cne 'a3a61042c8dc48ea02af9201fccf57c1da88a3cc52abf5de7c7fc56ffbd39374'){throw 'LSO checksum ledger requires the reviewed complete original'}
    $before='    if ((mask & (PB_TX_IP4_CHECKSUM | PB_TX_TCP4_CHECKSUM)) != (PB_TX_IP4_CHECKSUM | PB_TX_TCP4_CHECKSUM)) mask &= ~PB_TX_LSO4;'+"`n"+
        '    if (!(mask & PB_TX_TCP6_CHECKSUM)) mask &= ~PB_TX_LSO6;'+"`n"
    $after='    /* LSO calculates checksums for its own segments independently of the'+"`n"+
        '     * ordinary TX checksum configuration. Its own gates are intersected'+"`n"+
        '     * above; RX coalescing retains its validation prerequisites below. */'+"`n"
    if([regex]::Matches($Body,[regex]::Escape($before)).Count -ne 1){throw 'LSO checksum repair boundary changed'}
    $expected=$Body.Replace($before,$after)
    if((Get-LsoChecksumLedgerHash $expected) -cne '18dfa704c1f7d73533b5c7780d8be583cf00f7efe0ea206a0cfba44730d1de05'){throw 'LSO checksum projection differs from reviewed repair'}
    if($PSBoundParameters.ContainsKey('CurrentBody') -and $expected -cne $CurrentBody){throw 'Policy differs outside reviewed LSO checksum repair'}
    return $expected
}
function Assert-LsoChecksumFilePair {
    param([Parameter(Mandatory)][string]$Before,[Parameter(Mandatory)][string]$Current,[Parameter(Mandatory)][string]$Relative)
    $old=(Get-FileHash -LiteralPath $Before).Hash.ToLowerInvariant();$new=(Get-FileHash -LiteralPath $Current).Hash.ToLowerInvariant()
    if($old -ceq $new){return}
    if($Relative.Replace('\','/') -cne 'common/offload_policy.c'){throw "Unchanged dependency differs:$Relative"}
    if($old -cne 'a3a61042c8dc48ea02af9201fccf57c1da88a3cc52abf5de7c7fc56ffbd39374' -or $new -cne '18dfa704c1f7d73533b5c7780d8be583cf00f7efe0ea206a0cfba44730d1de05'){throw 'Policy raw bytes are not reviewed original/LSO repair'}
    $null=Update-LsoChecksumLedger -Body ([IO.File]::ReadAllText($Before)) -CurrentBody ([IO.File]::ReadAllText($Current))
}
function Assert-LsoChecksumDependency {
    param([Parameter(Mandatory)][string]$BaselineDirectory,[Parameter(Mandatory)][string]$Relative)
    Assert-LsoChecksumFilePair -Before (Join-Path $BaselineDirectory $Relative) -Current (Join-Path $script:RepoRoot $Relative) -Relative $Relative
}
function Test-LsoChecksumLedger {
    param([Parameter(Mandatory)][string]$Original,[Parameter(Mandatory)][string]$Repaired,[Parameter(Mandatory)][string]$Directory)
    if((Update-LsoChecksumLedger -Body $Original -CurrentBody $Repaired) -cne $Repaired){throw 'LSO ledger positive projection failed'}
    $checks=@(@{name='exact-text';accepted=$true});$cases=@(
        @{name='wrong-baseline';before=$Repaired;after=$Repaired},
        @{name='unrepaired-current';before=$Original;after=$Original},
        @{name='unrelated-suffix';before=$Original;after=$Repaired+"`n"},
        @{name='changed-rx-gate';before=$Original;after=$Repaired.Replace('if (!(mask & PB_RX_TCP6_CHECKSUM))','if (0)')},
        @{name='missing-original-line';before=$Original.Replace('mask &= ~PB_TX_LSO6;','');after=$Repaired},
        @{name='original-crlf';before=$Original.Replace("`n","`r`n");after=$Repaired},
        @{name='repair-crlf';before=$Original;after=$Repaired.Replace("`n","`r`n")})
    foreach($case in $cases){$rejected=$false;try{$null=Update-LsoChecksumLedger -Body $case.before -CurrentBody $case.after}catch{$rejected=$true};if(!$rejected){throw "LSO text ledger accepted:$($case.name)"};$checks+=@{name=$case.name;expected_rejection=$rejected}}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $old=Join-Path $Directory 'original.c';$new=Join-Path $Directory 'repaired.c';$utf8=[Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($old,$Original,$utf8);[IO.File]::WriteAllText($new,$Repaired,$utf8)
    Assert-LsoChecksumFilePair -Before $old -Current $new -Relative 'common/offload_policy.c';$checks+=@{name='exact-raw';accepted=$true}
    $files=@();foreach($label in @('old-bom','new-bom','old-utf16','new-utf16','old-crlf','new-crlf')){
        $body=if($label.StartsWith('old')){$Original}else{$Repaired};$path=Join-Path $Directory "$label.c"
        if($label.EndsWith('bom')){[IO.File]::WriteAllText($path,$body,[Text.UTF8Encoding]::new($true))}
        elseif($label.EndsWith('utf16')){[IO.File]::WriteAllText($path,$body,[Text.UnicodeEncoding]::new($false,$true))}
        else{[IO.File]::WriteAllText($path,$body.Replace("`n","`r`n"),$utf8)}
        $beforePath=if($label.StartsWith('old')){$path}else{$old};$afterPath=if($label.StartsWith('new')){$path}else{$new}
        $rejected=$false;try{Assert-LsoChecksumFilePair -Before $beforePath -Current $afterPath -Relative 'common/offload_policy.c'}catch{$rejected=$true}
        if(!$rejected){throw "LSO raw ledger accepted:$label"};$checks+=@{name=$label;expected_rejection=$true};$files+=@{path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $rejected=$false;try{Assert-LsoChecksumFilePair -Before $old -Current $new -Relative 'common/packet.c'}catch{$rejected=$true}
    if(!$rejected){throw 'LSO raw ledger accepted another file'};$checks+=@{name='wrong-relative';expected_rejection=$true}
    return @{checks=$checks;files=$files;original=@{path=$old;sha256=(Get-FileHash $old).Hash.ToLowerInvariant()};repaired=@{path=$new;sha256=(Get-FileHash $new).Hash.ToLowerInvariant()}}
}
