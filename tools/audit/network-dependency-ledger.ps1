. (Join-Path $PSScriptRoot 'lso-checksum-ledger.ps1')
. (Join-Path $PSScriptRoot 'checksum-family-ledger.ps1')
# Network dependency exceptions remain separate exact complete-file contracts.
# A caller must first authenticate its original snapshot/manifest as before.
function Assert-NetworkDependencyFilePair {
    param([Parameter(Mandatory)][string]$Before,[Parameter(Mandatory)][string]$Current,[Parameter(Mandatory)][string]$Relative)
    switch -CaseSensitive ($Relative.Replace('\','/')) {
        'common/offload_policy.c' {Assert-LsoChecksumFilePair -Before $Before -Current $Current -Relative $Relative;return}
        'driver/v2/session_describe.h' {Assert-ChecksumFamilyFilePair -Before $Before -Current $Current -Relative $Relative;return}
        default {if((Get-FileHash -LiteralPath $Before).Hash -cne (Get-FileHash -LiteralPath $Current).Hash){throw "Unchanged network dependency differs:$Relative"}}
    }
}
function Assert-NetworkDependency {
    param([Parameter(Mandatory)][string]$BaselineDirectory,[Parameter(Mandatory)][string]$Relative)
    Assert-NetworkDependencyFilePair -Before (Join-Path $BaselineDirectory $Relative) -Current (Join-Path $script:RepoRoot $Relative) -Relative $Relative
}
function Test-NetworkDependencyLedger {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][string]$ChecksumOriginal,[Parameter(Mandatory)][string]$ChecksumRepaired,
        [Parameter(Mandatory)][string]$PolicyOriginal,[Parameter(Mandatory)][string]$PolicyRepaired)
    if(![IO.Path]::IsPathFullyQualified($Directory)){throw 'Network router contracts require an absolute directory'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null;$encoding=[Text.UTF8Encoding]::new($false)
    $paths=@{};foreach($pair in @(@('checksum-before.h',$ChecksumOriginal),@('checksum-after.h',$ChecksumRepaired),@('policy-before.c',$PolicyOriginal),@('policy-after.c',$PolicyRepaired))){$path=Join-Path $Directory $pair[0];[IO.File]::WriteAllText($path,$pair[1],$encoding);$paths[$pair[0]]=$path}
    $cases=@(
        @{name='checksum-exact';before='checksum-before.h';after='checksum-after.h';relative='driver/v2/session_describe.h';reject=$false},
        @{name='policy-exact';before='policy-before.c';after='policy-after.c';relative='common/offload_policy.c';reject=$false},
        @{name='checksum-windows-path';before='checksum-before.h';after='checksum-after.h';relative='driver\v2\session_describe.h';reject=$false},
        @{name='same-unknown';before='checksum-before.h';after='checksum-before.h';relative='driver/v2/session_read.h';reject=$false},
        @{name='different-unknown';before='checksum-before.h';after='checksum-after.h';relative='driver/v2/session_read.h';reject=$true},
        @{name='send-not-network-exception';before='checksum-before.h';after='checksum-after.h';relative='driver/v2/session_send.h';reject=$true},
        @{name='checksum-wrong-case';before='checksum-before.h';after='checksum-after.h';relative='driver/v2/Session_Describe.h';reject=$true},
        @{name='policy-wrong-case';before='policy-before.c';after='policy-after.c';relative='common/Offload_Policy.c';reject=$true},
        @{name='checksum-reverse';before='checksum-after.h';after='checksum-before.h';relative='driver/v2/session_describe.h';reject=$true},
        @{name='policy-reverse';before='policy-after.c';after='policy-before.c';relative='common/offload_policy.c';reject=$true},
        @{name='checksum-on-policy';before='checksum-before.h';after='checksum-after.h';relative='common/offload_policy.c';reject=$true},
        @{name='policy-on-checksum';before='policy-before.c';after='policy-after.c';relative='driver/v2/session_describe.h';reject=$true})
    $checks=@();foreach($case in $cases){$rejected=$false;try{Assert-NetworkDependencyFilePair -Before $paths[$case.before] -Current $paths[$case.after] -Relative $case.relative}catch{$rejected=$true};if($rejected -ne $case.reject){throw "Network router contract failed:$($case.name)"};$checks+=@{name=$case.name;expected_rejection=$case.reject;passed=$true}}
    return @{checks=$checks;files=@($paths.Values|Sort-Object|ForEach-Object {@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}})}
}
