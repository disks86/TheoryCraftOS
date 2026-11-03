/*
 * TheoryCraftOS - a minimal UEFI x86_64 operating system.
 * Copyright (C) 2026 Christopher Schaefer
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* advapi32.dll: performance counters, the provider side (PerfStartProvider, PerfCreateInstance, PerfSetULongCounterValue ...). A provider keeps its counter sets and their
 * instances in its own process: the template given to PerfSetCounterSetInfo says where each counter lies in the data block that follows an instance's header and name. The query side
 * (PerfOpenQueryHandle, PerfQueryCounterData ...) reads the memory of providers in other processes through a performance service, which this OS does not have: it answers
 * ERROR_NOT_SUPPORTED. */
#include "security.h"
#include <perflib.h>

#define MAX_PROVIDERS 16
#define MAX_SETS 8
#define MAX_INSTANCES 256
#define PROVIDER_BASE 0x50524600u

typedef struct {
    GUID guid;
    DWORD counters;
    PERF_COUNTER_INFO *info;      /* a copy of the template's counter descriptions */
    DWORD data_size;              /* the bytes of counter values of one instance */
    PERF_COUNTERSET_INSTANCE *instances[MAX_INSTANCES];
    DWORD instance_count;
} CounterSet;
typedef struct {
    BOOL used;
    GUID guid;
    PERFLIBREQUEST callback;
    CounterSet sets[MAX_SETS];
    DWORD set_count;
} Provider;
static Provider g_providers[MAX_PROVIDERS];
static volatile LONG g_lock;
static void lock(void) { while (InterlockedCompareExchange(&g_lock, 1, 0)) Sleep(0); }
static void unlock(void) { InterlockedExchange(&g_lock, 0); }

static Provider *provider_of(HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v <= PROVIDER_BASE || v > PROVIDER_BASE + MAX_PROVIDERS) return NULL;
    return g_providers[v - PROVIDER_BASE - 1].used ? &g_providers[v - PROVIDER_BASE - 1] : NULL;
}
static CounterSet *set_of(Provider *p, const GUID *g)
{
    DWORD i;
    for (i = 0; i < p->set_count; ++i) if (!memcmp(&p->sets[i].guid, g, sizeof(GUID))) return &p->sets[i];
    return NULL;
}

