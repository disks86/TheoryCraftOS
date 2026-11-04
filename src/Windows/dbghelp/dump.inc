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

/* Part of dbghelp.c: MiniDumpWriteDump and MiniDumpReadDumpStream. The dump has the thread list (with the context and the stack of each thread), the module list, the system
 * and miscellaneous information, the exception stream if one is given, the user streams and either the stacks as the memory list or, with MiniDumpWithFullMemory, every committed
 * readable range as the 64-bit memory list; MiniDumpWithFullMemoryInfo adds the region list. Not done: the callback of MINIDUMP_CALLBACK_INFORMATION (it is never called), handle
 * data, thread information, unloaded modules, the other optional streams, and version resources or CodeView records for the modules. */

typedef struct { BYTE *d; DWORD len, cap; BOOL fail; } DBUF;

static DWORD dbuf_reserve(DBUF *b, DWORD n, DWORD align) {
  const DWORD at = (b->len + align - 1) & ~(align - 1);
  if (b->fail || at < b->len || n > 0x7FFFFFFFu - at) { b->fail = TRUE; return 0; }
  if (at + n > b->cap) {
    DWORD cap = b->cap ? b->cap : 4096;
    while (cap < at + n) cap *= 2;
    BYTE *p = xrealloc(b->d, cap);
    if (!p) { b->fail = TRUE; return 0; }
    b->d = p;
    b->cap = cap;
  }
  b->len = at + n;
  return at;
}
#define DB(b, type, off) ((type *)((b)->d + (off)))

/* MINIDUMP_STRING for a UTF-16 string: returns the RVA. */
static DWORD dbuf_string(DBUF *b, const WCHAR *s) {
  const DWORD n = (DWORD)wlen(s);
  const DWORD at = dbuf_reserve(b, 4 + (n + 1) * 2, 4);
  if (b->fail) return 0;
  *DB(b, DWORD, at) = n * 2;
  mcopy(b->d + at + 4, s, (n + 1) * 2);
  return at;
}

typedef struct {
  DWORD id, suspend, priority_class, priority;
  DWORD64 teb, stack_start;
  DWORD stack_size;
  CONTEXT *context;
  DWORD stack_rva_at;      /* position of the Rva of the stack descriptor, for the full-memory layout */
} DTHREAD;

typedef struct { DWORD64 start, size; } RANGE;

typedef struct { NTSTATUS exit_status; PVOID teb; HANDLE process_id, thread_id; ULONG_PTR affinity; LONG priority, base_priority; } DTHREAD_BASIC;
__declspec(dllimport) NTSTATUS NTAPI RtlGetVersion(PRTL_OSVERSIONINFOW);

#define open_thread(access, id) OpenThread((access), FALSE, (id))

static BOOL file_write(HANDLE file, const void *data, SIZE_T n) {
  const BYTE *p = data;
  while (n) {
    const DWORD chunk = n > 0x10000 ? 0x10000 : (DWORD)n;
    DWORD got = 0;
    if (!WriteFile(file, p, chunk, &got, NULL) || got != chunk) return FALSE;
    p += chunk;
    n -= chunk;
  }
  return TRUE;
}

static BOOL readable_region(const MEMORY_BASIC_INFORMATION *m) {
  return m->State == MEM_COMMIT && m->Protect != 0 && !(m->Protect & (PAGE_NOACCESS | PAGE_GUARD));
}

/* All regions of the address space of a process, in order (state MEM_FREE ones left out when committed_only is not wanted: the caller filters). */
static MEMORY_BASIC_INFORMATION *query_regions(HANDLE process, DWORD *count) {
  DWORD cap = 256, n = 0;
  MEMORY_BASIC_INFORMATION *list = xalloc(cap * sizeof(*list));
  *count = 0;
  if (!list) return NULL;
  DWORD64 address = 0;
  for (;;) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQueryEx(process, (LPCVOID)(ULONG_PTR)address, &mbi, sizeof(mbi)) || mbi.RegionSize == 0) break;
    if (n == cap) {
      MEMORY_BASIC_INFORMATION *bigger = xrealloc(list, (SIZE_T)cap * 2 * sizeof(*list));
      if (!bigger || cap > 0x1000000) break;
      list = bigger;
      cap *= 2;
    }
    list[n++] = mbi;
    const DWORD64 next = (DWORD64)(ULONG_PTR)mbi.BaseAddress + mbi.RegionSize;
    if (next <= address) break;
    address = next;
  }
  *count = n;
  return list;
}

