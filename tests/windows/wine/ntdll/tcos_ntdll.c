/* Tests of the parts of ntdll.dll that TheoryCraftOS added after the Wine tests: waiting on addresses, SRW locks, run-once, keyed events, alerts, and the thread pool.
 * Copyright (C) 2026 Christopher Schaefer. Licensed under the GNU Affero General Public License, version 3 or later. */

#include <stdarg.h>
#include <stdio.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/test.h"

static NTSTATUS (WINAPI *pTpAllocPool)(TP_POOL **, PVOID);
static NTSTATUS (WINAPI *pTpAllocWork)(TP_WORK **, PTP_WORK_CALLBACK, PVOID, TP_CALLBACK_ENVIRON *);
static VOID (WINAPI *pTpPostWork)(TP_WORK *);
static VOID (WINAPI *pTpWaitForWork)(TP_WORK *, BOOL);
static VOID (WINAPI *pTpReleaseWork)(TP_WORK *);
static VOID (WINAPI *pTpReleasePool)(TP_POOL *);
static VOID (WINAPI *pTpSetPoolMaxThreads)(TP_POOL *, DWORD);

static NTSTATUS (WINAPI *pTpAllocCleanupGroup)(TP_CLEANUP_GROUP **);
static NTSTATUS (WINAPI *pTpSimpleTryPost)(PTP_SIMPLE_CALLBACK, PVOID, TP_CALLBACK_ENVIRON *);
static VOID (WINAPI *pTpReleaseCleanupGroupMembers)(TP_CLEANUP_GROUP *, BOOL, PVOID);
static VOID (WINAPI *pTpReleaseCleanupGroup)(TP_CLEANUP_GROUP *);
static NTSTATUS (WINAPI *pTpCallbackMayRunLong)(TP_CALLBACK_INSTANCE *);

static void CALLBACK work_cb(TP_CALLBACK_INSTANCE *instance, void *userdata, TP_WORK *work)
{
    Sleep(20);
    InterlockedIncrement((LONG *)userdata);
}

static void test_work(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_WORK *work = NULL;
    TP_POOL *pool = NULL;
    NTSTATUS status;
    LONG userdata = 0;
    int i;

    status = pTpAllocPool(&pool, NULL);
    ok(!status, "TpAllocPool %lx\n", status);
    pTpSetPoolMaxThreads(pool, 1);
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpAllocWork(&work, work_cb, &userdata, &environment);
    ok(!status && work, "TpAllocWork %lx\n", status);
    printf("work %p pool %p\n", work, pool); fflush(stdout);
    for (i = 0; i < 5; i++) pTpPostWork(work);
    printf("posted\n"); fflush(stdout);
    pTpWaitForWork(work, FALSE);
    printf("waited\n"); fflush(stdout);
    ok(userdata == 5, "userdata %ld\n", userdata);
    userdata = 0;
    for (i = 0; i < 10; i++) pTpPostWork(work);
    pTpWaitForWork(work, TRUE);
    printf("after cancel userdata %ld\n", userdata); fflush(stdout);
    ok(userdata < 10, "cancel: userdata %ld\n", userdata);
    pTpReleaseWork(work);
    pTpReleasePool(pool);
}

static void CALLBACK blocked_cb(TP_CALLBACK_INSTANCE *instance, void *userdata)
{
    HANDLE *semaphores = userdata;
    NTSTATUS status = pTpCallbackMayRunLong(instance);
    int i;
    printf("blocked_cb start %lx in %lu main %lu\n", status, GetCurrentThreadId(), 0ul); fflush(stdout);
    ReleaseSemaphore(semaphores[1], 1, NULL);
    for (i = 0; i < 4; i++)
        ok(WaitForSingleObject(semaphores[0], 1000) == WAIT_OBJECT_0, "cancel callback %d did not come\n", i);
    ReleaseSemaphore(semaphores[1], 1, NULL);
}
static void CALLBACK never_cb(TP_CALLBACK_INSTANCE *instance, void *userdata) { printf("never_cb ran in %lu\n", GetCurrentThreadId()); fflush(stdout); ok(0, "unexpected callback\n"); }
static void CALLBACK cancel_cb(void *object, void *userdata)
{
    HANDLE *semaphores = userdata;
    printf("cancel_cb %p\n", object); fflush(stdout);
    ReleaseSemaphore(semaphores[0], 1, NULL);
}

