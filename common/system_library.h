/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include <Windows.h>
/* Linker-owned ASCII system DLL basenames only. Keep the restricted modern
 * search. Unpatched Win7 lacks these flags: use an absolute system path and
 * make that directory the first dependency directory, without changing the
 * process-wide search configuration. Never downgrade other load failures. */
static HMODULE LoadSystemLibrary(LPCSTR Name)
{
    UINT length = 0;
    if (!Name) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    for (; length < MAX_PATH && Name[length]; ++length) {
        BYTE c = (BYTE)Name[length];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            SetLastError(ERROR_INVALID_NAME); return NULL;
        }
    }
    if (length < 5 || length == MAX_PATH || Name[0] == '.' || Name[length - 4] != '.' ||
        (Name[length - 3] != 'd' && Name[length - 3] != 'D') ||
        (Name[length - 2] != 'l' && Name[length - 2] != 'L') ||
        (Name[length - 1] != 'l' && Name[length - 1] != 'L')) {
        SetLastError(ERROR_INVALID_NAME); return NULL;
    }
    HMODULE library = LoadLibraryExA(Name, NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (library) return library;
    DWORD error = GetLastError();
    if (error != ERROR_INVALID_PARAMETER) return NULL;
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (!kernel || GetProcAddress(kernel, "AddDllDirectory")) { SetLastError(error); return NULL; }
    WCHAR path[MAX_PATH];
    UINT directory = GetSystemDirectoryW(path, ARRAYSIZE(path));
    if (!directory) { error = GetLastError(); SetLastError(error ? error : ERROR_GEN_FAILURE); return NULL; }
    if (directory >= ARRAYSIZE(path)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return NULL; }
    UINT separator = path[directory - 1] != L'\\';
    if ((SIZE_T)directory + separator + length + 1 > ARRAYSIZE(path)) {
        SetLastError(ERROR_INSUFFICIENT_BUFFER); return NULL;
    }
    if (separator) path[directory++] = L'\\';
    for (UINT i = 0; i < length; ++i) path[directory + i] = (BYTE)Name[i];
    path[directory + length] = 0;
    return LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
}
