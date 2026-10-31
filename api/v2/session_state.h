/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "lifecycle.h"
#ifdef PB_API_TESTS
#include "test.h"
#endif
struct PB_USER_QUEUE {
    PB_SESSION *Session;
    uint32_t Index;
    volatile LONG Owner;
    /* Runtime counter writes require the unique queue owner with shared
     * DataGate, or lifecycle's exclusive DataGate. Progress readers only take
     * acquire snapshots; counters alone never authorize physical reclamation.
     * Keep all writers serialized when adding queue/lifecycle paths. */
    volatile LONG Loans, Published;
    HANDLE KernelEvent, UserEvent;
    PB_QUEUE_VIEW Rings[PB_QUEUE_COUNT];
    PB_POOL Tx, Rx;
    PB_SLOT *TxSlots, *RxSlots;
};
struct PB_SESSION {
    /* Lifetime is a reader pin for external resource users; only the final
     * resource-release worker takes it exclusively, after ClosingEvent is set.
     * DataGate protects finite ring/pool operations. State never spans I/O.
     * Public/Core references protect this shell, including progress waiters.
     * Never acquire Lifetime/Control/DataGate/RuntimeLock while holding State.
     * Final release may briefly publish State while holding exclusive Lifetime. */
    SRWLOCK Lifetime, DataGate, Control, State;
    CONDITION_VARIABLE Changed;
    volatile LONG References, PublicReleased;
    HANDLE Device, ClosingEvent, ProgressEvent;
    PB_ADAPTER *Adapter;
    uint8_t *Region;
    PB_SHARED_LIFECYCLE *SharedLifecycle;
    PB_LAYOUT Layout;
    uint64_t Epoch;
    uint32_t Experimental;
    volatile LONG Draining, Fatal, CloseRequested, ResourcesClosed;
    PB_USER_QUEUE Queues[PB_MAX_QUEUES];
    PTP_POOL Pool;
    PTP_WORK Work;
    HMODULE Module, TailModule;
    BOOL Scheduled, FinalRequested, PolicyOccupied, PolicyQueued, PolicyAbandoned;
    uint32_t PolicyMask;
    PB_RESULT PolicyResult;
    PB_CLOSE_PROGRESS Progress;
    uint64_t LastKernelSequence;
    PB_SESSION *RuntimeNext;
    BOOL Admitted, Registered;
#ifdef PB_API_TESTS
    uint32_t Test;
    volatile LONG TestActive, TestWaiters, TestFault, TestQueries, TestFailures;
    volatile LONG TestBoundaryThreads[PB_TEST_HOOKS];
    HANDLE TestEntered[PB_TEST_HOOKS], TestGate[PB_TEST_HOOKS];
    PB_QUEUE_VIEW TestRings[PB_MAX_QUEUES][PB_QUEUE_COUNT];
    PB_POOL TestTxPools[PB_MAX_QUEUES];
    PB_SLOT *TestTxSlots[PB_MAX_QUEUES];
#endif
};