static void test_group_cancel(void)
{
    TP_CALLBACK_ENVIRON environment;
    TP_CLEANUP_GROUP *group = NULL;
    TP_POOL *pool = NULL;
    HANDLE semaphores[2];
    NTSTATUS status;

    semaphores[0] = CreateSemaphoreA(NULL, 0, 4, NULL);
    semaphores[1] = CreateSemaphoreA(NULL, 0, 1, NULL);
    pTpAllocPool(&pool, NULL);
    pTpSetPoolMaxThreads(pool, 1);
    pTpAllocCleanupGroup(&group);
    memset(&environment, 0, sizeof(environment));
    environment.Version = 1;
    environment.Pool = pool;
    status = pTpSimpleTryPost(blocked_cb, semaphores, &environment);
    ok(!status, "TpSimpleTryPost %lx\n", status);
    ok(WaitForSingleObject(semaphores[1], 1000) == WAIT_OBJECT_0, "first callback did not start\n");
    environment.CleanupGroup = group;
    environment.CleanupGroupCancelCallback = cancel_cb;
    status = pTpSimpleTryPost(never_cb, (void *)0xdeadbeef, &environment);
    ok(!status, "TpSimpleTryPost %lx\n", status);
    printf("releasing members\n"); fflush(stdout);
    pTpReleaseCleanupGroupMembers(group, TRUE, semaphores);
    printf("released members\n"); fflush(stdout);
    Sleep(100);
    ReleaseSemaphore(semaphores[0], 1, NULL);
    ReleaseSemaphore(semaphores[0], 1, NULL);
    ReleaseSemaphore(semaphores[0], 1, NULL);
    ok(WaitForSingleObject(semaphores[1], 4000) == WAIT_OBJECT_0, "blocked callback did not end\n");
    pTpReleaseCleanupGroup(group);
    pTpReleasePool(pool);
}

/* ---- LPC ports: a server thread accepts a connection and answers a request with the text upside down ---- */

typedef struct { USHORT DataLength, TotalLength, Type, DataInfoOffset; ULONG_PTR Process, Thread; ULONG MessageId; ULONG pad; ULONG_PTR ClientViewSize; } PM;
typedef struct { PM header; char text[64]; } PMSG;
static NTSTATUS (WINAPI *pNtCreatePort)(HANDLE *, OBJECT_ATTRIBUTES *, ULONG, ULONG, ULONG);
static NTSTATUS (WINAPI *pNtConnectPort)(HANDLE *, UNICODE_STRING *, void *, void *, void *, ULONG *, void *, ULONG *);
static NTSTATUS (WINAPI *pNtListenPort)(HANDLE, PM *);
static NTSTATUS (WINAPI *pNtAcceptConnectPort)(HANDLE *, void *, PM *, BOOLEAN, void *, void *);
static NTSTATUS (WINAPI *pNtCompleteConnectPort)(HANDLE);
static NTSTATUS (WINAPI *pNtRequestWaitReplyPort)(HANDLE, PM *, PM *);
static NTSTATUS (WINAPI *pNtReplyWaitReceivePort)(HANDLE, void **, PM *, PM *);
static NTSTATUS (WINAPI *pNtRequestPort)(HANDLE, PM *);
static void (WINAPI *pRtlInitUnicodeString)(UNICODE_STRING *, const WCHAR *);
static HANDLE g_lpc_ready;
static const WCHAR lpc_name[] = L"\\RPC Control\\TcosTestPort";

