/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>
#include "helper_test_root.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"%s:%d: %s error=%lu\n",__FILE__,__LINE__,#x,GetLastError());ExitProcess(1);}}while(0)
typedef BOOL (WINAPI *CONFIGURE)(LPCWSTR,LPCWSTR,HANDLE);
typedef BOOL (WINAPI *EXECUTE)(LPCWSTR,LPCWSTR,LPWSTR,DWORD);
typedef VOID (WINAPI *ACTION)(void);
typedef DWORD (WINAPI *COUNT)(void);
typedef BOOL (WINAPI *SHUTDOWN)(DWORD);
typedef VOID (WINAPI *SETUP)(HANDLE,HANDLE,HANDLE,DWORD);
typedef VOID (WINAPI *STATE)(DWORD *,WCHAR *);
typedef struct FIXTURE {HMODULE Module;HANDLE Detached;EXECUTE Execute;ACTION Stop;COUNT Jobs,Owned,Loaded;SHUTDOWN Shutdown;SETUP Setup;STATE State;} FIXTURE;
static WCHAR Executable[MAX_PATH],Root[MAX_PATH];static ULONG Serial;
static VOID RootPath(WCHAR *path,LPCWSTR parent,LPCWSTR leaf)
{CHECK(_snwprintf_s(path,MAX_PATH,_TRUNCATE,L"%s\\%s",parent,leaf)>0);}
static VOID RootOutput(VOID)
{
    char utf8[4*MAX_PATH];
    CHECK(WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,Root,-1,utf8,sizeof(utf8),NULL,NULL)>0);
    CHECK(fprintf(stderr,"OWNED_ROOT pid=%lu path=%s\n",GetCurrentProcessId(),utf8)>0&&fflush(stderr)==0);
}
static BOOL WINAPI RootDenied(LPCWSTR path,LPSECURITY_ATTRIBUTES security)
{(void)path;(void)security;SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
static BOOL WINAPI RootNoError(LPCWSTR path,LPSECURITY_ATTRIBUTES security)
{(void)path;(void)security;SetLastError(ERROR_SUCCESS);return FALSE;}
typedef struct ROOT_CALL {LPCWSTR Parent;DWORD Pid;FILETIME Created;WCHAR Path[MAX_PATH];} ROOT_CALL;
static DWORD WINAPI RootThread(VOID *opaque)
{ROOT_CALL *call=opaque;CHECK(PbHelperTestCreateRoot(call->Parent,call->Pid,&call->Created,call->Path,_countof(call->Path),CreateDirectoryW));return 0;}
static VOID RootSelfTests(const FILETIME *created)
{
    WCHAR parent[MAX_PATH],paths[PB_HELPER_ROOT_ATTEMPTS][MAX_PATH],failed[MAX_PATH],file[MAX_PATH];
    RootPath(parent,Root,L"root-self-test");CHECK(CreateDirectoryW(parent,NULL));
    /* Keep every candidate occupied, including a sentinel in the first one.
     * A collision is skipped, never adopted or removed by the constructor. */
    for(DWORD i=0;i<PB_HELPER_ROOT_ATTEMPTS;++i){
        CHECK(PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,paths[i],MAX_PATH,CreateDirectoryW));
        for(DWORD j=0;j<i;++j)CHECK(wcscmp(paths[i],paths[j]));
    }
    RootPath(file,paths[0],L"preserved");HANDLE sentinel=CreateFileW(file,GENERIC_WRITE|GENERIC_READ,0,NULL,CREATE_NEW,0,NULL);CHECK(sentinel!=INVALID_HANDLE_VALUE);
    const DWORD marker=0x13572468;DWORD written,read,observed;
    CHECK(WriteFile(sentinel,&marker,sizeof(marker),&written,NULL)&&written==sizeof(marker));
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,failed,MAX_PATH,CreateDirectoryW)&&!failed[0]&&GetLastError()==ERROR_ALREADY_EXISTS);
    for(DWORD i=0;i<PB_HELPER_ROOT_ATTEMPTS;++i){DWORD attributes=GetFileAttributesW(paths[i]);CHECK(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_DIRECTORY));}
    CHECK(SetFilePointer(sentinel,0,NULL,FILE_BEGIN)==0&&ReadFile(sentinel,&observed,sizeof(observed),&read,NULL)&&read==sizeof(observed)&&observed==marker&&CloseHandle(sentinel)&&DeleteFileW(file));
    for(DWORD i=0;i<PB_HELPER_ROOT_ATTEMPTS;++i)CHECK(RemoveDirectoryW(paths[i]));
    /* A file in the parent position is not ours to remove. */
    RootPath(file,parent,L"parent-file");sentinel=CreateFileW(file,GENERIC_WRITE|GENERIC_READ,0,NULL,CREATE_NEW,0,NULL);CHECK(sentinel!=INVALID_HANDLE_VALUE);
    CHECK(WriteFile(sentinel,&marker,sizeof(marker),&written,NULL)&&written==sizeof(marker));
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(file,GetCurrentProcessId(),created,failed,MAX_PATH,CreateDirectoryW)&&!failed[0]&&GetLastError()!=ERROR_SUCCESS);
    CHECK(SetFilePointer(sentinel,0,NULL,FILE_BEGIN)==0&&ReadFile(sentinel,&observed,sizeof(observed),&read,NULL)&&read==sizeof(observed)&&observed==marker&&CloseHandle(sentinel)&&DeleteFileW(file));
    WCHAR longPath[MAX_PATH+64];wmemset(longPath,L'x',_countof(longPath));longPath[0]=L'C';longPath[1]=L':';longPath[2]=L'\\';longPath[_countof(longPath)-1]=0;
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(longPath,GetCurrentProcessId(),created,failed,MAX_PATH,CreateDirectoryW)&&!failed[0]&&GetLastError()==ERROR_FILENAME_EXCED_RANGE);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,failed,1,CreateDirectoryW)&&!failed[0]&&GetLastError()==ERROR_INSUFFICIENT_BUFFER);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,failed,MAX_PATH,RootDenied)&&!failed[0]&&GetLastError()==ERROR_ACCESS_DENIED);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,failed,MAX_PATH,RootNoError)&&!failed[0]&&GetLastError()==ERROR_GEN_FAILURE);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(NULL,GetCurrentProcessId(),created,failed,MAX_PATH,CreateDirectoryW)&&!failed[0]&&GetLastError()==ERROR_INVALID_PARAMETER);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(L"",GetCurrentProcessId(),created,failed,MAX_PATH,CreateDirectoryW)&&!failed[0]&&GetLastError()==ERROR_INVALID_PARAMETER);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),NULL,failed,MAX_PATH,CreateDirectoryW)&&!failed[0]&&GetLastError()==ERROR_INVALID_PARAMETER);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,failed,MAX_PATH,NULL)&&!failed[0]&&GetLastError()==ERROR_INVALID_PARAMETER);
    CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,NULL,MAX_PATH,CreateDirectoryW)&&GetLastError()==ERROR_INVALID_PARAMETER);
    failed[0]=L'x';CHECK(!PbHelperTestCreateRoot(parent,GetCurrentProcessId(),created,failed,0,CreateDirectoryW)&&failed[0]==L'x'&&GetLastError()==ERROR_INVALID_PARAMETER);
    ROOT_CALL calls[2]={{.Parent=parent,.Pid=GetCurrentProcessId(),.Created=*created},{.Parent=parent,.Pid=GetCurrentProcessId(),.Created=*created}};HANDLE threads[2];
    for(DWORD i=0;i<2;++i){threads[i]=CreateThread(NULL,0,RootThread,calls+i,0,NULL);CHECK(threads[i]);}
    for(DWORD i=0;i<2;++i){DWORD code;CHECK(WaitForSingleObject(threads[i],5000)==WAIT_OBJECT_0&&GetExitCodeThread(threads[i],&code)&&!code&&CloseHandle(threads[i]));}
    CHECK(wcscmp(calls[0].Path,calls[1].Path)&&RemoveDirectoryW(calls[0].Path)&&RemoveDirectoryW(calls[1].Path)&&RemoveDirectoryW(parent));
    puts("ROOT_SELFTEST: first/all32 collisions preserved, file/long/short/denied/zero-error failures publish no path, concurrent creators joined, owned test paths removed");
}
static VOID RootDone(VOID)
{CHECK(RemoveDirectoryW(Root)&&GetFileAttributesW(Root)==INVALID_FILE_ATTRIBUTES);}
static FIXTURE Load(const WCHAR *path)
{
    FIXTURE f={0};f.Detached=CreateEventW(NULL,TRUE,FALSE,NULL);CHECK(f.Detached);f.Module=LoadLibraryW(path);CHECK(f.Module);
#define GET(field,name) do{*(FARPROC *)&f.field=GetProcAddress(f.Module,name);CHECK(f.field);}while(0)
    GET(Execute,"Execute");GET(Stop,"Stop");GET(Jobs,"Jobs");GET(Owned,"Owned");GET(Loaded,"Loaded");GET(Shutdown,"Shutdown");GET(Setup,"ConfigureTerminal");GET(State,"TerminalState");
#undef GET
    CONFIGURE configure=(CONFIGURE)(void *)GetProcAddress(f.Module,"Configure");CHECK(configure&&configure(Executable,Root,f.Detached));return f;
}
typedef struct CALL {FIXTURE *F;WCHAR Args[320],Response[9];BOOL Okay;DWORD Error;} CALL;
static DWORD WINAPI ExecuteThread(void *opaque)
{CALL *c=opaque;c->Okay=c->F->Execute(L"gated",c->Args,c->Response,9);c->Error=GetLastError();return 0;}
static void Scenario(const WCHAR *path,BOOL cancel,DWORD result,DWORD exitCode,BOOL dropModule)
{
    FIXTURE f=Load(path);WCHAR enteredName[128],gateName[128];
    CHECK(swprintf_s(enteredName,128,L"Local\\Pb-terminal-entered-%lu-%lu",GetCurrentProcessId(),++Serial)>0);
    CHECK(swprintf_s(gateName,128,L"Local\\Pb-terminal-gate-%lu-%lu",GetCurrentProcessId(),Serial)>0);
    HANDLE childEntered=CreateEventW(NULL,TRUE,FALSE,enteredName),childGate=CreateEventW(NULL,TRUE,FALSE,gateName);
    HANDLE retryEntered=CreateEventW(NULL,FALSE,FALSE,NULL),retryGate=CreateEventW(NULL,FALSE,FALSE,NULL);CHECK(childEntered&&childGate&&retryEntered&&retryGate);
    f.Setup(retryEntered,retryGate,childGate,result);
    CALL call={.F=&f};CHECK(swprintf_s(call.Args,320,L"%s %s %lu",enteredName,gateName,exitCode)>0);
    HANDLE caller=CreateThread(NULL,0,ExecuteThread,&call,0,NULL);CHECK(caller&&WaitForSingleObject(childEntered,5000)==WAIT_OBJECT_0);
    DWORD state[4];WCHAR directory[MAX_PATH];ULONGLONG end=GetTickCount64()+5000;
    do{f.State(state,directory);CHECK(GetTickCount64()<end);if(!state[1])SwitchToThread();}while(!state[1]);
    HANDLE process=OpenProcess(SYNCHRONIZE|PROCESS_QUERY_LIMITED_INFORMATION,FALSE,state[1]);CHECK(process);
    if(cancel)f.Stop();else CHECK(SetEvent(childGate));
    for(DWORD retry=1;retry<=2;++retry) {
        CHECK(WaitForSingleObject(retryEntered,5000)==WAIT_OBJECT_0);f.State(state,directory);
        CHECK(state[0]==1&&state[2]==retry&&state[3]>=1&&f.Jobs()==1&&f.Owned()>0&&f.Loaded()>0);
        CHECK(GetFileAttributesW(directory)!=INVALID_FILE_ATTRIBUTES);
        WCHAR dll[MAX_PATH];CHECK(swprintf_s(dll,MAX_PATH,L"%s\\setupapihost.dll",directory)>0&&GetFileAttributesW(dll)!=INVALID_FILE_ATTRIBUTES);
        if(cancel)CHECK(WaitForSingleObject(process,0)==WAIT_TIMEOUT);
        else CHECK(WaitForSingleObject(process,5000)==WAIT_OBJECT_0);
        if(retry==1)CHECK(SetEvent(retryGate));
    }
    if(cancel) {
        CHECK(WaitForSingleObject(caller,5000)==WAIT_OBJECT_0&&!call.Okay&&call.Error==ERROR_CANCELLED);
        CHECK(!f.Shutdown(0)&&GetLastError()==ERROR_TIMEOUT);
    }
    if(dropModule){CHECK(cancel&&FreeLibrary(f.Module)&&WaitForSingleObject(f.Detached,0)==WAIT_TIMEOUT);}
    CHECK(SetEvent(childGate)&&WaitForSingleObject(process,5000)==WAIT_OBJECT_0&&SetEvent(retryGate));
    if(!cancel) {
        CHECK(WaitForSingleObject(caller,5000)==WAIT_OBJECT_0&&!call.Okay);
        CHECK(call.Error==(DWORD)(result==WAIT_FAILED?ERROR_NOT_ENOUGH_MEMORY:ERROR_GEN_FAILURE));
    }
    if(!dropModule) {
        end=GetTickCount64()+5000;while(f.Jobs()||f.Owned()){CHECK(GetTickCount64()<end);SwitchToThread();}
        CHECK(!f.Loaded()&&f.Shutdown(5000)&&FreeLibrary(f.Module));
    }
    CHECK(WaitForSingleObject(f.Detached,5000)==WAIT_OBJECT_0&&GetFileAttributesW(directory)==INVALID_FILE_ATTRIBUTES);
    CHECK(CloseHandle(f.Detached)&&CloseHandle(process)&&CloseHandle(caller)&&CloseHandle(childEntered)&&CloseHandle(childGate)&&CloseHandle(retryEntered)&&CloseHandle(retryGate));
}
static int Child(int argc,WCHAR **argv)
{
    CHECK(argc==6&&!wcscmp(argv[2],L"gated"));HANDLE entered=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[3]),gate=OpenEventW(SYNCHRONIZE,FALSE,argv[4]);CHECK(entered&&gate);
    DWORD written;CHECK(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),L"0",sizeof(WCHAR),&written,NULL)&&written==sizeof(WCHAR));
    CHECK(SetEvent(entered)&&WaitForSingleObject(gate,10000)==WAIT_OBJECT_0&&CloseHandle(entered)&&CloseHandle(gate));return (int)wcstoul(argv[5],NULL,10);
}
int wmain(int argc,WCHAR **argv)
{
    if(argc>1&&!wcscmp(argv[1],L"child"))return Child(argc,argv);
    CHECK(argc==2||argc==3);CHECK(GetModuleFileNameW(NULL,Executable,MAX_PATH));
    WCHAR parent[MAX_PATH];DWORD chars=GetFullPathNameW(L"out\\terminal-helper-tests",MAX_PATH,parent,NULL);CHECK(chars&&chars<MAX_PATH);
    CHECK(CreateDirectoryW(parent,NULL)||GetLastError()==ERROR_ALREADY_EXISTS);
    FILETIME created,exited,kernel,user;CHECK(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user));
    CHECK(PbHelperTestCreateRoot(parent,GetCurrentProcessId(),&created,Root,MAX_PATH,CreateDirectoryW));RootOutput();RootSelfTests(&created);
    if(argc==3){CHECK(!wcscmp(argv[2],L"--baseline-control"));Scenario(argv[1],TRUE,WAIT_FAILED,0,FALSE);RootDone();return 0;}
    Scenario(argv[1],TRUE,WAIT_FAILED,0,FALSE);Scenario(argv[1],FALSE,WAIT_FAILED,0,FALSE);
    DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    const DWORD results[]={WAIT_FAILED,WAIT_TIMEOUT,WAIT_ABANDONED_0};
    for(unsigned round=0;round<3;++round)for(unsigned i=0;i<3;++i) {
        Scenario(argv[1],TRUE,results[i],0,round&1);
        Scenario(argv[1],FALSE,results[i],STILL_ACTIVE,FALSE);
    }
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    RootDone();
    puts("PASS: actual helper process cleanup across failed/unexpected termination waits, bounded retry, cancellation, late exit and legitimate exit code 259; process/package/dependencies/job/module retained until signaled, caller DLL reference can drop safely, joined children and zero owned-resource/process-handle residue.");return 0;
}
