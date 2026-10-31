/* SPDX-License-Identifier: GPL-2.0 */
#pragma once
#include "../include/packetbridge_v2_abi.h"

typedef enum PB_RESULT {
    PB_OK = 0, PB_INVALID = 1, PB_FULL = 2, PB_EMPTY = 3, PB_CORRUPT = 4,
    PB_STALE = 5, PB_BUSY = 6, PB_STOPPED = 7, PB_QUARANTINED = 8, PB_UNSUPPORTED = 9
} PB_RESULT;

/* Receive completion descriptor status 4 is a per-packet rejection after the
 * published token was consumed, not evidence of a damaged queue. Keep its 2.0.0
 * wire value for existing clients. An API call returning PB_CORRUPT still means
 * queue/ownership corruption and must not be downgraded to a packet rejection. */
#define PB_RX_REJECTED PB_CORRUPT

typedef struct PB_QUEUE_VIEW {
    PB_QUEUE_SHARED *Shared;
    PB_DESCRIPTOR *Entries;
    uint64_t Cursor;
    uint64_t PeerCursor;
    uint32_t Capacity;
    uint32_t Producer;
} PB_QUEUE_VIEW;

/* Each view has one serialized owner. Shared cursor loads acquire the peer's
 * published entries/free space; cursor stores and the arm/recheck handshake
 * publish only after the corresponding copy. Use PbAttachQueue's aligned layout. */

typedef struct PB_SLOT {
    uint32_t Generation;
    uint32_t State;
} PB_SLOT;
typedef struct PB_RX_SLOT { uint32_t Generation, Active; } PB_RX_SLOT;
#define PB_SLOT_FREE 0u
#define PB_SLOT_RESERVED 1u
#define PB_SLOT_PUBLISHED 2u
#define PB_SLOT_NDIS 3u
#define PB_SLOT_COMPLETE 4u
#define PB_SLOT_RETIRED 5u

typedef struct PB_POOL {
    PB_SLOT *Slots;
    uint64_t Epoch;
    uint32_t Count;
    uint32_t Next;
    uint16_t Queue;
    /* Derived private index. Mutate slot ownership only through pool helpers;
     * mirror imports and native-acknowledged retirement obey the same rule. */
    uint32_t FreeBits[(PB_MAX_SLOTS + 31u) / 32u];
} PB_POOL;

#define PB_SESSION_RUNNING 1u
#define PB_SESSION_DRAINING 2u
#define PB_SESSION_QUARANTINED 3u
#define PB_SESSION_STOPPED 4u
typedef struct PB_LIFETIME {
    uint64_t Deadline;
    uint32_t State;
    uint32_t References;
} PB_LIFETIME;

PB_RESULT PbCalculateLayout(uint32_t queues, uint32_t slots, uint32_t capacity, PB_LAYOUT *layout);
PB_RESULT PbValidateRegionAddress(uint64_t address, uint32_t size);
PB_RESULT PbAttachQueue(PB_QUEUE_VIEW *view, void *region, const PB_LAYOUT *layout, uint32_t queue, uint32_t ring, int producer);
PB_RESULT PbQueuePush(PB_QUEUE_VIEW *view, const PB_DESCRIPTOR *items, uint32_t count, uint32_t *accepted, uint32_t *notify);
/* Recycle producer: publish tokens in full ABI descriptors with zero metadata,
 * without a caller-side descriptor batch. Same space validation/notification. */
PB_RESULT PbQueuePushTokens(PB_QUEUE_VIEW *view, const PB_TOKEN *tokens, uint32_t count, uint32_t *accepted, uint32_t *notify);
/* The source is a caller-serialized private circular buffer, distinct from the
 * destination ring. Publish the accepted prefix once, even across either wrap.
 * This does not advance the source head or release its ownership. */
PB_RESULT PbQueuePushRing(PB_QUEUE_VIEW *view, const PB_DESCRIPTOR *items, uint32_t capacity, uint32_t head,
    uint32_t count, uint32_t *accepted, uint32_t *notify);
PB_RESULT PbQueuePop(PB_QUEUE_VIEW *view, PB_DESCRIPTOR *items, uint32_t count, uint32_t *received);
/* Token-only consumer for recycle rings. The ring still contains full ABI
 * descriptors. Snapshot tokens before publishing consumption; the caller must
 * validate their ownership, exactly as after a full-descriptor pop. */
PB_RESULT PbQueuePopTokens(PB_QUEUE_VIEW *view, PB_TOKEN *tokens, uint32_t count, uint32_t *received);
PB_RESULT PbQueuePopBudget(PB_QUEUE_VIEW *view, PB_DESCRIPTOR *items, uint32_t count, uint32_t byteBudget, uint32_t *received);
PB_RESULT PbQueueArm(PB_QUEUE_VIEW *view, uint32_t *ready);
/* Consumer-side, validated private snapshot; caller serializes this view. */
PB_RESULT PbQueueSnapshotProducer(PB_QUEUE_VIEW *view, uint64_t *producer);
PB_RESULT PbQueueSpace(PB_QUEUE_VIEW *view, uint32_t *space);
PB_RESULT PbPoolInitialize(PB_POOL *pool, PB_SLOT *slots, uint32_t count, uint64_t epoch, uint16_t queue);
PB_RESULT PbPoolReserve(PB_POOL *pool, PB_TOKEN *token);
PB_RESULT PbPoolTransition(PB_POOL *pool, const PB_TOKEN *token, uint32_t before, uint32_t after);
/* Track a caller-validated descriptor batch in a mirrored pool. The complete
 * batch must be free, scoped and newer; failure leaves every slot unchanged. */
PB_RESULT PbPoolImport(PB_POOL *pool, const PB_DESCRIPTOR *items, uint32_t count);
PB_RESULT PbValidateTokens(PB_POOL *pool, const PB_TOKEN *tokens, uint32_t count, uint32_t state);
PB_RESULT PbValidateDescriptor(const PB_LAYOUT *layout, uint64_t epoch, uint32_t queue, int rx, const PB_DESCRIPTOR *d, uint32_t types);
/* Called under the queue RX lock. A repeated/old live token never authorizes a completion. */
PB_RESULT PbAdmitReceive(const PB_LAYOUT *layout, uint64_t epoch, uint32_t queue, PB_RX_SLOT *slots,
    const PB_DESCRIPTOR *descriptor, int draining, uint32_t *completeOnFailure);
void PbLifetimeInitialize(PB_LIFETIME *life);
PB_RESULT PbLifetimeAcquire(PB_LIFETIME *life);
PB_RESULT PbLifetimeRelease(PB_LIFETIME *life);
PB_RESULT PbLifetimeDrain(PB_LIFETIME *life, uint64_t now, uint32_t timeout);
PB_RESULT PbLifetimeObserve(PB_LIFETIME *life, uint64_t now);
