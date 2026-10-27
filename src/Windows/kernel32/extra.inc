/* More of kernel32: small functions that need nothing new from the kernel, fibers, and the thread pool with timer queues (included by kernel32.c). */

/* ---- small things -------------------------------------------------------------------------------------------------------------------------------------- */

EXPORT int WINAPI MulDiv(int number, int numerator, int denominator) {
  if (denominator == 0) return -1;
  const long long product = (long long)number * numerator;
  const long long d = denominator;
  const long long half = (d < 0 ? -d : d) / 2;
  const long long rounded = (product >= 0) == (d >= 0) ? (product >= 0 ? product + half : product - half) : (product >= 0 ? product + half : product - half);
  const long long result = rounded / d;
  if (result > 0x7FFFFFFFll || result < -0x7FFFFFFFll - 1) return -1;
  return (int)result;
}
EXPORT LONG WINAPI CompareFileTime(const FILETIME *a, const FILETIME *b) {
  const ULONGLONG x = ((ULONGLONG)a->dwHighDateTime << 32) | a->dwLowDateTime, y = ((ULONGLONG)b->dwHighDateTime << 32) | b->dwLowDateTime;
  return x < y ? -1 : x > y ? 1 : 0;
}
EXPORT BOOL WINAPI FileTimeToDosDateTime(const FILETIME *ft, LPWORD date, LPWORD time) {
  SYSTEMTIME st;
  if (!FileTimeToSystemTime(ft, &st) || st.wYear < 1980 || st.wYear > 2107) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *date = (WORD)(((st.wYear - 1980) << 9) | (st.wMonth << 5) | st.wDay);
  *time = (WORD)((st.wHour << 11) | (st.wMinute << 5) | (st.wSecond / 2));
  return TRUE;
}
EXPORT BOOL WINAPI DosDateTimeToFileTime(WORD date, WORD time, LPFILETIME ft) {
  SYSTEMTIME st;
  memset(&st, 0, sizeof(st));
  st.wYear = (WORD)(1980 + (date >> 9));
  st.wMonth = (WORD)((date >> 5) & 15);
  st.wDay = (WORD)(date & 31);
  st.wHour = (WORD)(time >> 11);
  st.wMinute = (WORD)((time >> 5) & 63);
  st.wSecond = (WORD)((time & 31) * 2);
  if (!SystemTimeToFileTime(&st, ft)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}

/* Pointers hidden in memory: turned with a per-process cookie. */
static ULONG_PTR pointer_cookie(void) {
  static volatile ULONG_PTR cookie;
  if (!cookie) {
    ULONG c = 0;
    NtQueryInformationProcess(NtCurrentProcess(), 36, &c, sizeof(c), NULL);      /* ProcessCookie */
    cookie = ((ULONG_PTR)c << 32) | c | 1;
  }
  return cookie;
}
EXPORT PVOID WINAPI EncodePointer(PVOID p) { const ULONG_PTR c = pointer_cookie(); ULONG_PTR v = (ULONG_PTR)p ^ c; const unsigned r = (unsigned)(c & 63); return (PVOID)(r ? (v >> r) | (v << (64 - r)) : v); }
EXPORT PVOID WINAPI DecodePointer(PVOID p) { const ULONG_PTR c = pointer_cookie(); ULONG_PTR v = (ULONG_PTR)p; const unsigned r = (unsigned)(c & 63); v = r ? (v << r) | (v >> (64 - r)) : v; return (PVOID)(v ^ c); }
EXPORT PVOID WINAPI EncodeSystemPointer(PVOID p) { return EncodePointer(p); }
EXPORT PVOID WINAPI DecodeSystemPointer(PVOID p) { return DecodePointer(p); }

/* The IsBad... functions look at the page protections (a pointer the program may use without a fault). */
static BOOL range_allowed(const void *p, SIZE_T length, BOOL write) {
  if (length == 0) return TRUE;
  if (!p) return FALSE;
  const BYTE *at = p, *end = at + length;
  if (end < at) return FALSE;
  while (at < end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(at, &info, sizeof(info)) || info.State != MEM_COMMIT) return FALSE;
    const DWORD prot = info.Protect & 0xFF;
    const BOOL readable = prot == PAGE_READONLY || prot == PAGE_READWRITE || prot == 0x08 || prot == 0x20 || prot == 0x40 || prot == 0x80;
    const BOOL writable = prot == PAGE_READWRITE || prot == 0x08 || prot == 0x40 || prot == 0x80;
    if (write ? !writable : !readable) return FALSE;
    at = (const BYTE *)info.BaseAddress + info.RegionSize;
  }
  return TRUE;
}
EXPORT BOOL WINAPI IsBadReadPtr(const void *p, UINT_PTR length) { return !range_allowed(p, length, FALSE); }
EXPORT BOOL WINAPI IsBadWritePtr(LPVOID p, UINT_PTR length) { return !range_allowed(p, length, TRUE); }
EXPORT BOOL WINAPI IsBadCodePtr(FARPROC p) { return !range_allowed((const void *)p, 1, FALSE); }
EXPORT BOOL WINAPI IsBadStringPtrA(LPCSTR s, UINT_PTR max) {
  for (UINT_PTR i = 0; i < max; ++i) { if (!range_allowed(s + i, 1, FALSE)) return TRUE; if (!s[i]) return FALSE; }
  return FALSE;
}
EXPORT BOOL WINAPI IsBadStringPtrW(LPCWSTR s, UINT_PTR max) {
  for (UINT_PTR i = 0; i < max; ++i) { if (!range_allowed(s + i, 2, FALSE)) return TRUE; if (!s[i]) return FALSE; }
  return FALSE;
}
EXPORT BOOL WINAPI FlushInstructionCache(HANDLE process, LPCVOID base, SIZE_T size) { (void)process; (void)base; (void)size; return TRUE; }      /* (x86 keeps instruction and data caches coherent) */

