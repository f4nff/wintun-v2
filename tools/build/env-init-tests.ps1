# Small fragment builder. Caller initializes common.ps1 and records executions.
function Build-EnvInitTests {
    param([Parameter(Mandatory)][string]$Directory,[ValidateSet('iso','ms')][string]$Mode='iso',[switch]$MaybeWow64)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'EnvInit output must be absolute'}
    if($script:SourceFingerprint -notmatch '^[a-f0-9]{64}$'){throw 'EnvInit requires shipping source fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'api\main.c'
    $accepted='543ea0dbbed261c5b685be0313d37f317c724f73114fcb448a41023a131d308f'
    if((Get-FileHash $source).Hash.ToLowerInvariant() -ne $accepted){throw 'Accepted whole main.c changed; review EnvInit binding'}
    $body=[IO.File]::ReadAllText($source);$first='static void EnvInit(VOID)';$next='static INIT_ONCE RuntimeOnce = INIT_ONCE_STATIC_INIT;'
    foreach($marker in @($first,$next)){if([regex]::Matches($body,[regex]::Escape($marker)).Count -ne 1){throw 'EnvInit fragment marker is not unique'}}
    $offset=$body.IndexOf($first);$end=$body.IndexOf($next,$offset);if($end -le $offset){throw 'EnvInit fragment is reversed'}
    $fragment=$body.Substring($offset,$end-$offset);$encoding=[Text.UTF8Encoding]::new($false)
    $selected=Join-Path $Directory 'env-init-actual.h';[IO.File]::WriteAllText($selected,$fragment,$encoding)
    if([IO.File]::ReadAllText($selected) -cne $body.Substring($offset,$end-$offset)){throw 'EnvInit extraction changed'}
    $test=Join-Path $script:RepoRoot 'tests\model\env_init_tests.c';$entry=Join-Path $Directory 'entry.c'
    $lines=@('#define PB_ENV_INIT_SOURCE "'+$selected.Replace('\','/')+'"')
    $lines+='#include "'+$test.Replace('\','/')+'"'
    [IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',"/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($MaybeWow64){$flags+='/DMAYBE_WOW64=1'}
    $exe=Join-Path $Directory 'env-init.exe';Invoke-BuildTool cl.exe ($flags+@($entry,'/link',"/out:$exe"))|Out-Host
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();mode=$Mode;maybe_wow64=[bool]$MaybeWow64;ndebug=$true;pb_api_tests=$false;
        extraction=@{source=$source;source_sha256=$accepted;first=$first;next=$next;offset=$offset;length=$end-$offset;path=$selected;sha256=(Get-FileHash $selected).Hash.ToLowerInvariant()};
        entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();test=$test;test_sha256=(Get-FileHash $test).Hash.ToLowerInvariant();arguments=$flags;
        method='Verbatim EnvInit span from accepted complete main.c, with exact markers/offset/length and source/output hashes; no production edit. Actual version/capability control flow uses SDK-boundary tables, real borrowed kernel32 handle and current-process pseudo-handle. API call counts are branch evidence, not oldOS runtime/module-refcount/leak proof. No Load/FreeLibrary, heap, thread or owned handle is created. Native GetProcessHandleCount remains equal. Default no-MAYBE branch has7 version cases with the actual x64 process-machine constant; retained MAYBE_WOW64 source contract adds12 capability/failure cases and models IMAGE_FILE_PROCESS as I386 so same-process and WOW-AMD64 fallback assignments differ. This does not claim an x86 build or ABI test. Missing kernel is defensive and architecture results are explicit models, not real host/process migration. Current shipping x64 API pipeline does not define MAYBE_WOW64. NDEBUG/FP65 and PB_API_TESTS rejection remain enforced.'}
}