__declspec(dllexport) ULONG WINAPI PerfStartProviderEx(LPGUID guid, PPERF_PROVIDER_CONTEXT context, HANDLE *out)
{
    DWORD i;
    if (!guid || !out) return ERROR_INVALID_PARAMETER;
    if (context && context->ContextSize < sizeof(*context)) return ERROR_INVALID_PARAMETER;
    lock();
    for (i = 0; i < MAX_PROVIDERS; ++i)
        if (!g_providers[i].used) {
            memset(&g_providers[i], 0, sizeof(g_providers[i]));
            g_providers[i].used = TRUE;
            g_providers[i].guid = *guid;
            g_providers[i].callback = context ? context->ControlCallback : NULL;
            *out = (HANDLE)(ULONG_PTR)(PROVIDER_BASE + i + 1);
            unlock();
            return ERROR_SUCCESS;
        }
    unlock();
    return ERROR_NOT_ENOUGH_MEMORY;
}
__declspec(dllexport) ULONG WINAPI PerfStartProvider(LPGUID guid, PERFLIBREQUEST callback, HANDLE *out)
{
    PERF_PROVIDER_CONTEXT context;
    memset(&context, 0, sizeof(context));
    context.ContextSize = sizeof(context);
    context.ControlCallback = callback;
    return PerfStartProviderEx(guid, &context, out);
}
__declspec(dllexport) ULONG WINAPI PerfStopProvider(HANDLE h)
{
    Provider *p = provider_of(h);
    DWORD i, j;
    if (!p) return ERROR_INVALID_HANDLE;
    lock();
    for (i = 0; i < p->set_count; ++i) {
        for (j = 0; j < p->sets[i].instance_count; ++j) free(p->sets[i].instances[j]);
        free(p->sets[i].info);
    }
    p->used = FALSE;
    unlock();
    return ERROR_SUCCESS;
}
/* template: a PERF_COUNTERSET_INFO followed by NumCounters PERF_COUNTER_INFO */
__declspec(dllexport) ULONG WINAPI PerfSetCounterSetInfo(HANDLE h, PPERF_COUNTERSET_INFO t, ULONG size)
{
    Provider *p = provider_of(h);
    CounterSet *s;
    DWORD i, data = 0;
    if (!p) return ERROR_INVALID_HANDLE;
    if (!t || size < sizeof(*t)) return ERROR_INVALID_PARAMETER;
    if (t->NumCounters == 0 || t->NumCounters > 1024 || size < sizeof(*t) + t->NumCounters * sizeof(PERF_COUNTER_INFO)) return ERROR_INVALID_PARAMETER;
    if (memcmp(&t->ProviderGuid, &p->guid, sizeof(GUID))) return ERROR_INVALID_PARAMETER;
    if (t->InstanceType > 4) return ERROR_INVALID_PARAMETER;
    lock();
    if ((s = set_of(p, &t->CounterSetGuid))) { unlock(); return ERROR_ALREADY_EXISTS; }
    if (p->set_count >= MAX_SETS) { unlock(); return ERROR_NOT_ENOUGH_MEMORY; }
    s = &p->sets[p->set_count];
    s->guid = t->CounterSetGuid;
    s->counters = t->NumCounters;
    s->info = malloc(t->NumCounters * sizeof(PERF_COUNTER_INFO));
    if (!s->info) { unlock(); return ERROR_NOT_ENOUGH_MEMORY; }
    memcpy(s->info, (BYTE *)(t + 1), t->NumCounters * sizeof(PERF_COUNTER_INFO));
    for (i = 0; i < s->counters; ++i) {
        if (s->info[i].Size != 4 && s->info[i].Size != 8 && s->info[i].Size != sizeof(void *)) { free(s->info); unlock(); return ERROR_INVALID_PARAMETER; }
        if (s->info[i].Offset + s->info[i].Size > data) data = s->info[i].Offset + s->info[i].Size;
    }
    s->data_size = (data + 7) & ~7u;
    p->set_count++;
    unlock();
    return ERROR_SUCCESS;
}
__declspec(dllexport) PPERF_COUNTERSET_INSTANCE WINAPI PerfCreateInstance(HANDLE h, LPCGUID guid, LPCWSTR name, ULONG id)
{
    Provider *p = provider_of(h);
    CounterSet *s;
    PERF_COUNTERSET_INSTANCE *inst;
    DWORD name_bytes, i;
    if (!p || !guid || !name) { SetLastError(!p ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER); return NULL; }
    name_bytes = (DWORD)(wcslen(name) + 1) * 2;
    lock();
    if (!(s = set_of(p, guid))) { unlock(); SetLastError(ERROR_NOT_FOUND); return NULL; }
    for (i = 0; i < s->instance_count; ++i) {
        PERF_COUNTERSET_INSTANCE *o = s->instances[i];
        if (o->InstanceId == id && !wcsicmp((WCHAR *)((BYTE *)o + o->InstanceNameOffset), name)) { unlock(); SetLastError(ERROR_ALREADY_EXISTS); return NULL; }
    }
    if (s->instance_count >= MAX_INSTANCES) { unlock(); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    inst = calloc(1, sizeof(*inst) + ((name_bytes + 7) & ~7u) + s->data_size);
    if (!inst) { unlock(); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    inst->CounterSetGuid = *guid;
    inst->dwSize = sizeof(*inst) + ((name_bytes + 7) & ~7u) + s->data_size;
    inst->InstanceId = id;
    inst->InstanceNameOffset = sizeof(*inst);
    inst->InstanceNameSize = name_bytes;
    memcpy((BYTE *)inst + sizeof(*inst), name, name_bytes);
    s->instances[s->instance_count++] = inst;
    unlock();
    return inst;
}
__declspec(dllexport) ULONG WINAPI PerfDeleteInstance(HANDLE h, PPERF_COUNTERSET_INSTANCE inst)
{
    Provider *p = provider_of(h);
    DWORD i, j;
    if (!p) return ERROR_INVALID_HANDLE;
    if (!inst) return ERROR_INVALID_PARAMETER;
    lock();
    for (i = 0; i < p->set_count; ++i)
        for (j = 0; j < p->sets[i].instance_count; ++j)
            if (p->sets[i].instances[j] == inst) {
                p->sets[i].instances[j] = p->sets[i].instances[--p->sets[i].instance_count];
                free(inst);
                unlock();
                return ERROR_SUCCESS;
            }
    unlock();
    return ERROR_INVALID_PARAMETER;
}
__declspec(dllexport) PPERF_COUNTERSET_INSTANCE WINAPI PerfQueryInstance(HANDLE h, LPCGUID guid, LPCWSTR name, ULONG id)
{
    Provider *p = provider_of(h);
    CounterSet *s;
    DWORD i;
    PERF_COUNTERSET_INSTANCE *found = NULL;
    if (!p || !guid || !name) { SetLastError(!p ? ERROR_INVALID_HANDLE : ERROR_INVALID_PARAMETER); return NULL; }
    lock();
    if ((s = set_of(p, guid)))
        for (i = 0; i < s->instance_count && !found; ++i) {
            PERF_COUNTERSET_INSTANCE *o = s->instances[i];
            if (o->InstanceId == id && !wcsicmp((WCHAR *)((BYTE *)o + o->InstanceNameOffset), name)) found = o;
        }
    unlock();
    if (!found) SetLastError(ERROR_NOT_FOUND);
    return found;
}

/* The place of a counter's value in an instance, with its size. */
static BYTE *counter_slot(Provider *p, PERF_COUNTERSET_INSTANCE *inst, ULONG id, DWORD *size)
{
    CounterSet *s;
    DWORD i;
    if (!inst || !(s = set_of(p, &inst->CounterSetGuid))) return NULL;
    for (i = 0; i < s->counters; ++i)
        if (s->info[i].CounterId == id) {
            *size = s->info[i].Size;
            return (BYTE *)inst + sizeof(*inst) + ((inst->InstanceNameSize + 7) & ~7u) + s->info[i].Offset;
        }
    return NULL;
}
static ULONG change(HANDLE h, PPERF_COUNTERSET_INSTANCE inst, ULONG id, ULONGLONG value, int op, DWORD width)
{
    Provider *p = provider_of(h);
    BYTE *slot;
    DWORD size = 0;
    ULONGLONG current = 0;
    if (!p) return ERROR_INVALID_HANDLE;
    lock();
    slot = counter_slot(p, inst, id, &size);
    if (!slot) { unlock(); return ERROR_NOT_FOUND; }
    if (size != width) { unlock(); return ERROR_INVALID_PARAMETER; }
    if (size == 4) { DWORD v; memcpy(&v, slot, 4); current = v; } else memcpy(&current, slot, 8);
    current = op == 0 ? value : (op > 0 ? current + value : current - value);
    if (size == 4) { DWORD v = (DWORD)current; memcpy(slot, &v, 4); } else memcpy(slot, &current, 8);
    unlock();
    return ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WINAPI PerfSetULongCounterValue(HANDLE h, PPERF_COUNTERSET_INSTANCE i, ULONG id, ULONG v) { return change(h, i, id, v, 0, 4); }
__declspec(dllexport) ULONG WINAPI PerfSetULongLongCounterValue(HANDLE h, PPERF_COUNTERSET_INSTANCE i, ULONG id, ULONGLONG v) { return change(h, i, id, v, 0, 8); }
__declspec(dllexport) ULONG WINAPI PerfIncrementULongCounterValue(HANDLE h, PPERF_COUNTERSET_INSTANCE i, ULONG id, ULONG v) { return change(h, i, id, v, 1, 4); }
__declspec(dllexport) ULONG WINAPI PerfIncrementULongLongCounterValue(HANDLE h, PPERF_COUNTERSET_INSTANCE i, ULONG id, ULONGLONG v) { return change(h, i, id, v, 1, 8); }
__declspec(dllexport) ULONG WINAPI PerfDecrementULongCounterValue(HANDLE h, PPERF_COUNTERSET_INSTANCE i, ULONG id, ULONG v) { return change(h, i, id, v, -1, 4); }
__declspec(dllexport) ULONG WINAPI PerfDecrementULongLongCounterValue(HANDLE h, PPERF_COUNTERSET_INSTANCE i, ULONG id, ULONGLONG v) { return change(h, i, id, v, -1, 8); }
/* a counter that reads a variable of the provider: the pointer is the value, read when the instance is */
__declspec(dllexport) ULONG WINAPI PerfSetCounterRefValue(HANDLE h, PPERF_COUNTERSET_INSTANCE inst, ULONG id, PVOID address)
{
    Provider *p = provider_of(h);
    BYTE *slot;
    DWORD size = 0;
    if (!p) return ERROR_INVALID_HANDLE;
    if (!address) return ERROR_INVALID_PARAMETER;
    lock();
    slot = counter_slot(p, inst, id, &size);
    if (!slot) { unlock(); return ERROR_NOT_FOUND; }
    if (size != sizeof(void *)) { unlock(); return ERROR_INVALID_PARAMETER; }
    memcpy(slot, &address, sizeof(void *));
    unlock();
    return ERROR_SUCCESS;
}

/* the query side */
#define PERF_UNSUPPORTED(name, ...) __declspec(dllexport) ULONG WINAPI name(__VA_ARGS__) { return ERROR_NOT_SUPPORTED; }
PERF_UNSUPPORTED(PerfOpenQueryHandle, PVOID machine, PVOID handle)
PERF_UNSUPPORTED(PerfCloseQueryHandle, PVOID handle)
PERF_UNSUPPORTED(PerfAddCounters, PVOID handle, PVOID identifiers, ULONG size)
PERF_UNSUPPORTED(PerfDeleteCounters, PVOID handle, PVOID identifiers, ULONG size)
PERF_UNSUPPORTED(PerfQueryCounterData, PVOID handle, PVOID data, ULONG size, PVOID returned)
PERF_UNSUPPORTED(PerfQueryCounterInfo, PVOID handle, PVOID info, ULONG size, PVOID returned)
PERF_UNSUPPORTED(PerfQueryCounterSetRegistrationInfo, PVOID machine, PVOID guid, ULONG type, ULONG id, PVOID buffer, PVOID size)
PERF_UNSUPPORTED(PerfEnumerateCounterSet, PVOID machine, PVOID guids, ULONG size, PVOID returned)
PERF_UNSUPPORTED(PerfEnumerateCounterSetInstances, PVOID machine, PVOID guid, PVOID instances, ULONG size, PVOID returned)
PERF_UNSUPPORTED(PerfRegCloseKey, PVOID key)
PERF_UNSUPPORTED(PerfRegEnumKey, PVOID key, ULONG index, PVOID name, PVOID size, PVOID reserved, PVOID cls, PVOID cls_size, PVOID time)
PERF_UNSUPPORTED(PerfRegEnumValue, PVOID key, ULONG index, PVOID name, PVOID size, PVOID reserved, PVOID type, PVOID data, PVOID data_size)
PERF_UNSUPPORTED(PerfRegQueryInfoKey, PVOID key, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g, PVOID h, PVOID i, PVOID j, PVOID k)
PERF_UNSUPPORTED(PerfRegQueryValue, PVOID key, PVOID name, PVOID reserved, PVOID type, PVOID data, PVOID size)
PERF_UNSUPPORTED(PerfRegSetValue, PVOID key, PVOID name, ULONG reserved, ULONG type, PVOID data, ULONG size)
