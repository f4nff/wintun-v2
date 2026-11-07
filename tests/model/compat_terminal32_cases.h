/* SPDX-License-Identifier: GPL-2.0 */
/* Public old-ABI pointers survive the hosted native terminal while the byte
 * rings remain owned by their public session. Native IRQ/NDIS and scheduler
 * park are not modeled by this test. End never overlaps public packet users. */
static struct {volatile LONG GateArmed;BOOL Receive;HANDLE Entered,Continue;} Terminal32;
static DWORD Terminal32CasesCount,Terminal32PublicJoins;
static VOID Terminal32Gate(VOID)
{
    if(InterlockedCompareExchange(&Terminal32.GateArmed,0,1)==1){
        CHECK(!PbTestWaiters(Native)&&SetEvent(Terminal32.Entered));
        CHECK(WaitForSingleObject(Terminal32.Continue,10000)==WAIT_OBJECT_0);
    }
}
static PB_RESULT WINAPI Terminal32Reserve(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t count,uint32_t *done)
{if(!Terminal32.Receive)Terminal32Gate();return RecordedReserve(q,leases,count,done);}
static PB_RESULT WINAPI Terminal32Receive(PB_USER_QUEUE *q,PB_LEASE *leases,uint32_t count,uint32_t *done)
{if(Terminal32.Receive)Terminal32Gate();return RecordedReceive(q,leases,count,done);}
typedef struct TERMINAL32_CALL {TUN_SESSION *Session;BYTE **Packets;const BYTE *Expected;HANDLE Start;DWORD First,Stride,Calls;} TERMINAL32_CALL;
static DWORD WINAPI Terminal32Caller(VOID *opaque)
{
    TERMINAL32_CALL *job=opaque;CHECK(WaitForSingleObject(job->Start,10000)==WAIT_OBJECT_0);
    LONG minimum=job->First?1:0;
    for(LONG i=(LONG)job->First;i>=minimum;i-=(LONG)job->Stride){
        if(Terminal32.Receive){CHECK(!memcmp(job->Packets[i],job->Expected+(SIZE_T)i*1500,1500));WintunReleaseReceivePacket(job->Session,job->Packets[i]);}
        else WintunSendPacket(job->Session,job->Packets[i]);
        job->Calls++;
        if(!i)break;
    }
    return 0;
}
static VOID Terminal32Join(HANDLE thread)
{DWORD code;CHECK(WaitForSingleObject(thread,10000)==WAIT_OBJECT_0&&GetExitCodeThread(thread,&code)&&!code&&CloseHandle(thread));++Terminal32PublicJoins;}
static VOID Terminal32Idle(VOID)
{ULONGLONG end=GetTickCount64()+10000;while(!PbTestWaiters(Native)){CHECK(GetTickCount64()<end);SwitchToThread();}}
static VOID Terminal32Case(BOOL ipv6,BOOL udp,BOOL receive,BOOL wrap,BOOL before,const char *phase)
{
    CHECK(!ReadAcquire(&Tun32.Enabled)&&!BatchMode);memset(&Tun32,0,sizeof(Tun32));memset(&Terminal32,0,sizeof(Terminal32));Terminal32.Receive=receive;
    Terminal32.Entered=CreateEventW(NULL,TRUE,FALSE,NULL);Terminal32.Continue=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(Terminal32.Entered&&Terminal32.Continue);
    InterlockedExchange(&Tun32.Enabled,TRUE);BatchMode=1;ReserveCalls=CommitCalls=ReceiveCalls=ReleaseCalls=Cancelled=ReadSignals=WakeSignals=ReleasedPackets=0;
    CHECK(!ReserveLimit&&!CommitLimit&&!ReceiveLimit&&!ReleaseLimit);
    BatchEntered=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGate=CreateEventW(NULL,TRUE,FALSE,NULL);BatchGateUsed=0;CHECK(BatchEntered&&BatchGate);
    TUN_SESSION *s=Start();CHECK(s&&WaitForSingleObject(BatchEntered,10000)==WAIT_OBJECT_0);
    s->Api.ReserveBatch=Terminal32Reserve;s->Api.ReceiveBatch=Terminal32Receive;
    Tun32.LegacySend=s->Descriptor.Receive.Ring->Data;Tun32.LegacyReceive=s->Descriptor.Send.Ring->Data;Tun32.LegacyBytes=s->Capacity+TUN_MAX_PACKET_SIZE-TUN_ALIGNMENT;
    Tun32.NativeSend=(BYTE *)Region+Layout.Queues[0].RxData;Tun32.NativeReceive=(BYTE *)Region+Layout.Queues[0].TxData;Tun32.NativeBytes=(SIZE_T)Layout.SlotCount*PB_BLOCK_SIZE;
    ULONG base=wrap?s->Capacity-TUN_ALIGNMENT:0;
    if(receive){s->Send.Head=s->Send.HeadRelease=base;s->Descriptor.Send.Ring->Head=s->Descriptor.Send.Ring->Tail=base;}
    else{s->Receive.Tail=s->Receive.TailRelease=base;s->Descriptor.Receive.Ring->Head=s->Descriptor.Receive.Ring->Tail=base;}
    BYTE *expected=Tun32Buffer(32*1500),*packets[32];
    for(DWORD i=0;i<32;++i){Tun32Packet(expected+(SIZE_T)i*1500,1500,ipv6,udp,receive?1:0,i);
        if(receive)CHECK(PbTestSend(Native,0,expected+(SIZE_T)i*1500,1500,0,0)==PB_OK);
        else{packets[i]=WintunAllocateSendPacket(s,1500);CHECK(packets[i]);memcpy(packets[i],expected+(SIZE_T)i*1500,1500);}
    }
    CHECK(SetEvent(BatchGate));
    if(receive){for(DWORD i=0;i<32;++i){DWORD bytes;packets[i]=Receive(s,&bytes);CHECK(bytes==1500&&!memcmp(packets[i],expected+(SIZE_T)i*1500,1500));}}
    HANDLE start=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(start);TERMINAL32_CALL calls[4];HANDLE threads[4];
    for(DWORD i=0;i<4;++i){calls[i]=(TERMINAL32_CALL){s,packets,expected,start,31-i,4,0};threads[i]=CreateThread(NULL,0,Terminal32Caller,calls+i,0,NULL);CHECK(threads[i]);}
    CHECK(SetEvent(start));for(DWORD i=0;i<4;++i){Terminal32Join(threads[i]);CHECK(calls[i].Calls==(i==3?7u:8u));}CHECK(CloseHandle(start));
    if(receive)CHECK(s->Send.PacketsToRelease==32&&s->Send.HeadRelease==base&&s->Descriptor.Send.Ring->Head==base);
    else CHECK(s->Receive.PacketsToRelease==32&&s->Receive.TailRelease==base&&s->Descriptor.Receive.Ring->Tail==base);
    Terminal32Idle();
    HANDLE lastStart=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(lastStart);TERMINAL32_CALL last={s,packets,expected,lastStart,0,1,0};
    if(before){
        /* The marker only proves the wait-code segment. No remaining producer
         * can signal User/Closing; this public last return is the real Wake.
         * A forward API gate after Wait selects the terminal-before-admission
         * race without suspending a thread or resetting the auto event. */
        InterlockedExchange(&Terminal32.GateArmed,1);HANDLE thread=CreateThread(NULL,0,Terminal32Caller,&last,0,NULL);CHECK(thread&&SetEvent(lastStart));
        CHECK(WaitForSingleObject(Terminal32.Entered,10000)==WAIT_OBJECT_0);Terminal32Join(thread);CHECK(last.Calls==1);
        PbTestFault(Native);CHECK(SetEvent(Terminal32.Continue));
        CHECK(WaitForSingleObject(s->Thread,10000)==WAIT_OBJECT_0);
    }else{
        PbTestFault(Native);CHECK(WaitForSingleObject(s->Thread,10000)==WAIT_OBJECT_0);
        HANDLE thread=CreateThread(NULL,0,Terminal32Caller,&last,0,NULL);CHECK(thread&&SetEvent(lastStart));Terminal32Join(thread);CHECK(last.Calls==1);
    }
    /* Native is allowed to be freed already. Inspect only private old-ring
     * state and host counters until every public user has joined, then End. */
    CHECK(s->Descriptor.Receive.Ring->Head==MAXDWORD&&s->Descriptor.Send.Ring->Tail==MAXDWORD&&WakeSignals==1);
    if(receive)CHECK(!s->Send.PacketsToRelease&&s->Send.HeadRelease==s->Send.Head&&s->Descriptor.Send.Ring->Head==s->Send.Head&&!Tun32.Copies[0]&&Tun32.Copies[1]==32&&Tun32.Bytes[1]==48000&&ReleasedPackets==32&&Tun32.Calls[TUN32_RELEASE]==1);
    else CHECK(!s->Receive.PacketsToRelease&&s->Receive.TailRelease==s->Receive.Tail&&s->Descriptor.Receive.Ring->Tail==s->Receive.Tail&&!Tun32.Copies[0]&&!Tun32.Copies[1]&&!Tun32.Calls[TUN32_COMMIT]);
    CHECK(!Tun32.Calls[TUN32_RESERVE]&&!Tun32.Calls[TUN32_POLL]&&!Cancelled);
    InterlockedExchange(&Tun32.Enabled,FALSE);WintunEndSession(s);EmptyRuntime();BatchMode=0;
    CHECK(CloseHandle(lastStart)&&CloseHandle(Terminal32.Entered)&&CloseHandle(Terminal32.Continue)&&CloseHandle(BatchEntered)&&CloseHandle(BatchGate));BatchEntered=BatchGate=NULL;
    Tun32Free(expected);memset(&Tun32,0,sizeof(Tun32));memset(&Terminal32,0,sizeof(Terminal32));++Terminal32CasesCount;
    printf("TERMINAL32 phase=%s ip=%u transport=%s receive=%u wrap=%u before=%u pointers=32 publicJoins=5 publicWake=1 copy=%u bytes=%u nativePublish=0 owners=0\n",phase,ipv6?6:4,udp?"UDP":"TCP",receive,wrap,before,receive?32:0,receive?48000:0);
}
static VOID Terminal32Matrix(const char *phase)
{for(DWORD ip=0;ip<2;++ip)if(Tun32Selected(ip))for(DWORD udp=0;udp<2;++udp)for(DWORD receive=0;receive<2;++receive)for(DWORD wrap=0;wrap<2;++wrap)for(DWORD before=0;before<2;++before)Terminal32Case(ip,udp,receive,wrap,before,phase);}
static VOID Terminal32Cases(VOID)
{
    Terminal32CasesCount=Terminal32PublicJoins=0;Terminal32Matrix("warm");DWORD warm=Terminal32CasesCount,before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));Terminal32Matrix("measure");
    CHECK(warm==(PB_TEST_IP_VERSION?16u:32u)&&Terminal32CasesCount==2*warm&&Terminal32PublicJoins==5*Terminal32CasesCount&&GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    printf("PASS: %lu terminal private32 cases warm%lu measure%lu publicJoins%lu handles%lu/%lu; native worker exited, real Wake and original pointers retire before End\n",Terminal32CasesCount,warm,warm,Terminal32PublicJoins,before,after);
}
