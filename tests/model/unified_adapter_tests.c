/* SPDX-License-Identifier: GPL-2.0 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <bcrypt.h>
#include "../../api/adapter.h"
#include "../../api/v2/adapter.h"
#define CHECK(x) do { if(!(x)){fprintf(stderr,"%d: %s\n",__LINE__,#x);ExitProcess(1);} }while(0)
static LONG Admitted,Live;
static DWORD NativeError;
static GUID Requested;
static BOOL FailAlloc;
static LONG CryptoAt,CryptoCalls,CryptoHandles;
static BOOL CryptoFail(void) { return ++CryptoCalls==CryptoAt; }
static NTSTATUS WINAPI TestAlgorithm(BCRYPT_ALG_HANDLE *a,LPCWSTR id,LPCWSTR provider,ULONG flags)
{ if(CryptoFail())return (NTSTATUS)0xc0000017L; NTSTATUS r=BCryptOpenAlgorithmProvider(a,id,provider,flags);if(r>=0)CryptoHandles++;return r; }
static NTSTATUS WINAPI TestCreateHash(BCRYPT_ALG_HANDLE a,BCRYPT_HASH_HANDLE *h,PUCHAR object,ULONG size,PUCHAR secret,ULONG secretSize,ULONG flags)
{ if(CryptoFail())return (NTSTATUS)0xc0000017L;NTSTATUS r=BCryptCreateHash(a,h,object,size,secret,secretSize,flags);if(r>=0)CryptoHandles++;return r; }
static NTSTATUS WINAPI TestHashData(BCRYPT_HASH_HANDLE h,PUCHAR bytes,ULONG size,ULONG flags)
{ return CryptoFail()?(NTSTATUS)0xc0000017L:BCryptHashData(h,bytes,size,flags); }
static NTSTATUS WINAPI TestFinish(BCRYPT_HASH_HANDLE h,PUCHAR bytes,ULONG size,ULONG flags)
{ return CryptoFail()?(NTSTATUS)0xc0000017L:BCryptFinishHash(h,bytes,size,flags); }
static NTSTATUS WINAPI TestDestroy(BCRYPT_HASH_HANDLE h) { CHECK(CryptoHandles>0);CryptoHandles--;return BCryptDestroyHash(h); }
static NTSTATUS WINAPI TestCloseAlgorithm(BCRYPT_ALG_HANDLE a,ULONG f) { CHECK(CryptoHandles>0);CryptoHandles--;return BCryptCloseAlgorithmProvider(a,f); }
BOOL PbRuntimeReserveAdapter(void) { if(Admitted==16) return FALSE; Admitted++; return TRUE; }
void PbRuntimeReleaseAdapter(void) { CHECK(Admitted>0); Admitted--; }
WINTUN_ADAPTER_HANDLE WINAPI WintunCreateAdapter(LPCWSTR name,LPCWSTR type,const GUID *guid)
{
    CHECK(!wcscmp(name,L"test")&&!wcscmp(type,L"PacketBridge")); Requested=*guid;
    if(NativeError){SetLastError(NativeError);return NULL;}
    WINTUN_ADAPTER *a=calloc(1,sizeof(*a));CHECK(a);a->InterfaceFilename=L"unified-native";Live++;return a;
}
void WINAPI WintunCloseAdapter(WINTUN_ADAPTER *a) { CHECK(Live>0);Live--;free(a); }
void WINAPI WintunGetAdapterLUID(WINTUN_ADAPTER *a,NET_LUID *luid) { CHECK(a);luid->Value=42; }
static void *TestCalloc(size_t n,size_t s) { return FailAlloc?NULL:calloc(n,s); }
#define calloc TestCalloc
#define BCryptOpenAlgorithmProvider TestAlgorithm
#define BCryptCreateHash TestCreateHash
#define BCryptHashData TestHashData
#define BCryptFinishHash TestFinish
#define BCryptDestroyHash TestDestroy
#define BCryptCloseAlgorithmProvider TestCloseAlgorithm
#include "../../api/v2/adapter_unified.c"
#undef calloc
int main(void)
{
    PB_ADAPTER_DIAGNOSTICS_EX d={0}; PB_ADAPTER *a=(void *)1;
    d.Base.Size=sizeof(d);d.Base.Schema=2;
    CHECK(PbCreateAdapterEx2(NULL,L"id",&a,&d)==PB_INVALID&&!a&&!Admitted);
    CHECK(PbCreateAdapterEx2(L"test",L"../bad",&a,&d)==PB_INVALID&&!a);
    d.Base.Size--;CHECK(PbCreateAdapterEx2(L"test",L"id",&a,&d)==PB_INVALID);d.Base.Size++;
    FailAlloc=TRUE;CHECK(PbCreateAdapterEx2(L"test",L"id",&a,&d)==PB_FULL&&!a&&!Admitted);FailAlloc=FALSE;
    for(CryptoAt=1;CryptoAt<=5;++CryptoAt) {
        CryptoCalls=0;CHECK(PbCreateAdapterEx2(L"test",L"id",&a,&d)==PB_UNSUPPORTED&&!a&&!Admitted&&!CryptoHandles);
    }
    CryptoAt=0;
    NativeError=ERROR_ACCESS_DENIED;
    CHECK(PbCreateAdapterEx2(L"test",L"id",&a,&d)==PB_UNSUPPORTED&&!a&&!Admitted);
    CHECK(d.Base.Win32Error==ERROR_ACCESS_DENIED&&d.Base.Attempts==1);NativeError=0;
    CHECK(PbCreateAdapterEx2(L"test",L"id",&a,&d)==PB_OK&&Admitted==1&&Live==1);
    GUID first=Requested;uint64_t luid;CHECK(PbAdapterLuid(a,&luid)==PB_OK&&luid==42);
    CHECK(!wcscmp(PbAdapterPath(a),L"unified-native"));
    PbAdapterRetain(a);PbAdapterRelease(a);CHECK(Live==1);PbAdapterRelease(a);CHECK(!Live&&!Admitted);
    CHECK(PbAdapterCreate(L"test",L"id",&a)==PB_OK&&!memcmp(&first,&Requested,sizeof(first)));PbAdapterRelease(a);
    CHECK(PbAdapterCreate(L"test",L"other",&a)==PB_OK&&memcmp(&first,&Requested,sizeof(first)));PbAdapterRelease(a);
    PB_ADAPTER *all[16];for(unsigned i=0;i<16;++i)CHECK(PbAdapterCreate(L"test",L"id",all+i)==PB_OK);
    CHECK(PbAdapterCreate(L"test",L"id",&a)==PB_FULL&&!a&&Live==16);
    for(unsigned i=0;i<16;++i)PbAdapterRelease(all[i]);CHECK(!Admitted&&!Live&&!CryptoHandles);
    puts("PASS: shared installer adapter, input validation, OOM/native failures, deterministic instance identity, retained ownership and bounded admission rollback.");return 0;
}
