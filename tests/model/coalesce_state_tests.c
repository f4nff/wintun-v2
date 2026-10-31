/* SPDX-License-Identifier: GPL-2.0 */
/* Keep all existing packet/checksum/busy/short-final assertions while poisoning
 * each caller state and observing the actual Begin initialization address. */
#define PB_COALESCE_SOURCE "coalesce_state_wrapper.h"
#define main CoalescePathCases
#include "coalesce_path_tests.c"
#undef main
int main(VOID)
{
    CHECK(CoalescePathCases()==0 && StateCalls && StateBegins && StateMerged && StateFallbacks && StateDirect+StatePrivate==StateBegins && !StateOutput);
    printf("PASS: poisoned coalescing state: calls=%lu begin=%lu merged=%lu fallback=%lu direct=%lu private=%lu; initializer location observed, copied-lease guard and existing exact packet/metadata/checksum/busy assertions passed.\n",StateCalls,StateBegins,StateMerged,StateFallbacks,StateDirect,StatePrivate);return 0;
}