static __attribute__((noinline)) void capture_self(CONTEXT *c) { RtlCaptureContext(c); }

static BOOL write_dump(HANDLE process, DWORD pid, HANDLE file, DWORD type, PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION user_streams) {
  const BOOL self = pid == GetCurrentProcessId();
  const BOOL full = (type & MiniDumpWithFullMemory) != 0;
  DBUF b = {0};
  DTHREAD *threads = NULL;
  DWORD nthreads = 0;
  BOOL ok = FALSE;
  RANGE *ranges = NULL;
  DWORD nranges = 0;
  MEMORY_BASIC_INFORMATION *regions = NULL;
  DWORD nregions = 0;

  /* threads */
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  DWORD thread_cap = 16;
  threads = xalloc(thread_cap * sizeof(DTHREAD));
  if (!threads) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    for (BOOL more = Thread32First(snapshot, &te); more; more = Thread32Next(snapshot, &te)) {
      if (te.th32OwnerProcessID != pid) continue;
      if (nthreads == thread_cap) {
        DTHREAD *bigger = xrealloc(threads, (SIZE_T)thread_cap * 2 * sizeof(DTHREAD));
        if (!bigger) break;
        threads = bigger;
        thread_cap *= 2;
      }
      DTHREAD *t = &threads[nthreads];
      mzero(t, sizeof(*t));
      t->id = te.th32ThreadID;
      t->context = xalloc(sizeof(CONTEXT));
      if (!t->context) break;
      nthreads++;
    }
    CloseHandle(snapshot);
  }
  for (DWORD i = 0; i < nthreads; i++) {
    DTHREAD *t = &threads[i];
    t->priority_class = GetPriorityClass(process);
    HANDLE h = open_thread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, t->id);
    if (!h) continue;
    t->priority = (DWORD)GetThreadPriority(h);
    t->context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT;
    if (self && t->id == GetCurrentThreadId()) {
      capture_self(t->context);
      t->context->ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS | CONTEXT_FLOATING_POINT;
    } else {
      const DWORD previous = self ? 0 : SuspendThread(h);
      if (previous != (DWORD)-1) {
        t->suspend = self ? 0 : previous;
        if (!GetThreadContext(h, t->context)) mzero(t->context, sizeof(CONTEXT));
        if (!self) ResumeThread(h);
      } else mzero(t->context, sizeof(CONTEXT));
    }
    DTHREAD_BASIC tbi;
    mzero(&tbi, sizeof(tbi));
    if (NtQueryInformationThread(h, (THREADINFOCLASS)0, &tbi, sizeof(tbi), NULL) >= 0) t->teb = (DWORD64)(ULONG_PTR)tbi.teb;
    CloseHandle(h);
    DWORD64 tib[3];      /* ExceptionList, StackBase, StackLimit */
    const DWORD64 sp = t->context->Rsp;
    if (t->teb && target_read(process, t->teb, tib, sizeof(tib)) && sp >= tib[2] && sp < tib[1]) {
      DWORD64 end = tib[1];
      if (end - sp > (1u << 20)) end = sp + (1u << 20);
      t->stack_start = sp;
      t->stack_size = (DWORD)(end - sp);
    }
  }

  /* the memory */
  if (full) {
    regions = query_regions(process, &nregions);
    if (!regions) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
    ranges = xalloc((SIZE_T)(nregions ? nregions : 1) * sizeof(RANGE));
    if (!ranges) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
    for (DWORD i = 0; i < nregions; i++)
      if (readable_region(&regions[i])) { ranges[nranges].start = (DWORD64)(ULONG_PTR)regions[i].BaseAddress; ranges[nranges].size = regions[i].RegionSize; nranges++; }
  }

  /* layout: header, directory (the streams' count is known below), then the streams */
  DWORD max_streams = 14 + (user_streams ? user_streams->UserStreamCount : 0);
  const DWORD header_at = dbuf_reserve(&b, sizeof(MINIDUMP_HEADER), 4);
  const DWORD dir_at = dbuf_reserve(&b, max_streams * sizeof(MINIDUMP_DIRECTORY), 4);
  DWORD nstreams = 0;
  if (b.fail) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
