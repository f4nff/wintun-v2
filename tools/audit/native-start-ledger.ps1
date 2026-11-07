# Historical audits permit only the independently reviewed Allocate failure contract.
# Keep whole-file comparisons: this projection must never discard unrelated bytes.
function Get-NativeStartLedgerHash([string]$Text) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return [Convert]::ToHexString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()}
    finally{$hash.Dispose()}
}
function Get-NativeStartLedgerRegion([string]$Body) {
    $text=$Body.Replace("`r`n","`n")
    if([regex]::Matches($text,'static PB_RESULT Allocate\(').Count -ne 1){throw 'Allocate signature is not unique'}
    $matches=[regex]::Matches($text,'(?s)static PB_RESULT Allocate\(const PB_CONFIG \*config, PB_SESSION \*\*out\).*?(?=/\* Consumes StartAdapterSession)')
    if($matches.Count -ne 1){throw 'Allocate region is not unique'}
    return @{text=$text;region=$matches[0]}
}
function Update-NativeStartFailureLedger {
    param([Parameter(Mandatory)][string]$Body,[string]$CurrentBody)
    if(!$PSBoundParameters.ContainsKey('CurrentBody')){
        $CurrentBody=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'api\v2\session.c'))
    }
    $before=Get-NativeStartLedgerRegion $Body
    $after=Get-NativeStartLedgerRegion $CurrentBody
    if((Get-NativeStartLedgerHash $before.region.Value) -ne 'e60a061eafa0fc5e43a38c3cd4027a56b3c396c4d5872ed7f66828e578db51b4'){
        throw 'Allocate baseline region is not the reviewed original'
    }
    if((Get-NativeStartLedgerHash $after.region.Value) -ne 'a02124e7e107d2648c02dd8d1c91b5dde4d70e69ac039caccbf4a91d2ab39ed0'){
        throw 'Allocate current region is not the reviewed repair'
    }
    return $before.text.Substring(0,$before.region.Index)+$after.region.Value+
        $before.text.Substring($before.region.Index+$before.region.Length)
}
function Assert-NativeStartDependency {
    param([Parameter(Mandatory)][string]$BaselineDirectory,[Parameter(Mandatory)][string]$Relative)
    $before=Join-Path $BaselineDirectory $Relative
    $current=Join-Path $script:RepoRoot $Relative
    if($Relative.Replace('\','/') -ceq 'api/v2/session.c'){
        if((Get-FileHash -LiteralPath $before).Hash.ToLowerInvariant() -ne '5cda986df0d2ed72774de988e3afe5274809bb3b480e1e1818490a9b1d2f75d4'){
            throw 'Unexpected frozen native session source'
        }
        $expected=Update-NativeStartFailureLedger ([IO.File]::ReadAllText($before))
        if($expected -cne [IO.File]::ReadAllText($current).Replace("`r`n","`n")){
            throw 'Native session differs outside the reviewed Allocate repair'
        }
    }elseif((Get-FileHash -LiteralPath $before).Hash -ne (Get-FileHash -LiteralPath $current).Hash){
        throw "Unchanged native dependency differs: $Relative"
    }
}
