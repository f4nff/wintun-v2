/* SPDX-License-Identifier: GPL-2.0 */
/* Never expected to load: the test puts this DLL in its current directory. */
__declspec(dllexport) int PoisonedSystemLibrary(void) {return 1;}
