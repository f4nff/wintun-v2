/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s error=%lu\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
typedef BOOL (WINAPI *ACQUIRE)(void);
typedef VOID (WINAPI *ACTION)(void);
typedef BOOL (WINAPI *CONFIGURE)(LPCWSTR,LPCWSTR,HANDLE);
typedef VOID (WINAPI *SETUP)(HANDLE,HANDLE,DWORD,DWORD);
typedef VOID (WINAPI *STATE)(DWORD *);
typedef DWORD (WINAPI *COUNT)(void);
typedef BOOL (WINAPI *SHUTDOWN)(DWORD);
typedef struct FIXTURE {HMODULE Module;HANDLE Detached;ACQUIRE Acquire,Touch;ACTION Release,Stop,Clear;SETUP Setup;STATE State;COUNT Loaded,Owned,Jobs;SHUTDOWN Shutdown;} FIXTURE;
static FIXTURE Load(LPCWSTR path)
{
    FIXTURE f={0};f.Detached=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f.Detached);f.Module=LoadLibraryW(path);CHECK(f.Module);
#define BIND(field,name) do{*(FARPROC *)&f.field=GetProcAddress(f.Module,name);CHECK(f.field);}while(0)
    BIND(Acquire,"Acquire");BIND(Touch,"Touch");BIND(Release,"Release");BIND(Stop,"Stop");BIND(Clear,"ClearWaitFailure");
    BIND(Setup,"ConfigureWait");BIND(State,"RuntimeState");BIND(Loaded,"Loaded");BIND(Owned,"Owned");BIND(Jobs,"Jobs");BIND(Shutdown,"Shutdown");
