/* SPDX-License-Identifier: GPL-2.0 */
/* Whole production logger, native CRT/FormatMessage/LocalFree. Only documented
 * FormatMessage failures are injected; no registry/device API is exercised. */
#ifdef PB_API_TESTS
#error Logger ownership tests require shipping paths
#endif
#ifndef NDEBUG
#error Logger ownership tests require the shipping NDEBUG branch
#endif
#include <winsock2.h>
#include "guard_memory.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include "../../include/packetbridge_version.h"
#include "../../api/logger.h"
#define CHECK SAFETY_CHECK
static DWORD WINAPI ObserveFormat(DWORD,LPCVOID,DWORD,DWORD,LPWSTR,DWORD,va_list *);
static HLOCAL WINAPI ObserveFree(HLOCAL);
#define FormatMessageW ObserveFormat
#define LocalFree ObserveFree
#ifndef PB_LOGGER_SOURCE
#define PB_LOGGER_SOURCE "../../api/logger.c"
#endif
#pragma warning(push)
#pragma warning(disable:4100) /* Production NopLogger's unused callback arguments. */
#include PB_LOGGER_SOURCE
#pragma warning(pop)
#undef FormatMessageW
#undef LocalFree
_Static_assert(sizeof(PB_SOURCE_STRING)==65,"logger tests require shipping fingerprint layout");

static HLOCAL Locals[8];
static ULONG Live,Created,Freed,Peak,Formats,Successful,FailMask,Callbacks,Nested,Depth,Repairs;
static ULONG Cases,TotalRepairs;
static BOOL Reenter,ErrorMessage;
static LPCWSTR Expected;
static DWORD ExpectedError;
static const DWORD CallbackError=ERROR_INVALID_DATA,FreeError=ERROR_INVALID_HANDLE;

