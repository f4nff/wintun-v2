/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <Windows.h>
/* The executor copies input before dispatch. Run and Dispose execute on the
 * owning worker; Dispose consumes an unclaimed successful result. */
typedef ULONG_PTR (*MAIN_OPERATION_RUN)(const VOID *Input);
typedef VOID (*MAIN_OPERATION_DISPOSE)(ULONG_PTR Result);
ULONG_PTR MainRunOperation(MAIN_OPERATION_RUN Run, MAIN_OPERATION_DISPOSE Dispose, const VOID *Input, SIZE_T Size);
BOOL MainWaitForObject(HANDLE Object, HANDLE Stop, DWORD Timeout);
BOOL MainOperationSleep(DWORD Delay);
VOID MainOperationRequestShutdown(VOID);
BOOL MainOperationShutdown(DWORD Timeout);
VOID MainOperationDetach(VOID);
