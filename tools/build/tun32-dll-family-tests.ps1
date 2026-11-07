# Separate family executables retain the original legacy/v2 queue test host.
function Build-Tun32DllFamilyTests {
    param([Parameter(Mandatory)][string]$Directory,[Parameter(Mandatory)][ValidateSet(4,6)][int]$IpVersion,
        [ValidateSet('iso','ms')][string]$Mode='iso',[switch]$Guarded)
    if(-not [IO.Path]::IsPathFullyQualified($Directory)){throw 'TUN32 DLL family output must be absolute'}
    if($script:SourceFingerprint -cnotmatch '^[a-f0-9]{64}$'){throw 'TUN32 DLL family requires shipping fingerprint'}
    New-Item -ItemType Directory -Path $Directory -Force|Out-Null
    $version=Join-Path $script:RepoRoot 'include\packetbridge_version.h';$test=Join-Path $script:RepoRoot 'tests\model\compat_session_tests.c'
    $lines=@(('#include "'+$version.Replace('\','/')+'"'),
        '_Static_assert(sizeof(PB_SOURCE_STRING)==65,"TUN32 DLL family requires shipping fingerprint");',
        '_Static_assert(PB_VERSION_MAJOR==2&&PB_VERSION_MINOR==0&&PB_VERSION_PATCH==0,"Version remains2.0.0");',
        '#if PB_TEST_IP_VERSION != 4 && PB_TEST_IP_VERSION != 6','#error Select an independent IP family','#endif',
        '#define main Tun32FamilyHostMain',('#include "'+$test.Replace('\','/')+'"'),'#undef main',
        'int main(int argc,char **argv){printf("FAMILY: IPv%d\n",PB_TEST_IP_VERSION);return Tun32FamilyHostMain(argc,argv);}')
    $entry=Join-Path $Directory 'entry.c';[IO.File]::WriteAllText($entry,($lines -join "`n")+"`n",[Text.UTF8Encoding]::new($false))
    $flags=@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DNDEBUG','/DPB_API_TESTS=1',"/DPB_TEST_IP_VERSION=$IpVersion","/volatile:$Mode","/Fo$Directory\")+
        @($script:VersionArguments|Where-Object {$_ -notlike '/DPB_SOURCE_FINGERPRINT=*'})+"/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
    if($Guarded){$flags+='/DPB_GUARD_TESTS=1'}
    $sources=@('api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
    $exe=Join-Path $Directory "tun32-dll-ipv$IpVersion.exe"
    Invoke-BuildTool cl.exe ($flags+@($entry)+$sources+@('/link',"/out:$exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))|Out-Host
    $paths=@(rg --files api common include);if($LASTEXITCODE -ne 0){throw 'Cannot enumerate DLL family dependencies'}
    $paths+=@('tests\model\compat_session_tests.c','tests\model\compat_tun32_cases.h','tests\model\compat_terminal32_cases.h','tests\safety\guard_memory.h')
    $recorded=@($paths|Sort-Object -Unique|ForEach-Object {$path=Join-Path $script:RepoRoot $_;@{relative=$_;path=$path;sha256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant()}})
    return [ordered]@{path=$exe;sha256=(Get-FileHash $exe).Hash.ToLowerInvariant();ip_version=$IpVersion;mode=$Mode;guarded=[bool]$Guarded;flags=$flags;
        entry=$entry;entry_sha256=(Get-FileHash $entry).Hash.ToLowerInvariant();sources=$recorded;
        method='Actual unchanged legacy session/worker plus actual v2 queue/pool methods through the existing PB_API_TESTS native peer. New entry only checks65-byte fingerprint/version, renames the test main and reports the selected family. NDEBUG and SDK-default historical adapter link dependency, not a Windows7 runtime claim. Guard only caller expected payload buffers; native/legacy allocations retain original allocators.'}
}
