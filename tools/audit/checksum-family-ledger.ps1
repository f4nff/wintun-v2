# Admit only the reviewed complete-file NDIS checksum-family selection repair.
function Get-ChecksumFamilyLedgerHash([string]$Text){
    return [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()
}
function Update-ChecksumFamilyLedger {
    param([Parameter(Mandatory)][string]$Body,[string]$CurrentBody)
    if((Get-ChecksumFamilyLedgerHash $Body) -cne '79aa2380c77311ef9bf5353cdc2ee0b75bac2126a858d40bbddbd2e37fe6f694'){throw 'Checksum family ledger requires the reviewed complete original'}
    $before='        if (c.Transmit.IpHeaderChecksum || c.Transmit.TcpChecksum || c.Transmit.UdpChecksum) {'+"`r`n"
    $after='        /* NDIS defines no checksum task when neither family is selected.'+"`r`n"+
        '         * Only interpret task selectors after that explicit family gate. */'+"`r`n"+
        '        if ((c.Transmit.IsIPv4 || c.Transmit.IsIPv6) &&'+"`r`n"+
        '            (c.Transmit.IpHeaderChecksum || c.Transmit.TcpChecksum || c.Transmit.UdpChecksum)) {'+"`r`n"
    if([regex]::Matches($Body,[regex]::Escape($before)).Count -ne 1){throw 'Checksum family repair boundary changed'}
    $expected=$Body.Replace($before,$after)
    if((Get-ChecksumFamilyLedgerHash $expected) -cne 'f25f4ea67821a2d1b0efe95e5bf50570dc6bceeec261ae0ee7e535c0d3d11f97'){throw 'Checksum family projection differs from reviewed repair'}
    if($PSBoundParameters.ContainsKey('CurrentBody') -and $expected -cne $CurrentBody){throw 'Describe differs outside reviewed checksum-family repair'}
    return $expected
}
function Assert-ChecksumFamilyFilePair {
    param([Parameter(Mandatory)][string]$Before,[Parameter(Mandatory)][string]$Current,[Parameter(Mandatory)][string]$Relative)
    $old=(Get-FileHash -LiteralPath $Before).Hash.ToLowerInvariant();$new=(Get-FileHash -LiteralPath $Current).Hash.ToLowerInvariant()
    if($old -ceq $new){return}
    if($Relative.Replace('\','/') -cne 'driver/v2/session_describe.h'){throw "Unchanged dependency differs:$Relative"}
    if($old -cne '79aa2380c77311ef9bf5353cdc2ee0b75bac2126a858d40bbddbd2e37fe6f694' -or $new -cne 'f25f4ea67821a2d1b0efe95e5bf50570dc6bceeec261ae0ee7e535c0d3d11f97'){throw 'Describe raw bytes are not reviewed original/family repair'}
    $null=Update-ChecksumFamilyLedger -Body ([IO.File]::ReadAllText($Before)) -CurrentBody ([IO.File]::ReadAllText($Current))
}
function Test-ChecksumFamilyLedger {
    param([Parameter(Mandatory)][string]$Original,[Parameter(Mandatory)][string]$Repaired,[Parameter(Mandatory)][string]$Directory)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Checksum family contracts require an absolute directory'}
    if((Update-ChecksumFamilyLedger -Body $Original -CurrentBody $Repaired) -cne $Repaired){throw 'Checksum family positive projection failed'}
    $checks=@(@{name='exact-text';accepted=$true});$cases=@(
        @{name='wrong-baseline';before=$Repaired;after=$Repaired},
        @{name='unrepaired-current';before=$Original;after=$Original},
        @{name='unrelated-suffix';before=$Original;after=$Repaired+"`r`n"},
        @{name='changed-active-family-validation';before=$Original;after=$Repaired.Replace('!!c.Transmit.IsIPv4 != (ip == 4)','!!c.Transmit.IsIPv4 == (ip == 4)')},
        @{name='changed-gso-branch';before=$Original;after=$Repaired.Replace('if (lso.Value || uso.Value)','if (lso.Value && uso.Value)')},
        @{name='original-lf';before=$Original.Replace("`r`n","`n");after=$Repaired},
        @{name='repair-lf';before=$Original;after=$Repaired.Replace("`r`n","`n")})
    foreach($case in $cases){$rejected=$false;try{$null=Update-ChecksumFamilyLedger -Body $case.before -CurrentBody $case.after}catch{$rejected=$true};if(!$rejected){throw "Checksum family text ledger accepted:$($case.name)"};$checks+=@{name=$case.name;expected_rejection=$true}}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $old=Join-Path $Directory 'original.h';$new=Join-Path $Directory 'repaired.h';$utf8=[Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($old,$Original,$utf8);[IO.File]::WriteAllText($new,$Repaired,$utf8)
    Assert-ChecksumFamilyFilePair -Before $old -Current $new -Relative 'driver/v2/session_describe.h';$checks+=@{name='exact-raw';accepted=$true}
    $files=@();foreach($label in @('old-bom','new-bom','old-utf16','new-utf16','old-lf','new-lf')){
        $body=if($label.StartsWith('old')){$Original}else{$Repaired};$path=Join-Path $Directory "$label.h"
        if($label.EndsWith('bom')){[IO.File]::WriteAllText($path,$body,[Text.UTF8Encoding]::new($true))}
        elseif($label.EndsWith('utf16')){[IO.File]::WriteAllText($path,$body,[Text.UnicodeEncoding]::new($false,$true))}
        else{[IO.File]::WriteAllText($path,$body.Replace("`r`n","`n"),$utf8)}
        $beforePath=if($label.StartsWith('old')){$path}else{$old};$afterPath=if($label.StartsWith('new')){$path}else{$new}
        $rejected=$false;try{Assert-ChecksumFamilyFilePair -Before $beforePath -Current $afterPath -Relative 'driver/v2/session_describe.h'}catch{$rejected=$true}
        if(!$rejected){throw "Checksum family raw ledger accepted:$label"};$checks+=@{name=$label;expected_rejection=$true};$files+=@{path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $rejected=$false;try{Assert-ChecksumFamilyFilePair -Before $old -Current $new -Relative 'driver/v2/session_send.h'}catch{$rejected=$true}
    if(!$rejected){throw 'Checksum family raw ledger accepted another file'};$checks+=@{name='wrong-relative';expected_rejection=$true}
    return @{checks=$checks;files=$files;original=@{path=$old;sha256=(Get-FileHash $old).Hash.ToLowerInvariant()};repaired=@{path=$new;sha256=(Get-FileHash $new).Hash.ToLowerInvariant()}}
}
# This audit tool is itself frozen by the offload-send replay. Its only change
# authenticates old/current headers before validating whole current snapshots.
# It is deliberately separate from the two-path production dependency router.
function Update-ChecksumFamilyProjectionLedger {
    param([Parameter(Mandatory)][string]$Body,[string]$CurrentBody)
    if((Get-ChecksumFamilyLedgerHash $Body) -cne 'bf7949bbec7ba5789e06955c22e4152dccaa92293a3b2c511f99d4b1fed56158'){throw 'Checksum projection ledger requires the reviewed complete original tool'}
    $expected=". (Join-Path `$PSScriptRoot 'network-dependency-ledger.ps1')`n"+$Body
    $marker='$source=Join-Path $script:RepoRoot "driver\v2\session_$name.h";$old=Join-Path $baseline "driver\v2\session_$name.h"'+"`n"
    if([regex]::Matches($expected,[regex]::Escape($marker)).Count -ne 1){throw 'Checksum projection source boundary changed'}
    $expected=$expected.Replace($marker,$marker+'        Assert-NetworkDependencyFilePair -Before $old -Current $source -Relative "driver/v2/session_$name.h"'+"`n")
    $before='$part.length -ne [IO.File]::ReadAllText($old).Length -or'+"`n"+'            $part.source_sha256 -cne (Get-FileHash $old).Hash.ToLowerInvariant()'
    $after='$part.length -ne [IO.File]::ReadAllText($source).Length -or'+"`n"+'            $part.source_sha256 -cne (Get-FileHash $source).Hash.ToLowerInvariant()'
    if([regex]::Matches($expected,[regex]::Escape($before)).Count -ne 1){throw 'Checksum projection whole-header boundary changed'}
    $expected=$expected.Replace($before,$after)
    if((Get-ChecksumFamilyLedgerHash $expected) -cne '0357e28044713dc7ff21c71db15a7436f0e0417f87f8e451a78e27793a784b03'){throw 'Checksum projection tool differs from reviewed repair'}
    if($PSBoundParameters.ContainsKey('CurrentBody') -and $expected -cne $CurrentBody){throw 'Projection tool differs outside reviewed header authentication repair'}
    return $expected
}
function Assert-ChecksumFamilyProjectionFilePair {
    param([Parameter(Mandatory)][string]$Before,[Parameter(Mandatory)][string]$Current,[Parameter(Mandatory)][string]$Relative)
    $old=(Get-FileHash -LiteralPath $Before).Hash.ToLowerInvariant();$new=(Get-FileHash -LiteralPath $Current).Hash.ToLowerInvariant()
    if($old -ceq $new){return}
    if($Relative.Replace('\','/') -cne 'tools/audit/tun32-projection.ps1'){throw "Unchanged audit dependency differs:$Relative"}
    if($old -cne 'bf7949bbec7ba5789e06955c22e4152dccaa92293a3b2c511f99d4b1fed56158' -or $new -cne '0357e28044713dc7ff21c71db15a7436f0e0417f87f8e451a78e27793a784b03'){throw 'Projection raw tool bytes are not reviewed original/repair'}
    $null=Update-ChecksumFamilyProjectionLedger -Body ([IO.File]::ReadAllText($Before)) -CurrentBody ([IO.File]::ReadAllText($Current))
}
function Test-ChecksumFamilyProjectionLedger {
    param([Parameter(Mandatory)][string]$Original,[Parameter(Mandatory)][string]$Repaired,[Parameter(Mandatory)][string]$Directory)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Checksum projection contracts require an absolute directory'}
    if((Update-ChecksumFamilyProjectionLedger -Body $Original -CurrentBody $Repaired) -cne $Repaired){throw 'Checksum projection positive failed'}
    $checks=@(@{name='exact-text';accepted=$true});$cases=@(
        @{name='wrong-baseline';before=$Repaired;after=$Repaired},
        @{name='unrepaired-current';before=$Original;after=$Original},
        @{name='unrelated-suffix';before=$Original;after=$Repaired+"`n"},
        @{name='missing-authentication';before=$Original;after=$Repaired.Replace('Assert-NetworkDependencyFilePair -Before $old -Current $source','Write-Output -InputObject $source')},
        @{name='wrong-source-length';before=$Original;after=$Repaired.Replace('[IO.File]::ReadAllText($source).Length','[IO.File]::ReadAllText($old).Length')},
        @{name='original-normalized-lf';before=$Original.Replace("`r`n","`n");after=$Repaired},
        @{name='repair-normalized-lf';before=$Original;after=$Repaired.Replace("`r`n","`n")})
    foreach($case in $cases){$rejected=$false;try{$null=Update-ChecksumFamilyProjectionLedger -Body $case.before -CurrentBody $case.after}catch{$rejected=$true};if(!$rejected){throw "Checksum projection text ledger accepted:$($case.name)"};$checks+=@{name=$case.name;expected_rejection=$true}}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $old=Join-Path $Directory 'original.ps1';$new=Join-Path $Directory 'repaired.ps1';$utf8=[Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($old,$Original,$utf8);[IO.File]::WriteAllText($new,$Repaired,$utf8)
    Assert-ChecksumFamilyProjectionFilePair -Before $old -Current $new -Relative 'tools/audit/tun32-projection.ps1';$checks+=@{name='exact-raw';accepted=$true}
    $files=@();foreach($label in @('old-bom','new-bom','old-utf16','new-utf16','old-normalized-lf','new-normalized-lf')){
        $body=if($label.StartsWith('old')){$Original}else{$Repaired};$path=Join-Path $Directory "$label.ps1"
        if($label.EndsWith('bom')){[IO.File]::WriteAllText($path,$body,[Text.UTF8Encoding]::new($true))}
        elseif($label.EndsWith('utf16')){[IO.File]::WriteAllText($path,$body,[Text.UnicodeEncoding]::new($false,$true))}
        else{[IO.File]::WriteAllText($path,$body.Replace("`r`n","`n"),$utf8)}
        $beforePath=if($label.StartsWith('old')){$path}else{$old};$afterPath=if($label.StartsWith('new')){$path}else{$new}
        $rejected=$false;try{Assert-ChecksumFamilyProjectionFilePair -Before $beforePath -Current $afterPath -Relative 'tools/audit/tun32-projection.ps1'}catch{$rejected=$true}
        if(!$rejected){throw "Checksum projection raw ledger accepted:$label"};$checks+=@{name=$label;expected_rejection=$true};$files+=@{path=$path;sha256=(Get-FileHash $path).Hash.ToLowerInvariant()}
    }
    $rejected=$false;try{Assert-ChecksumFamilyProjectionFilePair -Before $old -Current $new -Relative 'tools/audit/verify-tun32-offload-send.ps1'}catch{$rejected=$true}
    if(!$rejected){throw 'Checksum projection ledger accepted another tool'};$checks+=@{name='wrong-relative';expected_rejection=$true}
    return @{checks=$checks;files=$files;original=@{path=$old;sha256=(Get-FileHash $old).Hash.ToLowerInvariant()};repaired=@{path=$new;sha256=(Get-FileHash $new).Hash.ToLowerInvariant()}}
}