EXPORT UINT WINAPI GetSystemWindowsDirectoryW(LPWSTR buffer, UINT capacity) { return GetWindowsDirectoryW(buffer, capacity); }
EXPORT UINT WINAPI GetSystemWindowsDirectoryA(LPSTR buffer, UINT capacity) { return GetWindowsDirectoryA(buffer, capacity); }
EXPORT DWORD WINAPI GetLogicalDriveStringsW(DWORD capacity, LPWSTR buffer) {
  if (capacity < 5) return 5;
  buffer[0] = 'C'; buffer[1] = ':'; buffer[2] = '\\'; buffer[3] = 0; buffer[4] = 0;
  return 4;
}
EXPORT DWORD WINAPI GetLogicalDriveStringsA(DWORD capacity, LPSTR buffer) {
  if (capacity < 5) return 5;
  buffer[0] = 'C'; buffer[1] = ':'; buffer[2] = '\\'; buffer[3] = 0; buffer[4] = 0;
  return 4;
}

EXPORT DWORD WINAPI GetActiveProcessorCount(WORD group) {
  if (group != 0 && group != 0xFFFF) return 0;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  return si.dwNumberOfProcessors;
}
EXPORT DWORD WINAPI GetMaximumProcessorCount(WORD group) { return GetActiveProcessorCount(group); }
EXPORT WORD WINAPI GetActiveProcessorGroupCount(void) { return 1; }
EXPORT WORD WINAPI GetMaximumProcessorGroupCount(void) { return 1; }
EXPORT BOOL WINAPI QueryUnbiasedInterruptTime(PULONGLONG t) { if (!t) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *t = GetTickCount64() * 10000; return TRUE; }
EXPORT DWORD WINAPI GetProcessIdOfThread(HANDLE thread) {
  BYTE info[48];
  if (!NT_SUCCESS(NtQueryInformationThread(thread, 0, info, sizeof(info), NULL))) { set_error(ERROR_INVALID_HANDLE); return 0; }
  return (DWORD)get64(info + 16);
}
EXPORT BOOL WINAPI QueryProcessCycleTime(HANDLE process, PULONG64 cycles) {
  ULONGLONG t[4];
  const NTSTATUS status = NtQueryInformationProcess(process, 4, t, sizeof(t), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *cycles = t[3] * 300;      /* (100 ns at about 3 GHz) */
  return TRUE;
}
EXPORT BOOL WINAPI QueryThreadCycleTime(HANDLE thread, PULONG64 cycles) {
  ULONGLONG t[4];
  const NTSTATUS status = NtQueryInformationThread(thread, 1, t, sizeof(t), NULL);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  *cycles = t[3] * 300;
  return TRUE;
}
EXPORT DWORD WINAPI GetProcessVersion(DWORD pid) { (void)pid; return (10u << 16) | 0u; }
EXPORT LCID WINAPI GetUserDefaultLCID(void) { return 0x0409; }
EXPORT LCID WINAPI GetSystemDefaultLCID(void) { return 0x0409; }
EXPORT LANGID WINAPI GetUserDefaultLangID(void) { return 0x0409; }
EXPORT LANGID WINAPI GetSystemDefaultLangID(void) { return 0x0409; }
EXPORT LANGID WINAPI GetUserDefaultUILanguage(void) { return 0x0409; }
EXPORT LANGID WINAPI GetSystemDefaultUILanguage(void) { return 0x0409; }
EXPORT LANGID WINAPI GetThreadUILanguage(LANGID l) { (void)l; return 0x0409; }
EXPORT LCID WINAPI GetThreadLocale(void) { return 0x0409; }
EXPORT BOOL WINAPI SetThreadLocale(LCID l) { return l == 0x0409 || l == 0x0400 || l == 0x0800; }
EXPORT int WINAPI GetUserDefaultLocaleName(LPWSTR name, int capacity) { static const WCHAR n[] = {'e', 'n', '-', 'U', 'S', 0}; if (capacity < 6) return 0; memcpy(name, n, sizeof(n)); return 6; }
EXPORT int WINAPI GetSystemDefaultLocaleName(LPWSTR name, int capacity) { return GetUserDefaultLocaleName(name, capacity); }

/* A device control request: no device here answers one. */
EXPORT BOOL WINAPI DeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD returned, LPOVERLAPPED overlapped) {
  (void)h; (void)code; (void)in; (void)in_size; (void)out; (void)out_size;
  if (returned) *returned = 0;
  complete_overlapped(overlapped, (NTSTATUS)0xC0000010, 0);
  set_error(ERROR_INVALID_FUNCTION);
  return FALSE;
}
/* Hard and symbolic links: supported where the mounted file system has a notion of a file separate from its one name (not every one does; CreateHardLink on a
 * directory, or where the file system has none of this, fails with ERROR_NOT_SUPPORTED, which is also what a real hard link on FAT gets). */
