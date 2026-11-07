# Build helper only: caller initializes common.ps1 and owns execution recording.
function Build-RetainedOwnerTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-diagnostic-pin')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Retained owner output directory must be absolute'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $encoding=[Text.UTF8Encoding]::new($false);$apiDirectory=Join-Path $script:RepoRoot 'api\v2'
    $originalSession=Join-Path $apiDirectory 'session.c';$originalLife=Join-Path $apiDirectory 'session_lifecycle.h'
    $lifecycle=$originalLife;$mutation=$null
    if($Variant -eq 'without-diagnostic-pin') {
        $text=[IO.File]::ReadAllText($originalLife).Replace("`r`n","`n")
        $pattern='(?s)static PB_RESULT WINAPI GetRetained\(uint32_t index, PB_CLOSE_PROGRESS \*out\).*?(?=static void CALLBACK LifecycleWorker\()'
        $matches=[regex]::Matches($text,$pattern);if($matches.Count -ne 1){throw 'Diagnostic helper is not unique'}
        $original=$matches[0].Value
        $retain='    if (s) Retain(s);'
        $drop='PB_RESULT result = GetCloseProgress(s, out); DropReference(s); return result;'
        foreach($part in @($retain,$drop)){if([regex]::Matches($original,[regex]::Escape($part)).Count -ne 1){throw 'Diagnostic ownership pair is not unique'}}
        $changed=$original.Replace($retain,'    /* Observer control: diagnostic Retain/Drop ownership pair omitted. */').Replace($drop,'PB_RESULT result = GetCloseProgress(s, out); return result;')
        $lifecycle=Join-Path $Directory 'lifecycle-without-diagnostic-pin.h'
        [IO.File]::WriteAllText($lifecycle,$text.Replace($original,$changed),$encoding)
        $mutation=[ordered]@{function='GetRetained';removed=@('if (s) Retain(s);','DropReference(s);');
            original_function_sha256=Get-RetainedTextHash $original;mutated_function_sha256=Get-RetainedTextHash $changed;
            teardown='Observe refs2 with public+registry alive; release and join unpinned diagnostic before public release; no repair or unpaired Drop.'}
    }
    $text=[IO.File]::ReadAllText($originalSession)
    $generated=[regex]::Replace($text,'(?m)^#include "([^"]+)"',{
        param($match)
        $include=if($match.Groups[1].Value -eq 'session_lifecycle.h'){$lifecycle}else{[IO.Path]::GetFullPath((Join-Path $apiDirectory $match.Groups[1].Value))}
        '#include "'+$include.Replace('\','/')+'"'
    })
    $session=Join-Path $Directory 'session.c';[IO.File]::WriteAllText($session,$generated,$encoding)
    # The original fixture functions were renamed before its inclusion. Only
    # actual session expansion uses these wrappers; the base ledger stays intact.
    $entry=Join-Path $Directory 'session-entry.h'
    [IO.File]::WriteAllText($entry,@'
#undef TrackedFree
#undef TrackedPoolClose
#undef TrackedUnpin
#define TrackedFree RetainedFree
#define TrackedPoolClose RetainedPoolClose
#define TrackedUnpin RetainedUnpin
#define AcquireSRWLockShared RetainedShared
#include PB_RETAINED_SESSION_SOURCE
#undef AcquireSRWLockShared
'@,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    $lines+='#define PB_RETAINED_SESSION_SOURCE "'+$session.Replace('\','/')+'"'
    $lines+='#define PB_NATIVE_POLICY_SESSION_SOURCE "'+$entry.Replace('\','/')+'"'
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\safety\retained_owner_fixture.c').Replace('\','/')+'"'
    $source=Join-Path $Directory 'fixture.c';[IO.File]::WriteAllText($source,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2',"/volatile:$Mode",'/W4','/WX','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000')+$script:VersionArguments
    $program=Join-Path $Directory 'retained-owner.exe'
    Invoke-BuildTool cl.exe ($flags+@("/Fo$Directory\",'tests\model\retained_owner_tests.c','/link',"/out:$program")) | Out-Host
    $dll=Join-Path $Directory "retained-owner-$Variant.dll"
    Invoke-BuildTool cl.exe ($flags+@('/LD',"/Fo$Directory\",$source,'common\v2_core.c','common\packet.c','common\flow.c','/link',"/out:$dll",("/implib:"+(Join-Path $Directory "retained-owner-$Variant.lib")),
        '/EXPORT:PbGetInterfaceV2','/EXPORT:PbGetLifecycleInterface','bcrypt.lib')) | Out-Host
    $bound=@($originalSession,$originalLife,$lifecycle,$session,$entry,$source,
        (Join-Path $script:RepoRoot 'tests\safety\native_policy_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\native_policy_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\safety\retained_owner_fixture.c'),(Join-Path $script:RepoRoot 'tests\model\retained_owner_fixture.h'),
        (Join-Path $script:RepoRoot 'tests\model\retained_owner_tests.c'))|Select-Object -Unique|ForEach-Object {
            [ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$program;sha256=(Get-FileHash -LiteralPath $program).Hash.ToLowerInvariant();fixture=$dll;fixture_sha256=(Get-FileHash -LiteralPath $dll).Hash.ToLowerInvariant();
        volatile=$Mode;guarded=[bool]$Guarded;variant=$Variant;session=$session;lifecycle=$lifecycle;entry=$entry;shim=$source;sources=@($bound);mutation=$mutation}
}
function Get-RetainedTextHash([string]$Text) {
    $hash=[Security.Cryptography.SHA256]::Create()
    try{return [Convert]::ToHexString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes($Text))).ToLowerInvariant()}finally{$hash.Dispose()}
}
