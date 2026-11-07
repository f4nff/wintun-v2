/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s (%lu)\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
typedef VOID (WINAPI *CONFIGURE)(LONG,DWORD,DWORD);
typedef BOOL (WINAPI *PROBE)(DWORD);
typedef DWORD (WINAPI *QUERY)(VOID);
typedef VOID (WINAPI *RESET)(VOID);
typedef BOOL (WINAPI *EXECUTE)(LPCWSTR,LPCWSTR,LPWSTR,DWORD);
typedef BOOL (WINAPI *HELD)(HANDLE,HANDLE,LONG *);
typedef BOOL (WINAPI *SHUTDOWN)(DWORD);
typedef struct FIXTURE {HMODULE Module;CONFIGURE Configure;PROBE Probe;QUERY State,Owned,Jobs,Steps,Signals;RESET Reset,Stop;EXECUTE Execute;HELD Held;SHUTDOWN Shutdown;} FIXTURE;
static DWORD Scenarios;
static FIXTURE Load(LPCWSTR path)
{
    FIXTURE f={.Module=LoadLibraryW(path)};CHECK(f.Module);
#define GET(field,name,type) f.field=(type)GetProcAddress(f.Module,name);CHECK(f.field)
    GET(Configure,"InitConfigure",CONFIGURE);GET(Probe,"InitProbe",PROBE);GET(State,"InitState",QUERY);
    GET(Owned,"InitOwned",QUERY);GET(Jobs,"Jobs",QUERY);GET(Steps,"Steps",QUERY);GET(Reset,"InitReset",RESET);
    GET(Signals,"InitSignals",QUERY);
    GET(Execute,"Execute",EXECUTE);GET(Held,"InitReserveHeld",HELD);GET(Stop,"Stop",RESET);GET(Shutdown,"Shutdown",SHUTDOWN);
#undef GET
    return f;
}
static VOID Clean(FIXTURE *f)
{CHECK(!f->Jobs());f->Reset();CHECK(!f->State()&&!f->Owned());++Scenarios;}
static VOID Control(FIXTURE *f,DWORD kind)
{
    BOOL okay;DWORD error;
    if(kind==4) {
        f->Configure(0,ERROR_NOT_ENOUGH_MEMORY,0);CHECK(f->Probe(0));
        f->Configure(2,ERROR_NOT_ENOUGH_MEMORY,0);WCHAR response[9];okay=f->Execute(L"unused",L"",response,9);error=GetLastError();
    }else {f->Configure(1,ERROR_NOT_ENOUGH_MEMORY,kind==3?ERROR_ALREADY_EXISTS:0);okay=f->Probe(kind==2?1:0);error=GetLastError();}
    if((kind<=2&&okay)||(kind>=3&&error!=ERROR_NOT_ENOUGH_MEMORY)) {
        Clean(f);CHECK(FreeLibrary(f->Module));
        const char *markers[]={"","MISSING_STOP_EVENTS_ADMITTED","CLEANUP_INIT_FAILURE_REPORTED_SUCCESS","FIRST_EVENT_ERROR_OVERWRITTEN","JOB_FIRST_EVENT_ERROR_LOST"};
        fprintf(stderr,"%s\n",markers[kind]);ExitProcess(60+kind);
    }
    CHECK(!okay&&error==ERROR_NOT_ENOUGH_MEMORY);Clean(f);
}
typedef struct CALL {FIXTURE *Fixture;HANDLE Entered,Gate;LONG *Arrived;BOOL Okay;DWORD Error;} CALL;
static DWORD WINAPI ReserveThread(VOID *opaque)
{CALL *call=opaque;call->Okay=call->Fixture->Held(call->Entered,call->Gate,call->Arrived);call->Error=GetLastError();return 0;}
static VOID Concurrent(FIXTURE *f,DWORD step,DWORD failureError)
{
    f->Configure((LONG)step,failureError,0);
    HANDLE entered=CreateEventW(NULL,TRUE,FALSE,NULL),gate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(entered&&gate);
    CALL calls[8];HANDLE threads[8];LONG arrived=0;
    for(DWORD i=0;i<8;++i){calls[i]=(CALL){f,entered,gate,&arrived,FALSE,0};threads[i]=CreateThread(NULL,0,ReserveThread,calls+i,0,NULL);CHECK(threads[i]);}
    CHECK(WaitForSingleObject(entered,5000)==WAIT_OBJECT_0&&f->Jobs()==4&&f->State()==3&&f->Owned()==2&&f->Steps()==2+step);
    f->Stop();CHECK(!f->Shutdown(0)&&GetLastError()==ERROR_TIMEOUT);
    f->Configure(0,ERROR_NOT_ENOUGH_MEMORY,0);CHECK(!f->Probe(0)&&GetLastError()==ERROR_SHUTDOWN_IN_PROGRESS&&!f->Steps());
    CHECK(SetEvent(gate)&&WaitForMultipleObjects(8,threads,TRUE,5000)==WAIT_OBJECT_0);
    DWORD admitted=0,failed=0,busy=0;
    for(DWORD i=0;i<8;++i) {
        CHECK(CloseHandle(threads[i]));if(calls[i].Okay)++admitted;
        else if(calls[i].Error==ERROR_BUSY)++busy;
        else {CHECK(calls[i].Error==(failureError?failureError:ERROR_GEN_FAILURE));++failed;}
    }
    CHECK(admitted==4&&failed==(step?1u:0u)&&busy==4-failed&&!f->Jobs()&&f->Shutdown(1000)&&CloseHandle(entered)&&CloseHandle(gate));Clean(f);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2||argc==3);FIXTURE f=Load(argv[1]);
    if(argc==3)Control(&f,wcstoul(argv[2],NULL,10));
    else {
        const DWORD failures[]={0,ERROR_NOT_ENOUGH_MEMORY,ERROR_ACCESS_DENIED,ERROR_TOO_MANY_OPEN_FILES};
        const DWORD successes[]={0,ERROR_ALREADY_EXISTS,ERROR_INVALID_DATA};
        DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
        for(DWORD pass=0;pass<3;++pass) {
            for(DWORD target=0;target<4;++target)for(DWORD step=1;step<=2;++step)
                for(DWORD e=0;e<ARRAYSIZE(failures);++e)for(DWORD s=0;s<ARRAYSIZE(successes);++s) {
                    f.Configure((LONG)step,failures[e],successes[s]);BOOL okay=f.Probe(target);DWORD error=GetLastError();
                    CHECK(!okay&&error==(failures[e]?failures[e]:ERROR_GEN_FAILURE));
                    CHECK(!f.State()&&!f.Jobs()&&!f.Owned()&&f.Steps()==step&&f.Signals()==(step==2?1u:0u));Clean(&f);
                }
            for(DWORD target=0;target<2;++target) {
                f.Configure(0,ERROR_NOT_ENOUGH_MEMORY,ERROR_ALREADY_EXISTS);CHECK(f.Probe(target?3:0));
                CHECK(f.State()==(target?12u:3u)&&f.Owned()==2&&f.Steps()==2&&!f.Jobs()&&f.Signals()==1);
                CHECK(f.Probe(target?3:0)&&f.Steps()==2);Clean(&f);
            }
            for(DWORD step=1;step<=9;++step) {
                f.Configure((LONG)step,ERROR_NOT_ENOUGH_MEMORY,0);WCHAR response[9];CHECK(!f.Execute(L"unused",L"",response,9));
                CHECK(GetLastError()==ERROR_NOT_ENOUGH_MEMORY&&!f.Jobs()&&f.Steps()==step&&f.Owned()==(step<=2?0u:2u));
                CHECK(f.Signals()==(step==1?0u:1u));Clean(&f);
            }
            CHECK(GetProcessHandleCount(GetCurrentProcess(),&after));
            printf("TRACE: complete matrix pass=%lu process handles=%lu/%lu\n",pass,before,after);
            /* Warm one complete matrix before comparing process-wide counts.
             * Application acquisitions are checked even during that first pass. */
            if(pass)CHECK(after<=before);else before=after;
        }
        Concurrent(&f,0,ERROR_NOT_ENOUGH_MEMORY);
        for(DWORD step=1;step<=2;++step){Concurrent(&f,step,ERROR_NOT_ENOUGH_MEMORY);Concurrent(&f,step,0);}
    }
    CHECK(FreeLibrary(f.Module));
    if(argc==2) {
        DWORD baseline,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&baseline));
        for(DWORD reload=0;reload<16;++reload) {
            f=Load(argv[1]);f.Configure(4,ERROR_NOT_ENOUGH_MEMORY,0);WCHAR response[9];
            CHECK(!f.Execute(L"unused",L"",response,9)&&GetLastError()==ERROR_NOT_ENOUGH_MEMORY&&f.Steps()==4);
            Clean(&f);CHECK(FreeLibrary(f.Module));
            CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after<=baseline);
        }
        printf("TRACE: sixteen fresh DLL initialization/rollback/unload cycles process handles=%lu/%lu\n",baseline,after);
    }
    printf("PASS: %lu cold-init/rollback schedules; first failure preserved, complete event-pair publication, no false admission or queued work, first-use contention and joined threads, zero tracked residue.\n",Scenarios);return 0;
}
