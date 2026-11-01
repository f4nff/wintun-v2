# Hosted guarded-memory tests only; never installs/loads a driver or stages a package.
$safetyDir=Join-Path $outDir 'safety'
$safetyObjects=Join-Path $safetyDir 'obj'
Start-TestRecording $safetyDir 'safety-results.json'
New-Item -ItemType Directory -Path $safetyDir,$safetyObjects -Force|Out-Null
Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include","/fo$safetyObjects\version.res")+$script:VersionArguments+@('tools\build\artifact.rc'))
$flags=@('/nologo','/std:c17','/MT','/O2','/Zi','/W4','/WX',"/Fo$safetyObjects\","/Fd$safetyObjects\safety.pdb")+$script:VersionArguments
Invoke-BuildTool 'cl.exe' ($flags+@('/LD','tests\safety\legacy_cleanup_fixture.c','/link',"/out:$safetyDir\legacy-cleanup-fixture.dll","/implib:$safetyObjects\legacy-cleanup-fixture.lib"))
Invoke-BuildTool 'cl.exe' ($flags+@('tests\safety\legacy_cleanup_tests.c','/link',"/out:$safetyDir\legacy-cleanup-tests.exe"))
Invoke-TestTool "$safetyDir\legacy-cleanup-tests.exe" @("$safetyDir\legacy-cleanup-fixture.dll")
$legacyFlags=@($flags | Where-Object { $_ -ne '/std:c17' })+@('/DMAYBE_WOW64','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/wd4100','/wd4201')
Invoke-BuildTool 'cl.exe' ($legacyFlags+@('/LD','tests\safety\legacy_operation_fixture.c','/link',"/out:$safetyDir\legacy-operation-fixture.dll","/implib:$safetyObjects\legacy-operation-fixture.lib",'ntdll.lib','advapi32.lib','delayimp.lib','/DELAY:UNLOAD','/DELAYLOAD:advapi32.dll'))
Invoke-BuildTool 'cl.exe' ($flags+@('tests\safety\legacy_operation_tests.c','/link',"/out:$safetyDir\legacy-operation-tests.exe"))
Invoke-TestTool "$safetyDir\legacy-operation-tests.exe" @("$safetyDir\legacy-operation-fixture.dll")
Invoke-BuildTool 'cl.exe' ($legacyFlags+@('/LD','tests\safety\legacy_runtime_fixture.c','api\logger.c','/link',"/out:$safetyDir\legacy-runtime-fixture.dll","/implib:$safetyObjects\legacy-runtime-fixture.lib",'ntdll.lib','advapi32.lib','setupapi.lib','shlwapi.lib','shell32.lib','delayimp.lib','/DELAY:UNLOAD','/DELAYLOAD:advapi32.dll','/DELAYLOAD:setupapi.dll','/DELAYLOAD:shlwapi.dll','/DELAYLOAD:shell32.dll'))
Invoke-BuildTool 'cl.exe' ($flags+@('tests\safety\legacy_runtime_tests.c','/link',"/out:$safetyDir\legacy-runtime-tests.exe"))
Invoke-TestTool "$safetyDir\legacy-runtime-tests.exe" @("$safetyDir\legacy-runtime-fixture.dll")
$cases=@(
    @{name='packet-bounds';sources=@('common\packet.c','tests\safety\packet_bounds_tests.c');defines=@();libs=@()},
    @{name='checksum';sources=@('common\packet.c','tests\model\checksum_tests.c');defines=@();libs=@()},
    @{name='queue-order';sources=@('tests\model\queue_order_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='queue-pop';sources=@('tests\model\queue_pop_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='queue-ring';sources=@('tests\model\queue_ring_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='exchange';sources=@('common\v2_core.c','tests\model\exchange_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='default-config';sources=@('common\v2_core.c','tests\model\default_config_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='token-queue';sources=@('tests\model\token_queue_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='token-publish';sources=@('tests\model\token_publish_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='reference-publication';sources=@('tests\model\receive_reference_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='fault-query';sources=@('common\v2_core.c','common\ethernet.c','common\flow.c','common\packet.c','tests\model\fault_query_tests.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@()},
    @{name='pool-index';sources=@('tests\model\pool_index_tests.c');defines=@();libs=@()},
    @{name='token-validation';sources=@('tests\model\token_validation_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='completion-progress';sources=@('common\v2_core.c','tests\model\completion_progress_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='packet-request';sources=@('common\packet.c','tests\safety\packet_request_tests.c');defines=@();libs=@()},
    @{name='guard-fuzz';sources=@('common\v2_core.c','common\packet.c','common\coalesce.c','common\ethernet.c','tests\safety\guard_fuzz.c');defines=@();libs=@()},
    @{name='guard-nbl';sources=@('common\v2_core.c','common\ethernet.c','tests\model\receive_nbl_tests.c');defines=@('/DPB_GUARD_TESTS=1');libs=@()},
    @{name='guard-return-batch';sources=@('common\v2_core.c','common\ethernet.c','tests\model\return_batch_tests.c');defines=@('/DPB_GUARD_TESTS=1');libs=@()},
    @{name='guard-return-reclaim';sources=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\return_reclaim_tests.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@()},
    @{name='guard-pause-wake';sources=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\pause_wake_tests.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@()},
    @{name='guard-coalesce-state';sources=@('common\v2_core.c','common\coalesce.c','tests\model\coalesce_state_tests.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@()},
    @{name='guard-return-coalesce';sources=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\return_coalesce_tests.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@()},
    @{name='guard-return-diagnostics';sources=@('common\v2_core.c','common\ethernet.c','tests\model\return_diagnostics_tests.c');defines=@('/DPB_GUARD_TESTS=1');libs=@()},
    @{name='guard-drain-boundary';sources=@('common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\drain_boundary_tests.c');defines=@('/DPB_GUARD_TESTS=1');libs=@()},
    @{name='guard-coalesce-path';sources=@('common\v2_core.c','common\coalesce.c','tests\model\coalesce_path_tests.c');defines=@();libs=@()},
    @{name='guard-describe-checksum';sources=@('common\packet.c','tests\model\describe_checksum_tests.c');defines=@();libs=@()},
    @{name='guard-send-window';sources=@('tests\model\send_window_tests.c');defines=@();libs=@()},
    @{name='guard-send-diagnostics';sources=@('tests\model\send_diagnostics_tests.c');defines=@();libs=@()},
    @{name='guard-fault-drain';sources=@('common\v2_core.c','common\ethernet.c','tests\model\fault_drain_tests.c');defines=@('/DPB_GUARD_TESTS=1');libs=@()},
    @{name='dll-allocation';sources=@('tests\safety\dll_allocation_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@();libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='completion-retire';sources=@('tests\model\completion_retire_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='ownership-counter';sources=@('tests\model\ownership_counter_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='close-wake';sources=@('tests\model\close_wake_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c','common\ethernet.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='close-retry';sources=@('tests\model\close_retry_tests.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='query-error';sources=@('tests\model\query_error_tests.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='policy-wait';sources=@('tests\model\policy_wait_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='sync-policy';sources=@('tests\model\sync_policy_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='drain-wait';sources=@('tests\model\drain_wait_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='close-completion';sources=@('tests\model\close_completion_budget_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='dll-admission';sources=@('tests\safety\dll_admission_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@();libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='adapter-callback';sources=@('tests\safety\adapter_callback_tests.c');defines=@();libs=@()}
    @{name='driver-recovery';sources=@('tests\model\driver_recovery_tests.c');defines=@();libs=@('setupapi.lib')}
)
foreach($case in $cases) {
    $exe=Join-Path $safetyDir "pb$($case.name)-tests.exe"
    Invoke-BuildTool 'cl.exe' ($flags+$case.defines+$case.sources+@('/link',"/out:$exe","$safetyObjects\version.res")+$case.libs)
    Invoke-TestTool $exe @()
}
Write-TestRecording 'passed'
Write-ArtifactManifest $safetyDir 'guarded-hosted-safety-tests' @{kernel_runtime_tested=$false}
