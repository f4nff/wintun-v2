# Hosted guarded-memory tests only; never installs/loads a driver or stages a package.
$safetyDir=Join-Path $outDir 'safety'
$safetyObjects=Join-Path $safetyDir 'obj'
Start-TestRecording $safetyDir 'safety-results.json'
New-Item -ItemType Directory -Path $safetyDir,$safetyObjects -Force|Out-Null
Invoke-BuildTool (Join-Path $script:SdkBin 'rc.exe') (@('/nologo',"/I$script:RepoRoot\include","/fo$safetyObjects\version.res")+$script:VersionArguments+@('tools\build\artifact.rc'))
$flags=@('/nologo','/std:c17','/MT','/O2','/Zi','/W4','/WX',"/Fo$safetyObjects\","/Fd$safetyObjects\safety.pdb")+$script:VersionArguments
. (Join-Path $PSScriptRoot 'runtime-wait-tests.ps1')
Invoke-RuntimeWaitTests -Directory $safetyDir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'helper-termination-tests.ps1')
Invoke-HelperTerminationTests -Directory $safetyDir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'helper-io-tests.ps1')
Invoke-HelperIoTests -Directory $safetyDir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'event-init-tests.ps1')
Invoke-EventInitTests -Directory $safetyDir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'system-loader-tests.ps1')
Invoke-SystemLoaderTests -Directory $safetyDir -BaseFlags $flags
. (Join-Path $PSScriptRoot 'kernel-return-tests.ps1')
$outerReturn=Build-KernelReturnTests -Directory (Join-Path $safetyDir 'outer-return-halt') -Mode iso -Guarded
Invoke-TestTool $outerReturn.path @()
. (Join-Path $PSScriptRoot 'kernel-register-tests.ps1')
$registerOwner=Build-KernelRegisterTests -Directory (Join-Path $safetyDir 'register-owner') -Mode iso -Guarded
Invoke-TestTool $registerOwner.path @()
. (Join-Path $PSScriptRoot 'kernel-process-tests.ps1')
$processOwner=Build-KernelProcessTests -Directory (Join-Path $safetyDir 'process-owner') -Mode iso -Guarded
Invoke-TestTool $processOwner.path @()
. (Join-Path $PSScriptRoot 'callback-handoff-tests.ps1')
$callbackHandoff=Build-CallbackHandoffTests -Directory (Join-Path $safetyDir 'callback-handoff') -Mode iso -Guarded
Invoke-TestTool $callbackHandoff.path @($callbackHandoff.fixture)
. (Join-Path $PSScriptRoot 'native-policy-tests.ps1')
$nativePolicy=Build-NativePolicyTests -Directory (Join-Path $safetyDir 'native-policy') -Mode iso -Guarded
Invoke-TestTool $nativePolicy.path @($nativePolicy.fixture)
. (Join-Path $PSScriptRoot 'terminal-wake-tests.ps1')
$terminalWake=Build-TerminalWakeTests -Directory (Join-Path $safetyDir 'terminal-wake') -Mode iso -Guarded
Invoke-TestTool $terminalWake.path @()
. (Join-Path $PSScriptRoot 'helper-response-tests.ps1')
$helperResponse=Build-HelperResponseTests -Directory (Join-Path $safetyDir 'helper-response') -Mode iso -Guarded
Invoke-TestTool $helperResponse.path @()
. (Join-Path $PSScriptRoot 'win7-create-tests.ps1')
$win7Create=Build-Win7CreateTests -Directory (Join-Path $safetyDir 'win7-create') -Mode iso -Guarded
Invoke-TestTool $win7Create.path @()
. (Join-Path $PSScriptRoot 'native-reclaim-tests.ps1')
$nativeReclaim=Build-NativeReclaimTests -Directory (Join-Path $safetyDir 'native-reclaim') -Mode iso -Guarded
Invoke-TestTool $nativeReclaim.path @()
. (Join-Path $PSScriptRoot 'adapter-open-tests.ps1')
$adapterOpen=Build-AdapterOpenTests -Directory (Join-Path $safetyDir 'adapter-open') -Mode iso -Guarded
Invoke-TestTool $adapterOpen.path @()
. (Join-Path $PSScriptRoot 'retained-owner-tests.ps1')
$retainedOwner=Build-RetainedOwnerTests -Directory (Join-Path $safetyDir 'retained-owner') -Mode iso -Guarded
Invoke-TestTool $retainedOwner.path @($retainedOwner.fixture)
. (Join-Path $PSScriptRoot 'dispatch-register-tests.ps1')
$dispatchRegister=Build-DispatchRegisterTests -Directory (Join-Path $safetyDir 'dispatch-register') -Mode iso -Guarded
Invoke-TestTool $dispatchRegister.path @()
. (Join-Path $PSScriptRoot 'status-lifetime-tests.ps1')
$statusLifetime=Build-StatusLifetimeTests -Directory (Join-Path $safetyDir 'status-lifetime') -Mode iso -Guarded
Invoke-TestTool $statusLifetime.path @($statusLifetime.fixture)
. (Join-Path $PSScriptRoot 'native-start-error-tests.ps1')
$nativeStartError=Build-NativeStartErrorTests -Directory (Join-Path $safetyDir 'native-start-error') -Mode iso -Guarded
Invoke-TestTool $nativeStartError.path @()
. (Join-Path $PSScriptRoot 'facade-stop-tests.ps1')
$facadeStop=Build-FacadeStopTests -Directory (Join-Path $safetyDir 'facade-stop') -Mode iso -Guarded
Invoke-TestTool $facadeStop.path @($facadeStop.fixture)
. (Join-Path $PSScriptRoot 'indication-pause-tests.ps1')
$indicationPause=Build-IndicationPauseTests -Directory (Join-Path $safetyDir 'indication-pause') -Mode iso -Guarded
Invoke-TestTool $indicationPause.path @()
. (Join-Path $PSScriptRoot 'send-completion-tests.ps1')
$sendCompletion=Build-SendCompletionTests -Directory (Join-Path $safetyDir 'send-completion') -Mode iso -Guarded
Invoke-TestTool $sendCompletion.path @()
. (Join-Path $PSScriptRoot 'unpublished-close-tests.ps1')
$unpublishedClose=Build-UnpublishedCloseTests -Directory (Join-Path $safetyDir 'unpublished-close') -Mode iso -Guarded
Invoke-TestTool $unpublishedClose.path @($unpublishedClose.fixture)
. (Join-Path $PSScriptRoot 'adapter-runtime-tests.ps1')
$adapterRuntime=Build-AdapterRuntimeTests -Directory (Join-Path $safetyDir 'adapter-runtime') -Mode iso -Guarded
Invoke-TestTool $adapterRuntime.path @($adapterRuntime.fixture)
. (Join-Path $PSScriptRoot 'offload-handler-tests.ps1')
$offloadHandler=Build-OffloadHandlerTests -Directory (Join-Path $safetyDir 'offload-handler') -Mode iso -Guarded
Invoke-TestTool $offloadHandler.path @()
. (Join-Path $PSScriptRoot 'wait-retirement-tests.ps1')
$waitRetirement=Build-WaitRetirementTests -Directory (Join-Path $safetyDir 'wait-retirement') -Mode iso -Guarded
Invoke-TestTool $waitRetirement.path @($waitRetirement.fixture)
. (Join-Path $PSScriptRoot 'runtime-init-tests.ps1')
$runtimeInit=Build-RuntimeInitTests -Directory (Join-Path $safetyDir 'runtime-init') -Mode iso -Guarded
Invoke-TestTool $runtimeInit.path @($runtimeInit.fixture)
. (Join-Path $PSScriptRoot 'drain-policy-tests.ps1')
$drainPolicy=Build-DrainPolicyTests -Directory (Join-Path $safetyDir 'drain-policy') -Mode iso -Guarded
Invoke-TestTool $drainPolicy.path @($drainPolicy.fixture)
. (Join-Path $PSScriptRoot 'late-commit-tests.ps1')
$lateCommit=Build-LateCommitTests -Directory (Join-Path $safetyDir 'late-commit') -Mode iso -Guarded
Invoke-TestTool $lateCommit.path @()
. (Join-Path $PSScriptRoot 'operation-adapter-tests.ps1')
$operationAdapter=Build-OperationAdapterTests -Directory (Join-Path $safetyDir 'operation-adapter') -Mode iso -Guarded
Invoke-TestTool $operationAdapter.path @($operationAdapter.fixture)
. (Join-Path $PSScriptRoot 'policy-shell-tests.ps1')
$policyShell=Build-PolicyShellTests -Directory (Join-Path $safetyDir 'policy-shell') -Mode iso -Guarded
Invoke-TestTool $policyShell.path @($policyShell.fixture)
. (Join-Path $PSScriptRoot 'control-reply-tests.ps1')
$controlReply=Build-ControlReplyTests -Directory (Join-Path $safetyDir 'control-reply') -Mode iso -Guarded
Invoke-TestTool $controlReply.path @()
. (Join-Path $PSScriptRoot 'cleanup-namespace-tests.ps1')
$cleanupNamespace=Build-CleanupNamespaceTests -Directory (Join-Path $safetyDir 'cleanup-namespace') -Mode iso -Guarded
Invoke-TestTool $cleanupNamespace.path @($cleanupNamespace.fixture)
. (Join-Path $PSScriptRoot 'facade-backpressure-tests.ps1')
$facadeBackpressure=Build-FacadeBackpressureTests -Directory (Join-Path $safetyDir 'facade-backpressure') -Mode iso -Guarded
Invoke-TestTool $facadeBackpressure.path @($facadeBackpressure.fixture)
. (Join-Path $PSScriptRoot 'miniport-init-tests.ps1')
$miniportInit=Build-MiniportInitTests -Directory (Join-Path $safetyDir 'miniport-init') -Mode iso -Guarded
Invoke-TestTool $miniportInit.path @()
. (Join-Path $PSScriptRoot 'worker-start-tests.ps1')
$workerStart=Build-WorkerStartTests -Directory (Join-Path $safetyDir 'worker-start') -Mode iso -Guarded
Invoke-TestTool $workerStart.path @()
. (Join-Path $PSScriptRoot 'completion-turn-tests.ps1')
$completionTurn=Build-CompletionTurnTests -Directory (Join-Path $safetyDir 'completion-turn') -Mode iso -Guarded
Invoke-TestTool $completionTurn.path @($completionTurn.fixture)
Invoke-BuildTool cl.exe ($flags+@('/DNDEBUG','/DPB_GUARD_TESTS=1','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso','tests\safety\logger_owner_tests.c','/link',"/out:$safetyDir\logger-owner-tests.exe",'ntdll.lib'))
Invoke-TestTool "$safetyDir\logger-owner-tests.exe" @()
. (Join-Path $PSScriptRoot 'operation-construction-tests.ps1')
$operationConstruction=Build-OperationConstructionTests -Directory (Join-Path $safetyDir 'operation-construction') -Mode iso -Guarded
Invoke-TestTool $operationConstruction.path @($operationConstruction.fixture)
. (Join-Path $PSScriptRoot 'helper-drain-tests.ps1')
$helperDrain=Build-HelperDrainTests -Directory (Join-Path $safetyDir 'helper-drain') -Mode iso -Guarded
Invoke-TestTool $helperDrain.path @($helperDrain.fixture)
. (Join-Path $PSScriptRoot 'drain-fault-capture-tests.ps1')
$drainFaultCapture=Build-DrainFaultCaptureTests -Directory (Join-Path $safetyDir 'drain-fault-capture') -Mode iso -Guarded
Invoke-TestTool $drainFaultCapture.path @()
. (Join-Path $PSScriptRoot 'send-describe-tests.ps1')
$sendDescribe=Build-SendDescribeTests -Directory (Join-Path $safetyDir 'send-describe') -Mode iso -Guarded
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
        $tun32=Build-Tun32SysTests -Directory (Join-Path $safetyDir "ipv$ipVersion-tun32-$direction") -Mode iso -Direction $direction -IpVersion $ipVersion -Guarded
        Invoke-TestTool $tun32.path @()
    }
    $tun32Budget=Build-Tun32SendBudgetTests -Directory (Join-Path $safetyDir "ipv$ipVersion-tun32-send-budget") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $tun32Budget.path @()
    $tun32Coalesce=Build-Tun32CoalesceDrainTests -Directory (Join-Path $safetyDir "ipv$ipVersion-tun32-coalesce-drain") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $tun32Coalesce.path @()
    $tun32Dll=Build-Tun32DllFamilyTests -Directory (Join-Path $safetyDir "ipv$ipVersion-tun32-dll") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $tun32Dll.path @('--tun32')
    Invoke-TestTool $tun32Dll.path @('--tun32-partial')
    Invoke-TestTool $tun32Dll.path @('--tun32-terminal')
    $offload32=Build-Tun32OffloadSendTests -Directory (Join-Path $safetyDir "ipv$ipVersion-tun32-offload-send") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $offload32.path @()
    Invoke-TestTool $offload32.path @('--checksum-independent-lso')
    $request32=Build-PacketRequestFamily32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-packet-request32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $request32.path @()
    $lsoPolicy=Build-LsoChecksumPolicyTests -Directory (Join-Path $safetyDir "ipv$ipVersion-lso-checksum-policy") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $lsoPolicy.path @()
    $checksumMetadata=Build-Tun32ChecksumMetadataTests -Directory (Join-Path $safetyDir "ipv$ipVersion-checksum-metadata32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $checksumMetadata.path @()
    $returnReuse=Build-Tun32ReturnReuseTests -Directory (Join-Path $safetyDir "ipv$ipVersion-return-reuse32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $returnReuse.path @()
    $rejectedReuse=Build-RejectedReuse32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-rejected-reuse32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $rejectedReuse.path @()
    $workerPause=Build-Tun32WorkerPauseTests -Directory (Join-Path $safetyDir "ipv$ipVersion-worker-pause32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $workerPause.path @()
    $duplex=Build-DuplexBackpressure32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-duplex-backpressure32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $duplex.path @($duplex.fixture)
    $multiqueueDrain=Build-Tun32MultiqueueDrainTests -Directory (Join-Path $safetyDir "ipv$ipVersion-multiqueue-drain32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $multiqueueDrain.path @()
    $crossQueue=Build-CrossQueuePrepare32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-cross-queue-prepare32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $crossQueue.path @($crossQueue.fixture)
    $localRetirement=Build-LocalRetirement32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-local-retirement32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $localRetirement.path @($localRetirement.fixture)
    $completionArm=Build-CompletionArm32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-completion-arm32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $completionArm.path @()
    $coalesceDual=Build-CoalesceDualTail32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-coalesce-dual-tail32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $coalesceDual.path @()
    $duplexBudget=Build-DuplexBudget32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-duplex-budget32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $duplexBudget.path @($duplexBudget.fixture)
    $controlBatch=Build-ControlBatch32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-control-batch32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $controlBatch.path @()
    $statusOwnership=Build-StatusOwnership32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-status-ownership32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $statusOwnership.path @($statusOwnership.fixture)
    $receiveAdmission=Build-ReceiveAdmission32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-receive-admission32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $receiveAdmission.path @()
    $policyAbandon=Build-PolicyAbandon32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-policy-abandon32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $policyAbandon.path @($policyAbandon.fixture)
    $recycleSend=Build-RecycleSend32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-recycle-send32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $recycleSend.path @()
    $retirementHint=Build-RetirementHint32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-retirement-hint32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $retirementHint.path @($retirementHint.fixture)
    $referenceHandoff=Build-ReferenceHandoff32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-reference-handoff32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $referenceHandoff.path @()
    $reuseTerminal=Build-ReuseTerminal32Tests -Directory (Join-Path $safetyDir "ipv$ipVersion-reuse-terminal32") -Mode iso -IpVersion $ipVersion -Guarded
    Invoke-TestTool $reuseTerminal.path @($reuseTerminal.fixture)
}
. (Join-Path $PSScriptRoot 'env-init-tests.ps1')
foreach($maybeWow64 in @($false,$true)) {
    $environment=Build-EnvInitTests -Directory (Join-Path $safetyDir "env-init-$maybeWow64") -Mode iso -MaybeWow64:$maybeWow64
    Invoke-TestTool $environment.path @()
}
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
    @{name='orphan-owner';sources=@('tests\safety\orphan_owner_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@()},
    @{name='namespace-init';sources=@('tests\safety\namespace_init_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('advapi32.lib')},
    @{name='resource-failure';sources=@('tests\safety\resource_failure_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('advapi32.lib','shlwapi.lib')},
    @{name='registry-string';sources=@('tests\safety\registry_string_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('advapi32.lib')},
    @{name='helper-start';sources=@('tests\safety\helper_start_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@()},
    @{name='win7-readiness';sources=@('tests\safety\win7_readiness_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@()},
    @{name='adapter-data';sources=@('tests\safety\adapter_data_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('ole32.lib')},
    @{name='driver-version';sources=@('tests\safety\driver_version_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('version.lib','ntdll.lib')},
    @{name='driver-enumeration';sources=@('tests\safety\driver_enumeration_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('shlwapi.lib','uuid.lib')},
    @{name='adapter-name-log';sources=@('tests\safety\adapter_name_log_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@()},
    @{name='adapter-rename';sources=@('tests\safety\adapter_rename_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('ole32.lib')},
    @{name='helper-host';sources=@('tests\safety\helper_host_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('shell32.lib','uuid.lib');link=@("/implib:$safetyObjects\helper-host-tests.lib")},
    @{name='helper-identity';sources=@('tests\safety\helper_identity_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('uuid.lib')},
    @{name='adapter-discovery';sources=@('tests\safety\adapter_discovery_tests.c');defines=@('/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('cfgmgr32.lib','uuid.lib')},
    @{name='kernel-object-guard';sources=@('tests\safety\kernel_object_guard_tests.c','common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c');defines=@('/DPB_GUARD_TESTS=1','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@()},
    @{name='pause-unregister';sources=@('tests\model\pause_unregister_tests.c','common\v2_core.c','common\ethernet.c','common\coalesce.c','common\packet.c');defines=@('/DPB_GUARD_TESTS=1','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@()},
    @{name='batch-output';sources=@('tests\model\batch_output_tests.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='system-library';sources=@('tests\model\system_library_tests.c');defines=@();libs=@()},
    @{name='lifecycle-park';sources=@('tests\model\lifecycle_park_tests.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
    @{name='termination-wait';sources=@('tests\model\termination_wait_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='recycle-budget';sources=@('tests\model\recycle_budget_tests.c');defines=@('/volatile:iso');libs=@()},
    @{name='mdl-mapping';sources=@('tests\model\mdl_mapping_wdk.c','tests\model\mdl_mapping_tests.c');defines=@('/D_AMD64_','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/DPOOL_NX_OPTIN=1','/DPOOL_ZERO_DOWN_LEVEL_SUPPORT','/DPB_MDL_DIRECT_MAPPING=1','/wd4324',"/I$script:WdkRoot\Include\$script:KitVersion\km");libs=@()},
    @{name='return-alias';sources=@('tests\model\return_alias_tests.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/volatile:iso');libs=@()},
    @{name='compat-pump';sources=@('tests\model\compat_session_tests.c','api\v2\session.c','api\v2\adapter.c','api\v2\driver_package.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_API_TESTS=1','/volatile:iso');libs=@('bcrypt.lib','setupapi.lib','swdevice.lib','ole32.lib','iphlpapi.lib','advapi32.lib','newdev.lib')},
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
    @{name='guard-open-construction';sources=@('tests\model\open_error_tests.c','common\v2_core.c','common\packet.c','common\flow.c');defines=@('/DPB_GUARD_TESTS=1','/DNDEBUG','/D_WIN32_WINNT=0x0601','/DNTDDI_VERSION=0x06010000','/volatile:iso');libs=@('bcrypt.lib')},
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
    $caseLinkFlags=@()
    if($case.ContainsKey('link')){$caseLinkFlags=@($case.link)}
    Invoke-BuildTool 'cl.exe' ($flags+$case.defines+$case.sources+@('/link',"/out:$exe","$safetyObjects\version.res")+$case.libs+$caseLinkFlags)
    Invoke-TestTool $exe @()
}
Write-TestRecording 'passed'
Write-ArtifactManifest $safetyDir 'guarded-hosted-safety-tests' @{kernel_runtime_tested=$false}