#define ADD_STREAM(stream_type, size, align, offset_var) \
  DWORD offset_var = dbuf_reserve(&b, (size), (align)); \
  do { MINIDUMP_DIRECTORY *d_ = DB(&b, MINIDUMP_DIRECTORY, dir_at) + nstreams++; d_->StreamType = (stream_type); d_->Location.DataSize = (size); d_->Location.Rva = offset_var; } while (0)

  /* system information */
  {
    SYSTEM_INFO si;
    RTL_OSVERSIONINFOW vi;
    GetSystemInfo(&si);
    mzero(&vi, sizeof(vi));
    vi.dwOSVersionInfoSize = sizeof(vi);
    RtlGetVersion(&vi);
    const DWORD csd = dbuf_string(&b, L"");
    ADD_STREAM(SystemInfoStream, sizeof(MINIDUMP_SYSTEM_INFO), 4, at);
    MINIDUMP_SYSTEM_INFO *s = DB(&b, MINIDUMP_SYSTEM_INFO, at);
    s->ProcessorArchitecture = si.wProcessorArchitecture;
    s->ProcessorLevel = si.wProcessorLevel;
    s->ProcessorRevision = si.wProcessorRevision;
    s->NumberOfProcessors = (UCHAR)(si.dwNumberOfProcessors > 255 ? 255 : si.dwNumberOfProcessors);
    s->ProductType = 1;      /* VER_NT_WORKSTATION */
    s->MajorVersion = vi.dwMajorVersion;
    s->MinorVersion = vi.dwMinorVersion;
    s->BuildNumber = vi.dwBuildNumber;
    s->PlatformId = vi.dwPlatformId;
    s->CSDVersionRva = csd;
  }
  /* miscellaneous information */
  {
    FILETIME created, exited, kernel, user;
    ADD_STREAM(MiscInfoStream, sizeof(MINIDUMP_MISC_INFO), 4, at);
    MINIDUMP_MISC_INFO *m = DB(&b, MINIDUMP_MISC_INFO, at);
    m->SizeOfInfo = sizeof(MINIDUMP_MISC_INFO);
    m->Flags1 = MINIDUMP_MISC1_PROCESS_ID;
    m->ProcessId = pid;
    if (GetProcessTimes(process, &created, &exited, &kernel, &user)) {
      const ULONGLONG c = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
      m->Flags1 |= MINIDUMP_MISC1_PROCESS_TIMES;
      m->ProcessCreateTime = (ULONG32)((c - 116444736000000000ull) / 10000000ull);
      m->ProcessUserTime = (ULONG32)((((ULONGLONG)user.dwHighDateTime << 32) | user.dwLowDateTime) / 10000000ull);
      m->ProcessKernelTime = (ULONG32)((((ULONGLONG)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime) / 10000000ull);
    }
  }
  /* modules */
  {
    HMODULE modules[1024];
    DWORD needed = 0;
    DWORD count = 0;
    if (K32EnumProcessModules(process, modules, sizeof(modules), &needed)) count = needed / sizeof(HMODULE) < 1024 ? needed / sizeof(HMODULE) : 1024;
    DWORD *name_rvas = xalloc((count ? count : 1) * sizeof(DWORD));
    if (!name_rvas) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
    for (DWORD i = 0; i < count; i++) {
      WCHAR path[MAX_PATH];
      if (!K32GetModuleFileNameExW(process, modules[i], path, MAX_PATH)) path[0] = 0;
      name_rvas[i] = dbuf_string(&b, path);
    }
    ADD_STREAM(ModuleListStream, 4 + count * sizeof(MINIDUMP_MODULE), 4, at);
    *DB(&b, DWORD, at) = count;
    for (DWORD i = 0; i < count; i++) {
      MINIDUMP_MODULE *m = (MINIDUMP_MODULE *)(b.d + at + 4) + i;
      MODULEINFO mi;
      BYTE head[0x400];
      PEINFO pe;
      m->BaseOfImage = (DWORD64)(ULONG_PTR)modules[i];
      m->ModuleNameRva = name_rvas[i];
      if (K32GetModuleInformation(process, modules[i], &mi, sizeof(mi))) m->SizeOfImage = mi.SizeOfImage;
      if (target_read(process, m->BaseOfImage, head, sizeof(head)) && pe_parse(head, sizeof(head), &pe)) {
        m->CheckSum = pe.checksum;
        m->TimeDateStamp = pe.timestamp;
        if (!m->SizeOfImage) m->SizeOfImage = pe.size_of_image;
      }
    }
    xfree(name_rvas);
  }
  /* threads and their contexts (a stack is copied here unless the dump holds all memory) */
  {
    DWORD *context_at = xalloc((nthreads ? nthreads : 1) * sizeof(DWORD)), *stack_at = xalloc((nthreads ? nthreads : 1) * sizeof(DWORD));
    if (!context_at || !stack_at) { xfree(context_at); xfree(stack_at); SetLastError(ERROR_OUTOFMEMORY); goto done; }
    for (DWORD i = 0; i < nthreads; i++) {
      context_at[i] = dbuf_reserve(&b, sizeof(CONTEXT), 16);
      if (!b.fail) mcopy(b.d + context_at[i], threads[i].context, sizeof(CONTEXT));
      stack_at[i] = 0;
      if (!full && threads[i].stack_size) {
        stack_at[i] = dbuf_reserve(&b, threads[i].stack_size, 16);
        if (!b.fail && !target_read(process, threads[i].stack_start, b.d + stack_at[i], threads[i].stack_size)) mzero(b.d + stack_at[i], threads[i].stack_size);
      }
    }
    ADD_STREAM(ThreadListStream, 4 + nthreads * sizeof(MINIDUMP_THREAD), 4, at);
    *DB(&b, DWORD, at) = nthreads;
    for (DWORD i = 0; i < nthreads && !b.fail; i++) {
      MINIDUMP_THREAD *t = (MINIDUMP_THREAD *)(b.d + at + 4) + i;
      t->ThreadId = threads[i].id;
      t->SuspendCount = threads[i].suspend;
      t->PriorityClass = threads[i].priority_class;
      t->Priority = threads[i].priority;
      t->Teb = threads[i].teb;
      t->Stack.StartOfMemoryRange = threads[i].stack_start;
      t->Stack.Memory.DataSize = threads[i].stack_size;
      t->Stack.Memory.Rva = stack_at[i];
      t->ThreadContext.DataSize = sizeof(CONTEXT);
      t->ThreadContext.Rva = context_at[i];
      threads[i].stack_rva_at = at + 4 + i * (DWORD)sizeof(MINIDUMP_THREAD) + (DWORD)offsetof(MINIDUMP_THREAD, Stack.Memory.Rva);
    }
    if (!full) {
      DWORD with_stack = 0;
      for (DWORD i = 0; i < nthreads; i++) if (threads[i].stack_size) with_stack++;
      ADD_STREAM(MemoryListStream, 4 + with_stack * sizeof(MINIDUMP_MEMORY_DESCRIPTOR), 4, mat);
      *DB(&b, DWORD, mat) = with_stack;
      DWORD k = 0;
      for (DWORD i = 0; i < nthreads && !b.fail; i++) {
        if (!threads[i].stack_size) continue;
        MINIDUMP_MEMORY_DESCRIPTOR *d = (MINIDUMP_MEMORY_DESCRIPTOR *)(b.d + mat + 4) + k++;
        d->StartOfMemoryRange = threads[i].stack_start;
        d->Memory.DataSize = threads[i].stack_size;
        d->Memory.Rva = stack_at[i];
      }
    }
    xfree(context_at);
    xfree(stack_at);
  }
  /* the exception */
  if (exception && exception->ExceptionPointers) {
    EXCEPTION_POINTERS pointers;
    EXCEPTION_RECORD record;
    CONTEXT *context = xalloc(sizeof(CONTEXT));
    BOOL have = FALSE;
    if (context) {
      if (exception->ClientPointers) have = target_read(process, (DWORD64)(ULONG_PTR)exception->ExceptionPointers, &pointers, sizeof(pointers));
      else { pointers = *exception->ExceptionPointers; have = TRUE; }
      if (have && exception->ClientPointers) have = target_read(process, (DWORD64)(ULONG_PTR)pointers.ExceptionRecord, &record, sizeof(record)) && target_read(process, (DWORD64)(ULONG_PTR)pointers.ContextRecord, context, sizeof(CONTEXT));
      else if (have) { record = *pointers.ExceptionRecord; mcopy(context, pointers.ContextRecord, sizeof(CONTEXT)); }
    }
    if (have) {
      const DWORD context_at = dbuf_reserve(&b, sizeof(CONTEXT), 16);
      if (!b.fail) mcopy(b.d + context_at, context, sizeof(CONTEXT));
      ADD_STREAM(ExceptionStream, sizeof(MINIDUMP_EXCEPTION_STREAM), 4, at);
      MINIDUMP_EXCEPTION_STREAM *e = DB(&b, MINIDUMP_EXCEPTION_STREAM, at);
      e->ThreadId = exception->ThreadId;
      e->ExceptionRecord.ExceptionCode = record.ExceptionCode;
      e->ExceptionRecord.ExceptionFlags = record.ExceptionFlags;
      e->ExceptionRecord.ExceptionRecord = (DWORD64)(ULONG_PTR)record.ExceptionRecord;
      e->ExceptionRecord.ExceptionAddress = (DWORD64)(ULONG_PTR)record.ExceptionAddress;
      e->ExceptionRecord.NumberParameters = record.NumberParameters;
      for (DWORD i = 0; i < record.NumberParameters && i < EXCEPTION_MAXIMUM_PARAMETERS; i++) e->ExceptionRecord.ExceptionInformation[i] = record.ExceptionInformation[i];
      e->ThreadContext.DataSize = sizeof(CONTEXT);
      e->ThreadContext.Rva = context_at;
    }
    xfree(context);
  }
  /* the thread information (times, exit status) */
  if (type & MiniDumpWithThreadInfo) {
    ADD_STREAM(ThreadInfoListStream, (DWORD)(sizeof(MINIDUMP_THREAD_INFO_LIST) + (SIZE_T)nthreads * sizeof(MINIDUMP_THREAD_INFO)), 4, at);
    MINIDUMP_THREAD_INFO_LIST *l = DB(&b, MINIDUMP_THREAD_INFO_LIST, at);
    l->SizeOfHeader = sizeof(*l);
    l->SizeOfEntry = sizeof(MINIDUMP_THREAD_INFO);
    l->NumberOfEntries = nthreads;
    for (DWORD i = 0; i < nthreads && !b.fail; i++) {
      MINIDUMP_THREAD_INFO *e = (MINIDUMP_THREAD_INFO *)(b.d + at + sizeof(*l)) + i;
      FILETIME created, exited, kernel, user;
      HANDLE h = open_thread(THREAD_QUERY_INFORMATION, threads[i].id);
      e->ThreadId = threads[i].id;
      e->ExitStatus = STILL_ACTIVE;
      if (h) {
        if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
          e->CreateTime = ((ULONG64)created.dwHighDateTime << 32) | created.dwLowDateTime;
          e->KernelTime = ((ULONG64)kernel.dwHighDateTime << 32) | kernel.dwLowDateTime;
          e->UserTime = ((ULONG64)user.dwHighDateTime << 32) | user.dwLowDateTime;
        }
        CloseHandle(h);
      }
    }
  }
  /* the thread names, if the system keeps them */
  {
    typedef HRESULT(WINAPI * GET_DESCRIPTION)(HANDLE, PWSTR *);
    GET_DESCRIPTION get = (GET_DESCRIPTION)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetThreadDescription");
    if (get && nthreads) {
      DWORD64 *name_rvas = xalloc((SIZE_T)nthreads * sizeof(DWORD64));
      if (!name_rvas) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
      for (DWORD i = 0; i < nthreads; i++) {
        PWSTR name = NULL;
        HANDLE h = open_thread(THREAD_QUERY_LIMITED_INFORMATION, threads[i].id);
        if (h && SUCCEEDED(get(h, &name)) && name) {
          name_rvas[i] = dbuf_string(&b, name);
          LocalFree(name);
        } else name_rvas[i] = dbuf_string(&b, L"");
        if (h) CloseHandle(h);
      }
      ADD_STREAM(ThreadNamesStream, (DWORD)(4 + (SIZE_T)nthreads * sizeof(MINIDUMP_THREAD_NAME)), 4, at);
      *DB(&b, DWORD, at) = nthreads;
      for (DWORD i = 0; i < nthreads && !b.fail; i++) {
        MINIDUMP_THREAD_NAME *e = (MINIDUMP_THREAD_NAME *)(b.d + at + 4) + i;
        e->ThreadId = threads[i].id;
        e->RvaOfThreadName = name_rvas[i];
      }
      xfree(name_rvas);
    }
  }
  /* the region list */
  if (type & MiniDumpWithFullMemoryInfo) {
    if (!regions) regions = query_regions(process, &nregions);
    DWORD used = 0;
    for (DWORD i = 0; i < nregions; i++) if (regions[i].State != MEM_FREE) used++;
    ADD_STREAM(MemoryInfoListStream, sizeof(MINIDUMP_MEMORY_INFO_LIST) + used * sizeof(MINIDUMP_MEMORY_INFO), 4, at);
    MINIDUMP_MEMORY_INFO_LIST *l = DB(&b, MINIDUMP_MEMORY_INFO_LIST, at);
    l->SizeOfHeader = sizeof(*l);
    l->SizeOfEntry = sizeof(MINIDUMP_MEMORY_INFO);
    l->NumberOfEntries = used;
    MINIDUMP_MEMORY_INFO *e = (MINIDUMP_MEMORY_INFO *)(b.d + at + sizeof(*l));
    for (DWORD i = 0; i < nregions && !b.fail; i++) {
      if (regions[i].State == MEM_FREE) continue;
      e->BaseAddress = (DWORD64)(ULONG_PTR)regions[i].BaseAddress;
      e->AllocationBase = (DWORD64)(ULONG_PTR)regions[i].AllocationBase;
      e->AllocationProtect = regions[i].AllocationProtect;
      e->RegionSize = regions[i].RegionSize;
      e->State = regions[i].State;
      e->Protect = regions[i].Protect;
      e->Type = regions[i].Type;
      e++;
    }
  }
  /* the user streams */
  if (user_streams) {
    for (DWORD i = 0; i < user_streams->UserStreamCount; i++) {
      const MINIDUMP_USER_STREAM *u = &user_streams->UserStreamArray[i];
      ADD_STREAM(u->Type, u->BufferSize, 4, at);
      if (!b.fail && u->BufferSize) mcopy(b.d + at, u->Buffer, u->BufferSize);
    }
  }
  /* all of the memory: the descriptors here, the contents after the header block */
  DWORD memory64_at = 0;
  if (full) {
    ADD_STREAM(Memory64ListStream, (DWORD)(sizeof(MINIDUMP_MEMORY64_LIST) - sizeof(MINIDUMP_MEMORY_DESCRIPTOR64) + (SIZE_T)nranges * sizeof(MINIDUMP_MEMORY_DESCRIPTOR64)), 4, at);
    memory64_at = at;
    MINIDUMP_MEMORY64_LIST *l = DB(&b, MINIDUMP_MEMORY64_LIST, at);
    l->NumberOfMemoryRanges = nranges;
    for (DWORD i = 0; i < nranges && !b.fail; i++) {
      l->MemoryRanges[i].StartOfMemoryRange = ranges[i].start;
      l->MemoryRanges[i].DataSize = ranges[i].size;
    }
  }
  if (b.fail) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
  /* the data of the full memory starts after the header block; the stacks of the threads point into it */
  const DWORD data_start = (b.len + 15) & ~15u;
  if (full) {
    DB(&b, MINIDUMP_MEMORY64_LIST, memory64_at)->BaseRva = data_start;
    for (DWORD i = 0; i < nthreads; i++) {
      if (!threads[i].stack_size) continue;
      DWORD64 at = data_start;
      for (DWORD r = 0; r < nranges; r++) {
        if (threads[i].stack_start >= ranges[r].start && threads[i].stack_start - ranges[r].start < ranges[r].size) {
          *DB(&b, DWORD, threads[i].stack_rva_at) = (DWORD)(at + (threads[i].stack_start - ranges[r].start));
          break;
        }
        at += ranges[r].size;
      }
    }
  }
  MINIDUMP_HEADER *h = DB(&b, MINIDUMP_HEADER, header_at);
  h->Signature = MINIDUMP_SIGNATURE;
  h->Version = MINIDUMP_VERSION;
  h->NumberOfStreams = nstreams;
  h->StreamDirectoryRva = dir_at;
  {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    h->TimeDateStamp = (ULONG32)((((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) - 116444736000000000ull) / 10000000ull);
  }
  h->Flags = type;
  /* write it */
  if (!file_write(file, b.d, b.len)) goto done;
  if (full) {
    static const BYTE zeros[16];
    if (!file_write(file, zeros, data_start - b.len)) goto done;
    BYTE *chunk = xalloc(0x10000);
    if (!chunk) { SetLastError(ERROR_OUTOFMEMORY); goto done; }
    for (DWORD r = 0; r < nranges; r++) {
      for (DWORD64 off = 0; off < ranges[r].size; off += 0x10000) {
        const DWORD n = ranges[r].size - off < 0x10000 ? (DWORD)(ranges[r].size - off) : 0x10000;
        if (!target_read(process, ranges[r].start + off, chunk, n)) mzero(chunk, n);
        if (!file_write(file, chunk, n)) { xfree(chunk); goto done; }
      }
    }
    xfree(chunk);
  }
  ok = TRUE;
done:
  if (threads) { for (DWORD i = 0; i < nthreads; i++) xfree(threads[i].context); xfree(threads); }
  xfree(ranges);
  xfree(regions);
  xfree(b.d);
  return ok;
}

