# Shared finite allowance for the independently tested Open enumeration fix.
# Never derive the expected body from current production code or relax the
# caller's whole-aggregate comparison. Only this one frozen region may change.
function Update-AdapterOpenFailureLedger {
    param([Parameter(Mandatory)][string]$Body)
    $Body=$Body.Replace("`r`n","`n")
    $signature='OpenAdapterInternal(LPCWSTR Name)'
    if(([regex]::Matches($Body,[regex]::Escape($signature))).Count -ne 1){throw 'Open ledger signature is not unique'}
    $name=$Body.IndexOf($signature)
    $start=$Body.LastIndexOf('static WINTUN_ADAPTER_HANDLE',$name)
    $end=$Body.IndexOf('typedef struct ADAPTER_OPERATION_INPUT',$name)
    if($start -lt 0 -or $end -le $start){throw 'Cannot isolate frozen Open region for ledger'}
    $region=$Body.Substring($start,$end-$start)
    $before=@'
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
                break;
            continue;
        }
'@.Replace("`r`n","`n")
    $after=@'
        if (!SetupDiEnumDeviceInfo(DevInfo, EnumIndex, &DevInfoData))
        {
            DWORD Error = GetLastError();
            if (Error == ERROR_NO_MORE_ITEMS)
                break;
            LastError = LOG_ERROR(Error ? Error : ERROR_GEN_FAILURE, L"Failed to enumerate adapters");
            goto cleanupDevInfo;
        }
'@.Replace("`r`n","`n")
    if(([regex]::Matches($region,[regex]::Escape($before))).Count -ne 1){throw 'Open ledger enum branch is not unique'}
    return $Body.Substring(0,$start)+$region.Replace($before,$after)+$Body.Substring($end)
}