EXPORT BOOL WINAPI CreateHardLinkW(LPCWSTR link, LPCWSTR target, LPSECURITY_ATTRIBUTES sa) {
  (void)sa;
  WCHAR source_buffer[PATH_CHARS + 8], link_buffer[PATH_CHARS + 8];
  UNICODE_STRING source_name, link_name;
  OBJECT_ATTRIBUTES source_attributes, link_attributes;
  if (!nt_name(target, source_buffer, &source_name, &source_attributes) || !nt_name(link, link_buffer, &link_name, &link_attributes)) {
    set_error(ERROR_INVALID_PARAMETER);
    return FALSE;
  }
  HANDLE h = NULL;
  IO_STATUS_BLOCK iosb;
  NTSTATUS status = NtOpenFile(&h, FILE_READ_ATTRIBUTES | SYNCHRONIZE, &source_attributes, &iosb, 7, FILE_SYNCHRONOUS_IO_NONALERT_);
  if (!NT_SUCCESS(status)) return fail_nt(status);
  const ULONG name_bytes = link_name.Length;
  BYTE *info = RtlAllocateHeap(heap(), HEAP_ZERO_MEMORY, 20 + name_bytes + 2);
  if (!info) { NtClose(h); set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  *(ULONG *)(info + 16) = name_bytes;      /* [0]: ReplaceIfExists, left 0: CreateHardLink fails if the name is taken */
  memcpy(info + 20, link_name.Buffer, name_bytes);
  status = NtSetInformationFile(h, &iosb, info, 20 + name_bytes, 11);      /* FileLinkInformation */
  RtlFreeHeap(heap(), 0, info);
  NtClose(h);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOL WINAPI CreateHardLinkA(LPCSTR link, LPCSTR target, LPSECURITY_ATTRIBUTES sa) {
  WCHAR *a = to_wide(link), *b = to_wide(target);
  const BOOL ok = a && b ? CreateHardLinkW(a, b, sa) : FALSE;
  free_wide(a);
  free_wide(b);
  return ok;
}
EXPORT BOOLEAN WINAPI CreateSymbolicLinkW(LPCWSTR link, LPCWSTR target, DWORD flags) {
  WCHAR link_buffer[PATH_CHARS + 8];
  UNICODE_STRING link_name;
  OBJECT_ATTRIBUTES link_attributes;
  if (!nt_name(link, link_buffer, &link_name, &link_attributes) || !target || !*target) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  /* The target is given to the kernel as the program wrote it (not resolved against this process's directory): a relative one is the link's own business, resolved
   * against the link's directory, exactly like a real symbolic link. */
  UNICODE_STRING raw_target;
  raw_target.Buffer = (WCHAR *)target;
  raw_target.Length = raw_target.MaximumLength = (USHORT)(wcslen(target) * 2);
  const NTSTATUS status = NtCreateSymbolicLink(&link_attributes, &raw_target, (flags & SYMBOLIC_LINK_FLAG_DIRECTORY) ? 1 : 0);
  return NT_SUCCESS(status) ? TRUE : fail_nt(status);
}
EXPORT BOOLEAN WINAPI CreateSymbolicLinkA(LPCSTR link, LPCSTR target, DWORD flags) {
  WCHAR *a = to_wide(link), *b = target ? to_wide(target) : NULL;
  const BOOLEAN ok = a && b ? CreateSymbolicLinkW(a, b, flags) : FALSE;
  free_wide(a);
  free_wide(b);
  return ok;
}

/* ---- fibers -------------------------------------------------------------------------------------------------------------------------------------------- */

/* A fiber is a stack and the registers a switch has to keep. The thread's own stack is the first fiber (ConvertThreadToFiber). The thread's fiber-local storage is shared. */
typedef struct Fiber {
  PVOID fiber_data;                /* what CreateFiber was given (first, where the inline GetFiberData() of the headers looks: it reads the first word of the current fiber) */
  PVOID rsp;                       /* the saved stack pointer of a fiber that is not running */
  PVOID stack_base, stack_limit;   /* the stack's top and its bottom (the TEB's StackBase and StackLimit while it runs) */
  PVOID allocation;                /* the memory to free; NULL for the thread's own stack */
  LPFIBER_START_ROUTINE start;
  BOOL thread_fiber;
  DWORD magic;
} Fiber;
#define FIBER_MAGIC 0x46494252u
#define TEB_FIBER_DATA(teb) (*(Fiber **)TEB_BYTE(teb, 0x20))
#define TEB_STACK_BASE(teb) (*(PVOID *)TEB_BYTE(teb, 0x08))
#define TEB_STACK_LIMIT(teb) (*(PVOID *)TEB_BYTE(teb, 0x10))

/* switch_stacks(save, target): keeps the registers a Windows function has to keep (and xmm6-xmm15) on the stack, saves the stack pointer in *save and goes on with the one
   in `target`, which has the same layout. */
__asm__(".text\n.globl tcos_switch_stacks\ntcos_switch_stacks:\n"
        " push %rbx\n push %rbp\n push %rdi\n push %rsi\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " sub $160, %rsp\n"
        " movups %xmm6, 0(%rsp)\n movups %xmm7, 16(%rsp)\n movups %xmm8, 32(%rsp)\n movups %xmm9, 48(%rsp)\n movups %xmm10, 64(%rsp)\n"
        " movups %xmm11, 80(%rsp)\n movups %xmm12, 96(%rsp)\n movups %xmm13, 112(%rsp)\n movups %xmm14, 128(%rsp)\n movups %xmm15, 144(%rsp)\n"
        " mov %rsp, (%rcx)\n mov %rdx, %rsp\n"
        " movups 0(%rsp), %xmm6\n movups 16(%rsp), %xmm7\n movups 32(%rsp), %xmm8\n movups 48(%rsp), %xmm9\n movups 64(%rsp), %xmm10\n"
        " movups 80(%rsp), %xmm11\n movups 96(%rsp), %xmm12\n movups 112(%rsp), %xmm13\n movups 128(%rsp), %xmm14\n movups 144(%rsp), %xmm15\n"
        " add $160, %rsp\n"
        " pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rsi\n pop %rdi\n pop %rbp\n pop %rbx\n ret\n"
        ".globl tcos_fiber_entry\ntcos_fiber_entry:\n"      /* a new fiber's first return lands here with the fiber in r12 */
        " mov %r12, %rcx\n sub $32, %rsp\n call tcos_fiber_main\n int3\n");
extern void tcos_switch_stacks(PVOID *save, PVOID target);
extern void tcos_fiber_entry(void);

void tcos_fiber_main(Fiber *fiber) {
  fiber->start(fiber->fiber_data);
  ExitThread(0);      /* (a fiber that returns ends the thread) */
}

static Fiber *current_fiber(void) {
  Fiber *f = TEB_FIBER_DATA(NtCurrentTeb());
  return f && f->magic == FIBER_MAGIC ? f : NULL;
}
EXPORT PVOID WINAPI ConvertThreadToFiberEx(PVOID parameter, DWORD flags) {
  (void)flags;
  if (current_fiber()) { set_error(ERROR_ALREADY_EXISTS); return NULL; }
  Fiber *f = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(Fiber));
  if (!f) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  f->fiber_data = parameter;
  f->stack_base = TEB_STACK_BASE(NtCurrentTeb());
  f->stack_limit = TEB_STACK_LIMIT(NtCurrentTeb());
  f->thread_fiber = TRUE;
  f->magic = FIBER_MAGIC;
  TEB_FIBER_DATA(NtCurrentTeb()) = f;
  return f;
}
EXPORT PVOID WINAPI ConvertThreadToFiber(PVOID parameter) { return ConvertThreadToFiberEx(parameter, 0); }
EXPORT BOOL WINAPI ConvertFiberToThread(void) {
  Fiber *f = current_fiber();
  if (!f || !f->thread_fiber) { set_error(ERROR_ALREADY_THREAD); return FALSE; }
  TEB_FIBER_DATA(NtCurrentTeb()) = NULL;
  f->magic = 0;
  HeapFree(heap(), 0, f);
  return TRUE;
}
EXPORT BOOL WINAPI IsThreadAFiber(void) { return current_fiber() != NULL; }
EXPORT PVOID WINAPI GetCurrentFiber(void) { return current_fiber(); }
EXPORT PVOID WINAPI GetFiberData(void) { Fiber *f = current_fiber(); return f ? f->fiber_data : NULL; }
EXPORT PVOID WINAPI CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE start, PVOID parameter) {
  (void)flags;
  if (!start) { set_error(ERROR_INVALID_PARAMETER); return NULL; }
  SIZE_T size = reserve ? reserve : commit ? commit : 1024 * 1024;
  size = (size + 0xFFF) & ~(SIZE_T)0xFFF;
  if (size < 16384) size = 16384;
  BYTE *stack = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  Fiber *f = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(Fiber));
  if (!stack || !f) { if (stack) VirtualFree(stack, 0, MEM_RELEASE); if (f) HeapFree(heap(), 0, f); set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  f->fiber_data = parameter;
  f->start = start;
  f->allocation = stack;
  f->stack_limit = stack;
  f->stack_base = stack + size;
  f->magic = FIBER_MAGIC;
  /* The first switch to it pops the registers and "returns" to tcos_fiber_entry with the fiber in r12; the layout is what tcos_switch_stacks saved. */
  ULONG_PTR *top = (ULONG_PTR *)((((ULONG_PTR)stack + size) & ~(ULONG_PTR)15) - 64);
  ULONG_PTR *frame = top - (160 / 8) - 9;      /* xmm save area, then rbx rbp rdi rsi r12 r13 r14 r15 and the return address */
  memset(frame, 0, (top - frame) * 8);
  ULONG_PTR *regs = frame + 160 / 8;      /* r15 r14 r13 r12 rsi rdi rbp rbx ret */
  regs[3] = (ULONG_PTR)f;      /* r12 */
  regs[8] = (ULONG_PTR)tcos_fiber_entry;
  f->rsp = frame;
  return f;
}
EXPORT PVOID WINAPI CreateFiber(SIZE_T stack_size, LPFIBER_START_ROUTINE start, PVOID parameter) { return CreateFiberEx(stack_size, stack_size, 0, start, parameter); }
EXPORT void WINAPI SwitchToFiber(PVOID target) {
  Fiber *to = target, *from = current_fiber();
  if (!from || !to || to->magic != FIBER_MAGIC) return;
  if (to == from) return;
  TEB_FIBER_DATA(NtCurrentTeb()) = to;
  TEB_STACK_BASE(NtCurrentTeb()) = to->stack_base;
  TEB_STACK_LIMIT(NtCurrentTeb()) = to->stack_limit;
  tcos_switch_stacks(&from->rsp, to->rsp);
}
EXPORT void WINAPI DeleteFiber(PVOID fiber) {
  Fiber *f = fiber;
  if (!f || f->magic != FIBER_MAGIC) return;
  if (f == current_fiber()) {      /* a fiber deleting itself ends the thread */
    if (f->thread_fiber) { ConvertFiberToThread(); return; }
    ExitThread(0);
  }
  f->magic = 0;
  if (f->allocation) VirtualFree(f->allocation, 0, MEM_RELEASE);
  HeapFree(heap(), 0, f);
}

