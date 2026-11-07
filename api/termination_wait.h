/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <Windows.h>
/* Only for exclusively owned process/thread handles. Failure is not a
 * termination witness: the object and everything its owner may still access
 * must remain alive. Report the first failure after the real exit barrier.
 * A broken wait backs off rather than spinning or revoking that ownership. */
static DWORD WaitForOwnedTermination(HANDLE object)
{
    DWORD firstError = ERROR_SUCCESS;
    for (;;) {
        DWORD waited = WaitForSingleObject(object, INFINITE);
        if (waited == WAIT_OBJECT_0) return firstError;
        if (!firstError) {
            firstError = waited == WAIT_FAILED ? GetLastError() : ERROR_GEN_FAILURE;
            if (!firstError) firstError = ERROR_GEN_FAILURE;
        }
        Sleep(1000);
    }
}
