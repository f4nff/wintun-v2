/* SPDX-License-Identifier: GPL-2.0 */
/* Direct production pump tests. Scripted native boundaries make every accepted
 * prefix observable, including failures that are difficult to time in threads. */
static struct {
    BYTE *Data;
    uint32_t ReserveMax, CommitMax, ReleaseMax, ReceiveCount, Length, BadLease;
    uint32_t Reserved, Committed, Cancelled, Released, Receives;
    PB_RESULT ReserveResult, CommitResult, CancelResult, ReleaseResult;
} Pump;
static PB_RESULT WINAPI PumpReserve(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    (void)q;*done=0;if(Pump.ReserveResult!=PB_OK)return Pump.ReserveResult;
    *done=count<Pump.ReserveMax?count:Pump.ReserveMax;Pump.Reserved+=*done;
    for(uint32_t i=0;i<*done;++i) {
        memset(leases+i,0,sizeof(*leases));leases[i].Data=Pump.Data+i*PB_BLOCK_SIZE;leases[i].Capacity=65535;
        leases[i].Descriptor.Token.Slot=(uint16_t)i;
        if(Pump.BadLease==1)leases[i].Capacity=0;if(Pump.BadLease==2)leases[i].Data=NULL;
    }
    return PB_OK;
}
static PB_RESULT WINAPI PumpCommit(PB_USER_QUEUE *q,const PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    (void)q;*done=count<Pump.CommitMax?count:Pump.CommitMax;
    for(uint32_t i=0;i<*done;++i) {
        CHECK(leases[i].Descriptor.Length==32);
        for(unsigned j=0;j<32;++j)CHECK(((BYTE *)leases[i].Data)[j]==i+1);
    }
    Pump.Committed+=*done;return Pump.CommitResult;
}
static PB_RESULT WINAPI PumpCancel(PB_USER_QUEUE *q,const PB_TOKEN *tokens,uint32_t count)
{
    (void)q;for(uint32_t i=0;i<count;++i)CHECK(tokens[i].Slot==Pump.Committed+i);
    Pump.Cancelled+=count;return Pump.CancelResult;
}
static PB_RESULT WINAPI PumpReceive(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t count,uint32_t *done)
{
    (void)q;Pump.Receives++;*done=Pump.ReceiveCount;CHECK(*done<=count);
    for(uint32_t i=0;i<*done;++i) {
        memset(leases+i,0,sizeof(*leases));leases[i].Data=Pump.Data+i*PB_BLOCK_SIZE;leases[i].Capacity=65535;
        leases[i].Descriptor.Length=Pump.Length;leases[i].Descriptor.Token.Slot=(uint16_t)i;
        if(Pump.BadLease==1)leases[i].Capacity=0;if(Pump.BadLease==2)leases[i].Data=NULL;
        if(Pump.BadLease==3)leases[i].Descriptor.Type=PB_DESC_TCP_GSO;
    }
    return PB_OK;
}
static PB_RESULT WINAPI PumpRelease(PB_USER_QUEUE *q,const PB_TOKEN *tokens,uint32_t count,uint32_t *done)
{
    (void)q;*done=count<Pump.ReleaseMax?count:Pump.ReleaseMax;
    for(uint32_t i=0;i<*done;++i)CHECK(tokens[i].Slot==Pump.Released+i);
    Pump.Released+=*done;return Pump.ReleaseResult;
}
static void ResetPump(TUN_SESSION *s)
{
    BYTE *data=Pump.Data;memset(&Pump,0,sizeof(Pump));Pump.Data=data;
    Pump.ReserveMax=Pump.CommitMax=Pump.ReleaseMax=PB_MAX_BATCH;Pump.ReceiveCount=PB_MAX_BATCH;Pump.Length=32;
    memset(s->Descriptor.Receive.Ring,0,TUN_RING_SIZE(s->Capacity));
    memset(s->Descriptor.Send.Ring,0,TUN_RING_SIZE(s->Capacity));
}
static void PumpOutgoing(TUN_SESSION *s,uint32_t count,BOOL zeros)
{
    TUN_RING *ring=s->Descriptor.Receive.Ring;ULONG tail=0;
    for(uint32_t i=0;i<count;++i) {
        TUN_PACKET *p=(TUN_PACKET *)(ring->Data+tail);p->Size=32;memset(p->Data,(int)i+1,32);tail+=36;
        if(zeros){p=(TUN_PACKET *)(ring->Data+tail);p->Size=0;tail+=4;}
    }
    ring->Tail=tail;
}
static void PumpCases(void)
{
    TUN_SESSION s={0};s.Capacity=8*1024*1024;
    s.Descriptor.Send.Ring=VirtualAlloc(NULL,2*TUN_RING_SIZE(s.Capacity),MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    CHECK(s.Descriptor.Send.Ring);s.Descriptor.Receive.Ring=(TUN_RING *)((BYTE *)s.Descriptor.Send.Ring+TUN_RING_SIZE(s.Capacity));
    s.Descriptor.Send.TailMoved=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(s.Descriptor.Send.TailMoved);
    s.Api.ReserveBatch=PumpReserve;s.Api.CommitBatch=PumpCommit;s.Api.CancelBatch=PumpCancel;
    s.Api.ReceiveBatch=PumpReceive;s.Api.ReleaseBatch=PumpRelease;
    Pump.Data=VirtualAlloc(NULL,PB_MAX_BATCH*PB_BLOCK_SIZE,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);CHECK(Pump.Data);
    for(uint32_t count=1;count<=64;++count) {
        ResetPump(&s);PumpOutgoing(&s,count,FALSE);BOOL progress=FALSE;
        CHECK(CompatSendBatch(&s,NULL,&progress)==PB_OK&&progress&&Pump.Committed==count&&!Pump.Cancelled);
        CHECK(s.Descriptor.Receive.Ring->Head==s.Descriptor.Receive.Ring->Tail);
    }
    for(uint32_t accepted=0;accepted<=8;++accepted)for(unsigned zeros=0;zeros<2;++zeros) {
        ResetPump(&s);PumpOutgoing(&s,8,zeros);Pump.CommitMax=accepted;
        Pump.CommitResult=accepted?PB_OK:PB_FULL;BOOL progress=FALSE;
        CHECK(CompatSendBatch(&s,NULL,&progress)==PB_OK&&progress==(accepted!=0));
        CHECK(Pump.Committed==accepted&&Pump.Cancelled==8-accepted);
        CHECK(s.Descriptor.Receive.Ring->Head==accepted*(zeros?40:36));
    }
    PB_RESULT backpressure[]={PB_FULL,PB_EMPTY};
    for(unsigned i=0;i<2;++i) {
        ResetPump(&s);PumpOutgoing(&s,8,FALSE);Pump.ReserveResult=backpressure[i];BOOL progress=FALSE;
        CHECK(CompatSendBatch(&s,NULL,&progress)==PB_OK&&!progress&&!Pump.Committed&&!Pump.Cancelled);
    }
    for(unsigned bad=1;bad<=2;++bad) {
        ResetPump(&s);PumpOutgoing(&s,8,FALSE);Pump.BadLease=bad;BOOL progress=FALSE;
        CHECK(CompatSendBatch(&s,NULL,&progress)==PB_CORRUPT&&!progress&&Pump.Cancelled==8);
    }
    ResetPump(&s);PumpOutgoing(&s,8,FALSE);Pump.ReserveMax=0;BOOL progress=FALSE;
    CHECK(CompatSendBatch(&s,NULL,&progress)==PB_CORRUPT&&!progress);
    ResetPump(&s);PumpOutgoing(&s,8,FALSE);Pump.CommitMax=0;Pump.CommitResult=PB_STOPPED;
    CHECK(CompatSendBatch(&s,NULL,&progress)==PB_STOPPED&&!progress&&Pump.Cancelled==8);
    ResetPump(&s);PumpOutgoing(&s,8,FALSE);Pump.CommitMax=0;Pump.CancelResult=PB_CORRUPT;
    CHECK(CompatSendBatch(&s,NULL,&progress)==PB_CORRUPT&&!progress&&Pump.Cancelled==8);
    ResetPump(&s);s.Descriptor.Receive.Ring->Tail=4;((TUN_PACKET *)s.Descriptor.Receive.Ring->Data)->Size=MAXDWORD;
    CHECK(CompatSendBatch(&s,NULL,&progress)==PB_CORRUPT&&!Pump.Reserved);
    /* The byte budget, rather than object count, bounds maximum-size copying. */
    ResetPump(&s);Pump.Length=65535;COMPAT_PENDING pending={0};progress=FALSE;
    CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_OK&&progress&&Pump.Released==8&&pending.Count==64&&pending.Copied==8);
    CHECK(s.Descriptor.Send.Ring->Tail==8*TUN_ALIGN(65539));
    Pump.ReleaseMax=1;CompatReturnPending(&s,NULL,&pending);
    CHECK(!pending.Count&&Pump.Released==64); /* A stopped return can still accept only a prefix. */
    for(unsigned bad=1;bad<=3;++bad) {
        ResetPump(&s);Pump.BadLease=bad;pending=(COMPAT_PENDING){0};progress=FALSE;
        CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_CORRUPT&&!progress&&!Pump.Released);
    }
    ResetPump(&s);Pump.Length=65536;pending=(COMPAT_PENDING){0};progress=FALSE;
    CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_CORRUPT&&!Pump.Released);
    ResetPump(&s);Pump.ReceiveCount=0;pending=(COMPAT_PENDING){0};
    CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_CORRUPT&&!pending.Count);
    ResetPump(&s);Pump.ReleaseMax=0;Pump.ReleaseResult=PB_FULL;pending=(COMPAT_PENDING){0};progress=FALSE;
    CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_OK&&progress&&pending.Copied==64&&!pending.Retired);
    ULONG tail=s.Descriptor.Send.Ring->Tail;progress=FALSE;
    CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_OK&&!progress&&Pump.Receives==1&&s.Descriptor.Send.Ring->Tail==tail);
    Pump.ReleaseMax=1;Pump.ReleaseResult=PB_OK;progress=FALSE;
    CHECK(CompatReceiveBatch(&s,NULL,&pending,&progress)==PB_OK&&progress&&pending.Retired==1&&Pump.Receives==1&&s.Descriptor.Send.Ring->Tail==tail);
    CompatReturnPending(&s,NULL,&pending);CHECK(!pending.Count&&Pump.Released==64);
    CHECK(VirtualFree(Pump.Data,0,MEM_RELEASE)&&VirtualFree(s.Descriptor.Send.Ring,0,MEM_RELEASE)&&CloseHandle(s.Descriptor.Send.TailMoved));
    puts("PASS: production pump batches 1..64, zero-length boundaries, every commit prefix 0..8, cancellation failures, malformed leases, 512 KiB budget, release backpressure/retry, bounded stopped partial-prefix retirement.");
}