/* ---- the thread pool, timer queues and registered waits --------------------------------------------------------------------------------------------------- */

/* One pool for the process: worker threads made when work arrives and nobody is idle (up to twice the processors, and at least four), leaving after a while without work.
   Work objects, timers and waits post their callbacks to it; timer queues and waits have a thread each to count the time / watch the handle. */
typedef struct TpInstance { HANDLE event; HANDLE semaphore; LONG release_count; HANDLE mutex; CRITICAL_SECTION *section; HMODULE library; } TpInstance;
typedef struct TpItem { struct TpItem *next; void (*run)(struct TpItem *); PVOID object; PVOID context; } TpItem;

static struct { CRITICAL_SECTION lock; TpItem *head, *tail; HANDLE semaphore; LONG workers, idle, maximum; LONG ready; } g_pool;
static volatile LONG g_pool_state;      /* 0: not made, 1: being made, 2: ready */

static void pool_make(void) {
  if (g_pool_state == 2) return;
  if (InterlockedCompareExchange(&g_pool_state, 1, 0) == 0) {
    RtlInitializeCriticalSection(&g_pool.lock);
    g_pool.semaphore = CreateSemaphoreW(NULL, 0, 0x7FFFFFFF, NULL);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pool.maximum = si.dwNumberOfProcessors * 2 < 4 ? 4 : (LONG)si.dwNumberOfProcessors * 2;
    InterlockedExchange(&g_pool_state, 2);
  } else {
    while (g_pool_state != 2) Sleep(0);
  }
}

