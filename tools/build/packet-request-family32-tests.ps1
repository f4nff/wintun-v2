# Whole shared C source; only its explicit memcpy calls are observed by the host.
function Build-PacketRequestFamily32Tests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'Packet request family output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'Packet request family requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $source=Join-Path $script:RepoRoot 'common\packet.c';$accepted='bf3731c3cc5712d7b2cd409777302971118ca99e4fea00047d9583263a1e37c1'
    if((Get-FileHash -LiteralPath $source).Hash.ToLowerInvariant() -cne $accepted){throw 'Shared packet engine changed; review maximum request coverage'}
    $raw=Join-Path $Directory 'packet-actual.c';[IO.File]::WriteAllBytes($raw,[IO.File]::ReadAllBytes($source))
    $encoding=[Text.UTF8Encoding]::new($false);$header=Join-Path $Directory 'packet.h';$productionHeader=Join-Path $script:RepoRoot 'common\packet.h'
    [IO.File]::WriteAllText($header,'#include "'+$productionHeader.Replace('\','/')+'"'+"`n",$encoding)
    $test=Join-Path $script:RepoRoot 'tests\safety\packet_request_family32_tests.c';$entry=Join-Path $Directory 'entry.c'
    $lines=@(('#define PB_REQUEST_PACKET_SOURCE "'+$raw.Replace('\','/')+'"'),('#include "'+$test.Replace('\','/')+'"'))
    [IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",$encoding)
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DWINVER=0x0601','/DNTDDI_VERSION=0x06010000',
        "/DPB_TEST_IP_VERSION=$IpVersion","/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Guarded){$flags+='/DPB_GUARD_TESTS=1'}
    $exe=Join-Path $Directory "packet-request-ipv$IpVersion.exe"
    Invoke-BuildTool cl.exe ($flags+@($entry,'/link',"/out:$exe"))|Out-Host
    $paths=@('common\packet.c','common\packet.h','include\packetbridge_types.h','include\packetbridge_version.h','tests\safety\guard_memory.h','tests\safety\packet_request_family32_tests.c')
    $sources=@($paths|ForEach-Object {$path=Join-Path $script:RepoRoot $_;@{relative=$_;path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}})
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;ndebug=$true;pb_api_tests=$false;flags=$flags;
        source=$source;accepted_sha256=$accepted;raw=$raw;raw_sha256=(Get-FileHash $raw).Hash.ToLowerInvariant();header=$header;header_sha256=(Get-FileHash $header).Hash.ToLowerInvariant();
        entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();sources=$sources;
        method='Exact whole common/packet.c raw copy, with an out-only packet.h forwarding include; no C body projection or mutation. Test includes it in a narrow memcpy macro scope. Only explicit production metadata/header/payload memcpy call sites are counted; compiler lowering, test setup/oracle copies and CPU performance are outside that counter. Each family constructs TCP/UDP32 segments and six request shapes; successful aggregate payload bytes are copied exactly once. Guarded mode protects actual caller source, outputs, Plan, output metadata and written storage; SDK/CRT internals and production stack request are not guarded. No native device, driver, registry, session, module, thread or timer is created. All five caller allocations are freed after every case; native process handle equality covers warmed process scope. NDEBUG/FP65 mandatory and PB_API_TESTS forbidden; version2.0.0.'}
}
