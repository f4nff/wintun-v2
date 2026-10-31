/* SPDX-License-Identifier: GPL-2.0 */
/* Included by coalesce_path_tests after its accounting wrappers are defined. */
#pragma once
static PB_COALESCE *StateOutput;
static ULONG StateCalls,StateBegins,StateMerged,StateFallbacks,StateDirect,StatePrivate;
static PB_PACKET_STATUS StateBegin(PB_COALESCE *state,uint8_t *output,uint32_t capacity,const uint8_t *packet,uint32_t length)
{
    CHECK(StateOutput);++StateBegins;
    if(state==StateOutput) ++StateDirect;else ++StatePrivate;
    return Begin(state,output,capacity,packet,length);
}
#undef PbCoalesceBegin
#define PbCoalesceBegin StateBegin
#define PbV2TryCoalesce ActualStateTry
#ifndef PB_COALESCE_STATE_ACTUAL_SOURCE
#define PB_COALESCE_STATE_ACTUAL_SOURCE "../../driver/v2/session_coalesce.h"
#endif
#include PB_COALESCE_STATE_ACTUAL_SOURCE
#undef PbV2TryCoalesce
#undef PbCoalesceBegin
#define PbCoalesceBegin Begin
static ULONG PbV2TryCoalesce(PB_KERNEL_QUEUE *q,const PB_DESCRIPTOR *items,const BOOLEAN *valid,
    ULONG count,ULONG enabled,ULONG *copied,PB_COALESCE *state)
{
    CHECK(!StateOutput);++StateCalls;memset(state,0xa5,sizeof(*state));StateOutput=state;
    ULONG group=ActualStateTry(q,items,valid,count,enabled,copied,state);StateOutput=NULL;
    if(group>1) {CHECK(*copied<2 && state->Buffer==q->CoalesceBuffers[*copied] && state->Count==group && state->Finalized);++StateMerged;}
    else {CHECK(group==1 && *copied==MAXULONG);++StateFallbacks;}
    return group;
}