static DWORD WINAPI pool_worker(LPVOID parameter) {
  (void)parameter;
  for (;;) {
    EnterCriticalSection(&g_pool.lock);
    ++g_pool.idle;
    LeaveCriticalSection(&g_pool.lock);
    const DWORD got = WaitForSingleObject(g_pool.semaphore, 10000);      /* (a worker with nothing to do for ten seconds leaves) */
    EnterCriticalSection(&g_pool.lock);
    --g_pool.idle;
    TpItem *item = g_pool.head;
    if (item) { g_pool.head = item->next; if (!g_pool.head) g_pool.tail = NULL; }
    if (!item && got == WAIT_TIMEOUT) { --g_pool.workers; LeaveCriticalSection(&g_pool.lock); return 0; }
    LeaveCriticalSection(&g_pool.lock);
    if (item) { item->run(item); HeapFree(heap(), 0, item); }
  }
  return 0;
}

static BOOL pool_post(void (*run)(TpItem *), PVOID object, PVOID context) {
  pool_make();
  TpItem *item = HeapAlloc(heap(), 0, sizeof(TpItem));
  if (!item) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  item->next = NULL;
  item->run = run;
  item->object = object;
  item->context = context;
  EnterCriticalSection(&g_pool.lock);
  if (g_pool.tail) g_pool.tail->next = item; else g_pool.head = item;
  g_pool.tail = item;
  BOOL make_worker = g_pool.idle == 0 && g_pool.workers < g_pool.maximum;
  if (make_worker) ++g_pool.workers;
  LeaveCriticalSection(&g_pool.lock);
  if (make_worker) {
    HANDLE t = CreateThread(NULL, 0, pool_worker, NULL, 0, NULL);
    if (t) CloseHandle(t);
    else { EnterCriticalSection(&g_pool.lock); --g_pool.workers; LeaveCriticalSection(&g_pool.lock); }
  }
  ReleaseSemaphore(g_pool.semaphore, 1, NULL);
  return TRUE;
}

/* What a callback asked to have done when it returns. */
static void instance_finish(TpInstance *i) {
  if (i->section) LeaveCriticalSection(i->section);
  if (i->mutex) ReleaseMutex(i->mutex);
  if (i->semaphore) ReleaseSemaphore(i->semaphore, i->release_count, NULL);
  if (i->event) SetEvent(i->event);
  if (i->library) FreeLibrary(i->library);
}
EXPORT BOOL WINAPI CallbackMayRunLong(PVOID instance) { (void)instance; return TRUE; }
EXPORT void WINAPI SetEventWhenCallbackReturns(PVOID instance, HANDLE event) { ((TpInstance *)instance)->event = event; }
EXPORT void WINAPI ReleaseSemaphoreWhenCallbackReturns(PVOID instance, HANDLE semaphore, DWORD count) { ((TpInstance *)instance)->semaphore = semaphore; ((TpInstance *)instance)->release_count = (LONG)count; }
EXPORT void WINAPI ReleaseMutexWhenCallbackReturns(PVOID instance, HANDLE mutex) { ((TpInstance *)instance)->mutex = mutex; }
EXPORT void WINAPI LeaveCriticalSectionWhenCallbackReturns(PVOID instance, LPCRITICAL_SECTION section) { ((TpInstance *)instance)->section = section; }
EXPORT void WINAPI FreeLibraryWhenCallbackReturns(PVOID instance, HMODULE library) { ((TpInstance *)instance)->library = library; }
EXPORT void WINAPI DisassociateCurrentThreadFromCallback(PVOID instance) { instance_finish((TpInstance *)instance); memset(instance, 0, sizeof(TpInstance)); }

/* QueueUserWorkItem */
typedef DWORD (WINAPI *WORKER_FUNCTION)(PVOID);
static void run_user_work(TpItem *item) { ((WORKER_FUNCTION)item->object)(item->context); }
EXPORT BOOL WINAPI QueueUserWorkItem(WORKER_FUNCTION function, PVOID context, ULONG flags) { (void)flags; if (!function) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return pool_post(run_user_work, (PVOID)function, context); }

/* Simple callbacks, work objects */
typedef void (WINAPI *TP_SIMPLE_CALLBACK_)(PVOID instance, PVOID context);
typedef void (WINAPI *TP_WORK_CALLBACK_)(PVOID instance, PVOID context, PVOID work);
static void run_simple(TpItem *item) { TpInstance in; memset(&in, 0, sizeof(in)); ((TP_SIMPLE_CALLBACK_)item->object)(&in, item->context); instance_finish(&in); }
EXPORT BOOL WINAPI TrySubmitThreadpoolCallback(TP_SIMPLE_CALLBACK_ function, PVOID context, PVOID environment) { (void)environment; if (!function) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } return pool_post(run_simple, (PVOID)function, context); }

