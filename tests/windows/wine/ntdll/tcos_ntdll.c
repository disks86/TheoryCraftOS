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
}
