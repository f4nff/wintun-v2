# Historical audits permit only the reviewed post-exit helper error propagation.
# The complete canonical headers and raw source encodings remain constrained.
function Get-HelperDrainLedgerHash([string]$Text) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return [Convert]::ToHexString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()}
    finally{$hash.Dispose()}
}
function Update-HelperDrainLedger {
    param([Parameter(Mandatory)][string]$Body,[string]$CurrentBody)
    if((Get-HelperDrainLedgerHash $Body) -ne '7f5749ae9a3310f6a4d909dbcc29af78c5424b7268047245aec59073f788a76c'){
        throw 'Helper drain baseline is not the reviewed complete original'
    }
    $peekBefore='        if (!PeekNamedPipe(Stream->Read, NULL, 0, NULL, &available, NULL) || !available) break;'
    $peekAfter=@'
        if (!PeekNamedPipe(Stream->Read, NULL, 0, NULL, &available, NULL)) {
            DWORD error = GetLastError();
            HelperReadResult(Job, Stream, FALSE, 0, error); break;
        }
        if (!available) break;
'@
    $readBefore=@'
        if (!okay && GetLastError() == ERROR_IO_PENDING) {
            Stream->Pending = TRUE;
            okay = HelperAwaitIo(Job, Stream, &count);
        }
        if (!okay || !count) break;
        HelperConsume(Job, Stream, count);
'@
    $readAfter=@'
        DWORD error = okay ? ERROR_SUCCESS : GetLastError();
        if (!okay && error == ERROR_IO_PENDING) {
            Stream->Pending = TRUE;
            okay = HelperAwaitIo(Job, Stream, &count);
            error = okay ? ERROR_SUCCESS : GetLastError();
        }
        HelperReadResult(Job, Stream, okay, count, error);
        if (!okay || !count) break;
'@
    foreach($pair in @(@($peekBefore,$peekAfter),@($readBefore,$readAfter))){
        if([regex]::Matches($Body,[regex]::Escape($pair[0])).Count -ne 1){throw 'Helper drain change is not unique'}
        $Body=$Body.Replace($pair[0],$pair[1])
    }
    if((Get-HelperDrainLedgerHash $Body) -ne '0fea04423f2e1dd8c87ec50f51334cd49a73fcba9cb854b3023a406dee16303d'){
        throw 'Helper drain projection is not the reviewed complete repair'
    }
    if($PSBoundParameters.ContainsKey('CurrentBody') -and $Body -cne $CurrentBody){throw 'Helper differs outside the reviewed drain repair'}
    return $Body
}
function Assert-HelperDrainRepairedFile {
    param([Parameter(Mandatory)][string]$Path)
    if((Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -ne '0fea04423f2e1dd8c87ec50f51334cd49a73fcba9cb854b3023a406dee16303d'){
        throw 'Helper drain current raw file is not the reviewed repair'
    }
}
function Assert-HelperDrainFilePair {
    param([Parameter(Mandatory)][string]$Before,[Parameter(Mandatory)][string]$Current,[Parameter(Mandatory)][string]$Relative)
    $oldHash=(Get-FileHash -LiteralPath $Before).Hash.ToLowerInvariant()
    $newHash=(Get-FileHash -LiteralPath $Current).Hash.ToLowerInvariant()
    if($oldHash -eq $newHash){return}
    if($Relative.Replace('\','/') -cne 'api/rundll32_process.h'){throw "Unchanged helper dependency differs: $Relative"}
    if($oldHash -ne '7f5749ae9a3310f6a4d909dbcc29af78c5424b7268047245aec59073f788a76c'){throw 'Helper drain original raw file changed'}
    Assert-HelperDrainRepairedFile -Path $Current
    $null=Update-HelperDrainLedger -Body ([IO.File]::ReadAllText($Before)) -CurrentBody ([IO.File]::ReadAllText($Current))
}
function Assert-HelperDrainDependency {
    param([Parameter(Mandatory)][string]$BaselineDirectory,[Parameter(Mandatory)][string]$Relative)
    Assert-HelperDrainFilePair -Before (Join-Path $BaselineDirectory $Relative) -Current (Join-Path $script:RepoRoot $Relative) -Relative $Relative
}
function Test-HelperDrainLedger {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$Original,[Parameter(Mandatory)][string]$Repaired)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Helper drain contracts require an absolute directory'}
    if((Update-HelperDrainLedger -Body $Original -CurrentBody $Repaired) -cne $Repaired){throw 'Helper drain positive text contract failed'}
    $checks=@(@{name='exact-reviewed-text';expected_rejection=$false;passed=$true})
    $textCases=@(
        @{name='wrong-baseline';before=$Repaired;current=$Repaired},
        @{name='unrepaired-current';before=$Original;current=$Original},
        @{name='unrelated-suffix';before=$Original;current=($Repaired+"`n")},
        @{name='altered-classification';before=$Original;current=$Repaired.Replace('HelperReadResult(Job, Stream, FALSE, 0, error);','HelperError(Job, error);')},
        @{name='duplicate-boundary';before=($Original+' if (!okay || !count) break;');current=$Repaired},
        @{name='missing-boundary';before=$Original.Replace(' || !available','');current=$Repaired},
        @{name='baseline-crlf';before=$Original.Replace("`n","`r`n");current=$Repaired},
        @{name='current-crlf';before=$Original;current=$Repaired.Replace("`n","`r`n")}
    )
    foreach($case in $textCases){
        $rejected=$false;try{$null=Update-HelperDrainLedger -Body $case.before -CurrentBody $case.current}catch{$rejected=$true}
        if(!$rejected){throw "Helper drain text contract accepted: $($case.name)"}
        $checks+=@{name=$case.name;expected_rejection=$true;passed=$true}
    }
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $plain=[Text.UTF8Encoding]::new($false);$bom=[Text.UTF8Encoding]::new($true)
    $before=Join-Path $Directory 'original.h';$current=Join-Path $Directory 'repaired.h'
    $oldBom=Join-Path $Directory 'original-bom.h';$newBom=Join-Path $Directory 'repaired-bom.h'
    $utf16=Join-Path $Directory 'repaired-utf16.h';$crlf=Join-Path $Directory 'repaired-crlf.h'
    [IO.File]::WriteAllText($before,$Original,$plain);[IO.File]::WriteAllText($current,$Repaired,$plain)
    [IO.File]::WriteAllText($oldBom,$Original,$bom);[IO.File]::WriteAllText($newBom,$Repaired,$bom)
    [IO.File]::WriteAllText($utf16,$Repaired,[Text.Encoding]::Unicode);[IO.File]::WriteAllText($crlf,$Repaired.Replace("`n","`r`n"),$plain)
    $fileCases=@(
        @{name='same-original-bytes';before=$before;current=$before;relative='api/rundll32_process.h';reject=$false},
        @{name='exact-repair-bytes';before=$before;current=$current;relative='api/rundll32_process.h';reject=$false},
        @{name='different-relative';before=$before;current=$current;relative='api/rundll32.c';reject=$true},
        @{name='original-bom';before=$oldBom;current=$current;relative='api/rundll32_process.h';reject=$true},
        @{name='repaired-bom';before=$before;current=$newBom;relative='api/rundll32_process.h';reject=$true},
        @{name='repaired-utf16';before=$before;current=$utf16;relative='api/rundll32_process.h';reject=$true},
        @{name='repaired-crlf';before=$before;current=$crlf;relative='api/rundll32_process.h';reject=$true}
    )
    foreach($case in $fileCases){
        $rejected=$false;try{Assert-HelperDrainFilePair -Before $case.before -Current $case.current -Relative $case.relative}catch{$rejected=$true}
        if($rejected -ne $case.reject){throw "Helper drain byte contract failed: $($case.name)"}
        $checks+=@{name=$case.name;expected_rejection=$case.reject;passed=$true}
    }
    return @{checks=$checks;files=@($before,$current,$oldBom,$newBom,$utf16,$crlf)|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}}
}
