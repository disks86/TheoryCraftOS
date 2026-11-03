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

START_TEST(tcos_ntdll)
{
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
#define X(n) p##n = (void *)GetProcAddress(ntdll, #n)
    X(TpAllocPool); X(TpAllocWork); X(TpPostWork); X(TpWaitForWork); X(TpReleaseWork); X(TpReleasePool); X(TpSetPoolMaxThreads); X(TpAllocCleanupGroup); X(TpSimpleTryPost); X(TpReleaseCleanupGroupMembers); X(TpReleaseCleanupGroup); X(TpCallbackMayRunLong);
#undef X
    test_work();
    test_group_cancel();
}
