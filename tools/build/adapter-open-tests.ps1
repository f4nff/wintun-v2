# Exact complete function extraction; no production include split or body edits.
# Caller initializes common.ps1 and owns recording.
function Build-AdapterOpenTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('before','current')][string]$Variant='current')
    $Directory=[IO.Path]::GetFullPath($Directory);New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'api\adapter.c'
    if($Variant -eq 'before'){
        $source=Join-Path $script:RepoRoot 'out\diagnostic-owner-baseline\5fd7eb8e2353\api\adapter.c'
        if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -ne '7e9249b9216d3e144391d04d0a45892570e3a8cc858af66cf8e084f1e2a6660e'){throw 'Unexpected frozen adapter.c'}
    }
    $text=[IO.File]::ReadAllText($source);$signature='OpenAdapterInternal(LPCWSTR Name)';$endMarker='typedef struct ADAPTER_OPERATION_INPUT'
    if(([regex]::Matches($text,[regex]::Escape($signature))).Count -ne 1 -or ([regex]::Matches($text,[regex]::Escape($endMarker))).Count -ne 1){throw 'Adapter-open extraction markers are not unique'}
    $name=$text.IndexOf($signature);$start=$text.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$name);$end=$text.IndexOf($endMarker,$name)
    if($name -lt 0 -or $start -lt 0 -or $end -le $start){throw 'Cannot isolate complete OpenAdapterInternal'}
    $region=$text.Substring($start,$end-$start)
    if(!$region.StartsWith('static WINTUN_ADAPTER_HANDLE') -or !$region.Contains($signature) -or !$region.TrimEnd().EndsWith('}')){throw 'Invalid complete OpenAdapterInternal region'}
    $encoding=[Text.UTF8Encoding]::new($false);$helper=Join-Path $Directory 'adapter-open.h';[IO.File]::WriteAllText($helper,$region,$encoding)
    $regionHash=(Get-FileHash -LiteralPath $helper).Hash.ToLowerInvariant()
    if($Variant -eq 'before' -and $regionHash -ne '10d5be2bb44c99592f2f653c203299f1c70fb9af640af5560e31ba0997987a44'){throw 'Frozen OpenAdapterInternal region hash mismatch'}
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_ADAPTER_OPEN_SOURCE "'+$helper.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\adapter_open_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllLines($entry,$lines,$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'adapter-open.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",$entry,'/link',"/out:$program"))|Out-Host
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        source=$source;source_sha256=(Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant();region_offset_chars=$start;region_length_chars=($end-$start);
        region=$helper;region_sha256=$regionHash;entry=$entry;entry_sha256=(Get-FileHash -LiteralPath $entry).Hash.ToLowerInvariant()}
}
