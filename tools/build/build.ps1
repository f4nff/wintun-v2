param([ValidateSet('Tests', 'UnifiedTests', 'Safety', 'Driver', 'Probe', 'Api', 'FinalApi', 'UnsignedTestApi')][string]$Stage = 'Tests',
    [string]$SignedDriverDirectory, [string]$ExpectedManifest, [string]$UnsignedDriverDirectory)
. (Join-Path $PSScriptRoot 'common.ps1')
trap { if ($script:TestResultFile) { Write-TestRecording 'failed' }; throw }
Initialize-BuildEnvironment
$script:SourceFingerprint = Get-SourceFingerprint
$script:VersionArguments += "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
$outDir = Join-Path $script:RepoRoot "out\packet-path-v2\$script:Version\$($script:SourceFingerprint.Substring(0,12))"
$objectDir = Join-Path $outDir 'obj'
New-Item -ItemType Directory -Path $outDir, $objectDir -Force | Out-Null

if ($Stage -eq 'Tests') {
    Start-TestRecording $outDir
    Invoke-TestTool 'pwsh.exe' @('-NoProfile','-File','tools\audit\verify-source-fingerprint.ps1','-OutputDirectory',"$outDir\source-fingerprint")
    foreach ($resource in @('setupapihost', 'example')) {
        Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo', "/fo$objectDir\$resource.res") + $script:VersionArguments + @("$resource\resources.rc"))
    }
    Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo','/DPB_FILE_TYPE=VFT_DLL',"/I$script:RepoRoot\include", "/fo$objectDir\tools.res") + $script:VersionArguments + @('tools\build\artifact.rc'))
    Invoke-BuildTool 'cl.exe' (@('/nologo','/LD','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\packet.c','common\coalesce.c','common\ethernet.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbpacket-tests.dll",'/def:tests\packet\packet.def',"/implib:$objectDir\pbpacket-tests.lib","$objectDir\tools.res"))
    Invoke-TestTool 'python.exe' @('tests\packet\test_packet.py', "$outDir\pbpacket-tests.dll")
    Invoke-TestTool 'python.exe' @('tests\packet\test_coalesce.py', "$outDir\pbpacket-tests.dll")
    Invoke-TestTool 'python.exe' @('tests\packet\test_ethernet.py', "$outDir\pbpacket-tests.dll")
    foreach($ipVersion in @(4,6)) {
        Invoke-TestTool 'python.exe' @('tests\packet\test_packet_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_route_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_segment_edge_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_coalesce_edge_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_max_header_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_coalesce_variable_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_request_alias_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_retry_revalidate_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_coalesce_limit_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
        Invoke-TestTool 'python.exe' @('tests\packet\test_tcp_zero_family32.py', "$outDir\pbpacket-tests.dll", "$ipVersion")
    }
    Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include", "/fo$objectDir\core-tests.res") + $script:VersionArguments + @('tools\build\artifact.rc'))
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\", 'common\v2_core.c','common\offload_policy.c','tests\model\core_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbcore-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbcore-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\packet.c','tests\model\checksum_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbchecksum-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbchecksum-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\pool_index_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbpool-index-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbpool-index-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'tests\model\token_validation_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbtoken-validation-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbtoken-validation-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'tests\model\queue_order_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbqueue-order-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbqueue-order-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'tests\model\queue_pop_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbqueue-pop-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbqueue-pop-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'tests\model\queue_ring_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbqueue-ring-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbqueue-ring-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','tests\model\exchange_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbexchange-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbexchange-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','tests\model\default_config_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbdefault-config-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbdefault-config-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'tests\model\token_queue_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbtoken-queue-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbtoken-queue-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'tests\model\token_publish_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbtoken-publish-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbtoken-publish-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\sys_cleanup_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbsys-cleanup-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbsys-cleanup-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\indication_gate_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbindication-gate-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbindication-gate-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\context_gate_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbcontext-gate-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbcontext-gate-tests.exe" @()
    foreach ($test in @('session_query','ndis_callout','worker_poll','receive_reference')) {
        Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\", "tests\model\${test}_tests.c") + $script:VersionArguments + @('/link',"/out:$outDir\pb${test}-tests.exe", "$objectDir\core-tests.res"))
        Invoke-TestTool "$outDir\pb${test}-tests.exe" @()
    }
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','common\flow.c','common\packet.c','tests\model\fault_query_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbfault-query-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbfault-query-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','tests\model\receive_nbl_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbreceive-nbl-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbreceive-nbl-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','tests\model\return_batch_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbreturn-batch-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbreturn-batch-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\return_reclaim_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbreturn-reclaim-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbreturn-reclaim-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\pause_wake_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbpause-wake-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbpause-wake-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\return_coalesce_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbreturn-coalesce-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbreturn-coalesce-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','tests\model\return_diagnostics_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbreturn-diagnostics-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbreturn-diagnostics-tests.exe" @()
    Invoke-TestTool 'python.exe' @('tests\model\test_return_diagnostics.py', "$outDir\pbreturn-diagnostics-tests.exe")
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\send_diagnostics_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbsend-diagnostics-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbsend-diagnostics-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\driver_recovery_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbdriver-recovery-tests.exe", "$objectDir\tools.res",'setupapi.lib'))
    Invoke-TestTool "$outDir\pbdriver-recovery-tests.exe" @()
    Invoke-TestTool 'python.exe' @('tests\model\test_send_diagnostics.py', "$outDir\pbsend-diagnostics-tests.exe")
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','tests\model\drain_boundary_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbdrain-boundary-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbdrain-boundary-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\coalesce.c','tests\model\coalesce_path_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbcoalesce-path-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbcoalesce-path-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\coalesce.c','tests\model\coalesce_state_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbcoalesce-state-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbcoalesce-state-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','common\ethernet.c','tests\model\fault_drain_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbfault-drain-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbfault-drain-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\model\send_window_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbsend-window-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbsend-window-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\packet.c','tests\model\describe_checksum_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbdescribe-checksum-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbdescribe-checksum-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','tests\model\completion_progress_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbcompletion-progress-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbcompletion-progress-tests.exe" @()
    $apiTestSources = @('api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c')
    $apiTestLibs = @('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DPB_API_TESTS=1',"/Fo$objectDir\") + $apiTestSources + @('tests\model\api_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbapi-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbapi-tests.exe" @()
    $completionSources = @('tests\model\completion_retire_tests.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $completionSources + $script:VersionArguments + @('/link',"/out:$outDir\pbcompletion-retire-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbcompletion-retire-tests.exe" @()
    $ownershipSources = @('tests\model\ownership_counter_tests.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $ownershipSources + $script:VersionArguments + @('/link',"/out:$outDir\pbownership-counter-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbownership-counter-tests.exe" @()
    $closeWakeSources = @('tests\model\close_wake_tests.c','common\ethernet.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $closeWakeSources + $script:VersionArguments + @('/link',"/out:$outDir\pbclose-wake-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbclose-wake-tests.exe" @()
    $closeRetrySources = @('tests\model\close_retry_tests.c') + @($apiTestSources | Where-Object { $_ -notin @('api\v2\session.c','api\v2\adapter.c') })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $closeRetrySources + $script:VersionArguments + @('/link',"/out:$outDir\pbclose-retry-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbclose-retry-tests.exe" @()
    $queryErrorSources = @('tests\model\query_error_tests.c') + @($apiTestSources | Where-Object { $_ -notin @('api\v2\session.c','api\v2\adapter.c') })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $queryErrorSources + $script:VersionArguments + @('/link',"/out:$outDir\pbquery-error-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbquery-error-tests.exe" @()
    $policyWaitSources = @('tests\model\policy_wait_tests.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $policyWaitSources + $script:VersionArguments + @('/link',"/out:$outDir\pbpolicy-wait-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbpolicy-wait-tests.exe" @()
    $syncPolicySources = @('tests\model\sync_policy_tests.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $syncPolicySources + $script:VersionArguments + @('/link',"/out:$outDir\pbsync-policy-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbsync-policy-tests.exe" @()
    $drainWaitSources = @('tests\model\drain_wait_tests.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $drainWaitSources + $script:VersionArguments + @('/link',"/out:$outDir\pbdrain-wait-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbdrain-wait-tests.exe" @()
    $closeCompletionSources = @('tests\model\close_completion_budget_tests.c') + @($apiTestSources | Where-Object { $_ -ne 'api\v2\session.c' })
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/volatile:iso','/W4','/WX',"/Fo$objectDir\") + $closeCompletionSources + $script:VersionArguments + @('/link',"/out:$outDir\pbclose-completion-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pbclose-completion-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX','/DPB_API_TESTS=1',"/Fo$objectDir\") + $apiTestSources + @('tests\model\lifecycle_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pblifecycle-tests.exe", "$objectDir\core-tests.res") + $apiTestLibs)
    Invoke-TestTool "$outDir\pblifecycle-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'common\v2_core.c','tests\model\session_progress_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pbsession-progress-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pbsession-progress-tests.exe" @()
    Invoke-BuildTool 'cl.exe' (@('/nologo','/c','/std:c17','/MT','/O2','/W4','/WX','/DPB_EMBEDDED_DRIVER=1',"/Fo$objectDir\driver-package-final.obj",'api\v2\driver_package.c') + $script:VersionArguments)
    # Exercise final DLL resource extraction without embedding or installing a real driver.
    $fixtureDir = Join-Path $objectDir 'resource-fixture'
    New-Item -ItemType Directory -Path $fixtureDir -Force | Out-Null
    $resourceLines = @('#include <windows.h>')
    $resourceId = 101
    foreach ($name in @('packetbridge.sys','packetbridge.inf','packetbridge.cat')) {
        [IO.File]::WriteAllBytes((Join-Path $fixtureDir $name), [Text.Encoding]::ASCII.GetBytes("offline resource fixture: $name"))
        $resourceLines += "$resourceId RCDATA `"$((Join-Path $fixtureDir $name).Replace('\','/'))`""
        $resourceId++
    }
    $resourceLines | Set-Content -LiteralPath "$objectDir\resource-fixture.rc" -Encoding utf8
    Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') @('/nologo',"/fo$objectDir\resource-fixture.res", "$objectDir\resource-fixture.rc")
    Invoke-BuildTool 'link.exe' @('/nologo','/dll','/noentry','/machine:x64',"/out:$outDir\pbresource-tests.dll", "$objectDir\resource-fixture.res", "$objectDir\tools.res")
    & (Join-Path $PSScriptRoot '..\package\verify-embedded-driver.ps1') -Dll "$outDir\pbresource-tests.dll" -DriverDirectory $fixtureDir
    Invoke-BuildTool 'cl.exe' (@('/nologo','/LD','/std:c17','/MT','/O2','/W4','/WX','/DPB_API_TESTS=1',"/Fo$objectDir\") + $apiTestSources + $script:VersionArguments + @('/link',"/out:$outDir\pbapi-testshim.dll", '/def:tests\model\api_test_exports.def', "/implib:$objectDir\pbapi-testshim.lib", "$objectDir\tools.res") + $apiTestLibs)
    foreach($ipVersion in @(4,6)) {
        Invoke-TestTool 'python.exe' @('tests\packet\test_flow_family32.py', "$outDir\pbpacket-tests.dll", "$outDir\pbapi-testshim.dll", "$ipVersion")
    }
    $savedTestDll = $env:PB_TEST_DLL
    Invoke-BuildTool 'cl.exe' (@('/nologo','/std:c17','/MT','/O2','/W4','/WX',"/Fo$objectDir\",'tests\safety\lifecycle_unload_tests.c') + $script:VersionArguments + @('/link',"/out:$outDir\pblifecycle-unload-tests.exe", "$objectDir\core-tests.res"))
    Invoke-TestTool "$outDir\pblifecycle-unload-tests.exe" @("$outDir\pbapi-testshim.dll")
    try {
        $env:PB_TEST_DLL = "$outDir\pbapi-testshim.dll"
        foreach($ipFamily in @('ipv4','ipv6')) {
            Invoke-TestTool 'cargo.exe' @('test','--manifest-path','rust\Cargo.toml','--workspace','--offline','--features','native-tests',"batch32_${ipFamily}_",'--','--nocapture')
        }
        Invoke-TestTool 'cargo.exe' @('test','--manifest-path','rust\Cargo.toml','--workspace','--offline','--features','native-tests')
    } finally { $env:PB_TEST_DLL = $savedTestDll }
    if ((Get-Item -LiteralPath "$outDir\pbpacket-tests.dll").VersionInfo.FileVersion -ne $script:Version) { throw 'PE file version mismatch' }
    Invoke-TestTool 'pwsh.exe' @('-NoProfile','-File','tools\build\unified-tests.ps1')
    Write-TestRecording 'passed'
    Write-ArtifactManifest $outDir 'development-tests' @{ runtime_driver_tested = $false }
} elseif ($Stage -eq 'UnifiedTests') {
    . (Join-Path $PSScriptRoot 'unified-tests.ps1')
} elseif ($Stage -eq 'Safety') {
    . (Join-Path $PSScriptRoot 'safety.ps1')
} elseif ($Stage -eq 'Driver') {
    . (Join-Path $PSScriptRoot 'driver.ps1')
} elseif ($Stage -eq 'Probe') {
    . (Join-Path $PSScriptRoot 'probe.ps1')
} elseif ($Stage -in @('Api', 'FinalApi', 'UnsignedTestApi')) {
    . (Join-Path $PSScriptRoot 'api.ps1')
}