EXPORT BOOL WINAPI MiniDumpWriteDump(HANDLE process, DWORD pid, HANDLE file, MINIDUMP_TYPE type, PMINIDUMP_EXCEPTION_INFORMATION exception, PMINIDUMP_USER_STREAM_INFORMATION user_streams,
                                     PMINIDUMP_CALLBACK_INFORMATION callback) {
  if (!process || !file || file == INVALID_HANDLE_VALUE)      /* (-1 is the current process) */ { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
  if (type & ~MiniDumpValidTypeFlags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return write_dump(process, pid, file, type, exception, user_streams);
}

/* The stream of a type in the directory of a dump in memory. */
EXPORT BOOL WINAPI MiniDumpReadDumpStream(PVOID base, ULONG type, PMINIDUMP_DIRECTORY *directory, PVOID *stream, ULONG *size) {
  const MINIDUMP_HEADER *h = base;
  if (!h || h->Signature != MINIDUMP_SIGNATURE || (h->Version & 0xFFFF) != (MINIDUMP_VERSION & 0xFFFF)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  const MINIDUMP_DIRECTORY *d = (const MINIDUMP_DIRECTORY *)((const BYTE *)base + h->StreamDirectoryRva);
  for (ULONG i = 0; i < h->NumberOfStreams; i++) {
    if (d[i].StreamType != type) continue;
    if (directory) *directory = (PMINIDUMP_DIRECTORY)&d[i];
    if (stream) *stream = (BYTE *)base + d[i].Location.Rva;
    if (size) *size = d[i].Location.DataSize;
    return TRUE;
  }
  SetLastError(ERROR_INVALID_PARAMETER);
  return FALSE;
}
