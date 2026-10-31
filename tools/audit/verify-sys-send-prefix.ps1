param([string]$BaselineDirectory='out\sys-send-prefix-baseline\20cca9f9cce1')
# Actual send, MDL reader, Ethernet control and queue selection; no SYS execution.
$ErrorActionPreference='Stop'
. (Join-Path $PSScriptRoot '..\build\common.ps1')
trap {if($script:TestResultFile) {Write-TestRecording 'failed'};throw}
Initialize-BuildEnvironment
$script:SourceFingerprint=Get-SourceFingerprint
$baseline=(Resolve-Path -LiteralPath $BaselineDirectory).Path
$directory=Join-Path $script:RepoRoot "out\sys-send-prefix\$($script:SourceFingerprint.Substring(0,12))"
New-Item -ItemType Directory -Path $directory -Force | Out-Null
Start-TestRecording $directory 'operation-results.json'
$executables=@()
foreach($variant in @('before','after')) {
    foreach($volatile in @('iso','ms')) {
        $objects=Join-Path $directory "$variant-$volatile";New-Item -ItemType Directory -Path $objects -Force | Out-Null
        $lines=@()
        if($variant -eq 'before') {
            $lines+='#define PB_SEND_PREFIX_BASELINE 1'
            $lines+='#define PB_SEND_SOURCE "'+(Join-Path $baseline 'driver\v2\session_send.h').Replace('\','/')+'"'
        }
        $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\send_window_tests.c').Replace('\','/')+'"'
        $source=Join-Path $objects 'fixture.c';$lines | Set-Content -LiteralPath $source -Encoding utf8
        $exe=Join-Path $objects 'fixture.exe'
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2',"/volatile:$volatile",'/W4','/WX',"/Fo$objects\")+$script:VersionArguments+@($source,'/link',"/out:$exe"))
        $testArguments=if($variant -eq 'before') {@('--historical-send')}else{@()}
        Invoke-TestTool $exe $testArguments
        $executables+=@{variant=$variant;volatile=$volatile;path=$exe;sha256=(Get-FileHash -LiteralPath $exe).Hash.ToLowerInvariant()}
    }
}
# Reader extraction must preserve its production body exactly.
$old=[IO.File]::ReadAllText((Join-Path $baseline 'driver\v2\session.h')).Replace("`r`n","`n")
$start=$old.IndexOf('static BOOLEAN PbV2Read(');$end=$old.IndexOf('/* RESOURCES indications',$start)
$current=[IO.File]::ReadAllText((Join-Path $script:RepoRoot 'driver\v2\session_read.h')).Replace("`r`n","`n")
$now=$current.Substring($current.IndexOf('static BOOLEAN PbV2Read(')).Trim()
if($start -lt 0 -or $end -le $start -or $old.Substring($start,$end-$start).Trim() -ne $now){throw 'MDL reader body changed during extraction'}
if((Get-SourceFingerprint) -ne $script:SourceFingerprint){throw 'Sources changed during send-prefix checks'}
Write-TestRecording 'passed'
[ordered]@{source_tree_sha256=$script:SourceFingerprint;baseline_directory=$baseline;
    baseline_send_sha256=(Get-FileHash -LiteralPath (Join-Path $baseline 'driver\v2\session_send.h')).Hash.ToLowerInvariant();current_send_sha256=(Get-FileHash 'driver\v2\session_send.h').Hash.ToLowerInvariant();
    reader_body_unchanged=$true;
    historical_scope='Frozen Send uses original budgets, ownership, fault/callback and prefix contracts; current variant also runs later diagnostics and persistent-pool/late-return recovery. Frozen body remains unchanged.';
    method='Frozen/current actual Send; same actual MDL reader, Ethernet Inspect/Control and SelectQueue; both volatile modes. Independent guarded read-only MDLs, nonzero current offset/empty node, exact full payload copy by Describe stand-in, address-hash queue, ordinary prefix byte/map counts, short boundaries, control reply/drop, failed mappings/rollback, 2412 guarded reader spans and handle balance. Existing send lock/byte/attempt budgets, recycle faults, unlocked reentrant callback and completion-tail tests also run. Operation counts only; not kernel CPU/latency/throughput.';
    ordinary_prefix_bytes_before=128;ordinary_prefix_bytes_after=59;
    fragmented_example_prefix_maps_before=3;fragmented_example_prefix_maps_after=2;
    tradeoff='Control candidates need a second bounded read for the remaining original 128-byte maximum; total control bytes stay equal, but traversal/mapping work can increase. No reader cursor or mapping is cached across calls.';
    kernel_runtime_tested=$false;operation_record=$script:TestResultFile;executables=$executables} | ConvertTo-Json -Depth 8 |
    Set-Content -LiteralPath (Join-Path $directory 'evidence.json') -Encoding utf8
Write-Output (Join-Path $directory 'evidence.json')
