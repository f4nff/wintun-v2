# Build helper only. Reuse unchanged send/indication/NBL hosts in ignored output.
. (Join-Path $PSScriptRoot 'send-completion-tests.ps1')
function Build-ControlReplyTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded,
        [ValidateSet('current','without-failed-nbl-mdl-release')][string]$Variant='current')
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Control reply output must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'Control reply requires shipping source fingerprint'}
    $encoding=[Text.UTF8Encoding]::new($false);$base=Write-SendCompletionParts -Directory $Directory -Variant current
    $bankBefore='CHECK(!Session->Active&&Session->Returns.References==1&&SetEvent(BankWait));'
    $bankAfter='CHECK(SetEvent(BankWait));'
    $indication=[IO.File]::ReadAllText($base.reused)
    if([regex]::Matches($indication,[regex]::Escape($bankBefore)).Count -ne 1){throw 'Bank wait observer not unique'}
    $indicationHost=Join-Path $Directory 'control-indication-host.h'
    [IO.File]::WriteAllText($indicationHost,$indication.Replace($bankBefore,$bankAfter),$encoding)
    $hostSource=Join-Path $script:RepoRoot 'tests\model\send_completion_tests.c'
    $hostHash='cb019555263616c217a0f84d415808af30004266ef1fe3dfb1d28c82b4dc2938'
    if((Get-FileHash $hostSource).Hash.ToLowerInvariant() -ne $hostHash){throw 'Send completion host changed; review finite reuse'}
    $hostOriginal=Join-Path $Directory 'completion-host-original.c';[IO.File]::WriteAllBytes($hostOriginal,[IO.File]::ReadAllBytes($hostSource))
    $body=[IO.File]::ReadAllText($hostSource);$mainBefore='int main(int argc,char **argv)';$mainAfter='int ReusedCompletionMain(int argc,char **argv)'
    if([regex]::Matches($body,[regex]::Escape($mainBefore)).Count -ne 1){throw 'Reused main marker not unique'}
    $body=$body.Replace($mainBefore,$mainAfter);$includeChanges=[Collections.Generic.List[object]]::new()
    $body=[regex]::Replace($body,'(?m)^#include "([^"]+)"',{
        param($match);$path=[IO.Path]::GetFullPath((Join-Path (Split-Path $hostSource) $match.Groups[1].Value))
        $adapted='#include "'+$path.Replace('\','/')+'"';$includeChanges.Add([ordered]@{original=$match.Value;adapted=$adapted});$adapted
    })
    $completionHost=Join-Path $Directory 'control-completion-host.h';[IO.File]::WriteAllText($completionHost,$body,$encoding)
    $source=Join-Path $script:RepoRoot 'driver\v2\session.h';$text=[IO.File]::ReadAllText($source)
    $startMarker='static NDIS_STATUS PbV2ControlReply(';$endMarker='#include "session_describe.h"'
    if([regex]::Matches($text,[regex]::Escape($startMarker)).Count -ne 1 -or [regex]::Matches($text,[regex]::Escape($endMarker)).Count -ne 1){throw 'ControlReply source markers not unique'}
    $start=$text.IndexOf($startMarker);$end=$text.IndexOf($endMarker,$start);if($end -le $start){throw 'ControlReply boundaries reversed'}
    $original=$text.Substring($start,$end-$start);$originalPath=Join-Path $Directory 'reply-original.h';[IO.File]::WriteAllText($originalPath,$original,$encoding)
    $before='    IoFreeMdl(mdl); ExFreePoolWithTag(data, TUN_MEMORY_TAG);'
    $after='    if (nbl) IoFreeMdl(mdl); ExFreePoolWithTag(data, TUN_MEMORY_TAG);'
    if([regex]::Matches($original,[regex]::Escape($before)).Count -ne 1){throw 'Reply final MDL release not unique'}
    $selected=$original;$mutation=$null
    if($Variant -eq 'without-failed-nbl-mdl-release'){
        $selected=$selected.Replace($before,$after);$mutation=[ordered]@{original=$before;changed=$after;offset=$original.IndexOf($before);length=$before.Length}
    }
    $reply=Join-Path $Directory 'reply-selected.h';[IO.File]::WriteAllText($reply,$selected,$encoding)
    $calloutsSource=Join-Path $script:RepoRoot 'driver\v2\ndis_callouts.h';$callouts=Join-Path $Directory 'callouts-extracted.h'
    [IO.File]::WriteAllBytes($callouts,[IO.File]::ReadAllBytes($calloutsSource))
    $send=Join-Path $Directory 'send-selected.h';$sendSource=Join-Path $script:RepoRoot 'driver\v2\session_send.h'
    $declarations=@'
static BOOLEAN ActualControlRead(NET_BUFFER *,ULONG,UCHAR *,ULONG);
static NDIS_STATUS ActualControlReply(TUN_CTX *,const UCHAR *,ULONG);
static VOID ActualControlComplete(NDIS_HANDLE,NET_BUFFER_LIST *);
static VOID ControlReject(TUN_CTX *,uint64_t,NET_BUFFER_LIST *,NET_BUFFER *,NDIS_STATUS,ULONG,ULONG,ULONG,const PB_SEND_FAILURE *,ULONG,ULONG,ULONG,PB_KERNEL_QUEUE *,ULONG);
#define PbV2Read ActualControlRead
#define PbV2ControlReply ActualControlReply
#define PbCompleteSend ActualControlComplete
#define PbV2RejectSend ControlReject
'@
    $suffix=@'
#undef PbV2Read
#undef PbV2ControlReply
#undef PbCompleteSend
#undef PbV2RejectSend
'@
    [IO.File]::WriteAllText($send,$declarations+"`r`n#include `""+$sendSource.Replace('\','/')+"`"`r`n"+$suffix,$encoding)
    $lines=@();if($Guarded){$lines+='#define PB_GUARD_TESTS 1'}
    if($Variant -ne 'current'){$lines+='#define PB_CONTROL_WITHOUT_FAILED_MDL 1'}
    $lines+='#define PB_CONTROL_REUSED_HOST "'+$completionHost.Replace('\','/')+'"'
    $lines+='#define PB_CONTROL_REPLY_SOURCE "'+$reply.Replace('\','/')+'"'
    $lines+='#define PB_CONTROL_CALLOUTS_SOURCE "'+$callouts.Replace('\','/')+'"'
    $lines+='#define PB_SEND_COMPLETION_HOST "'+$indicationHost.Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_NBL_HOST "'+$base.host.Replace('\','/')+'"'
    $lines+='#define PB_REFERENCE_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_references.h').Replace('\','/')+'"'
    $lines+='#define PB_NBL_SOURCE "'+(Join-Path $script:RepoRoot 'driver\v2\session_nbl.h').Replace('\','/')+'"'
    $lines+='#define PB_INDICATION_PAUSE_SOURCE "'+$base.base.pause.Replace('\','/')+'"'
    foreach($part in $base.base.parts|Where-Object name -ne 'pause'){$lines+='#define PB_INDICATION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$part.path.Replace('\','/')+'"'}
    foreach($part in $base.parts|Where-Object name -ne 'cleanup'){$path=if($part.name -eq 'send'){$send}else{$part.path};$lines+='#define PB_SEND_COMPLETION_'+$part.name.ToUpperInvariant()+'_SOURCE "'+$path.Replace('\','/')+'"'}
    $lines+='#include "'+(Join-Path $script:RepoRoot 'tests\model\control_reply_tests.c').Replace('\','/')+'"'
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`r`n")+"`r`n",$encoding)
    $exe=Join-Path $Directory 'control-reply.exe'
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode",('/I'+(Join-Path $script:RepoRoot 'driver\v2')),"/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    Invoke-BuildTool cl.exe ($flags+@($entry,'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','common\flow.c','/link',"/out:$exe"))|Out-Host
    $sources=@($source,$hostSource,$hostOriginal,$completionHost,$base.reused,$indicationHost,$sendSource,$send,$originalPath,$reply,$calloutsSource,$callouts,$entry,
        (Join-Path $script:RepoRoot 'tests\model\control_reply_tests.c'))|Select-Object -Unique|ForEach-Object {[ordered]@{path=$_;sha256=(Get-FileHash -LiteralPath $_).Hash.ToLowerInvariant()}}
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();mode=$Mode;guarded=[bool]$Guarded;variant=$Variant;
        entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();sources=@($sources);
        extractions=[ordered]@{base=$base;host_source=$hostSource;host_source_sha256=$hostHash;host_original=$hostOriginal;host_original_sha256=(Get-FileHash $hostOriginal).Hash.ToLowerInvariant();
            host=$completionHost;host_sha256=(Get-FileHash $completionHost).Hash.ToLowerInvariant();include_changes=@($includeChanges);main_original=$mainBefore;main_adapted=$mainAfter;
            indication_host=$indicationHost;indication_sha256=(Get-FileHash $indicationHost).Hash.ToLowerInvariant();bank_original=$bankBefore;bank_adapted=$bankAfter;send_wrapper=$send;
            parts=@(@{name='reply';source=$source;source_sha256=(Get-FileHash $source).Hash.ToLowerInvariant();offset=$start;length=$original.Length;start_marker=$startMarker;end_marker=$endMarker;
                    path=$originalPath;sha256=(Get-FileHash $originalPath).Hash.ToLowerInvariant()},
                @{name='callouts';source=$calloutsSource;source_sha256=(Get-FileHash $calloutsSource).Hash.ToLowerInvariant();offset=0;length=[IO.File]::ReadAllText($calloutsSource).Length;start_marker='[entire header]';end_marker='[end of source]';path=$callouts;sha256=(Get-FileHash $callouts).Hash.ToLowerInvariant()});
            reply=$reply;reply_sha256=(Get-FileHash $reply).Hash.ToLowerInvariant();mutation=$mutation;
            adaptation='Reuse the frozen complete send/indication/NBL hosts: prior finite fields and observers unchanged; a new copy relaxes only the old receive-specific Active0/Returns1 assertion at the genuine bank wait, and this test independently checks Send Active1/Returns2. Completion host changes only unused main token and absolute quoted includes. Selected Send declares/binds four functions then includes its exact production header; actual Read/ControlReply/NDIS wrappers are separately included by this test. ControlReply changes only the selected single negative MDL release guard. All actual Free/barriers remain intact.'}}
}
