/* SPDX-License-Identifier: GPL-2.0 */
#include "guard_memory.h"
#include "../../api/wintun.h"
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
typedef BOOL (WINAPI *CONFIGURE)(LPCWSTR,LPCWSTR,HANDLE);
typedef BOOL (WINAPI *EXECUTE)(LPCWSTR,LPCWSTR,LPWSTR,DWORD);
typedef BOOL (WINAPI *ACQUIRE)(VOID);
typedef VOID (WINAPI *ACTION)(VOID);
typedef DWORD (WINAPI *COUNT)(VOID);
typedef VOID (WINAPI *FAILURE)(LONG);
typedef BOOL (WINAPI *SHUTDOWN)(DWORD);
typedef VOID (WINAPI *SET_LOGGER)(WINTUN_LOGGER_CALLBACK);
typedef struct { HMODULE Module; EXECUTE Execute; ACQUIRE Acquire,Touch; ACTION Release,Retain,Stop; COUNT Loaded,Jobs,Steps,Owned;
    FAILURE SetFailure; SHUTDOWN Shutdown; SET_LOGGER SetLogger; HANDLE Detached; } FIXTURE;
static WCHAR Executable[MAX_PATH], Root[MAX_PATH];
static HANDLE LoggerEntered, LoggerGate;
static LONG LogCalls;
static VOID CALLBACK TestLogger(WINTUN_LOGGER_LEVEL level, DWORD64 timestamp, LPCWSTR message)
{
    SAFETY_CHECK((level == WINTUN_LOG_INFO && timestamp == 123 && !wcscmp(message,L"line one")) ||
        (level == WINTUN_LOG_WARN && timestamp == 456 && !wcscmp(message,L"line two")));
    InterlockedIncrement(&LogCalls);
    if (LoggerEntered) { SetEvent(LoggerEntered); SAFETY_CHECK(WaitForSingleObject(LoggerGate,10000)==WAIT_OBJECT_0); }
}
static void WriteText(DWORD stream, LPCWSTR value)
{
    DWORD written; SAFETY_CHECK(WriteFile(GetStdHandle(stream),value,(DWORD)(wcslen(value)*sizeof(WCHAR)),&written,NULL));
}
static int Child(int argc,WCHAR **argv)
{
    SAFETY_CHECK(argc>=3);
    LPCWSTR mode=argv[2];
    if(!wcscmp(mode,L"empty")) return 0;
    if(!wcscmp(mode,L"short")) WriteText(STD_OUTPUT_HANDLE,L"0");
    else if(!wcscmp(mode,L"exact")) WriteText(STD_OUTPUT_HANDLE,L"FFFFFFFF");
    else if(!wcscmp(mode,L"oversize")) WriteText(STD_OUTPUT_HANDLE,L"123456789");
    else if(!wcscmp(mode,L"odd")) { const BYTE data[]={0x31,0,0x32};DWORD written;SAFETY_CHECK(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),data,sizeof(data),&written,NULL)); }
    else if(!wcscmp(mode,L"log")) {
        const WCHAR text[]=L"[+ 123] line one\n[- 456] line two";const BYTE *bytes=(const BYTE *)text;DWORD written;
        SAFETY_CHECK(WriteFile(GetStdHandle(STD_ERROR_HANDLE),bytes,7,&written,NULL));
        SAFETY_CHECK(WriteFile(GetStdHandle(STD_ERROR_HANDLE),bytes+7,sizeof(text)-sizeof(WCHAR)-7,&written,NULL));
        WriteText(STD_OUTPUT_HANDLE,L"0");
    } else if(!wcscmp(mode,L"badlog")) {
        WriteText(STD_ERROR_HANDLE,L"[+ 18446744073709551616] overflow\n");WriteText(STD_OUTPUT_HANDLE,L"0");
    } else if(!wcscmp(mode,L"loglimit")) {
        WCHAR block[2048];for(unsigned i=0;i<_countof(block);++i)block[i]=L'x';DWORD written;
        for(unsigned i=0;i<300;++i)if(!WriteFile(GetStdHandle(STD_ERROR_HANDLE),block,sizeof(block),&written,NULL))break;
    } else if(!wcscmp(mode,L"inherit")) {
        SAFETY_CHECK(argc>=4);HANDLE handle=(HANDLE)(uintptr_t)_wcstoui64(argv[3],NULL,10);DWORD flags;
        WriteText(STD_OUTPUT_HANDLE,GetHandleInformation(handle,&flags)?L"1":L"0");
    } else if(!wcscmp(mode,L"gated") || !wcscmp(mode,L"writer")) {
        SAFETY_CHECK(argc>=5);HANDLE entered=OpenEventW(EVENT_MODIFY_STATE,FALSE,argv[3]);
        HANDLE gate=OpenEventW(SYNCHRONIZE,FALSE,argv[4]);SAFETY_CHECK(entered&&gate&&SetEvent(entered));
        SAFETY_CHECK(WaitForSingleObject(gate,10000)==WAIT_OBJECT_0);CloseHandle(entered);CloseHandle(gate);
        if(!wcscmp(mode,L"gated")) WriteText(STD_OUTPUT_HANDLE,L"0");
    } else if(!wcscmp(mode,L"inherited_writer")) {
        SAFETY_CHECK(argc>=5);WCHAR command[1024];
        SAFETY_CHECK(swprintf_s(command,_countof(command),L"\"%s\" child writer %s %s",argv[0],argv[3],argv[4])>0);
        STARTUPINFOW startup={.cb=sizeof(startup),.dwFlags=STARTF_USESTDHANDLES,
            .hStdInput=GetStdHandle(STD_INPUT_HANDLE),.hStdOutput=GetStdHandle(STD_OUTPUT_HANDLE),.hStdError=GetStdHandle(STD_ERROR_HANDLE)};
        PROCESS_INFORMATION process;SAFETY_CHECK(CreateProcessW(argv[0],command,NULL,NULL,TRUE,CREATE_NO_WINDOW,NULL,NULL,&startup,&process));
        WCHAR pid[9];swprintf_s(pid,_countof(pid),L"%08lX",process.dwProcessId);
        CloseHandle(process.hThread);CloseHandle(process.hProcess);WriteText(STD_OUTPUT_HANDLE,pid);
    } else return 10;
    return 0;
}
static FIXTURE Load(LPCWSTR path)
{
    FIXTURE f={0};f.Detached=CreateEventW(NULL,TRUE,FALSE,NULL);SAFETY_CHECK(f.Detached);
    f.Module=LoadLibraryW(path);SAFETY_CHECK(f.Module);CONFIGURE configure;
#define GET(field,name) do{*(FARPROC *)&f.field=GetProcAddress(f.Module,name);SAFETY_CHECK(f.field);}while(0)
    GET(Execute,"Execute");GET(Acquire,"Acquire");GET(Touch,"Touch");GET(Release,"Release");GET(Retain,"Retain");GET(Stop,"Stop");
    GET(Loaded,"Loaded");GET(Jobs,"Jobs");GET(Steps,"Steps");GET(Owned,"Owned");GET(SetFailure,"SetFailure");GET(Shutdown,"Shutdown");GET(SetLogger,"SetTestLogger");
#undef GET
    *(FARPROC *)&configure=GetProcAddress(f.Module,"Configure");SAFETY_CHECK(configure&&configure(Executable,Root,f.Detached));return f;
}
static void Unload(FIXTURE *f)
{
    SAFETY_CHECK(FreeLibrary(f->Module)&&WaitForSingleObject(f->Detached,10000)==WAIT_OBJECT_0&&CloseHandle(f->Detached));
}
static void Idle(FIXTURE *f)
{
    ULONGLONG deadline=GetTickCount64()+10000;
    while(f->Jobs()||f->Owned()) {SAFETY_CHECK(GetTickCount64()<deadline);SwitchToThread();}
    SAFETY_CHECK(!f->Loaded());
}
static DWORD WINAPI DependencyThread(PVOID context)
{
    FIXTURE *f=context;
    for(unsigned i=0;i<100;++i) {SAFETY_CHECK(f->Acquire()&&f->Touch());f->Retain();f->Release();f->Release();}
    return 0;
}
typedef struct { FIXTURE *Fixture; LPCWSTR Function,Arguments; WCHAR Response[9]; BOOL Okay; DWORD Error; } CALL;
static DWORD WINAPI Call(PVOID context)
{
    CALL *call=context;call->Okay=call->Fixture->Execute(call->Function,call->Arguments,call->Response,_countof(call->Response));
    call->Error=GetLastError();return 0;
}
typedef struct { HANDLE Entered,Gate; WCHAR EnteredName[128],GateName[128],Arguments[280]; } GATES;
static GATES Gates(unsigned index)
{
    GATES gates={0};swprintf_s(gates.EnteredName,_countof(gates.EnteredName),L"Local\\Wintun-helper-entered-%lu-%u",GetCurrentProcessId(),index);
    swprintf_s(gates.GateName,_countof(gates.GateName),L"Local\\Wintun-helper-gate-%lu-%u",GetCurrentProcessId(),index);
    gates.Entered=CreateEventW(NULL,TRUE,FALSE,gates.EnteredName);gates.Gate=CreateEventW(NULL,TRUE,FALSE,gates.GateName);
    SAFETY_CHECK(gates.Entered&&gates.Gate);swprintf_s(gates.Arguments,_countof(gates.Arguments),L"%s %s",gates.EnteredName,gates.GateName);return gates;
}
static void CloseGates(GATES *gates) {SAFETY_CHECK(CloseHandle(gates->Entered)&&CloseHandle(gates->Gate));}
int wmain(int argc,WCHAR **argv)
{
    if(argc>1&&!wcscmp(argv[1],L"child")) return Child(argc,argv);
    SAFETY_CHECK(argc==2&&GetModuleFileNameW(NULL,Executable,_countof(Executable)));
    SAFETY_CHECK(GetFullPathNameW(L"out\\legacy-runtime-tests",_countof(Root),Root,NULL));
    SAFETY_CHECK(CreateDirectoryW(Root,NULL)||GetLastError()==ERROR_ALREADY_EXISTS);
    FIXTURE f=Load(argv[1]);SAFETY_CHECK(!f.Loaded()&&f.Acquire()&&f.Touch()&&f.Loaded());
    f.Retain();f.Release();SAFETY_CHECK(f.Loaded());f.Release();SAFETY_CHECK(!f.Loaded());
    HANDLE threads[4];for(unsigned i=0;i<4;++i) {threads[i]=CreateThread(NULL,0,DependencyThread,&f,0,NULL);SAFETY_CHECK(threads[i]);}
    SAFETY_CHECK(WaitForMultipleObjects(4,threads,TRUE,10000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i)SAFETY_CHECK(CloseHandle(threads[i]));SAFETY_CHECK(!f.Loaded());
    WCHAR response[9];SAFETY_CHECK(f.Execute(L"short",L"",response,9)&&!wcscmp(response,L"0"));Idle(&f);
    SAFETY_CHECK(f.Execute(L"exact",L"",response,9)&&!wcscmp(response,L"FFFFFFFF"));Idle(&f);
    SAFETY_CHECK(!f.Execute(L"oversize",L"",response,9)&&GetLastError()==ERROR_BUFFER_OVERFLOW);Idle(&f);
    SAFETY_CHECK(!f.Execute(L"odd",L"",response,9)&&GetLastError()==ERROR_INVALID_DATA);Idle(&f);
    SAFETY_CHECK(!f.Execute(L"empty",L"",response,9)&&GetLastError()==ERROR_INVALID_DATA);Idle(&f);
    f.SetLogger(TestLogger);SAFETY_CHECK(f.Execute(L"log",L"",response,9)&&LogCalls==2);Idle(&f);
    SAFETY_CHECK(!f.Execute(L"badlog",L"",response,9)&&GetLastError()==ERROR_INVALID_DATA&&LogCalls==2);Idle(&f);
    SAFETY_CHECK(!f.Execute(L"loglimit",L"",response,9)&&GetLastError()==ERROR_BUFFER_OVERFLOW);Idle(&f);
    SECURITY_ATTRIBUTES inherit={sizeof(inherit),NULL,TRUE};HANDLE sentinel=CreateEventW(&inherit,TRUE,FALSE,NULL);SAFETY_CHECK(sentinel);
    WCHAR argument[32];swprintf_s(argument,_countof(argument),L"%llu",(unsigned long long)(uintptr_t)sentinel);
    SAFETY_CHECK(f.Execute(L"inherit",argument,response,9)&&!wcscmp(response,L"0")&&CloseHandle(sentinel));Idle(&f);
    GATES writer=Gates(0);CALL call={.Fixture=&f,.Function=L"inherited_writer",.Arguments=writer.Arguments};
    HANDLE thread=CreateThread(NULL,0,Call,&call,0,NULL);SAFETY_CHECK(thread);
    SAFETY_CHECK(WaitForSingleObject(writer.Entered,5000)==WAIT_OBJECT_0&&WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);
    HANDLE writerProcess=call.Okay?OpenProcess(SYNCHRONIZE,FALSE,wcstoul(call.Response,NULL,16)):NULL;
    SAFETY_CHECK(writerProcess&&WaitForSingleObject(writerProcess,0)==WAIT_TIMEOUT&&SetEvent(writer.Gate)&&CloseHandle(thread));
    SAFETY_CHECK(WaitForSingleObject(writerProcess,5000)==WAIT_OBJECT_0&&CloseHandle(writerProcess));CloseGates(&writer);Idle(&f);
    /* Failure prefixes exercise the same transport and wait for its native owner. */
    f.SetFailure(0);SAFETY_CHECK(f.Execute(L"short",L"",response,9));Idle(&f);DWORD acquisitions=f.Steps();
    for(DWORD fail=1;fail<=acquisitions;++fail) {
        f.SetFailure((LONG)fail);SAFETY_CHECK(!f.Execute(L"short",L"",response,9));Idle(&f);
    }
    f.SetFailure(0);SAFETY_CHECK(f.Execute(L"short",L"",response,9));Idle(&f);Unload(&f);
    f=Load(argv[1]);SAFETY_CHECK(f.Acquire()&&f.Touch());f.Retain();f.Release();
    SAFETY_CHECK(!f.Shutdown(0)&&GetLastError()==ERROR_TIMEOUT);f.Release();
    SAFETY_CHECK(f.Shutdown(0)&&!f.Loaded()&&!f.Acquire()&&GetLastError()==ERROR_SHUTDOWN_IN_PROGRESS);Unload(&f);
    /* Deadline hands ownership over without killing an installer or dropping its package. */
    f=Load(argv[1]);GATES gates=Gates(1);call=(CALL){.Fixture=&f,.Function=L"gated",.Arguments=gates.Arguments};
    thread=CreateThread(NULL,0,Call,&call,0,NULL);SAFETY_CHECK(thread&&WaitForSingleObject(gates.Entered,5000)==WAIT_OBJECT_0);
    SAFETY_CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&!call.Okay&&call.Error==ERROR_TIMEOUT&&f.Jobs()==1);
    SAFETY_CHECK(!f.Execute(L"short",L"",response,9)&&GetLastError()==ERROR_SHUTDOWN_IN_PROGRESS);
    SAFETY_CHECK(!f.Shutdown(0)&&GetLastError()==ERROR_TIMEOUT&&FreeLibrary(f.Module));
    SAFETY_CHECK(WaitForSingleObject(f.Detached,0)==WAIT_TIMEOUT&&SetEvent(gates.Gate));
    SAFETY_CHECK(WaitForSingleObject(f.Detached,10000)==WAIT_OBJECT_0&&CloseHandle(f.Detached)&&CloseHandle(thread));CloseGates(&gates);
    /* Stop wakes four waiters while their admitted processes remain held. */
    f=Load(argv[1]);GATES many[4];CALL calls[4];
    for(unsigned i=0;i<4;++i) {
        many[i]=Gates(i+2);calls[i]=(CALL){.Fixture=&f,.Function=L"gated",.Arguments=many[i].Arguments};
        threads[i]=CreateThread(NULL,0,Call,&calls[i],0,NULL);SAFETY_CHECK(threads[i]&&WaitForSingleObject(many[i].Entered,5000)==WAIT_OBJECT_0);
    }
    SAFETY_CHECK(f.Jobs()==4&&!f.Execute(L"short",L"",response,9)&&GetLastError()==ERROR_BUSY);
    f.Stop();SAFETY_CHECK(WaitForMultipleObjects(4,threads,TRUE,5000)==WAIT_OBJECT_0);
    for(unsigned i=0;i<4;++i)SAFETY_CHECK(!calls[i].Okay&&(calls[i].Error==ERROR_CANCELLED||calls[i].Error==ERROR_TIMEOUT));
    SAFETY_CHECK(!f.Shutdown(0)&&GetLastError()==ERROR_TIMEOUT);
    for(unsigned i=0;i<4;++i)SAFETY_CHECK(SetEvent(many[i].Gate));
    SAFETY_CHECK(f.Shutdown(10000)&&!f.Jobs()&&!f.Loaded());
    SAFETY_CHECK(!f.Execute(L"short",L"",response,9)&&GetLastError()==ERROR_SHUTDOWN_IN_PROGRESS);
    for(unsigned i=0;i<4;++i) {CloseGates(&many[i]);SAFETY_CHECK(CloseHandle(threads[i]));}Unload(&f);
    /* A logger already on the stack participates in shutdown and unload ownership. */
    f=Load(argv[1]);LoggerEntered=CreateEventW(NULL,TRUE,FALSE,NULL);LoggerGate=CreateEventW(NULL,TRUE,FALSE,NULL);
    SAFETY_CHECK(LoggerEntered&&LoggerGate);f.SetLogger(TestLogger);
    call=(CALL){.Fixture=&f,.Function=L"log",.Arguments=L""};thread=CreateThread(NULL,0,Call,&call,0,NULL);SAFETY_CHECK(thread);
    SAFETY_CHECK(WaitForSingleObject(LoggerEntered,5000)==WAIT_OBJECT_0);f.Stop();
    SAFETY_CHECK(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0&&!call.Okay&&!f.Shutdown(0));
    SAFETY_CHECK(SetEvent(LoggerGate)&&f.Shutdown(10000));Unload(&f);
    SAFETY_CHECK(CloseHandle(thread)&&CloseHandle(LoggerEntered)&&CloseHandle(LoggerGate));
    printf("PASS: runtime dependency reload/unload under 400 concurrent leases; real child output, exact capacities, odd UTF-16, fragmented logs, handle whitelist and inherited-writer exit; %lu acquisition failures; timeout handoff, four-job bound, shutdown/logger and callback-tail unload.\n",acquisitions);
    return 0;
}
