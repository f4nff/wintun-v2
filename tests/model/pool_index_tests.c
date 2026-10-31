/* SPDX-License-Identifier: GPL-2.0 */
/* Actual pool core versus an independent round-robin state model. Guarded
 * storage checks non-power-of-two tails; no SYS timing or driver execution. */
#include "../safety/guard_memory.h"
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#define CHECK SAFETY_CHECK
#ifdef PB_POOL_PROBES
static ULONG64 SlotProbes,WordProbes;
#endif
#ifndef PB_POOL_CORE_SOURCE
#define PB_POOL_CORE_SOURCE "../../common/v2_core.c"
#endif
#include PB_POOL_CORE_SOURCE
typedef struct MODEL {PB_SLOT Slots[PB_MAX_SLOTS];uint32_t Count,Next;} MODEL;
static PB_RESULT ModelReserve(MODEL *m,PB_TOKEN *token)
{
    uint32_t at=m->Next%m->Count;
    for(uint32_t n=0;n<m->Count;++n) {
        uint32_t slot=at;at=(at+1u)%m->Count;PB_SLOT *s=m->Slots+slot;
        if(s->State!=PB_SLOT_FREE) continue;
        if(s->Generation==UINT32_MAX) {s->State=PB_SLOT_RETIRED;continue;}
        s->Generation++;s->State=PB_SLOT_RESERVED;m->Next=at;
        *token=(PB_TOKEN){42,s->Generation,(uint16_t)slot,3};return PB_OK;
    }
    m->Next=at;return PB_FULL;
}
static VOID CheckPool(PB_POOL *p,const MODEL *m)
{
    CHECK(p->Next==m->Next && !memcmp(p->Slots,m->Slots,m->Count*sizeof(*m->Slots)));
#ifndef PB_POOL_INDEX_BASELINE
    for(uint32_t i=0;i<PB_MAX_SLOTS;++i) {
        BOOLEAN expected=i<p->Count && p->Slots[i].State==PB_SLOT_FREE;
        CHECK(!!(p->FreeBits[i/32u]&(1u<<(i&31u)))==expected);
    }
#endif
}
static PB_RESULT Reserve(PB_POOL *p,MODEL *m,PB_TOKEN *token)
{
    PB_TOKEN expected;memset(&expected,0xa5,sizeof(expected));memset(token,0xa5,sizeof(*token));
    PB_RESULT want=ModelReserve(m,&expected),got=PbPoolReserve(p,token);
    CHECK(got==want && !memcmp(token,&expected,sizeof(*token)));CheckPool(p,m);return got;
}
static VOID Transition(PB_POOL *p,MODEL *m,PB_TOKEN token,uint32_t before,uint32_t after)
{
    CHECK(m->Slots[token.Slot].State==before && m->Slots[token.Slot].Generation==token.Generation);
    CHECK(PbPoolTransition(p,&token,before,after)==PB_OK);m->Slots[token.Slot].State=after;CheckPool(p,m);
}
static VOID AllSizes(VOID)
{
    ULONG64 sparse=0;
    for(uint32_t count=1;count<=PB_MAX_SLOTS;++count) {
        PB_POOL *p=PbTestGuardAlloc(sizeof(*p));PB_SLOT *slots=PbTestGuardAlloc(count*sizeof(*slots));MODEL m={.Count=count};PB_TOKEN held[PB_MAX_SLOTS],extra;
        CHECK(PbPoolInitialize(p,slots,count,42,3)==PB_OK);CheckPool(p,&m);
        for(uint32_t i=0;i<count;++i) CHECK(Reserve(p,&m,held+i)==PB_OK && held[i].Slot==i);
        CHECK(Reserve(p,&m,&extra)==PB_FULL);
        for(uint32_t freeSlot=0;freeSlot<count;++freeSlot) {
            Transition(p,&m,held[freeSlot],PB_SLOT_RESERVED,PB_SLOT_FREE);
            p->Next=m.Next=(freeSlot+1u)%count; /* The only free slot is last in round-robin order. */
            CHECK(Reserve(p,&m,held+freeSlot)==PB_OK && held[freeSlot].Slot==freeSlot);sparse++;
        }
        Transition(p,&m,held[count-1],PB_SLOT_RESERVED,PB_SLOT_FREE);p->Next=m.Next=UINT32_MAX-1;
        CHECK(Reserve(p,&m,held+count-1)==PB_OK && p->Next<count);
        for(uint32_t i=0;i<count;++i) Transition(p,&m,held[i],PB_SLOT_RESERVED,PB_SLOT_FREE);
        PbTestGuardFree(slots);PbTestGuardFree(p);
    }
    printf("PASS: all 512 pool sizes, %llu sole-free-slot round-robin cycles, full/token preservation, uint32 Next wrap and guarded index tails match the model.\n",sparse);
}
static uint32_t Random(uint32_t *state) {*state=*state*1664525u+1013904223u;return *state;}
static VOID MixedStates(VOID)
{
    const uint32_t counts[]={1,3,31,32,33,63,64,65,255,256,257,511,512};
    for(uint32_t c=0;c<_countof(counts);++c) {
        uint32_t count=counts[c],seed=count;PB_POOL *p=PbTestGuardAlloc(sizeof(*p));PB_SLOT *slots=PbTestGuardAlloc(count*sizeof(*slots));MODEL m={.Count=count};PB_TOKEN held[PB_MAX_SLOTS];
        CHECK(PbPoolInitialize(p,slots,count,42,3)==PB_OK);
        for(uint32_t step=0;step<20000;++step) {
            uint32_t draw=Random(&seed),slot=(draw>>8)%count,state=m.Slots[slot].State;
            if((draw&3)==0) {PB_TOKEN token;if(Reserve(p,&m,&token)==PB_OK) held[token.Slot]=token;}
            else if(state==PB_SLOT_FREE && (draw&31)==1) {p->Slots[slot].Generation=m.Slots[slot].Generation=UINT32_MAX;}
            else if(state>=PB_SLOT_RESERVED && state<=PB_SLOT_COMPLETE) {
                uint32_t after=state==PB_SLOT_RESERVED?((draw&4)?PB_SLOT_FREE:PB_SLOT_PUBLISHED):
                    state==PB_SLOT_PUBLISHED?((draw&4)?PB_SLOT_FREE:(draw&8)?PB_SLOT_NDIS:PB_SLOT_COMPLETE):state==PB_SLOT_NDIS?PB_SLOT_COMPLETE:PB_SLOT_FREE;
                PB_TOKEN bad=held[slot];bad.Epoch++;
                CHECK(PbPoolTransition(p,&bad,state,after)==PB_STALE);CheckPool(p,&m);
                Transition(p,&m,held[slot],state,after);
            }
            CheckPool(p,&m);
        }
        PbTestGuardFree(slots);PbTestGuardFree(p);
    }
    puts("PASS: 260000 mixed operation/model checks with out-of-order releases, stale tokens and generation exhaustion; exact slot generations/states/Next and index invariants.");
}
static VOID ExhaustedGenerations(VOID)
{
    const uint32_t counts[]={1,31,32,33,511,512};
    for(uint32_t c=0;c<_countof(counts);++c) {
        uint32_t count=counts[c];PB_POOL *p=PbTestGuardAlloc(sizeof(*p));PB_SLOT *slots=PbTestGuardAlloc(count*sizeof(*slots));MODEL m={.Count=count};PB_TOKEN token;
        CHECK(PbPoolInitialize(p,slots,count,42,3)==PB_OK);
        for(uint32_t i=0;i<count;++i) slots[i].Generation=m.Slots[i].Generation=UINT32_MAX;
        p->Next=m.Next=UINT32_MAX-1;CHECK(Reserve(p,&m,&token)==PB_FULL);
        for(uint32_t i=0;i<count;++i) CHECK(slots[i].State==PB_SLOT_RETIRED);
        CHECK(Reserve(p,&m,&token)==PB_FULL);PbTestGuardFree(slots);PbTestGuardFree(p);
    }
    puts("PASS: fully generation-exhausted pools retire every candidate once, preserve the failed token/normalized Next and remain full without generation reuse.");
}
#ifndef PB_POOL_INDEX_BASELINE
static VOID RejectImport(PB_POOL *p,PB_DESCRIPTOR *items,uint32_t count,PB_RESULT expected)
{
    PB_POOL saved=*p;PB_SLOT slots[PB_MAX_SLOTS];memcpy(slots,p->Slots,p->Count*sizeof(*slots));
    CHECK(PbPoolImport(p,items,count)==expected && !memcmp(p,&saved,sizeof(saved)) && !memcmp(slots,p->Slots,p->Count*sizeof(*slots)));
}
static VOID Imports(VOID)
{
    PB_POOL *p=PbTestGuardAlloc(sizeof(*p));PB_SLOT *slots=PbTestGuardAlloc(65*sizeof(*slots));PB_DESCRIPTOR items[3]={0};
    CHECK(PbPoolInitialize(p,slots,65,42,3)==PB_OK);
    for(uint32_t i=0;i<3;++i) items[i].Token=(PB_TOKEN){42,10+i,(uint16_t)(32*i),3};
    CHECK(PbPoolImport(p,items,3)==PB_OK && !p->Next);
    for(uint32_t i=0;i<3;++i) CHECK(slots[32*i].State==PB_SLOT_PUBLISHED && !(p->FreeBits[i]&1));
    PB_DESCRIPTOR bad[2]={0};bad[0].Token=(PB_TOKEN){42,1,1,3};bad[1].Token=(PB_TOKEN){42,20,32,3};
    RejectImport(p,bad,2,PB_BUSY);bad[1]=bad[0];RejectImport(p,bad,2,PB_INVALID);
    bad[1].Token.Slot=65;RejectImport(p,bad,2,PB_STALE);bad[1].Token.Slot=2;
    bad[1].Token.Epoch++;RejectImport(p,bad,2,PB_STALE);bad[1].Token.Epoch=42;
    bad[1].Token.Generation=0;RejectImport(p,bad,2,PB_STALE);bad[1].Token.Generation=1;
    bad[1].Token.Queue=4;RejectImport(p,bad,2,PB_STALE);bad[1].Token.Queue=3;
    RejectImport(p,NULL,1,PB_INVALID);RejectImport(p,bad,0,PB_INVALID);RejectImport(p,bad,33,PB_INVALID);
    CHECK(PbPoolTransition(p,&items[0].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);
    RejectImport(p,items,1,PB_STALE);items[0].Token.Generation++;
    CHECK(PbPoolImport(p,items,1)==PB_OK);
    CHECK(PbPoolTransition(p,&items[2].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);
    items[2].Token.Generation=UINT32_MAX;CHECK(PbPoolImport(p,items+2,1)==PB_OK);
    CHECK(PbPoolTransition(p,&items[2].Token,PB_SLOT_PUBLISHED,PB_SLOT_FREE)==PB_OK);
    p->Next=64;PB_TOKEN token;CHECK(PbPoolReserve(p,&token)==PB_OK && token.Slot==1 && slots[64].State==PB_SLOT_RETIRED && !p->FreeBits[2]);
    PB_TOKEN forged=token;forged.Generation--;CHECK(PbPoolTransition(p,&forged,PB_SLOT_RESERVED,PB_SLOT_FREE)==PB_STALE);
    CHECK(!(p->FreeBits[token.Slot/32u]&(1u<<(token.Slot&31u))));
    PbTestGuardFree(slots);PbTestGuardFree(p);
    puts("PASS: transactional mirrored imports, scoped/new generations, duplicate/busy/stale/malformed batch rollback, release/reimport and UINT32_MAX permanent retirement.");
}
static VOID InvalidCandidates(VOID)
{
    PB_POOL *p=PbTestGuardAlloc(sizeof(*p));PB_SLOT *slots=PbTestGuardAlloc(33*sizeof(*slots));PB_TOKEN token,held[33];
    CHECK(PbPoolInitialize(p,slots,33,42,3)==PB_OK);for(uint32_t i=0;i<33;++i) CHECK(PbPoolReserve(p,held+i)==PB_OK);
    p->FreeBits[1]=1u<<31; /* A damaged private index must not read slot 63 beyond the guard. */
    CHECK(PbPoolReserve(p,&token)==PB_CORRUPT);p->FreeBits[1]=0;p->FreeBits[0]=1u<<31;
    CHECK(PbPoolReserve(p,&token)==PB_CORRUPT && slots[31].State==PB_SLOT_RESERVED && slots[31].Generation==1);
    PbTestGuardFree(slots);PbTestGuardFree(p);puts("PASS: damaged private index candidates cannot reserve a busy slot or read beyond a non-power-of-two pool.");
}
#endif
#ifdef PB_POOL_PROBES
static VOID Cost(BOOLEAN full)
{
    PB_POOL p;PB_SLOT slots[512];PB_TOKEN held[512],token;CHECK(PbPoolInitialize(&p,slots,512,42,3)==PB_OK);
    for(uint32_t i=0;i<512;++i) CHECK(PbPoolReserve(&p,held+i)==PB_OK);
    if(!full) CHECK(PbPoolTransition(&p,held+511,PB_SLOT_RESERVED,PB_SLOT_FREE)==PB_OK);
    SlotProbes=WordProbes=0;
    for(uint32_t i=0;i<2000;++i) {
        CHECK(PbPoolReserve(&p,&token)==(full?PB_FULL:PB_OK));
        if(!full) CHECK(token.Slot==511 && PbPoolTransition(&p,&token,PB_SLOT_RESERVED,PB_SLOT_FREE)==PB_OK);
    }
#ifdef PB_POOL_INDEX_BASELINE
    CHECK(SlotProbes==2000ull*512 && !WordProbes);
#else
    CHECK(SlotProbes==2000ull*(full?1:2) && WordProbes==2000ull*16);
#endif
    printf("PASS: 512-slot %s: attempts=2000 slot_state_probes=%llu search_word_probes=%llu.\n",full?"full":"one-free",SlotProbes,WordProbes);
}
#endif
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    AllSizes();MixedStates();ExhaustedGenerations();
#ifndef PB_POOL_INDEX_BASELINE
    Imports();InvalidCandidates();
#endif
#ifdef PB_POOL_PROBES
    Cost(FALSE);Cost(TRUE);
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
