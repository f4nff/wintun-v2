/* SPDX-License-Identifier: GPL-2.0 */
/* Production batch validators versus an exact per-slot ownership model. The
 * model has no probabilistic filter; guarded sources and pools bound accesses. */
#include "../safety/guard_memory.h"
#include <stdlib.h>
#include <string.h>
#include "../../common/v2_core.h"
#define CHECK SAFETY_CHECK
#ifdef PB_VALIDATION_PROBES
static ULONG64 DuplicateComparisons,FilterProbes;
#endif
#ifndef PB_VALIDATION_CORE_SOURCE
#define PB_VALIDATION_CORE_SOURCE "../../common/v2_core.c"
#endif
#include PB_VALIDATION_CORE_SOURCE
static ULONG64 Cases;
static PB_RESULT Model(PB_POOL *p,const PB_TOKEN *tokens,const PB_DESCRIPTOR *items,ULONG count,ULONG state,BOOLEAN importing)
{
    if(!count || count>PB_MAX_BATCH || (importing?!items:!tokens)) return PB_INVALID;
    if(importing && (!p || !p->Slots || !p->Count || p->Count>PB_MAX_SLOTS)) return PB_INVALID;
    BOOLEAN seen[PB_MAX_SLOTS]={0};
    for(ULONG i=0;i<count;++i) {
        const PB_TOKEN *t=importing?&items[i].Token:tokens+i;
        if(!p || !p->Slots || t->Epoch!=p->Epoch || t->Queue!=p->Queue || t->Slot>=p->Count || !t->Generation) return PB_STALE;
        PB_SLOT *slot=p->Slots+t->Slot;
        if(importing?t->Generation<=slot->Generation:t->Generation!=slot->Generation) return PB_STALE;
        if(slot->State!=(importing?PB_SLOT_FREE:state)) return PB_BUSY;
        if(importing && !(p->FreeBits[t->Slot/32]&(1u<<(t->Slot&31)))) return PB_CORRUPT;
        CHECK(t->Slot<PB_MAX_SLOTS);
        if(seen[t->Slot]) return PB_INVALID;
        seen[t->Slot]=TRUE;
    }
    if(importing) for(ULONG i=0;i<count;++i) {
        ULONG slot=items[i].Token.Slot;p->Slots[slot].Generation=items[i].Token.Generation;p->Slots[slot].State=PB_SLOT_PUBLISHED;
        p->FreeBits[slot/32]&=~(1u<<(slot&31));
    }
    return PB_OK;
}
static VOID Reset(PB_POOL *p,PB_SLOT *slots,ULONG size,ULONG state,BOOLEAN importing)
{
    CHECK(PbPoolInitialize(p,slots,size,42,3)==PB_OK);
    for(ULONG i=0;i<size;++i) {slots[i].Generation=importing?16:17;slots[i].State=importing?PB_SLOT_FREE:state;}
    if(!importing && state!=PB_SLOT_FREE) memset(p->FreeBits,0,sizeof(p->FreeBits));
}
static VOID CheckCase(PB_POOL *p,const PB_TOKEN *tokens,const PB_DESCRIPTOR *items,ULONG count,ULONG state,BOOLEAN importing)
{
    PB_SLOT expectedSlots[PB_MAX_SLOTS];memcpy(expectedSlots,p->Slots,p->Count*sizeof(*p->Slots));
    PB_POOL expected=*p;expected.Slots=expectedSlots;
    PB_RESULT want=Model(&expected,tokens,items,count,state,importing);
    PB_RESULT got=importing?PbPoolImport(p,items,count):PbValidateTokens(p,tokens,count,state);
    expected.Slots=p->Slots;
    CHECK(got==want && !memcmp(&expected,p,sizeof(*p)) && !memcmp(expectedSlots,p->Slots,p->Count*sizeof(*p->Slots)));
    Cases++;
}
static ULONG Slot(ULONG i,ULONG size,ULONG shape)
{
    return (shape==0?i:shape==1?size-1-i%size:shape==2?(i%8)*64+i/8:i*73+31)%size;
}
static VOID Matrix(VOID)
{
    const ULONG sizes[]={1,2,3,7,8,31,32,33,63,64,65,127,128,255,256,511,512};
    PB_TEST_GUARD input=PbTestGuardCreate(PB_MAX_BATCH*sizeof(PB_DESCRIPTOR));
    PB_POOL *p=PbTestGuardAlloc(sizeof(*p));
    for(ULONG si=0;si<_countof(sizes);++si) {
        ULONG size=sizes[si];PB_SLOT *slots=PbTestGuardAlloc(size*sizeof(*slots));
        for(ULONG count=1;count<=PB_MAX_BATCH;++count) for(ULONG shape=0;shape<4;++shape) for(ULONG mode=0;mode<2;++mode) {
            BOOLEAN importing=(BOOLEAN)mode;ULONG state=(count+shape)%6;
            PB_TOKEN base[PB_MAX_BATCH],tokens[PB_MAX_BATCH];PB_DESCRIPTOR *items=(PB_DESCRIPTOR *)PbTestGuardSpan(&input,count*sizeof(*items));
            for(ULONG i=0;i<count;++i) base[i]=(PB_TOKEN){42,17,(USHORT)Slot(i,size,shape),3};
            for(ULONG fault=0;fault<9;++fault) for(ULONG position=0;position<(fault?count:1);++position) {
                Reset(p,slots,size,state,importing);memcpy(tokens,base,count*sizeof(*tokens));
                PB_TOKEN *t=tokens+position;
                if(fault==1) t->Epoch++;
                if(fault==2) t->Queue++;
                if(fault==3) t->Slot=(USHORT)size;
                if(fault==4) t->Generation=0;
                if(fault==5) t->Generation=16;
                if(fault==6) slots[t->Slot].State=importing?PB_SLOT_RESERVED:(state+1)%6;
                if(fault==7) {if(importing) p->FreeBits[t->Slot/32]&=~(1u<<(t->Slot&31));else t->Generation=UINT32_MAX;}
                if(fault==8) {if(position) tokens[position]=tokens[position-1];else t->Slot=UINT16_MAX;}
                memset(items,0xa5,count*sizeof(*items));for(ULONG i=0;i<count;++i) items[i].Token=tokens[i];
                PB_DESCRIPTOR saved[PB_MAX_BATCH];memcpy(saved,items,count*sizeof(*items));
                if(importing) CheckCase(p,NULL,items,count,state,TRUE);
                else {
                    /* Put the dense token array exactly against its guard too. */
                    PB_TOKEN *guarded=(PB_TOKEN *)PbTestGuardSpan(&input,count*sizeof(*guarded));
                    memcpy(guarded,tokens,count*sizeof(*guarded));CheckCase(p,guarded,NULL,count,state,FALSE);
                    CHECK(!memcmp(guarded,tokens,count*sizeof(*guarded)));
                }
                if(importing) CHECK(!memcmp(saved,items,count*sizeof(*items)));
            }
        }
        PbTestGuardFree(slots);
    }
    PbTestGuardFree(p);PbTestGuardDestroy(&input);
    printf("PASS: token validation/import model matrix: %llu cases, all batch lengths, 17 pool boundaries, sequential/reverse/strided/mixed slots, every error position, all slot states, exact first errors and transactional pool/index/Next equality.\n",Cases);
}
static VOID DuplicatePositions(VOID)
{
    PB_POOL *p=PbTestGuardAlloc(sizeof(*p));PB_SLOT *slots=PbTestGuardAlloc(PB_MAX_SLOTS*sizeof(*slots));
    PB_TEST_GUARD input=PbTestGuardCreate(PB_MAX_BATCH*sizeof(PB_DESCRIPTOR));ULONG64 before=Cases;
    for(ULONG count=2;count<=PB_MAX_BATCH;++count) for(ULONG first=0;first<count;++first) for(ULONG second=first+1;second<count;++second)
        for(ULONG mode=0;mode<2;++mode) for(ULONG precedence=0;precedence<3;++precedence) {
            BOOLEAN importing=(BOOLEAN)mode;Reset(p,slots,PB_MAX_SLOTS,PB_SLOT_PUBLISHED,importing);
            PB_TOKEN tokens[PB_MAX_BATCH];PB_DESCRIPTOR *items=(PB_DESCRIPTOR *)PbTestGuardSpan(&input,count*sizeof(*items));
            for(ULONG i=0;i<count;++i) tokens[i]=(PB_TOKEN){42,17,(USHORT)((i%8)*64+i/8),3};
            tokens[second]=tokens[first];
            if(precedence==1) tokens[second].Generation=0; /* This token's stale status precedes duplicate. */
            if(precedence==2 && second+1<count) tokens[second+1].Epoch++; /* An earlier duplicate precedes later stale. */
            memset(items,0x5a,count*sizeof(*items));for(ULONG i=0;i<count;++i) items[i].Token=tokens[i];
            CheckCase(p,tokens,items,count,PB_SLOT_PUBLISHED,importing);
        }
    PbTestGuardDestroy(&input);PbTestGuardFree(slots);PbTestGuardFree(p);
    printf("PASS: all duplicate position pairs and error precedence: %llu checks; same-bit distinct slots never authorize or reject ownership on their own.\n",Cases-before);
}
static VOID RejectionsAndFootprint(VOID)
{
    PB_POOL p,saved;PB_SLOT slots[2];Reset(&p,slots,2,PB_SLOT_PUBLISHED,FALSE);saved=p;
    PB_TOKEN token={42,17,0,3};PB_DESCRIPTOR item={.Token=token};
    CHECK(PbValidateTokens(&p,NULL,1,PB_SLOT_PUBLISHED)==PB_INVALID && PbValidateTokens(&p,&token,0,PB_SLOT_PUBLISHED)==PB_INVALID);
    CHECK(PbValidateTokens(&p,&token,PB_MAX_BATCH+1,PB_SLOT_PUBLISHED)==PB_INVALID && PbValidateTokens(NULL,&token,1,PB_SLOT_PUBLISHED)==PB_STALE);
    PB_POOL invalid=p;invalid.Slots=NULL;CHECK(PbValidateTokens(&invalid,&token,1,PB_SLOT_PUBLISHED)==PB_STALE);
    CHECK(PbPoolImport(NULL,&item,1)==PB_INVALID && PbPoolImport(&invalid,&item,1)==PB_INVALID && PbPoolImport(&p,NULL,1)==PB_INVALID);
    CHECK(PbPoolImport(&p,&item,0)==PB_INVALID && PbPoolImport(&p,&item,PB_MAX_BATCH+1)==PB_INVALID && !memcmp(&p,&saved,sizeof(p)));
    PB_TEST_GUARD input=PbTestGuardCreate(sizeof(PB_DESCRIPTOR));
    PB_TOKEN *t=(PB_TOKEN *)PbTestGuardSpan(&input,sizeof(*t));*t=token;
    CHECK(PbValidateTokens(&p,t,1,PB_SLOT_PUBLISHED)==PB_OK);
    t->Epoch++;CHECK(PbValidateTokens(&p,t,PB_MAX_BATCH,PB_SLOT_PUBLISHED)==PB_STALE);
    PB_DESCRIPTOR *d=(PB_DESCRIPTOR *)PbTestGuardSpan(&input,sizeof(*d));*d=item;d->Token.Epoch++;
    CHECK(PbPoolImport(&p,d,PB_MAX_BATCH)==PB_STALE);
    PbTestGuardDestroy(&input);
    puts("PASS: null/invalid requests, exact one-item guarded input and first-error short circuit without reading a later item; no source/pool mutation.");
}
#ifdef PB_VALIDATION_PROBES
static VOID Cost(ULONG count,BOOLEAN collision,BOOLEAN importing)
{
    PB_POOL p;PB_SLOT slots[PB_MAX_SLOTS];PB_TOKEN tokens[PB_MAX_BATCH];PB_DESCRIPTOR items[PB_MAX_BATCH]={0};
    Reset(&p,slots,PB_MAX_SLOTS,PB_SLOT_PUBLISHED,importing);
    for(ULONG i=0;i<count;++i) {tokens[i]=(PB_TOKEN){42,17,(USHORT)(collision?(i%8)*64+i/8:i),3};items[i].Token=tokens[i];}
    DuplicateComparisons=FilterProbes=0;
    CHECK((importing?PbPoolImport(&p,items,count):PbValidateTokens(&p,tokens,count,PB_SLOT_PUBLISHED))==PB_OK);
#ifdef PB_VALIDATION_BASELINE
    CHECK(DuplicateComparisons==(ULONG64)count*(count-1)/2 && !FilterProbes);
#else
    CHECK(DuplicateComparisons<=(ULONG64)count*(count-1)/2 && FilterProbes==(count<=8?0:count));
    if(count<=8) CHECK(DuplicateComparisons==(ULONG64)count*(count-1)/2);
    else if(!collision) CHECK(!DuplicateComparisons);
#endif
    printf("PASS: actual %s count=%lu collision=%u duplicate_comparisons=%llu filter_probes=%llu.\n",importing?"import":"validation",count,collision,DuplicateComparisons,FilterProbes);
}
#endif
int main(VOID)
{
    setvbuf(stdout,NULL,_IONBF,0);DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Matrix();DuplicatePositions();RejectionsAndFootprint();
#ifdef PB_VALIDATION_PROBES
    for(ULONG mode=0;mode<2;++mode) {Cost(1,FALSE,(BOOLEAN)mode);Cost(8,FALSE,(BOOLEAN)mode);Cost(32,FALSE,(BOOLEAN)mode);Cost(8,TRUE,(BOOLEAN)mode);Cost(32,TRUE,(BOOLEAN)mode);}
#endif
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after) && before==after);return 0;
}
