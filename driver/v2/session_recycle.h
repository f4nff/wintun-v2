/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
/* A failed recycle latches Faulted and must end the caller's transaction. Valid
 * prefixes stay retired; later shared tokens cannot authorize more work. */
_Must_inspect_result_
static PB_RESULT PbV2RecycleLocked(PB_KERNEL_QUEUE *q)
{
    PB_TOKEN tokens[PB_MAX_BATCH]; uint32_t count;
    /* Bound work even if the shared producer keeps publishing while we drain. */
    for (ULONG budget = 0; budget < 4; ++budget) {
        PB_RESULT result = PbQueuePopTokens(&q->Rings[PB_QUEUE_TX_RECYCLE], tokens, PB_MAX_BATCH, &count);
        if (result == PB_EMPTY) return PB_OK;
        if (result != PB_OK) { PbV2Fault(q->Session); return result; }
        for (ULONG i = 0; i < count; ++i) {
            result = PbPoolTransition(&q->TxPool, tokens + i, PB_SLOT_PUBLISHED, PB_SLOT_FREE);
            if (result != PB_OK) {
                if (i) q->TxPoolExhausted = FALSE; /* Valid prefix slots already became free. */
                PbV2Fault(q->Session); return result;
            }
        }
        /* An observed producer cursor is not a free slot. Only this validated
         * ownership release permits a cached full pool to be tried again. */
        if (count) q->TxPoolExhausted = FALSE;
    }
    return PB_OK;
}