static DWORD WINAPI lpc_server(void *arg)
{
    HANDLE port = NULL, comm = NULL;
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attr;
    PMSG request, reply;
    NTSTATUS status;
    void *context = NULL;
    int i;

    pRtlInitUnicodeString(&name, lpc_name);
    InitializeObjectAttributes(&attr, &name, 0, 0, NULL);
    status = pNtCreatePort(&port, &attr, 100, 0x100, 0);
    ok(!status, "NtCreatePort %lx\n", status);
    SetEvent(g_lpc_ready);
    memset(&request, 0, sizeof(request));
    status = pNtListenPort(port, &request.header);
    ok(!status, "NtListenPort %lx\n", status);
    ok(request.header.Type == 10, "connection request type %u\n", request.header.Type);
    ok(request.header.DataLength == 5 && !memcmp(request.text, "hello", 5), "connection info %u\n", request.header.DataLength);
    memcpy(request.text, "welcome", 8);
    request.header.DataLength = 8;
    request.header.TotalLength = sizeof(PM) + 8;
    status = pNtAcceptConnectPort(&comm, (void *)0x1234, &request.header, TRUE, NULL, NULL);
    ok(!status, "NtAcceptConnectPort %lx\n", status);
    status = pNtCompleteConnectPort(comm);
    ok(!status, "NtCompleteConnectPort %lx\n", status);
    memset(&request, 0, sizeof(request));
    status = pNtReplyWaitReceivePort(port, &context, NULL, &request.header);
    ok(!status, "NtReplyWaitReceivePort %lx\n", status);
    ok(request.header.Type == 1 && context == (void *)0x1234, "request type %u context %p\n", request.header.Type, context);
    for (i = 0; i < 3; i++) reply.text[i] = request.text[2 - i];
    reply.header = request.header;
    reply.header.DataLength = 3;
    reply.header.TotalLength = sizeof(PM) + 3;
    memset(&request, 0, sizeof(request));
    status = pNtReplyWaitReceivePort(port, &context, &reply.header, &request.header);
    ok(!status && request.header.Type == 3 && !memcmp(request.text, "dgm", 3), "datagram: status %lx type %u\n", status, request.header.Type);
    CloseHandle(comm);
    CloseHandle(port);
    return 0;
}

static void test_lpc(void)
{
    HANDLE thread, client = NULL;
    UNICODE_STRING name;
    PMSG request, reply;
    ULONG max_length = 0, info_length;
    char info[64];
    NTSTATUS status;

    g_lpc_ready = CreateEventA(NULL, FALSE, FALSE, NULL);
    thread = CreateThread(NULL, 0, lpc_server, NULL, 0, NULL);
    ok(WaitForSingleObject(g_lpc_ready, 5000) == WAIT_OBJECT_0, "server did not start\n");
    pRtlInitUnicodeString(&name, lpc_name);
    memcpy(info, "hello", 5);
    info_length = 5;
    status = pNtConnectPort(&client, &name, NULL, NULL, NULL, &max_length, info, &info_length);
    ok(!status, "NtConnectPort %lx\n", status);
    ok(info_length == 5 && !memcmp(info, "welcome", 5), "answer %lu %.5s\n", info_length, info);
    ok(max_length == 0x100, "max length %lx\n", max_length);
    memset(&request, 0, sizeof(request));
    request.header.DataLength = 3;
    request.header.TotalLength = sizeof(PM) + 3;
    memcpy(request.text, "abc", 3);
    memset(&reply, 0, sizeof(reply));
    status = pNtRequestWaitReplyPort(client, &request.header, &reply.header);
    ok(!status, "NtRequestWaitReplyPort %lx\n", status);
    ok(reply.header.DataLength == 3 && !memcmp(reply.text, "cba", 3), "reply %.3s\n", reply.text);
    ok(reply.header.MessageId == request.header.MessageId || request.header.MessageId == 0, "message ids\n");
    request.header.DataLength = 3;
    memcpy(request.text, "dgm", 3);
    status = pNtRequestPort(client, &request.header);
    ok(!status, "NtRequestPort %lx\n", status);
    ok(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "server did not end\n");
    CloseHandle(thread);
    CloseHandle(client);
    CloseHandle(g_lpc_ready);
    status = pNtConnectPort(&client, &name, NULL, NULL, NULL, &max_length, NULL, NULL);
    ok(status == STATUS_OBJECT_NAME_NOT_FOUND || status == 0xC0000041, "connect to a closed port %lx\n", status);
}

