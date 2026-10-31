/* SPDX-License-Identifier: GPL-2.0 */
/* Actual non-test-shim DLL QUERY and lifecycle, real OS handles/thread pool.
 * Only device I/O and software-device closure use the shared hosted fixture. */
#define main CloseRetryOriginalMain
#include "close_retry_tests.c"
#undef main

static DWORD ExpectedQueryError(LONG injection,DWORD stale)
{
    if(injection==QUERY_IO_FAILURE) return ERROR_DEVICE_NOT_CONNECTED;
#ifdef PB_QUERY_ERROR_BASELINE
    return stale;
#else
    (void)stale;return ERROR_INVALID_DATA;
#endif
}
static VOID InvalidQuery(LONG injection,DWORD stale,BOOL withAdapter)
{
    Begin(FAIL_NONE,FALSE);PB_ADAPTER *adapter=withAdapter?Adapter():NULL;PB_SESSION *s;
    CHECK(CallOpen(adapter,&s)==PB_OK);Retain(s);
    QueryStaleError=stale;InterlockedExchange(&QueryInjection,injection);
    PB_V2_STATUS status;SetLastError(ERROR_SUCCESS);
    CHECK(Api.GetStatus(s,&status)==PB_CORRUPT);DWORD error=GetLastError();
    CHECK(error==ExpectedQueryError(injection,stale));
    PB_RESULT closed=Api.Close(s,0);CHECK(closed==PB_QUARANTINED || closed==PB_CORRUPT);AwaitPark();
    PB_CLOSE_PROGRESS p;CHECK(Life.GetCloseProgress(s,&p)==PB_OK && p.Result==PB_CORRUPT &&
        p.LastError==ExpectedQueryError(injection,stale) && p.BlockStage==PB_BLOCK_KERNEL && p.Phase<PB_LIFE_PREPARED &&
        p.RetainedBytes==s->Layout.RegionSize && p.Flags&PB_LIFE_QUARANTINED && s->Region && !s->ResourcesClosed &&
        DeviceOwned && !CloseCalls && !CloseSuccesses && RuntimeCount==1);
    if(adapter) {Api.CloseAdapter(adapter);CHECK(!SwCloses && RuntimeAdapterCount==1);}
    /* Correcting only the QUERY response permits the original close path.
     * Error reporting must neither bypass nor permanently latch that barrier. */
    InterlockedExchange(&QueryInjection,QUERY_ACK_OK);CHECK(SetEvent(Progress) && SetEvent(ParkGate));
    WaitForThreadpoolWorkCallbacks(s->Work,FALSE);
    CHECK(s->ResourcesClosed && !s->Region && CloseCalls==1 && CloseSuccesses==1 && !DeviceOwned && !RuntimeCount);
    if(adapter) CHECK(SwCloses==1 && SwCloseOrder>DeviceCloseOrder);
    CHECK(Api.Close(s,2000)==PB_OK);DropReference(s);End();
}
static VOID ValidQuery(DWORD stale,BOOL withAdapter)
{
    Begin(FAIL_NONE,FALSE);PB_ADAPTER *adapter=withAdapter?Adapter():NULL;PB_SESSION *s;
    CHECK(CallOpen(adapter,&s)==PB_OK);Retain(s);QueryStaleError=stale;
    PB_V2_STATUS status;CHECK(Api.GetStatus(s,&status)==PB_OK && GetLastError()==stale &&
        status.Size==sizeof(status) && status.Epoch==s->Epoch);
    CHECK(Api.Close(s,2000)==PB_OK);WaitForThreadpoolWorkCallbacks(s->Work,FALSE);
    CHECK(CloseCalls==1 && CloseSuccesses==1 && !Parks);
    if(adapter) {CHECK(!SwCloses);Api.CloseAdapter(adapter);CHECK(SwCloses==1 && SwCloseOrder>DeviceCloseOrder);}
    DropReference(s);End();
}
int main(VOID)
{
    CHECK(PbGetInterfaceV2(2,sizeof(Api),&Api)==PB_OK && PbGetLifecycleInterface(1,sizeof(Life),&Life)==PB_OK);
    for(ULONG adapter=0;adapter<2;++adapter) for(ULONG stale=0;stale<2;++stale) {
        DWORD error=stale?ERROR_BUSY:ERROR_SUCCESS;
        for(LONG injection=QUERY_ACK_SHORT;injection<=QUERY_IO_FAILURE;++injection) InvalidQuery(injection,error,(BOOL)adapter);
        ValidQuery(error,(BOOL)adapter);
    }
#ifdef PB_QUERY_ERROR_BASELINE
    puts("PASS: 24 actual QUERY/close scenarios: frozen malformed success preserves stale zero/ERROR_BUSY in API and lifecycle progress; real I/O failures preserve ERROR_DEVICE_NOT_CONNECTED. Invalid acknowledgements retain mapping/device/adapter until corrected; valid replies recover exactly once with balanced resources.");
#else
    puts("PASS: 24 actual QUERY/close scenarios: short/long/Size/Epoch malformed successes report ERROR_INVALID_DATA despite stale zero/ERROR_BUSY; real I/O failures preserve ERROR_DEVICE_NOT_CONNECTED. Invalid acknowledgements retain mapping/device/adapter until corrected; valid replies recover exactly once with balanced resources.");
#endif
    return 0;
}
