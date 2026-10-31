/* SPDX-License-Identifier: GPL-2.0 */
#include "guard_memory.h"
#include <wchar.h>
#include <stdlib.h>
typedef struct OP_TEST_INPUT { HANDLE Entered,Gate,DisposeEntered,DisposeGate; DWORD Mode; WCHAR Name[16]; } OP_TEST_INPUT;
typedef ULONG_PTR (WINAPI *EXECUTE)(const OP_TEST_INPUT *);
typedef VOID (WINAPI *RELEASE)(ULONG_PTR);
typedef DWORD (WINAPI *COUNT)(VOID);
typedef VOID (WINAPI *FAILURE)(LONG);
typedef BOOL (WINAPI *SHUTDOWN)(DWORD);
typedef struct FIXTURE { HMODULE Module; HANDLE Detached; EXECUTE Execute; RELEASE Release; COUNT Jobs,Owned,Disposed,Acquisitions; FAILURE Fail; SHUTDOWN Shutdown; } FIXTURE;
static FIXTURE Load(LPCWSTR path)
{
    FIXTURE f={0}; f.Detached=CreateEventW(NULL,TRUE,FALSE,NULL); SAFETY_CHECK(f.Detached);
    f.Module=LoadLibraryW(path); SAFETY_CHECK(f.Module);
#define GET(field,name) do { *(FARPROC *)&f.field=GetProcAddress(f.Module,name); SAFETY_CHECK(f.field); } while(0)
    GET(Execute,"Execute"); GET(Release,"ReleaseResult"); GET(Jobs,"Jobs"); GET(Owned,"Owned"); GET(Disposed,"Disposed");
    GET(Acquisitions,"Acquisitions"); GET(Fail,"SetFailure"); GET(Shutdown,"Shutdown");
#undef GET
    VOID (WINAPI *configure)(HANDLE); *(FARPROC *)&configure=GetProcAddress(f.Module,"Configure"); SAFETY_CHECK(configure); configure(f.Detached); return f;
}
static void Idle(FIXTURE *f)
{
    ULONGLONG deadline=GetTickCount64()+5000;
    while(f->Jobs() || f->Owned()) { SAFETY_CHECK(GetTickCount64()<deadline); SwitchToThread(); }
}
static void Unload(FIXTURE *f)
{ SAFETY_CHECK(FreeLibrary(f->Module) && WaitForSingleObject(f->Detached,5000)==WAIT_OBJECT_0 && CloseHandle(f->Detached)); }
typedef struct CALL { EXECUTE Execute; OP_TEST_INPUT Input; ULONG_PTR Result; DWORD Error; } CALL;
static DWORD WINAPI Caller(VOID *opaque)
{ CALL *c=opaque; c->Result=c->Execute(&c->Input); c->Error=GetLastError(); return 0; }
static DWORD WINAPI AbandonMutex(VOID *mutex)
{ SAFETY_CHECK(WaitForSingleObject(mutex,1000)==WAIT_OBJECT_0); return 0; }
static OP_TEST_INPUT Input(DWORD mode)
{
    OP_TEST_INPUT input={.Mode=mode}; wcscpy_s(input.Name,_countof(input.Name),L"owned"); return input;
}
int wmain(int argc,WCHAR **argv)
{
    SAFETY_CHECK(argc==2); DWORD before,after;
    FIXTURE warm=Load(argv[1]); OP_TEST_INPUT input=Input(0); ULONG_PTR value=warm.Execute(&input); SAFETY_CHECK(value);
    DWORD acquisitions=warm.Acquisitions(); warm.Release(value); Idle(&warm); Unload(&warm);
    SAFETY_CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    FIXTURE mutexFixture=Load(argv[1]); HANDLE mutex=CreateMutexW(NULL,FALSE,NULL); SAFETY_CHECK(mutex);
    HANDLE abandoning=CreateThread(NULL,0,AbandonMutex,mutex,0,NULL);
    SAFETY_CHECK(abandoning && WaitForSingleObject(abandoning,1000)==WAIT_OBJECT_0 && CloseHandle(abandoning));
    input=Input(6); input.Gate=mutex; value=mutexFixture.Execute(&input); SAFETY_CHECK(value);
    mutexFixture.Release(value); Idle(&mutexFixture);
    SAFETY_CHECK(WaitForSingleObject(mutex,0)==WAIT_OBJECT_0 && ReleaseMutex(mutex) && CloseHandle(mutex));
    Unload(&mutexFixture); input=Input(0);
    for(DWORD fail=1;fail<=acquisitions;++fail) {
        FIXTURE f=Load(argv[1]); f.Fail((LONG)fail); SAFETY_CHECK(!f.Execute(&input)); Idle(&f);
        f.Fail(0); value=f.Execute(&input); SAFETY_CHECK(value); f.Release(value); Idle(&f); Unload(&f);
    }
    for(DWORD round=0;round<32;++round) {
        FIXTURE f=Load(argv[1]);
        for(DWORD i=0;i<32;++i) { input=Input(i%2?0:5); value=f.Execute(&input); SAFETY_CHECK(value); f.Release(value); Idle(&f); }
        input=Input(4); SAFETY_CHECK(!f.Execute(&input) && GetLastError()==ERROR_ACCESS_DENIED); Idle(&f);
        HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL);
        HANDLE disposed=CreateEventW(NULL,TRUE,FALSE,NULL),disposeGate=CreateEventW(NULL,TRUE,FALSE,NULL);
        SAFETY_CHECK(entered && gate && disposed && disposeGate);
        CALL call={.Execute=f.Execute,.Input=Input(round%2?1:2)};
        call.Input.Entered=entered; call.Input.Gate=gate; call.Input.DisposeEntered=disposed; call.Input.DisposeGate=disposeGate;
        HANDLE thread=CreateThread(NULL,0,Caller,&call,0,NULL); SAFETY_CHECK(thread && WaitForSingleObject(entered,2000)==WAIT_OBJECT_0);
        if(round%2) {
            f.Shutdown(0); SAFETY_CHECK(WaitForSingleObject(thread,1000)==WAIT_OBJECT_0 && !call.Result && call.Error==ERROR_CANCELLED);
            Idle(&f); SAFETY_CHECK(f.Shutdown(1000)); Unload(&f);
        } else {
            SAFETY_CHECK(WaitForSingleObject(thread,1500)==WAIT_OBJECT_0 && !call.Result && call.Error==ERROR_TIMEOUT);
            /* The caller's input can disappear while the worker/native call still runs. */
            wcscpy_s(call.Input.Name,_countof(call.Input.Name),L"changed");
            SAFETY_CHECK(f.Jobs()==1 && !f.Shutdown(0) && GetLastError()==ERROR_TIMEOUT);
            input=Input(0); SAFETY_CHECK(!f.Execute(&input) && GetLastError()==ERROR_SHUTDOWN_IN_PROGRESS);
            SAFETY_CHECK(SetEvent(gate) && WaitForSingleObject(disposed,1000)==WAIT_OBJECT_0);
            SAFETY_CHECK(FreeLibrary(f.Module) && WaitForSingleObject(f.Detached,0)==WAIT_TIMEOUT);
            SAFETY_CHECK(SetEvent(disposeGate) && WaitForSingleObject(f.Detached,5000)==WAIT_OBJECT_0 && CloseHandle(f.Detached));
        }
        SAFETY_CHECK(CloseHandle(thread) && CloseHandle(entered) && CloseHandle(gate) && CloseHandle(disposed) && CloseHandle(disposeGate));
    }
    FIXTURE bound=Load(argv[1]); CALL calls[4]; HANDLE threads[4];
    HANDLE gate=CreateEventW(NULL,TRUE,FALSE,NULL); SAFETY_CHECK(gate);
    for(DWORD i=0;i<4;++i) {
        calls[i]=(CALL){.Execute=bound.Execute,.Input=Input(2)}; calls[i].Input.Gate=gate;
        calls[i].Input.Entered=CreateEventW(NULL,TRUE,FALSE,NULL); SAFETY_CHECK(calls[i].Input.Entered);
        threads[i]=CreateThread(NULL,0,Caller,calls+i,0,NULL); SAFETY_CHECK(threads[i] && WaitForSingleObject(calls[i].Input.Entered,2000)==WAIT_OBJECT_0);
    }
    input=Input(0); SAFETY_CHECK(!bound.Execute(&input) && GetLastError()==ERROR_BUSY);
    bound.Shutdown(0);
    for(DWORD i=0;i<4;++i) SAFETY_CHECK(WaitForSingleObject(threads[i],1000)==WAIT_OBJECT_0 && !calls[i].Result && calls[i].Error==ERROR_CANCELLED);
    SAFETY_CHECK(SetEvent(gate) && bound.Shutdown(5000)); Idle(&bound); SAFETY_CHECK(bound.Disposed()==4); Unload(&bound);
    for(DWORD i=0;i<4;++i) SAFETY_CHECK(CloseHandle(threads[i]) && CloseHandle(calls[i].Input.Entered)); SAFETY_CHECK(CloseHandle(gate));
    FIXTURE nested=Load(argv[1]); input=Input(3); input.Gate=CreateEventW(NULL,TRUE,FALSE,NULL); SAFETY_CHECK(input.Gate);
    ULONGLONG start=GetTickCount64(); SAFETY_CHECK(!nested.Execute(&input)); DWORD error=GetLastError();
    SAFETY_CHECK((error==ERROR_TIMEOUT || error==ERROR_CANCELLED) && GetTickCount64()-start<450);
    Idle(&nested); Unload(&nested); SAFETY_CHECK(CloseHandle(input.Gate));
    /* Pool close releases OS worker bookkeeping asynchronously. Our DLL asserts
     * every owned object at detach; allow native pool teardown to finish and
     * tolerate handles already draining from the warm-up pool. */
    ULONGLONG teardown=GetTickCount64()+2000;
    do { SAFETY_CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)); if(after<=before) break; Sleep(1); } while(GetTickCount64()<teardown);
    SAFETY_CHECK(after<=before);
    printf("PASS: %lu acquisition failures, 32 real DLL cycles, 1024 result handoffs, copied input, reentry rejection, cancellable waits, total deadline, four-job bound, late-result disposal and blocked close/return-tail module lifetime; zero owned resources and no handle growth after pool teardown.\n",acquisitions);
    return 0;
}