/* ---- synchronization, WNF, hashes, names, tables ---- */

typedef struct { ULONG a[2], b[4]; BYTE in[64]; BYTE digest[16]; } MD5CTX;
typedef struct { ULONG unk[6], state[5], count[2]; BYTE buffer[64]; } SHACTX;
static LONG g_srw_counter;
static SRWLOCK g_srw = SRWLOCK_INIT;
static DWORD WINAPI srw_thread(void *arg)
{
    int i;
    for (i = 0; i < 2000; i++)
    {
        AcquireSRWLockExclusive(&g_srw);
        g_srw_counter++;
        ReleaseSRWLockExclusive(&g_srw);
    }
    return 0;
}

static DWORD WINAPI wait_address_thread(void *arg)
{
    volatile LONG *value = arg;
    LONG undesired = 0;
    NTSTATUS (WINAPI *wait)(volatile void *, void *, SIZE_T, LARGE_INTEGER *) = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlWaitOnAddress");
    while (*value == 0) wait(value, &undesired, sizeof(undesired), NULL);
    return 0;
}

static ULONG g_wnf_calls;
static NTSTATUS WINAPI wnf_cb(ULONGLONG name, ULONG stamp, void *type, void *context, const void *buffer, ULONG size)
{
    g_wnf_calls++;
    ok(size == 5 && !memcmp(buffer, "state", 5), "wnf data\n");
    return 0;
}

static void CALLBACK once_cb(TP_CALLBACK_INSTANCE *i, void *userdata) { InterlockedIncrement((LONG *)userdata); }
static DWORD CALLBACK work_item(void *p) { InterlockedIncrement((LONG *)p); return 0; }
typedef struct { BYTE opaque[0x80]; } AVL_TABLE;
#define RTL_AVL_TABLE AVL_TABLE
static int NTAPI avl_compare(AVL_TABLE *t, void *a, void *b) { int x = *(int *)a, y = *(int *)b; return x < y ? 0 : (x > y ? 1 : 2); }
static void *NTAPI avl_alloc(AVL_TABLE *t, ULONG size) { return HeapAlloc(GetProcessHeap(), 0, size); }
static void NTAPI avl_free(AVL_TABLE *t, void *p) { HeapFree(GetProcessHeap(), 0, p); }

