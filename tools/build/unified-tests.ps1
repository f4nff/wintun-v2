# Hosted unified compatibility tests. No device installation or kernel loading.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
Initialize-BuildEnvironment
$dir = Join-Path $script:RepoRoot 'out\unified-tests'
New-Item -ItemType Directory -Path $dir -Force | Out-Null
$script:SourceFingerprint = Get-SourceFingerprint
$script:VersionArguments += "/DPB_SOURCE_FINGERPRINT=$script:SourceFingerprint"
Start-TestRecording $dir
$flags = @('/nologo','/std:c17','/MT','/O2','/W4','/WX','/wd4200',
    "/Fo$dir\") + $script:VersionArguments
. (Join-Path $PSScriptRoot 'runtime-wait-tests.ps1')
Invoke-RuntimeWaitTests -Directory $dir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'helper-termination-tests.ps1')
Invoke-HelperTerminationTests -Directory $dir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'helper-io-tests.ps1')
Invoke-HelperIoTests -Directory $dir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'event-init-tests.ps1')
Invoke-EventInitTests -Directory $dir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'kernel-return-tests.ps1')
$outerReturn=Build-KernelReturnTests -Directory (Join-Path $dir 'outer-return-halt') -Mode iso
Invoke-TestTool $outerReturn.path @()
. (Join-Path $PSScriptRoot 'kernel-register-tests.ps1')
$registerOwner=Build-KernelRegisterTests -Directory (Join-Path $dir 'register-owner') -Mode iso
Invoke-TestTool $registerOwner.path @()
. (Join-Path $PSScriptRoot 'kernel-process-tests.ps1')
$processOwner=Build-KernelProcessTests -Directory (Join-Path $dir 'process-owner') -Mode iso
Invoke-TestTool $processOwner.path @()
. (Join-Path $PSScriptRoot 'callback-handoff-tests.ps1')
$callbackHandoff=Build-CallbackHandoffTests -Directory (Join-Path $dir 'callback-handoff') -Mode iso
Invoke-TestTool $callbackHandoff.path @($callbackHandoff.fixture)
. (Join-Path $PSScriptRoot 'native-policy-tests.ps1')
$nativePolicy=Build-NativePolicyTests -Directory (Join-Path $dir 'native-policy') -Mode iso
Invoke-TestTool $nativePolicy.path @($nativePolicy.fixture)
. (Join-Path $PSScriptRoot 'terminal-wake-tests.ps1')
$terminalWake=Build-TerminalWakeTests -Directory (Join-Path $dir 'terminal-wake') -Mode iso
Invoke-TestTool $terminalWake.path @()
. (Join-Path $PSScriptRoot 'helper-response-tests.ps1')
$helperResponse=Build-HelperResponseTests -Directory (Join-Path $dir 'helper-response') -Mode iso
Invoke-TestTool $helperResponse.path @()
. (Join-Path $PSScriptRoot 'win7-create-tests.ps1')
$win7Create=Build-Win7CreateTests -Directory (Join-Path $dir 'win7-create') -Mode iso
Invoke-TestTool $win7Create.path @()
. (Join-Path $PSScriptRoot 'native-reclaim-tests.ps1')
$nativeReclaim=Build-NativeReclaimTests -Directory (Join-Path $dir 'native-reclaim') -Mode iso
Invoke-TestTool $nativeReclaim.path @()
. (Join-Path $PSScriptRoot 'adapter-open-tests.ps1')
$adapterOpen=Build-AdapterOpenTests -Directory (Join-Path $dir 'adapter-open') -Mode iso
Invoke-TestTool $adapterOpen.path @()
. (Join-Path $PSScriptRoot 'retained-owner-tests.ps1')
$retainedOwner=Build-RetainedOwnerTests -Directory (Join-Path $dir 'retained-owner') -Mode iso
Invoke-TestTool $retainedOwner.path @($retainedOwner.fixture)
. (Join-Path $PSScriptRoot 'dispatch-register-tests.ps1')
$dispatchRegister=Build-DispatchRegisterTests -Directory (Join-Path $dir 'dispatch-register') -Mode iso
Invoke-TestTool $dispatchRegister.path @()
. (Join-Path $PSScriptRoot 'status-lifetime-tests.ps1')
$statusLifetime=Build-StatusLifetimeTests -Directory (Join-Path $dir 'status-lifetime') -Mode iso
Invoke-TestTool $statusLifetime.path @($statusLifetime.fixture)
. (Join-Path $PSScriptRoot 'native-start-error-tests.ps1')
$nativeStartError=Build-NativeStartErrorTests -Directory (Join-Path $dir 'native-start-error') -Mode iso
Invoke-TestTool $nativeStartError.path @()
. (Join-Path $PSScriptRoot 'facade-stop-tests.ps1')
$facadeStop=Build-FacadeStopTests -Directory (Join-Path $dir 'facade-stop') -Mode iso
Invoke-TestTool $facadeStop.path @($facadeStop.fixture)
. (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
$indicationPause=Build-IndicationPauseTests -Directory (Join-Path $dir 'indication-pause') -Mode iso
Invoke-TestTool $indicationPause.path @()
. (Join-Path $PSScriptRoot 'send-completion-tests.ps1')
$sendCompletion=Build-SendCompletionTests -Directory (Join-Path $dir 'send-completion') -Mode iso
Invoke-TestTool $sendCompletion.path @()
. (Join-Path $PSScriptRoot 'unpublished-close-tests.ps1')
$unpublishedClose=Build-UnpublishedCloseTests -Directory (Join-Path $dir 'unpublished-close') -Mode iso
Invoke-TestTool $unpublishedClose.path @($unpublishedClose.fixture)
. (Join-Path $PSScriptRoot 'adapter-runtime-tests.ps1')
$adapterRuntime=Build-AdapterRuntimeTests -Directory (Join-Path $dir 'adapter-runtime') -Mode iso
Invoke-TestTool $adapterRuntime.path @($adapterRuntime.fixture)
. (Join-Path $PSScriptRoot 'offload-handler-tests.ps1')
$offloadHandler=Build-OffloadHandlerTests -Directory (Join-Path $dir 'offload-handler') -Mode iso
Invoke-TestTool $offloadHandler.path @()
. (Join-Path $PSScriptRoot 'wait-retirement-tests.ps1')
$waitRetirement=Build-WaitRetirementTests -Directory (Join-Path $dir 'wait-retirement') -Mode iso
Invoke-TestTool $waitRetirement.path @($waitRetirement.fixture)
. (Join-Path $PSScriptRoot 'runtime-init-tests.ps1')
$runtimeInit=Build-RuntimeInitTests -Directory (Join-Path $dir 'runtime-init') -Mode iso
Invoke-TestTool $runtimeInit.path @($runtimeInit.fixture)
. (Join-Path $PSScriptRoot 'drain-policy-tests.ps1')
$drainPolicy=Build-DrainPolicyTests -Directory (Join-Path $dir 'drain-policy') -Mode iso
Invoke-TestTool $drainPolicy.path @($drainPolicy.fixture)
. (Join-Path $PSScriptRoot 'late-commit-tests.ps1')
$lateCommit=Build-LateCommitTests -Directory (Join-Path $dir 'late-commit') -Mode iso
Invoke-TestTool $lateCommit.path @()
. (Join-Path $PSScriptRoot 'operation-adapter-tests.ps1')
$operationAdapter=Build-OperationAdapterTests -Directory (Join-Path $dir 'operation-adapter') -Mode iso
Invoke-TestTool $operationAdapter.path @($operationAdapter.fixture)
. (Join-Path $PSScriptRoot 'policy-shell-tests.ps1')
$policyShell=Build-PolicyShellTests -Directory (Join-Path $dir 'policy-shell') -Mode iso
Invoke-TestTool $policyShell.path @($policyShell.fixture)
. (Join-Path $PSScriptRoot 'control-reply-tests.ps1')
$controlReply=Build-ControlReplyTests -Directory (Join-Path $dir 'control-reply') -Mode iso
Invoke-TestTool $controlReply.path @()
. (Join-Path $PSScriptRoot 'cleanup-namespace-tests.ps1')
$cleanupNamespace=Build-CleanupNamespaceTests -Directory (Join-Path $dir 'cleanup-namespace') -Mode iso
Invoke-TestTool $cleanupNamespace.path @($cleanupNamespace.fixture)
. (Join-Path $PSScriptRoot 'facade-backpressure-tests.ps1')
$facadeBackpressure=Build-FacadeBackpressureTests -Directory (Join-Path $dir 'facade-backpressure') -Mode iso
Invoke-TestTool $facadeBackpressure.path @($facadeBackpressure.fixture)
. (Join-Path $PSScriptRoot 'miniport-init-tests.ps1')
$miniportInit=Build-MiniportInitTests -Directory (Join-Path $dir 'miniport-init') -Mode iso
Invoke-TestTool $miniportInit.path @()
. (Join-Path $PSScriptRoot 'worker-start-tests.ps1')
$workerStart=Build-WorkerStartTests -Directory (Join-Path $dir 'worker-start') -Mode iso
Invoke-TestTool $workerStart.path @()
. (Join-Path $PSScriptRoot 'completion-turn-tests.ps1')
$completionTurn=Build-CompletionTurnTests -Directory (Join-Path $dir 'completion-turn') -Mode iso
Invoke-TestTool $completionTurn.path @($completionTurn.fixture)
Invoke-BuildTool cl.exe ($flags+@('/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\logger_owner_tests.c','/link',"/out:$dir\logger-owner-tests.exe",'ntdll.lib'))
Invoke-TestTool "$dir\logger-owner-tests.exe" @()
. (Join-Path $PSScriptRoot 'operation-construction-tests.ps1')
$operationConstruction=Build-OperationConstructionTests -Directory (Join-Path $dir 'operation-construction') -Mode iso
Invoke-TestTool $operationConstruction.path @($operationConstruction.fixture)
. (Join-Path $PSScriptRoot 'helper-drain-tests.ps1')
$helperDrain=Build-HelperDrainTests -Directory (Join-Path $dir 'helper-drain') -Mode iso
Invoke-TestTool $helperDrain.path @($helperDrain.fixture)
. (Join-Path $PSScriptRoot 'drain-fault-capture-tests.ps1')
$drainFaultCapture=Build-DrainFaultCaptureTests -Directory (Join-Path $dir 'drain-fault-capture') -Mode iso
Invoke-TestTool $drainFaultCapture.path @()
. (Join-Path $PSScriptRoot 'send-describe-tests.ps1')
$sendDescribe=Build-SendDescribeTests -Directory (Join-Path $dir 'send-describe') -Mode iso
Invoke-TestTool $sendDescribe.path @()
. (Join-Path $PSScriptRoot 'tun32-sys-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-send-budget-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-coalesce-drain-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-dll-family-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-offload-send-tests.ps1')
. (Join-Path $PSScriptRoot 'packet-request-family32-tests.ps1')
. (Join-Path $PSScriptRoot 'lso-checksum-policy-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-checksum-metadata-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-return-reuse-tests.ps1')
. (Join-Path $PSScriptRoot 'rejected-reuse32-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-worker-pause-tests.ps1')
. (Join-Path $PSScriptRoot 'duplex-backpressure32-tests.ps1')
. (Join-Path $PSScriptRoot 'tun32-multiqueue-drain-tests.ps1')
. (Join-Path $PSScriptRoot 'cross-queue-prepare32-tests.ps1')
. (Join-Path $PSScriptRoot 'local-retirement32-tests.ps1')
. (Join-Path $PSScriptRoot 'completion-arm32-tests.ps1')
. (Join-Path $PSScriptRoot 'coalesce-dual-tail32-tests.ps1')
. (Join-Path $PSScriptRoot 'duplex-budget32-tests.ps1')
. (Join-Path $PSScriptRoot 'control-batch32-tests.ps1')
. (Join-Path $PSScriptRoot 'status-ownership32-tests.ps1')
. (Join-Path $PSScriptRoot 'receive-admission32-tests.ps1')
. (Join-Path $PSScriptRoot 'policy-abandon32-tests.ps1')
. (Join-Path $PSScriptRoot 'recycle-send32-tests.ps1')
. (Join-Path $PSScriptRoot 'retirement-hint32-tests.ps1')
. (Join-Path $PSScriptRoot 'reference-handoff32-tests.ps1')
. (Join-Path $PSScriptRoot 'reuse-terminal32-tests.ps1')
foreach($ipVersion in @(4,6)) {
    foreach($direction in @('send','receive')) {
        $tun32=Build-Tun32SysTests -Directory (Join-Path $dir "ipv$ipVersion-tun32-$direction") -Mode iso -Direction $direction -IpVersion $ipVersion
        Invoke-TestTool $tun32.path @()
    }
    $tun32Budget=Build-Tun32SendBudgetTests -Directory (Join-Path $dir "ipv$ipVersion-tun32-send-budget") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $tun32Budget.path @()
    $tun32Coalesce=Build-Tun32CoalesceDrainTests -Directory (Join-Path $dir "ipv$ipVersion-tun32-coalesce-drain") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $tun32Coalesce.path @()
    $tun32Dll=Build-Tun32DllFamilyTests -Directory (Join-Path $dir "ipv$ipVersion-tun32-dll") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $tun32Dll.path @('--tun32')
    Invoke-TestTool $tun32Dll.path @('--tun32-partial')
    Invoke-TestTool $tun32Dll.path @('--tun32-terminal')
    $offload32=Build-Tun32OffloadSendTests -Directory (Join-Path $dir "ipv$ipVersion-tun32-offload-send") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $offload32.path @()
    Invoke-TestTool $offload32.path @('--checksum-independent-lso')
    $request32=Build-PacketRequestFamily32Tests -Directory (Join-Path $dir "ipv$ipVersion-packet-request32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $request32.path @()
    $lsoPolicy=Build-LsoChecksumPolicyTests -Directory (Join-Path $dir "ipv$ipVersion-lso-checksum-policy") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $lsoPolicy.path @()
    $checksumMetadata=Build-Tun32ChecksumMetadataTests -Directory (Join-Path $dir "ipv$ipVersion-checksum-metadata32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $checksumMetadata.path @()
    $returnReuse=Build-Tun32ReturnReuseTests -Directory (Join-Path $dir "ipv$ipVersion-return-reuse32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $returnReuse.path @()
    $rejectedReuse=Build-RejectedReuse32Tests -Directory (Join-Path $dir "ipv$ipVersion-rejected-reuse32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $rejectedReuse.path @()
    $workerPause=Build-Tun32WorkerPauseTests -Directory (Join-Path $dir "ipv$ipVersion-worker-pause32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $workerPause.path @()
    $duplex=Build-DuplexBackpressure32Tests -Directory (Join-Path $dir "ipv$ipVersion-duplex-backpressure32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $duplex.path @($duplex.fixture)
    $multiqueueDrain=Build-Tun32MultiqueueDrainTests -Directory (Join-Path $dir "ipv$ipVersion-multiqueue-drain32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $multiqueueDrain.path @()
    $crossQueue=Build-CrossQueuePrepare32Tests -Directory (Join-Path $dir "ipv$ipVersion-cross-queue-prepare32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $crossQueue.path @($crossQueue.fixture)
    $localRetirement=Build-LocalRetirement32Tests -Directory (Join-Path $dir "ipv$ipVersion-local-retirement32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $localRetirement.path @($localRetirement.fixture)
    $completionArm=Build-CompletionArm32Tests -Directory (Join-Path $dir "ipv$ipVersion-completion-arm32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $completionArm.path @()
    $coalesceDual=Build-CoalesceDualTail32Tests -Directory (Join-Path $dir "ipv$ipVersion-coalesce-dual-tail32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $coalesceDual.path @()
    $duplexBudget=Build-DuplexBudget32Tests -Directory (Join-Path $dir "ipv$ipVersion-duplex-budget32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $duplexBudget.path @($duplexBudget.fixture)
    $controlBatch=Build-ControlBatch32Tests -Directory (Join-Path $dir "ipv$ipVersion-control-batch32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $controlBatch.path @()
    $statusOwnership=Build-StatusOwnership32Tests -Directory (Join-Path $dir "ipv$ipVersion-status-ownership32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $statusOwnership.path @($statusOwnership.fixture)
    $receiveAdmission=Build-ReceiveAdmission32Tests -Directory (Join-Path $dir "ipv$ipVersion-receive-admission32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $receiveAdmission.path @()
    $policyAbandon=Build-PolicyAbandon32Tests -Directory (Join-Path $dir "ipv$ipVersion-policy-abandon32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $policyAbandon.path @($policyAbandon.fixture)
    $recycleSend=Build-RecycleSend32Tests -Directory (Join-Path $dir "ipv$ipVersion-recycle-send32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $recycleSend.path @()
    $retirementHint=Build-RetirementHint32Tests -Directory (Join-Path $dir "ipv$ipVersion-retirement-hint32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $retirementHint.path @($retirementHint.fixture)
    $referenceHandoff=Build-ReferenceHandoff32Tests -Directory (Join-Path $dir "ipv$ipVersion-reference-handoff32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $referenceHandoff.path @()
    $reuseTerminal=Build-ReuseTerminal32Tests -Directory (Join-Path $dir "ipv$ipVersion-reuse-terminal32") -Mode iso -IpVersion $ipVersion
    Invoke-TestTool $reuseTerminal.path @($reuseTerminal.fixture)
}
. (Join-Path $PSScriptRoot 'env-init-tests.ps1')
foreach($maybeWow64 in @($false,$true)) {
    $environment=Build-EnvInitTests -Directory (Join-Path $dir "env-init-$maybeWow64") -Mode iso -MaybeWow64:$maybeWow64
    Invoke-TestTool $environment.path @()
}
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\namespace_init_tests.c','/link',"/out:$dir\namespace-init-tests.exe",'advapi32.lib'))
Invoke-TestTool "$dir\namespace-init-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\resource_failure_tests.c','/link',"/out:$dir\resource-failure-tests.exe",'advapi32.lib','shlwapi.lib'))
Invoke-TestTool "$dir\resource-failure-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\registry_string_tests.c','/link',"/out:$dir\registry-string-tests.exe",'advapi32.lib'))
Invoke-TestTool "$dir\registry-string-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\helper_start_tests.c','/link',"/out:$dir\helper-start-tests.exe"))
Invoke-TestTool "$dir\helper-start-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\win7_readiness_tests.c','/link',"/out:$dir\win7-readiness-tests.exe"))
Invoke-TestTool "$dir\win7-readiness-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\adapter_data_tests.c','/link',"/out:$dir\adapter-data-tests.exe",'ole32.lib'))
Invoke-TestTool "$dir\adapter-data-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\driver_version_tests.c','/link',"/out:$dir\driver-version-tests.exe",'version.lib','ntdll.lib'))
Invoke-TestTool "$dir\driver-version-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\driver_enumeration_tests.c','/link',"/out:$dir\driver-enumeration-tests.exe",'shlwapi.lib','uuid.lib'))
Invoke-TestTool "$dir\driver-enumeration-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\adapter_name_log_tests.c','/link',"/out:$dir\adapter-name-log-tests.exe"))
Invoke-TestTool "$dir\adapter-name-log-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\adapter_rename_tests.c','/link',"/out:$dir\adapter-rename-tests.exe",'ole32.lib'))
Invoke-TestTool "$dir\adapter-rename-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\helper_host_tests.c','/link',"/out:$dir\helper-host-tests.exe","/implib:$dir\helper-host-tests.lib",'shell32.lib','uuid.lib'))
Invoke-TestTool "$dir\helper-host-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\helper_identity_tests.c','/link',"/out:$dir\helper-identity-tests.exe",'uuid.lib'))
Invoke-TestTool "$dir\helper-identity-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\adapter_discovery_tests.c','/link',"/out:$dir\adapter-discovery-tests.exe",'cfgmgr32.lib','uuid.lib'))
Invoke-TestTool "$dir\adapter-discovery-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/DPB_GUARD_TESTS=1','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\kernel_object_guard_tests.c','common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$dir\kernel-object-guard-tests.exe"))
Invoke-TestTool "$dir\kernel-object-guard-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\model\pause_unregister_tests.c','common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c','/link',"/out:$dir\pause-unregister-tests.exe"))
Invoke-TestTool "$dir\pause-unregister-tests.exe" @()
. (Join-Path $PSScriptRoot 'system-loader-tests.ps1')
Invoke-SystemLoaderTests -Directory $dir -BaseFlags $flags
Invoke-BuildTool cl.exe ($flags+@('tests\model\system_library_tests.c','/link',"/out:$dir\system-library-tests.exe"))
Invoke-TestTool "$dir\system-library-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/volatile:iso','tests\model\lifecycle_park_tests.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c',
    '/link',"/out:$dir\lifecycle-park-tests.exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
Invoke-TestTool "$dir\lifecycle-park-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/volatile:iso','tests\model\batch_output_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c',
    '/link',"/out:$dir\batch-output-tests.exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
Invoke-TestTool "$dir\batch-output-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','tests\safety\orphan_owner_tests.c','/link',"/out:$dir\orphan-owner-tests.exe"))
Invoke-TestTool "$dir\orphan-owner-tests.exe" @()
Invoke-BuildTool cl.exe ($flags+@('tests\model\termination_wait_tests.c','/link',"/out:$dir\termination-wait-tests.exe"))
Invoke-TestTool "$dir\termination-wait-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\platform_tests.c','/link',"/out:$dir\platform-tests.exe"))
Invoke-TestTool "$dir\platform-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\offload_format_tests.c','/link',"/out:$dir\offload-format-tests.exe"))
Invoke-TestTool "$dir\offload-format-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\platform_nbl_tests.c','/link',"/out:$dir\platform-nbl-tests.exe"))
Invoke-TestTool "$dir\platform-nbl-tests.exe" @()
$mdlFlags=@('/D_AMD64_','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/DPOOL_NX_OPTIN=1',
    '/DPOOL_ZERO_DOWN_LEVEL_SUPPORT','/DPB_MDL_DIRECT_MAPPING=1','/wd4324',"/I$script:WdkRoot\Include\$script:KitVersion\km")
Invoke-BuildTool 'cl.exe' ($flags + $mdlFlags + @('tests\model\mdl_mapping_wdk.c','tests\model\mdl_mapping_tests.c',
    '/link',"/out:$dir\mdl-mapping-tests.exe"))
Invoke-TestTool "$dir\mdl-mapping-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('/volatile:iso','tests\model\recycle_budget_tests.c','/link',"/out:$dir\recycle-budget-tests.exe"))
Invoke-TestTool "$dir\recycle-budget-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\adapter_name_tests.c','/link',"/out:$dir\adapter-name-tests.exe"))
Invoke-TestTool "$dir\adapter-name-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\unified_adapter_tests.c','/link',"/out:$dir\unified-adapter-tests.exe",'bcrypt.lib'))
Invoke-TestTool "$dir\unified-adapter-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\open_error_tests.c','common\v2_core.c','common\packet.c','common\flow.c',
    '/link',"/out:$dir\open-error-tests.exe",'bcrypt.lib'))
Invoke-TestTool "$dir\open-error-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('tests\model\return_alias_tests.c','common\v2_core.c','common\packet.c','common\flow.c',
    '/link',"/out:$dir\return-alias-tests.exe"))
Invoke-TestTool "$dir\return-alias-tests.exe" @()
Invoke-BuildTool 'cl.exe' ($flags + @('/DPB_API_TESTS=1','tests\model\compat_session_tests.c',
    'api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c',
    '/link',"/out:$dir\compat-tests.exe",'bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib'))
Invoke-TestTool "$dir\compat-tests.exe" @()
Invoke-TestTool 'python.exe' @('tests\package\test_unified_pe.py')
Write-TestRecording 'passed'