typedef struct TpWork { TP_WORK_CALLBACK_ callback; PVOID context; volatile LONG pending; volatile LONG cancelled; HANDLE idle; } TpWork;
static void run_work(TpItem *item) {
  TpWork *w = item->object;
  if (w->cancelled > 0) { InterlockedDecrement(&w->cancelled); }
  else { TpInstance in; memset(&in, 0, sizeof(in)); w->callback(&in, w->context, w); instance_finish(&in); }
  if (InterlockedDecrement(&w->pending) == 0) SetEvent(w->idle);
}
EXPORT PVOID WINAPI CreateThreadpoolWork(TP_WORK_CALLBACK_ callback, PVOID context, PVOID environment) {
  (void)environment;
  TpWork *w = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(TpWork));
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  w->callback = callback;
  w->context = context;
  w->idle = CreateEventW(NULL, TRUE, TRUE, NULL);
  return w;
}
EXPORT void WINAPI SubmitThreadpoolWork(PVOID work) {
  TpWork *w = work;
  if (InterlockedIncrement(&w->pending) == 1) ResetEvent(w->idle);
  if (!pool_post(run_work, w, NULL)) { if (InterlockedDecrement(&w->pending) == 0) SetEvent(w->idle); }
}
EXPORT void WINAPI WaitForThreadpoolWorkCallbacks(PVOID work, BOOL cancel_pending) {
  TpWork *w = work;
  if (cancel_pending) {      /* what has not begun does not run */
    EnterCriticalSection(&g_pool.lock);
    for (TpItem *i = g_pool.head; i; i = i->next) if (i->object == w && i->run == run_work) InterlockedIncrement(&w->cancelled);
    LeaveCriticalSection(&g_pool.lock);
  }
  WaitForSingleObject(w->idle, INFINITE);
}
EXPORT void WINAPI CloseThreadpoolWork(PVOID work) {
  TpWork *w = work;
  WaitForSingleObject(w->idle, INFINITE);
  CloseHandle(w->idle);
  HeapFree(heap(), 0, w);
}
EXPORT PVOID WINAPI CreateThreadpool(PVOID reserved) { (void)reserved; pool_make(); return &g_pool; }
EXPORT void WINAPI CloseThreadpool(PVOID pool) { (void)pool; }
EXPORT BOOL WINAPI SetThreadpoolThreadMinimum(PVOID pool, DWORD minimum) { (void)pool; (void)minimum; return TRUE; }
EXPORT void WINAPI SetThreadpoolThreadMaximum(PVOID pool, DWORD maximum) { (void)pool; if (maximum) { pool_make(); g_pool.maximum = (LONG)maximum; } }
EXPORT PVOID WINAPI CreateThreadpoolCleanupGroup(void) { return HeapAlloc(heap(), HEAP_ZERO_MEMORY, 16); }
EXPORT void WINAPI CloseThreadpoolCleanupGroup(PVOID group) { HeapFree(heap(), 0, group); }
EXPORT void WINAPI CloseThreadpoolCleanupGroupMembers(PVOID group, BOOL cancel, PVOID context) { (void)group; (void)cancel; (void)context; }

/* Timers and waits live in one list that a thread of its own watches. */
typedef enum { TIMER_POOL, TIMER_QUEUE, WAIT_POOL, WAIT_REGISTERED } TimerKind;
typedef void (WINAPI *TP_TIMER_CALLBACK_)(PVOID instance, PVOID context, PVOID timer);
typedef void (WINAPI *TP_WAIT_CALLBACK_)(PVOID instance, PVOID context, PVOID wait, ULONG result);
typedef void (WINAPI *WAITORTIMERCALLBACK_)(PVOID, BOOLEAN);
typedef struct TpTimer {
  struct TpTimer *next;
  TimerKind kind;
  PVOID callback, context;
  ULONGLONG due;            /* GetTickCount64 milliseconds; 0: not armed */
  DWORD period;
  volatile LONG pending;    /* callbacks posted and not finished */
  HANDLE idle;
  BOOL closing;
  DWORD flags;
  void *queue;
} TpTimer;
static struct { CRITICAL_SECTION lock; TpTimer *list; HANDLE wake; HANDLE thread; volatile LONG state; } g_timers;