static void test_misc(void)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    NTSTATUS (WINAPI *pNtQueryEvent)(HANDLE, int, void *, ULONG, ULONG *) = (void *)GetProcAddress(ntdll, "NtQueryEvent");
    NTSTATUS (WINAPI *pNtQuerySemaphore)(HANDLE, int, void *, ULONG, ULONG *) = (void *)GetProcAddress(ntdll, "NtQuerySemaphore");
    NTSTATUS (WINAPI *pNtClearEvent)(HANDLE) = (void *)GetProcAddress(ntdll, "NtClearEvent");
    NTSTATUS (WINAPI *pNtAlertThreadByThreadId)(HANDLE) = (void *)GetProcAddress(ntdll, "NtAlertThreadByThreadId");
    NTSTATUS (WINAPI *pRtlWaitOnAddress)(volatile void *, void *, SIZE_T, LARGE_INTEGER *) = (void *)GetProcAddress(ntdll, "RtlWaitOnAddress");
    void (WINAPI *pRtlWakeAddressAll)(void *) = (void *)GetProcAddress(ntdll, "RtlWakeAddressAll");
    void (WINAPI *pMD5Init)(MD5CTX *) = (void *)GetProcAddress(ntdll, "MD5Init");
    void (WINAPI *pMD5Update)(MD5CTX *, const BYTE *, ULONG) = (void *)GetProcAddress(ntdll, "MD5Update");
    void (WINAPI *pMD5Final)(MD5CTX *) = (void *)GetProcAddress(ntdll, "MD5Final");
    void (WINAPI *pA_SHAInit)(SHACTX *) = (void *)GetProcAddress(ntdll, "A_SHAInit");
    void (WINAPI *pA_SHAUpdate)(SHACTX *, const BYTE *, ULONG) = (void *)GetProcAddress(ntdll, "A_SHAUpdate");
    void (WINAPI *pA_SHAFinal)(SHACTX *, BYTE *) = (void *)GetProcAddress(ntdll, "A_SHAFinal");
    NTSTATUS (WINAPI *pNtCreateWnfStateName)(ULONGLONG *, int, int, BOOLEAN, void *, ULONG, void *) = (void *)GetProcAddress(ntdll, "NtCreateWnfStateName");
    NTSTATUS (WINAPI *pRtlPublishWnfStateData)(ULONGLONG, void *, const void *, ULONG, const void *) = (void *)GetProcAddress(ntdll, "RtlPublishWnfStateData");
    NTSTATUS (WINAPI *pRtlQueryWnfStateData)(ULONG *, ULONGLONG, void *, void *, void *) = (void *)GetProcAddress(ntdll, "RtlQueryWnfStateData");
    BOOLEAN (WINAPI *pRtlIsNameInExpression)(const UNICODE_STRING *, const UNICODE_STRING *, BOOLEAN, WCHAR *) = (void *)GetProcAddress(ntdll, "RtlIsNameInExpression");
    NTSTATUS (WINAPI *pLdrGetProcedureAddress)(HMODULE, ANSI_STRING *, ULONG, void **) = (void *)GetProcAddress(ntdll, "LdrGetProcedureAddress");
    NTSTATUS (WINAPI *pRtlQueueWorkItem)(void *, void *, ULONG) = (void *)GetProcAddress(ntdll, "RtlQueueWorkItem");
    void (WINAPI *pRtlInitializeGenericTableAvl)(RTL_AVL_TABLE *, void *, void *, void *, void *) = (void *)GetProcAddress(ntdll, "RtlInitializeGenericTableAvl");
    void *(WINAPI *pRtlInsertElementGenericTableAvl)(RTL_AVL_TABLE *, void *, ULONG, BOOLEAN *) = (void *)GetProcAddress(ntdll, "RtlInsertElementGenericTableAvl");
    void *(WINAPI *pRtlLookupElementGenericTableAvl)(RTL_AVL_TABLE *, void *) = (void *)GetProcAddress(ntdll, "RtlLookupElementGenericTableAvl");
    void *(WINAPI *pRtlEnumerateGenericTableAvl)(RTL_AVL_TABLE *, BOOLEAN) = (void *)GetProcAddress(ntdll, "RtlEnumerateGenericTableAvl");
    BOOLEAN (WINAPI *pRtlDeleteElementGenericTableAvl)(RTL_AVL_TABLE *, void *) = (void *)GetProcAddress(ntdll, "RtlDeleteElementGenericTableAvl");
    ULONG (WINAPI *pRtlNumberGenericTableElementsAvl)(RTL_AVL_TABLE *) = (void *)GetProcAddress(ntdll, "RtlNumberGenericTableElementsAvl");
    ULONG (WINAPI *pRtlCrc32)(const void *, SIZE_T, ULONG) = (void *)GetProcAddress(ntdll, "RtlCrc32");
    BYTE sha[20];
    static const BYTE sha_abc[20] = {0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d};
    static const BYTE md5_abc[16] = {0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72};
    MD5CTX md5;
    SHACTX sha_ctx;
    HANDLE event, semaphore, threads[4], work_done;
    ULONG info[2], size, i;
    LONG value = 0, undesired = 0, counter = 0;
    LARGE_INTEGER timeout;
    NTSTATUS status;
    RTL_AVL_TABLE table;
    BOOLEAN added;
    int keys[5] = {30, 10, 50, 20, 40};
    static const int expected_order[4] = {10, 20, 40, 50};
    void *element;
    UNICODE_STRING expression, name;
    void *address;
    ANSI_STRING procedure;
    ULONGLONG wnf_name = 0;
    SRWLOCK srw = SRWLOCK_INIT;

    /* queries of events and semaphores */
    event = CreateEventA(NULL, FALSE, TRUE, NULL);
    status = pNtQueryEvent(event, 0, info, 8, &size);
    ok(!status && info[0] == 1 && info[1] == 1 && size == 8, "NtQueryEvent %lx %lu %lu\n", status, info[0], info[1]);
    pNtClearEvent(event);
    pNtQueryEvent(event, 0, info, 8, &size);
    ok(info[1] == 0, "event cleared\n");
    semaphore = CreateSemaphoreA(NULL, 2, 5, NULL);
    status = pNtQuerySemaphore(semaphore, 0, info, 8, &size);
    ok(!status && info[0] == 2 && info[1] == 5, "NtQuerySemaphore %lx %lu %lu\n", status, info[0], info[1]);
    CloseHandle(event);
    CloseHandle(semaphore);

    /* waiting on an address, and the lock built on it */
    timeout.QuadPart = -500000;
    status = pRtlWaitOnAddress(&value, &undesired, sizeof(value), &timeout);
    ok(status == STATUS_TIMEOUT, "wait on address %lx\n", status);
    threads[0] = CreateThread(NULL, 0, wait_address_thread, &value, 0, NULL);
    Sleep(50);
    value = 1;
    pRtlWakeAddressAll(&value);
    ok(WaitForSingleObject(threads[0], 5000) == WAIT_OBJECT_0, "address waiter did not wake\n");
    CloseHandle(threads[0]);
    for (i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, srw_thread, NULL, 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, 20000);
    for (i = 0; i < 4; i++) CloseHandle(threads[i]);
    ok(g_srw_counter == 8000, "SRW counter %ld\n", g_srw_counter);
    ok(TryAcquireSRWLockShared(&srw) && TryAcquireSRWLockShared(&srw) && !TryAcquireSRWLockExclusive(&srw), "shared locks\n");
    ReleaseSRWLockShared(&srw);
    ReleaseSRWLockShared(&srw);
    ok(TryAcquireSRWLockExclusive(&srw), "exclusive after release\n");
    ReleaseSRWLockExclusive(&srw);

    /* keyed events, timers, work items */
    work_done = CreateEventA(NULL, TRUE, FALSE, NULL);
    pRtlQueueWorkItem(work_item, &counter, 0);
    pRtlQueueWorkItem(work_item, &counter, 0);
    for (i = 0; i < 100 && counter < 2; i++) Sleep(20);
    ok(counter == 2, "work items ran %ld\n", counter);
    CloseHandle(work_done);

    /* hashes */
    pMD5Init(&md5);
    pMD5Update(&md5, (const BYTE *)"abc", 3);
    pMD5Final(&md5);
    ok(!memcmp(md5.digest, md5_abc, 16), "MD5 of abc\n");
    pA_SHAInit(&sha_ctx);
    pA_SHAUpdate(&sha_ctx, (const BYTE *)"abc", 3);
    pA_SHAFinal(&sha_ctx, sha);
    ok(!memcmp(sha, sha_abc, 20), "SHA-1 of abc\n");
    ok(pRtlCrc32("123456789", 9, 0) == 0xCBF43926, "CRC-32 %lx\n", pRtlCrc32("123456789", 9, 0));

    /* WNF */
    status = pNtCreateWnfStateName(&wnf_name, 1, 1, FALSE, NULL, 64, NULL);
    ok(!status && wnf_name, "NtCreateWnfStateName %lx\n", status);
    status = pRtlPublishWnfStateData(wnf_name, NULL, "state", 5, NULL);
    ok(!status, "publish %lx\n", status);
    status = pRtlQueryWnfStateData(&size, wnf_name, wnf_cb, NULL, NULL);
    ok(!status && size == 1 && g_wnf_calls == 1, "query %lx stamp %lu calls %lu\n", status, size, g_wnf_calls);

    /* names */
    RtlInitUnicodeString(&expression, L"*.t?t");
    RtlInitUnicodeString(&name, L"readme.txt");
    ok(pRtlIsNameInExpression(&expression, &name, TRUE, NULL), "wildcard match\n");
    RtlInitUnicodeString(&name, L"readme.exe");
    ok(!pRtlIsNameInExpression(&expression, &name, TRUE, NULL), "wildcard mismatch\n");

    /* exports and forwarders */
    RtlInitAnsiString(&procedure, "RtlWaitOnAddress");
    status = pLdrGetProcedureAddress(ntdll, &procedure, 0, &address);
    ok(!status && address == (void *)pRtlWaitOnAddress, "LdrGetProcedureAddress %lx\n", status);
    RtlInitAnsiString(&procedure, "NoSuchExport");
    status = pLdrGetProcedureAddress(ntdll, &procedure, 0, &address);
    ok(status == 0xC0000139, "missing export %lx\n", status);

    /* the AVL table keeps its elements in order */
    pRtlInitializeGenericTableAvl(&table, avl_compare, avl_alloc, avl_free, NULL);
    for (i = 0; i < 5; i++) { element = pRtlInsertElementGenericTableAvl(&table, &keys[i], sizeof(int), &added); ok(element && added, "insert %d\n", keys[i]); }
    ok(pRtlNumberGenericTableElementsAvl(&table) == 5, "count\n");
    element = pRtlLookupElementGenericTableAvl(&table, &keys[3]);
    ok(element && *(int *)element == 20, "lookup\n");
    ok(pRtlDeleteElementGenericTableAvl(&table, &keys[0]), "delete\n");
    for (i = 0, element = pRtlEnumerateGenericTableAvl(&table, TRUE); element; element = pRtlEnumerateGenericTableAvl(&table, FALSE), i++)
        ok(*(int *)element == expected_order[i], "order %d\n", *(int *)element);
    ok(i == 4, "enumerated %lu\n", i);
}

