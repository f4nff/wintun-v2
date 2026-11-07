. (Join-Path $PSScriptRoot 'network-dependency-ledger.ps1')
# Keep the original complete-file per-NB diagnostic cursor contract unchanged.
# Only the two reviewed network paths delegate to their separate full-file ledgers.
function Get-SendDiagnosticLedgerHash([string]$Text) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return [Convert]::ToHexString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()}
    finally{$hash.Dispose()}
}
function Update-SendDiagnosticLedger {
    param([Parameter(Mandatory)][string]$Body,[string]$CurrentBody)
    if((Get-SendDiagnosticLedgerHash $Body) -ne '9ac96a6fbf63c04b0f1cbaf82ee457de4a7dc8644809a9b71dcb216bfd0ac225'){
        throw 'Send diagnostic baseline is not the reviewed complete original'
    }
    $before="            failedNb = nb;`r`n"
    $after=$before+"            /* Classification belongs to this NB; an early failure must not`r`n"+
        "             * inherit the preceding NB's queue or Ethernet offset. */`r`n"+
        "            failedQueue = PB_SEND_QUEUE_UNKNOWN;`r`n"+
        "            failedOffset = 0;`r`n"
    if([regex]::Matches($Body,[regex]::Escape($before)).Count -ne 1){throw 'Send diagnostic cursor boundary is not unique'}
    $expected=$Body.Replace($before,$after)
    if((Get-SendDiagnosticLedgerHash $expected) -ne '3ad7d9c28a9cda91b707a3dbaea3a3b23ef5483e0b4549c503695d58c73ee7c5'){
        throw 'Send diagnostic projection is not the reviewed complete repair'
    }
    if($PSBoundParameters.ContainsKey('CurrentBody') -and $expected -cne $CurrentBody){throw 'Send differs outside the reviewed diagnostic repair'}
    return $expected
}
function Assert-SendDiagnosticFilePair {
    param([Parameter(Mandatory)][string]$Before,[Parameter(Mandatory)][string]$Current,[Parameter(Mandatory)][string]$Relative)
    $beforeHash=(Get-FileHash -LiteralPath $Before).Hash.ToLowerInvariant()
    $currentHash=(Get-FileHash -LiteralPath $Current).Hash.ToLowerInvariant()
    if($beforeHash -eq $currentHash){return}
    if($Relative.Replace('\','/') -cin @('common/offload_policy.c','driver/v2/session_describe.h')){
        Assert-NetworkDependencyFilePair -Before $Before -Current $Current -Relative $Relative
        return
    }
    if($Relative.Replace('\','/') -cne 'driver/v2/session_send.h'){throw "Unchanged kernel dependency differs: $Relative"}
    # ReadAllText consumes BOMs and decodes UTF-16. Lock raw bytes before using
    # the pure text projection so encoding changes cannot bypass the contract.
    if($beforeHash -ne '9ac96a6fbf63c04b0f1cbaf82ee457de4a7dc8644809a9b71dcb216bfd0ac225' -or
        $currentHash -ne '3ad7d9c28a9cda91b707a3dbaea3a3b23ef5483e0b4549c503695d58c73ee7c5'){
        throw 'Send diagnostic raw files are not the reviewed original and repair'
    }
    $null=Update-SendDiagnosticLedger -Body ([IO.File]::ReadAllText($Before)) -CurrentBody ([IO.File]::ReadAllText($Current))
}
function Assert-SendDiagnosticDependency {
    param([Parameter(Mandatory)][string]$BaselineDirectory,[Parameter(Mandatory)][string]$Relative)
    Assert-SendDiagnosticFilePair -Before (Join-Path $BaselineDirectory $Relative) -Current (Join-Path $script:RepoRoot $Relative) -Relative $Relative
}
function Test-SendDiagnosticLedger {
    param([Parameter(Mandatory)][string]$Original,[Parameter(Mandatory)][string]$Repaired)
    if((Update-SendDiagnosticLedger -Body $Original -CurrentBody $Repaired) -cne $Repaired){throw 'Send diagnostic positive contract failed'}
    $cases=@(
        @{name='wrong-baseline';before=$Repaired;after=$Repaired},
        @{name='original-current';before=$Original;after=$Original},
        @{name='unrelated-suffix';before=$Original;after=($Repaired+"`r`n")},
        @{name='altered-repair';before=$Original;after=$Repaired.Replace('failedOffset = 0;','failedOffset = 14;')},
        @{name='duplicate-boundary';before=($Original+"            failedNb = nb;`r`n");after=$Repaired},
        @{name='missing-boundary';before=$Original.Replace("            failedNb = nb;`r`n",'');after=$Repaired},
        @{name='baseline-lf';before=$Original.Replace("`r`n","`n");after=$Repaired},
        @{name='current-lf';before=$Original;after=$Repaired.Replace("`r`n","`n")}
    )
    $checks=@(@{name='exact-reviewed-repair';expected_rejection=$false;passed=$true})
    foreach($case in $cases){
        $rejected=$false;try{$null=Update-SendDiagnosticLedger -Body $case.before -CurrentBody $case.after}catch{$rejected=$true}
        if(!$rejected){throw "Send diagnostic negative contract accepted: $($case.name)"}
        $checks+=@{name=$case.name;expected_rejection=$true;passed=$true}
    }
    return $checks
}
function Test-SendDiagnosticFileLedger {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$Original,[Parameter(Mandatory)][string]$Repaired)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Send diagnostic file contracts require an absolute output directory'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $plain=[Text.UTF8Encoding]::new($false);$bom=[Text.UTF8Encoding]::new($true)
    $originalPath=Join-Path $Directory 'original.h';$repairedPath=Join-Path $Directory 'repaired.h'
    $originalBom=Join-Path $Directory 'original-bom.h';$repairedBom=Join-Path $Directory 'repaired-bom.h'
    $repairedUtf16=Join-Path $Directory 'repaired-utf16.h';$repairedLf=Join-Path $Directory 'repaired-lf.h'
    [IO.File]::WriteAllText($originalPath,$Original,$plain);[IO.File]::WriteAllText($repairedPath,$Repaired,$plain)
    [IO.File]::WriteAllText($originalBom,$Original,$bom);[IO.File]::WriteAllText($repairedBom,$Repaired,$bom)
    [IO.File]::WriteAllText($repairedUtf16,$Repaired,[Text.Encoding]::Unicode);[IO.File]::WriteAllText($repairedLf,$Repaired.Replace("`r`n","`n"),$plain)
    $cases=@(
        @{name='same-original-bytes';before=$originalPath;current=$originalPath;relative='driver/v2/session_send.h';reject=$false},
        @{name='exact-repaired-bytes';before=$originalPath;current=$repairedPath;relative='driver/v2/session_send.h';reject=$false},
        @{name='different-relative';before=$originalPath;current=$repairedPath;relative='driver/v2/session_read.h';reject=$true},
        @{name='baseline-bom';before=$originalBom;current=$repairedPath;relative='driver/v2/session_send.h';reject=$true},
        @{name='current-bom';before=$originalPath;current=$repairedBom;relative='driver/v2/session_send.h';reject=$true},
        @{name='current-utf16';before=$originalPath;current=$repairedUtf16;relative='driver/v2/session_send.h';reject=$true},
        @{name='current-lf-bytes';before=$originalPath;current=$repairedLf;relative='driver/v2/session_send.h';reject=$true}
    )
    $checks=@()
    foreach($case in $cases){
        $rejected=$false;try{Assert-SendDiagnosticFilePair -Before $case.before -Current $case.current -Relative $case.relative}catch{$rejected=$true}
        if($rejected -ne $case.reject){throw "Send diagnostic byte contract failed: $($case.name)"}
        $checks+=@{name=$case.name;expected_rejection=$case.reject;passed=$true}
    }
    $files=@($originalPath,$repairedPath,$originalBom,$repairedBom,$repairedUtf16,$repairedLf)|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return @{checks=$checks;files=$files}
}