static void run_timer(TpItem *item) {
  TpTimer *t = item->object;
  TpInstance in;
  memset(&in, 0, sizeof(in));
  if (t->kind == TIMER_POOL) ((TP_TIMER_CALLBACK_)t->callback)(&in, t->context, t);
  else ((WAITORTIMERCALLBACK_)t->callback)(t->context, TRUE);
  instance_finish(&in);
  if (InterlockedDecrement(&t->pending) == 0) SetEvent(t->idle);
}
static DWORD WINAPI timer_thread(LPVOID p) {
  (void)p;
  for (;;) {
    EnterCriticalSection(&g_timers.lock);
    const ULONGLONG now = GetTickCount64();
    ULONGLONG next = ~(ULONGLONG)0;
    for (TpTimer *t = g_timers.list; t; t = t->next) {
      if (t->due == 0 || t->closing) continue;
      if (t->due <= now) {
        if (InterlockedIncrement(&t->pending) == 1) ResetEvent(t->idle);
        if (!pool_post(run_timer, t, NULL)) { if (InterlockedDecrement(&t->pending) == 0) SetEvent(t->idle); }
        t->due = t->period ? now + t->period : 0;
      }
      if (t->due != 0 && t->due < next) next = t->due;
    }
    LeaveCriticalSection(&g_timers.lock);
    WaitForSingleObject(g_timers.wake, next == ~(ULONGLONG)0 ? INFINITE : (DWORD)(next > now ? (next - now > 0x7FFFFFFF ? 0x7FFFFFFF : next - now) : 0));
  }
  return 0;
}
static void timers_start(void) {
  if (g_timers.state == 2) return;
  if (InterlockedCompareExchange(&g_timers.state, 1, 0) == 0) {
    RtlInitializeCriticalSection(&g_timers.lock);
    g_timers.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    g_timers.thread = CreateThread(NULL, 0, timer_thread, NULL, 0, NULL);
    InterlockedExchange(&g_timers.state, 2);
  } else {
    while (g_timers.state != 2) Sleep(0);
  }
}
static TpTimer *timer_new(TimerKind kind, PVOID callback, PVOID context) {
  timers_start();
  TpTimer *t = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(TpTimer));
  if (!t) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  t->kind = kind;
  t->callback = callback;
  t->context = context;
  t->idle = CreateEventW(NULL, TRUE, TRUE, NULL);
  EnterCriticalSection(&g_timers.lock);
  t->next = g_timers.list;
  g_timers.list = t;
  LeaveCriticalSection(&g_timers.lock);
  return t;
}
static void timer_arm(TpTimer *t, ULONGLONG delay_ms, DWORD period) {
  EnterCriticalSection(&g_timers.lock);
  t->due = GetTickCount64() + delay_ms + 1;
  t->period = period;
  LeaveCriticalSection(&g_timers.lock);
  SetEvent(g_timers.wake);
}
static void timer_disarm(TpTimer *t) {
  EnterCriticalSection(&g_timers.lock);
  t->due = 0;
  LeaveCriticalSection(&g_timers.lock);
}
static void timer_free(TpTimer *t) {
  EnterCriticalSection(&g_timers.lock);
  t->closing = TRUE;
  t->due = 0;
  for (TpTimer **link = &g_timers.list; *link; link = &(*link)->next) if (*link == t) { *link = t->next; break; }
  LeaveCriticalSection(&g_timers.lock);
  WaitForSingleObject(t->idle, INFINITE);
  CloseHandle(t->idle);
  HeapFree(heap(), 0, t);
}
/* A FILETIME (negative: relative, 100 ns) as milliseconds from now. */
static ULONGLONG due_to_delay(const FILETIME *due) {
  LONGLONG v = (LONGLONG)(((ULONGLONG)due->dwHighDateTime << 32) | due->dwLowDateTime);
  if (v < 0) return (ULONGLONG)(-v) / 10000;
  FILETIME now;
  GetSystemTimeAsFileTime(&now);
  const LONGLONG n = (LONGLONG)(((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime);
  return v > n ? (ULONGLONG)(v - n) / 10000 : 0;
}
EXPORT PVOID WINAPI CreateThreadpoolTimer(TP_TIMER_CALLBACK_ callback, PVOID context, PVOID environment) { (void)environment; return timer_new(TIMER_POOL, (PVOID)callback, context); }
EXPORT void WINAPI SetThreadpoolTimer(PVOID timer, PFILETIME due, DWORD period, DWORD window) {
  (void)window;
  if (!due) { timer_disarm(timer); return; }
  timer_arm(timer, due_to_delay(due), period);
}
EXPORT BOOL WINAPI IsThreadpoolTimerSet(PVOID timer) { return ((TpTimer *)timer)->due != 0; }
EXPORT void WINAPI WaitForThreadpoolTimerCallbacks(PVOID timer, BOOL cancel_pending) { if (cancel_pending) timer_disarm(timer); WaitForSingleObject(((TpTimer *)timer)->idle, INFINITE); }
EXPORT void WINAPI CloseThreadpoolTimer(PVOID timer) { timer_free(timer); }

/* Timer queues: a handle is a list owner; the timers it made are freed with it. */
typedef struct TpQueue { int dummy; } TpQueue;
EXPORT HANDLE WINAPI CreateTimerQueue(void) { timers_start(); return HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(TpQueue)); }
EXPORT BOOL WINAPI CreateTimerQueueTimer(PHANDLE new_timer, HANDLE queue, WAITORTIMERCALLBACK_ callback, PVOID context, DWORD due_ms, DWORD period_ms, ULONG flags) {
  TpTimer *t = timer_new(TIMER_QUEUE, (PVOID)callback, context);
  if (!t) return FALSE;
  t->queue = queue;
  t->flags = flags;
  timer_arm(t, due_ms, period_ms);
  *new_timer = t;
  return TRUE;
}
EXPORT BOOL WINAPI ChangeTimerQueueTimer(HANDLE queue, HANDLE timer, ULONG due_ms, ULONG period_ms) { (void)queue; timer_arm(timer, due_ms, period_ms); return TRUE; }
EXPORT BOOL WINAPI CancelTimerQueueTimer(HANDLE queue, HANDLE timer) { (void)queue; timer_disarm(timer); return TRUE; }
EXPORT BOOL WINAPI DeleteTimerQueueTimer(HANDLE queue, HANDLE timer, HANDLE completion) {
  (void)queue;
  timer_free(timer);
  if (completion && completion != INVALID_HANDLE_VALUE) SetEvent(completion);
  return TRUE;
}
EXPORT BOOL WINAPI DeleteTimerQueueEx(HANDLE queue, HANDLE completion) {
  EnterCriticalSection(&g_timers.lock);
  TpTimer *mine[64];
  int n = 0;
  for (TpTimer *t = g_timers.list; t && n < 64; t = t->next) if (t->kind == TIMER_QUEUE && t->queue == queue) mine[n++] = t;
  LeaveCriticalSection(&g_timers.lock);
  for (int i = 0; i < n; ++i) timer_free(mine[i]);
  HeapFree(heap(), 0, queue);
  if (completion && completion != INVALID_HANDLE_VALUE) SetEvent(completion);
  return TRUE;
}
EXPORT BOOL WINAPI DeleteTimerQueue(HANDLE queue) { return DeleteTimerQueueEx(queue, NULL); }

/* Waits for a handle: a thread per wait. The callback goes to the pool; a one-shot wait is then finished until it is set again. */
typedef struct TpWait { TP_WAIT_CALLBACK_ callback; WAITORTIMERCALLBACK_ simple; PVOID context; HANDLE handle, stop; DWORD timeout; DWORD flags; BOOL once; volatile LONG pending; HANDLE idle; HANDLE thread; volatile LONG closing; } TpWait;
static void run_wait(TpItem *item) {
  TpWait *w = item->object;
  const ULONG result = (ULONG)(ULONG_PTR)item->context;      /* WAIT_OBJECT_0 or WAIT_TIMEOUT */
  TpInstance in;
  memset(&in, 0, sizeof(in));
  if (w->callback) w->callback(&in, w->context, w, result); else w->simple(w->context, result == WAIT_TIMEOUT);
  instance_finish(&in);
  if (InterlockedDecrement(&w->pending) == 0) SetEvent(w->idle);
}
static DWORD WINAPI wait_thread(LPVOID p) {
  TpWait *w = p;
  for (;;) {
    HANDLE both[2] = {w->stop, w->handle};
    const DWORD r = WaitForMultipleObjects(2, both, FALSE, w->timeout);
    if (r == WAIT_OBJECT_0 || w->closing) return 0;      /* told to stop */
    if (InterlockedIncrement(&w->pending) == 1) ResetEvent(w->idle);
    if (!pool_post(run_wait, w, (PVOID)(ULONG_PTR)(r == WAIT_TIMEOUT ? WAIT_TIMEOUT : WAIT_OBJECT_0))) { if (InterlockedDecrement(&w->pending) == 0) SetEvent(w->idle); }
    if (w->once) return 0;      /* (a pool wait is set again with SetThreadpoolWait) */
  }
}
static TpWait *wait_new(void) {
  TpWait *w = HeapAlloc(heap(), HEAP_ZERO_MEMORY, sizeof(TpWait));
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
  w->idle = CreateEventW(NULL, TRUE, TRUE, NULL);
  w->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
  return w;
}
static void wait_stop(TpWait *w) {
  w->closing = 1;
  SetEvent(w->stop);
  if (w->thread) { WaitForSingleObject(w->thread, INFINITE); CloseHandle(w->thread); w->thread = NULL; }
  WaitForSingleObject(w->idle, INFINITE);
}
static void wait_free(TpWait *w) {
  wait_stop(w);
  CloseHandle(w->idle);
  CloseHandle(w->stop);
  HeapFree(heap(), 0, w);
}
EXPORT PVOID WINAPI CreateThreadpoolWait(TP_WAIT_CALLBACK_ callback, PVOID context, PVOID environment) {
  (void)environment;
  TpWait *w = wait_new();
  if (w) { w->callback = callback; w->context = context; }
  return w;
}
EXPORT void WINAPI SetThreadpoolWait(PVOID wait, HANDLE handle, PFILETIME timeout) {
  TpWait *w = wait;
  if (w->thread) { w->closing = 1; SetEvent(w->stop); WaitForSingleObject(w->thread, INFINITE); CloseHandle(w->thread); w->thread = NULL; w->closing = 0; ResetEvent(w->stop); }
  if (!handle) return;      /* the wait is cancelled */
  w->handle = handle;
  w->timeout = timeout ? (DWORD)due_to_delay(timeout) : INFINITE;
  w->flags = 8;
  w->once = TRUE;      /* (a pool wait fires once per SetThreadpoolWait) */
  w->thread = CreateThread(NULL, 0, wait_thread, w, 0, NULL);
}
EXPORT void WINAPI WaitForThreadpoolWaitCallbacks(PVOID wait, BOOL cancel_pending) { TpWait *w = wait; if (cancel_pending && w->thread) SetThreadpoolWait(w, NULL, NULL); WaitForSingleObject(w->idle, INFINITE); }
EXPORT void WINAPI CloseThreadpoolWait(PVOID wait) { wait_free(wait); }

EXPORT BOOL WINAPI RegisterWaitForSingleObject(PHANDLE new_wait, HANDLE object, WAITORTIMERCALLBACK_ callback, PVOID context, ULONG timeout_ms, ULONG flags) {
  TpWait *w = wait_new();
  if (!w) return FALSE;
  w->simple = callback;
  w->context = context;
  w->handle = object;
  w->timeout = timeout_ms;
  w->flags = flags;
  w->once = (flags & 8) != 0;      /* WT_EXECUTEONLYONCE: the wait ends after the first callback; otherwise it keeps waiting */
  w->thread = CreateThread(NULL, 0, wait_thread, w, 0, NULL);
  *new_wait = w;
  return TRUE;
}
EXPORT BOOL WINAPI UnregisterWaitEx(HANDLE wait, HANDLE completion) {
  TpWait *w = wait;
  if (!w) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  wait_free(w);
  if (completion && completion != INVALID_HANDLE_VALUE) SetEvent(completion);
  return TRUE;
}
EXPORT BOOL WINAPI UnregisterWait(HANDLE wait) { return UnregisterWaitEx(wait, NULL); }