START_TEST(tcos_ntdll)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
#define X(n) p##n = (void *)GetProcAddress(ntdll, #n)
    X(TpAllocPool); X(TpAllocWork); X(TpPostWork); X(TpWaitForWork); X(TpReleaseWork); X(TpReleasePool); X(TpSetPoolMaxThreads); X(TpAllocCleanupGroup); X(TpSimpleTryPost); X(TpReleaseCleanupGroupMembers); X(TpReleaseCleanupGroup); X(TpCallbackMayRunLong);
#undef X
    test_work();
    test_group_cancel();
    pNtCreatePort = (void *)GetProcAddress(ntdll, "NtCreatePort");
    pNtConnectPort = (void *)GetProcAddress(ntdll, "NtConnectPort");
    pNtListenPort = (void *)GetProcAddress(ntdll, "NtListenPort");
    pNtAcceptConnectPort = (void *)GetProcAddress(ntdll, "NtAcceptConnectPort");
    pNtCompleteConnectPort = (void *)GetProcAddress(ntdll, "NtCompleteConnectPort");
    pNtRequestWaitReplyPort = (void *)GetProcAddress(ntdll, "NtRequestWaitReplyPort");
    pNtReplyWaitReceivePort = (void *)GetProcAddress(ntdll, "NtReplyWaitReceivePort");
    pNtRequestPort = (void *)GetProcAddress(ntdll, "NtRequestPort");
    pRtlInitUnicodeString = (void *)GetProcAddress(ntdll, "RtlInitUnicodeString");
    test_lpc();
    test_misc();
}