static DWORD WINAPI ObserveFormat(DWORD flags,LPCVOID source,DWORD id,DWORD language,LPWSTR output,DWORD size,va_list *args)
{
    CHECK(flags&FORMAT_MESSAGE_ALLOCATE_BUFFER);++Formats;
    if(Formats<=2 && (FailMask&(1u<<(Formats-1)))){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return 0;}
    DWORD result=FormatMessageW(flags,source,id,language,output,size,args);
    if(result){
        HLOCAL p=*(HLOCAL *)output;CHECK(p);ULONG i;for(i=0;i<_countof(Locals)&&Locals[i];++i){}CHECK(i<_countof(Locals));
        Locals[i]=p;++Live;++Created;if(Live>Peak)Peak=Live;if(Formats<=2)Successful|=1u<<(Formats-1);
    }
    return result;
}
static HLOCAL WINAPI ObserveFree(HLOCAL p)
{
    if(p){ULONG i;for(i=0;i<_countof(Locals)&&Locals[i]!=p;++i){}CHECK(i<_countof(Locals));
        CHECK(!LocalFree(p));Locals[i]=NULL;CHECK(Live);--Live;++Freed;}
    /* A boundary fault model, not a claim that native LocalFree changes GLE. */
    SetLastError(FreeError);return NULL;
}
static VOID CALLBACK Record(WINTUN_LOGGER_LEVEL level,DWORD64 timestamp,LPCWSTR line)
{
    CHECK(timestamp&&line&&wcsnlen(line,8192)<8192);++Callbacks;
    LPCWSTR prefix=Depth?L"nested":Expected;CHECK(prefix);
    SIZE_T length=wcslen(prefix);CHECK(!wcsncmp(line,prefix,length));
    if(Depth||ErrorMessage){
        WCHAR code[32];CHECK(swprintf_s(code,_countof(code),L"Code 0x%08X",Depth?0:ExpectedError)>0);
        CHECK(level==WINTUN_LOG_ERR&&line[length]==L':'&&wcsstr(line,code));
    }else CHECK(level==WINTUN_LOG_INFO&&!wcscmp(line,prefix));
    if(Reenter&&!Depth){++Depth;++Nested;CHECK(LoggerError(0,L"nested")==0);--Depth;}
    SetLastError(CallbackError);
}
static VOID Begin(VOID)
{
    CHECK(!Live&&!Depth);for(ULONG i=0;i<_countof(Locals);++i)CHECK(!Locals[i]);
    Created=Freed=Peak=Formats=Successful=FailMask=Callbacks=Nested=Repairs=0;Reenter=ErrorMessage=FALSE;
    WintunSetLogger(Record);
}
static VOID End(VOID)
{CHECK(!Live&&Created==Freed&&!Depth&&Peak<=4);for(ULONG i=0;i<_countof(Locals);++i)CHECK(!Locals[i]);TotalRepairs+=Repairs;++Cases;}
enum { LogDirect,LogFormatted,ErrorDirect,ErrorFormatted,LastFormatted };
static VOID Scenario(ULONG method,ULONG length,DWORD code,DWORD ambient,ULONG failures,BOOL nested)
{
    Begin();FailMask=failures;Reenter=nested;ErrorMessage=method>=ErrorDirect;ExpectedError=method==LastFormatted?ambient:code;
#ifdef PB_GUARD_TESTS
    PB_TEST_GUARD guard=PbTestGuardCreate(((SIZE_T)length+1)*sizeof(WCHAR));
    WCHAR *prefix=(WCHAR *)PbTestGuardSpan(&guard,((SIZE_T)length+1)*sizeof(WCHAR));
#else
    WCHAR *prefix=malloc(((SIZE_T)length+1)*sizeof(WCHAR));CHECK(prefix);
#endif
    for(ULONG i=0;i<length;++i)prefix[i]=(i&1)?L'x':L'\x7f51';prefix[length]=0;
    WCHAR rendered[1024];Expected=prefix;
    if(method==LogFormatted||method==ErrorFormatted||method==LastFormatted){
        ULONG kept=length<1024?length:1023;memcpy(rendered,prefix,kept*sizeof(WCHAR));rendered[kept]=0;
        if(length>=1024)rendered[1022]=L'\x2026';Expected=rendered;
    }
    SetLastError(ambient);DWORD result;
    switch(method){
    case LogDirect:result=LoggerLog(WINTUN_LOG_INFO,prefix);break;
    case LogFormatted:result=LOG(WINTUN_LOG_INFO,L"%s",prefix);break;
    case ErrorDirect:result=LoggerError(code,prefix);break;
    case ErrorFormatted:result=LOG_ERROR(code,L"%s",prefix);break;
    default:result=LOG_LAST_ERROR(L"%s",prefix);break;
    }
    if(method==LogDirect||method==LogFormatted||method==LastFormatted)CHECK(result==ambient&&GetLastError()==ambient);
    else CHECK(result==code); /* Direct Error methods promise their return code;
                              * callers/LAST wrapper restore GLE as appropriate. */
    CHECK(!Repairs&&Nested==(ULONG)(nested&&Callbacks!=0));
    if(method<ErrorDirect)CHECK(Callbacks==(nested?2u:1u)&&Formats==(nested?2u:0u));
    else{CHECK(Formats==(Nested?4u:2u));CHECK(Callbacks==((Successful&2)?(Nested?2u:1u):0u));}
#ifdef PB_GUARD_TESTS
    PbTestGuardDestroy(&guard);
#else
    free(prefix);
#endif
    End();
}
static VOID Setter(VOID)
{
    Begin();WintunSetLogger(NULL);SetLastError(ERROR_ACCESS_DENIED);
    CHECK(LoggerLog(WINTUN_LOG_INFO,L"disabled")==ERROR_ACCESS_DENIED&&GetLastError()==ERROR_ACCESS_DENIED);
    CHECK(LoggerError(0,L"disabled")==0&&Formats==2&&!Callbacks&&Successful==3);End();WintunSetLogger(Record);
}
static VOID Matrix(VOID)
{
    const ULONG lengths[]={0,1,2,1022,1023,1024,4096};const DWORD errors[]={0,ERROR_ACCESS_DENIED,0xe000020bu,MAXDWORD},ambient[]={0,ERROR_ACCESS_DENIED,MAXDWORD};
    for(ULONG method=LogDirect;method<=LastFormatted;++method)for(ULONG n=0;n<_countof(lengths);++n){
        if(method==ErrorDirect&&lengths[n]>1023)continue;
        for(ULONG a=0;a<_countof(ambient);++a)for(ULONG nested=0;nested<2;++nested){
            if(method<ErrorDirect)Scenario(method,lengths[n],0,ambient[a],0,nested!=0);
            else for(ULONG e=0;e<(method==LastFormatted?1u:_countof(errors));++e)for(ULONG fail=0;fail<4;++fail)
                Scenario(method,lengths[n],errors[e],ambient[a],fail,nested!=0);
        }
    }
    Setter();
}
static VOID Selected(BOOL negative)
{
    Begin();Expected=L"owned";ErrorMessage=TRUE;ExpectedError=0;SetLastError(MAXDWORD);
    CHECK(LoggerError(0,Expected)==0&&Formats==2&&Successful==3&&Callbacks==1&&Peak==2);
    if(negative){
        /* The complete production method already returned. Only the missing
         * native SystemMessage Local allocation remains; no callback is live. */
        CHECK(Live==1&&Created==2&&Freed==1&&!Depth);ULONG i;for(i=0;i<_countof(Locals)&&!Locals[i];++i){}CHECK(i<_countof(Locals));
        CHECK(!ObserveFree(Locals[i]));++Repairs;
    }else CHECK(!Live&&Freed==2&&!Repairs);
    End();
}
int main(int argc,char **argv)
{
    BOOL selected=argc==2&&!strcmp(argv[1],"133");CHECK(argc==1||selected);
#ifdef PB_LOGGER_MISSING_SYSTEM_FREE
    BOOL negative=TRUE;CHECK(selected);
#else
    BOOL negative=FALSE;
#endif
    if(selected){Selected(negative);Selected(negative);}else Matrix();
    ULONG warm=Cases,warmRepairs=TotalRepairs;Cases=TotalRepairs=0;DWORD before,after;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    if(selected)Selected(negative);else Matrix();
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after&&!Live);
    CHECK(TotalRepairs==(ULONG)negative&&warmRepairs==(negative?warm:0));WintunSetLogger(NULL);
    printf("PASS: %lu logger ownership cases plus %lu checked warmup; native FormatMessage/LocalFree, active CHECK with NDEBUG, bounded callback reentry, return/GLE contracts, zero Local owners; handles %lu/%lu; compensation%lu/%lu\n",Cases,warm,before,after,TotalRepairs,warmRepairs);
    if(negative){puts("LOGGER_SYSTEM_MESSAGE_NOT_FREED: complete actual LoggerError returned with one native SystemMessage allocation; one real LocalFree compensation per case, all callbacks returned and zero Local owners before133");return 133;}
    return 0;
}