#undef BIND
    CONFIGURE configure=(CONFIGURE)(void *)GetProcAddress(f.Module,"Configure");CHECK(configure&&configure(L"unused",L".",f.Detached));return f;
}
static VOID Unload(FIXTURE *f)
{
    DWORD state[7];f->State(state);CHECK(!state[0]&&!state[1]&&!state[5]&&!f->Owned()&&!f->Jobs()&&!f->Loaded());
    CHECK(FreeLibrary(f->Module)&&WaitForSingleObject(f->Detached,5000)==WAIT_OBJECT_0&&CloseHandle(f->Detached));
}
typedef struct JOB {FIXTURE *F;BOOL Okay;DWORD Error;} JOB;
static DWORD WINAPI ReleaseThread(void *opaque)
{
    JOB *j=opaque;SetLastError(0x2311);j->F->Release();j->Error=GetLastError();return 0;
}
static DWORD WINAPI AcquireThread(void *opaque)
{
    JOB *j=opaque;j->Okay=j->F->Acquire();j->Error=GetLastError();if(j->Okay)j->F->Release();return 0;
}
typedef struct GATES {HANDLE Entered,Gate,Thread;JOB Release;} GATES;
static VOID Begin(FIXTURE *f,GATES *g,DWORD error,DWORD spurious)
{
    memset(g,0,sizeof(*g));g->Entered=CreateEventW(NULL,TRUE,FALSE,NULL);g->Gate=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(g->Entered&&g->Gate);
    f->Setup(g->Entered,g->Gate,error,spurious);CHECK(f->Acquire()&&f->Touch()&&f->Loaded());
    g->Release.F=f;g->Thread=CreateThread(NULL,0,ReleaseThread,&g->Release,0,NULL);CHECK(g->Thread&&WaitForSingleObject(g->Entered,5000)==WAIT_OBJECT_0);
    DWORD state[7];f->State(state);CHECK(!state[0]&&state[1]&&!state[2]&&f->Loaded());
}
static VOID Finish(FIXTURE *f,GATES *g)
{
    f->Clear();CHECK(SetEvent(g->Gate)&&WaitForSingleObject(g->Thread,5000)==WAIT_OBJECT_0&&g->Release.Error==0x2311);
    CHECK(CloseHandle(g->Thread)&&CloseHandle(g->Gate)&&CloseHandle(g->Entered));
    DWORD state[7];f->State(state);CHECK(!state[0]&&!state[1]&&!state[5]&&!f->Loaded());
}
static VOID FailedWait(LPCWSTR path,DWORD error)
{
    FIXTURE f=Load(path);GATES g;Begin(&f,&g,error,0);
    CHECK(!f.Acquire());DWORD captured=GetLastError(),state[7];f.State(state);
    CHECK(captured==error&&state[3]==1&&!state[0]&&state[1]&&!state[2]);Finish(&f,&g);
    CHECK(f.Acquire()&&f.Touch());f.Release();Unload(&f);
}
static VOID ConcurrentWait(LPCWSTR path,BOOL stopping,DWORD spurious)
{
    FIXTURE f=Load(path);GATES g;Begin(&f,&g,0,spurious);
    JOB jobs[8];HANDLE threads[8];
    for(unsigned i=0;i<8;++i){jobs[i]=(JOB){.F=&f};threads[i]=CreateThread(NULL,0,AcquireThread,jobs+i,0,NULL);CHECK(threads[i]);}
    DWORD state[7];ULONGLONG end=GetTickCount64()+5000;
    do{f.State(state);CHECK(GetTickCount64()<end);if(state[6]<8)SwitchToThread();}while(state[6]<8);
    CHECK(state[3]>=8+spurious);
    CHECK(!state[0]&&state[1]&&!state[2]&&WaitForMultipleObjects(8,threads,TRUE,0)==WAIT_TIMEOUT);
    if(stopping) {
        f.Stop();f.State(state);
        if(!state[4]){fputs("BLOCKED_ACQUIRE_NOT_WOKEN\n",stderr);ExitProcess(43);}
        CHECK(state[4]==1);
        CHECK(WaitForMultipleObjects(8,threads,TRUE,5000)==WAIT_OBJECT_0);
        for(unsigned i=0;i<8;++i)CHECK(!jobs[i].Okay&&jobs[i].Error==ERROR_SHUTDOWN_IN_PROGRESS);
        f.State(state);CHECK(!state[0]&&state[1]&&state[2]&&f.Loaded());
        CHECK(!f.Shutdown(0)&&GetLastError()==ERROR_TIMEOUT);
        f.State(state);CHECK(state[4]==1); /* Repeated stop must not broadcast again. */
        Finish(&f,&g);CHECK(f.Shutdown(5000));
    } else {CHECK(SetEvent(g.Gate)&&WaitForMultipleObjects(8,threads,TRUE,5000)==WAIT_OBJECT_0);Finish(&f,&g);for(unsigned i=0;i<8;++i)CHECK(jobs[i].Okay);}
    for(unsigned i=0;i<8;++i)CHECK(CloseHandle(threads[i]));Unload(&f);
}
int wmain(int argc,WCHAR **argv)
{
    CHECK(argc==2||argc==3);
    if(argc==3&&!wcscmp(argv[2],L"--failure-only")){FailedWait(argv[1],ERROR_NOT_ENOUGH_MEMORY);return 0;}
    if(argc==3&&!wcscmp(argv[2],L"--stop-only")){ConcurrentWait(argv[1],TRUE,0);return 0;}
    CHECK(argc==2);
    const DWORD errors[]={ERROR_NOT_ENOUGH_MEMORY,ERROR_OUTOFMEMORY,ERROR_INVALID_PARAMETER,ERROR_OPERATION_ABORTED,ERROR_GEN_FAILURE};
    /* Warm delay-loader/system caches before process-handle accounting. */
    for(unsigned i=0;i<2;++i)FailedWait(argv[1],errors[0]);
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(unsigned i=0;i<5;++i)FailedWait(argv[1],errors[i]);
    for(unsigned round=0;round<20;++round){ConcurrentWait(argv[1],FALSE,round%4);ConcurrentWait(argv[1],TRUE,round%4);}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    puts("PASS: actual last-lease dependency unload, five CV failures preserve errors without retry/admission; eight real waiters, spurious wakes, stop-before-unload completion, no early resource shutdown, 40 concurrent load/unload cycles, joined threads and zero heap/job/owned-resource/process-handle residue.");return 0;
}
