/* SPDX-License-Identifier: GPL-2.0 */
/* Test-package namespaces must survive PID reuse and abandoned old controls.
 * A candidate belongs to this invocation only after atomic creation succeeds. */
#ifndef PB_HELPER_TEST_ROOT_H
#define PB_HELPER_TEST_ROOT_H
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#define PB_HELPER_ROOT_ATTEMPTS 32u
typedef BOOL (WINAPI *PB_HELPER_ROOT_CREATE)(LPCWSTR,LPSECURITY_ATTRIBUTES);
static BOOL PbHelperTestCreateRoot(LPCWSTR parent,DWORD processId,const FILETIME *created,
    LPWSTR directory,DWORD directoryChars,PB_HELPER_ROOT_CREATE createDirectory)
{
    if(!directory||!directoryChars){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    directory[0]=0;
    if(!parent||!*parent||!created||!createDirectory){SetLastError(ERROR_INVALID_PARAMETER);return FALSE;}
    WCHAR absolute[MAX_PATH],candidate[MAX_PATH];
    DWORD count=GetFullPathNameW(parent,_countof(absolute),absolute,NULL);
    if(!count){DWORD error=GetLastError();SetLastError(error?error:ERROR_GEN_FAILURE);return FALSE;}
    if(count>=_countof(absolute)){SetLastError(ERROR_FILENAME_EXCED_RANGE);return FALSE;}
    for(DWORD attempt=1;attempt<=PB_HELPER_ROOT_ATTEMPTS;++attempt){
        int length=_snwprintf_s(candidate,_countof(candidate),_TRUNCATE,L"%s\\run-%08lX-%08lX%08lX-%02lu",
            absolute,processId,created->dwHighDateTime,created->dwLowDateTime,attempt);
        if(length<0){SetLastError(ERROR_FILENAME_EXCED_RANGE);return FALSE;}
        if((DWORD)length>=directoryChars){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
        if(createDirectory(candidate,NULL)){
            memcpy(directory,candidate,((SIZE_T)length+1)*sizeof(WCHAR));return TRUE;
        }
        DWORD error=GetLastError();
        if(error!=ERROR_ALREADY_EXISTS&&error!=ERROR_FILE_EXISTS){SetLastError(error?error:ERROR_GEN_FAILURE);return FALSE;}
    }
    SetLastError(ERROR_ALREADY_EXISTS);return FALSE;
}
#endif
