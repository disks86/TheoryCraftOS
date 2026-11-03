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

/* ntdll.dll: the thread pool (Tp*) and the older Rtl interfaces over it: work items, timer queues and registered waits. A pool is a queue of tasks and worker threads that a
 * semaphore wakes; one thread per process looks after the timers and the waits of every object (up to 63 waits at a time). Included by ntdll.c after sync.inc. */

#define TP_WORK_ 1
#define TP_TIMER_ 2
#define TP_WAIT_ 3
#define TP_IO_ 4
#define TP_SIMPLE_ 5
#define TP_OTHER_ 6      /* alpc and job notification objects: made and released, they never call back */
#define TP_STATUS_TIMEOUT_ ((NTSTATUS)0x102)
#define TP_MAX_WAITS 63
#define TP_IDLE_MS 10000

struct TP_POOL_;
struct TP_CLEANUP_;
struct TP_OBJ_;
typedef struct TP_TASK_ {
  struct TP_TASK_ *next;
  struct TP_OBJ_ *object;
  ULONG_PTR a, b, c, d;      /* a wait's result; an I/O completion's overlapped, status and bytes */
} TP_TASK_;

typedef struct TP_POOL_ {
  LONG refs;
  RTL_CRITICAL_SECTION lock;
  TP_TASK_ *head, *tail;
  HANDLE semaphore;
  LONG workers, idle, maximum, minimum;
  SIZE_T stack_reserve, stack_commit;
  struct TP_OBJ_ *last;       /* the object whose callback was taken last: the next one is of another object if there is one (a busy object does not starve the rest) */
  volatile LONG closing;      /* the program has let go of the pool: its threads go when the queue is empty */
  HANDLE port;      /* the I/O completion port of the objects for files */
  LONG port_thread;
} TP_POOL_;

typedef struct TP_OBJ_ {
  ULONG kind;
  volatile LONG refs;           /* the user's and one for each callback queued or running */
  volatile LONG outstanding;    /* callbacks queued or running (what the waiting functions wait for) */
  volatile LONG running;        /* the same, but a callback that disassociated itself still counts (what the cleanup group waits for) */
  volatile LONG released;
  TP_POOL_ *pool;
  struct TP_CLEANUP_ *group;
  struct TP_OBJ_ *group_next, *group_prev;
  PVOID callback, context, cancel_callback, finalization;
  HANDLE completion_event;      /* set when the last callback is over (RtlDeleteTimer, RtlDeregisterWaitEx) */
  struct TP_OBJ_ *next, *prev;  /* the lists of timers and of waits that are set */
  BOOLEAN set;                  /* TpIsTimerSet: stays set after a timer fired once, until it is cancelled */
  BOOLEAN armed;                /* in the manager's list: will fire */
  LONGLONG due;                 /* timer: when it fires next (system time); wait: when it times out (0: never) */
  LONG period;                  /* timer: in milliseconds */
  LONG mgr;                     /* wait: the manager that waits for it */
  HANDLE handle;                /* wait: the object waited for; io: the file */
  PVOID rtl;                    /* the Rtl interface over the object (see below) */
  volatile LONG started;        /* io: operations started and not finished */
} TP_OBJ_;

typedef struct TP_CLEANUP_ {
  TP_OBJ_ *head;
  volatile LONG released;
} TP_CLEANUP_;

typedef struct TP_INSTANCE_ {
  TP_OBJ_ *object;
  HANDLE event;
  HANDLE semaphore;
  LONG semaphore_count;
  HANDLE mutex;
  PRTL_CRITICAL_SECTION section;
  PVOID library;
  BOOLEAN disassociated;
} TP_INSTANCE_;

typedef VOID (NTAPI *TP_SIMPLE_CALLBACK_)(PVOID instance, PVOID context);
typedef VOID (NTAPI *TP_WORK_CALLBACK_)(PVOID instance, PVOID context, PVOID work);
typedef VOID (NTAPI *TP_TIMER_CALLBACK_)(PVOID instance, PVOID context, PVOID timer);
typedef VOID (NTAPI *TP_WAIT_CALLBACK_)(PVOID instance, PVOID context, PVOID wait, ULONG result);
typedef VOID (NTAPI *TP_IO_CALLBACK_)(PVOID instance, PVOID context, PVOID overlapped, PIO_STATUS_BLOCK iosb, PVOID io);
typedef VOID (NTAPI *TP_FINALIZATION_CALLBACK_)(PVOID instance, PVOID context);
typedef VOID (NTAPI *TP_CLEANUP_CANCEL_CALLBACK_)(PVOID object_context, PVOID cleanup_context);

/* The callback environment the program fills in (TP_CALLBACK_ENVIRON_V1 to V3). */
typedef struct TP_ENVIRON_ {
  ULONG version;
  PVOID pool;
  PVOID cleanup_group;
  PVOID cancel_callback;
  PVOID race_dll;
  PVOID activation_context;
  PVOID finalization;
  ULONG flags;
} TP_ENVIRON_;

typedef struct TP_RTL_ { ULONG kind; PVOID function; ULONG flags; TP_OBJ_ *object; } TP_RTL_;
static RTL_CRITICAL_SECTION g_tp_lock;      /* the lists of groups, timers and waits */
static INIT_ONCE g_tp_once;
static TP_POOL_ *g_tp_default_pool;
static TP_OBJ_ *g_tp_timers;
#define TP_MAX_MANAGERS 16      /* a manager thread waits for up to 63 handles: this many of them make 1008 waits */
typedef struct TP_MGR_ { HANDLE event; TP_OBJ_ *waits; LONG count; LONG started; } TP_MGR_;
static TP_MGR_ g_tp_mgr[TP_MAX_MANAGERS];      /* manager 0 also keeps the timers, runs the callbacks for its own thread and looks after starved pools */
static LONG g_tp_default_maximum = 64;

static void tp_free(PVOID p) { RtlFreeHeap(process_heap(), 0, p); }
static PVOID tp_alloc(SIZE_T size) { return RtlAllocateHeap(process_heap(), HEAP_ZERO_MEMORY, size); }

static ULONG NTAPI tp_init_once_fn(PINIT_ONCE once, PVOID parameter, PVOID *context) {
  (void)once; (void)parameter; (void)context;
  RtlInitializeCriticalSection(&g_tp_lock);
  return 1;
}
static void tp_init(void) { RtlRunOnceExecuteOnce(&g_tp_once, tp_init_once_fn, NULL, NULL); }

static NTSTATUS tp_create_thread(PVOID start, PVOID parameter, SIZE_T stack_reserve) {
  HANDLE thread = NULL;
  const NTSTATUS status = NtCreateThreadEx(&thread, 0x1FFFFF, NULL, NtCurrentProcess(), start, parameter, 0, 0, stack_reserve, stack_reserve, NULL);
  if (NT_SUCCESS(status)) NtClose(thread);
  return status;
}

#define TP_MAX_POOLS 64
static TP_POOL_ *g_tp_pools[TP_MAX_POOLS];
static void tp_pool_register(TP_POOL_ *pool, BOOLEAN add) {
  tp_init();
  RtlEnterCriticalSection(&g_tp_lock);
  for (int i = 0; i < TP_MAX_POOLS; ++i) {
    if (add && !g_tp_pools[i]) { g_tp_pools[i] = pool; break; }
    if (!add && g_tp_pools[i] == pool) { g_tp_pools[i] = NULL; break; }
  }
  RtlLeaveCriticalSection(&g_tp_lock);
}
static NTSTATUS tp_make_pool(TP_POOL_ **out) {
  TP_POOL_ *pool = tp_alloc(sizeof(*pool));
  if (!pool) return STATUS_NO_MEMORY;
  pool->refs = 1;
  RtlInitializeCriticalSection(&pool->lock);
  const NTSTATUS status = NtCreateSemaphore(&pool->semaphore, 0x1F0003, NULL, 0, 0x7FFFFFFF);
  if (!NT_SUCCESS(status)) { RtlDeleteCriticalSection(&pool->lock); tp_free(pool); return status; }
  pool->maximum = 500;
  tp_pool_register(pool, TRUE);
  *out = pool;
  return STATUS_SUCCESS;
}
static void tp_pool_unref(TP_POOL_ *pool) {
  if (__sync_sub_and_fetch(&pool->refs, 1) != 0 || pool == g_tp_default_pool) return;
  tp_pool_register(pool, FALSE);
  NtClose(pool->semaphore);
  RtlDeleteCriticalSection(&pool->lock);
  tp_free(pool);
}
static TP_POOL_ *tp_default_pool(void) {
  tp_init();
  RtlEnterCriticalSection(&g_tp_lock);
  if (!g_tp_default_pool) {
    TP_POOL_ *pool = NULL;
    if (NT_SUCCESS(tp_make_pool(&pool))) { pool->maximum = g_tp_default_maximum; g_tp_default_pool = pool; }
  }
  TP_POOL_ *pool = g_tp_default_pool;
  RtlLeaveCriticalSection(&g_tp_lock);
  return pool;
}

/* ---- running callbacks ---- */

static void tp_object_unref(TP_OBJ_ *object);
static void tp_release(TP_OBJ_ *object);
static void tp_instance_finish(TP_INSTANCE_ *i) {
  if (i->section) RtlLeaveCriticalSection(i->section);
  if (i->mutex) NtReleaseMutant(i->mutex, NULL);
  if (i->semaphore) NtReleaseSemaphore(i->semaphore, i->semaphore_count, NULL);
  if (i->event) NtSetEvent(i->event, NULL);
  if (i->library) LdrUnloadDll(i->library);
  memset(i, 0, sizeof(*i));
}
/* The callbacks of an object are over: whoever waits for it is told, and the event somebody wanted set is set. */
static void tp_callback_done(TP_OBJ_ *object) {
  if (__sync_sub_and_fetch(&object->outstanding, 1) == 0) {
    RtlWakeAddressAll((PVOID)&object->outstanding);
    HANDLE event = __sync_lock_test_and_set(&object->completion_event, NULL);
    if (event) NtSetEvent(event, NULL);
  }
}
static void tp_rtl_callback(TP_OBJ_ *object, TP_TASK_ *task);
static void tp_run(TP_TASK_ *task) {
  TP_OBJ_ *object = task->object;
  TP_INSTANCE_ instance;
  memset(&instance, 0, sizeof(instance));
  instance.object = object;
  PVOID cancelled_marker = NULL;
  (void)cancelled_marker;
  if (object->rtl) tp_rtl_callback(object, task);
  else switch (object->kind) {
    case TP_WORK_: ((TP_WORK_CALLBACK_)object->callback)(&instance, object->context, object); break;
    case TP_SIMPLE_: ((TP_SIMPLE_CALLBACK_)object->callback)(&instance, object->context); break;
    case TP_TIMER_: ((TP_TIMER_CALLBACK_)object->callback)(&instance, object->context, object); break;
    case TP_WAIT_: ((TP_WAIT_CALLBACK_)object->callback)(&instance, object->context, object, (ULONG)task->a); break;
    case TP_IO_: {
      IO_STATUS_BLOCK iosb;
      memset(&iosb, 0, sizeof(iosb));
      iosb.Status = (NTSTATUS)task->b;
      iosb.Information = task->c;
      ((TP_IO_CALLBACK_)object->callback)(&instance, object->context, (PVOID)task->a, &iosb, object);
      break;
    }
    default: break;
  }
  if (object->finalization) ((TP_FINALIZATION_CALLBACK_)object->finalization)(&instance, object->context);
  tp_instance_finish(&instance);
  if (!instance.disassociated) tp_callback_done(object);
  tp_free(task);
  if (object->kind == TP_SIMPLE_) tp_release(object);      /* (nobody holds a simple callback's object: it goes with the callback) */
  if (__sync_sub_and_fetch(&object->running, 1) == 0) RtlWakeAddressAll((PVOID)&object->running);
  tp_object_unref(object);
}

static NTSTATUS tp_pool_start_worker(TP_POOL_ *pool);
static DWORD NTAPI tp_worker(PVOID parameter) {
  TP_POOL_ *pool = parameter;
  for (;;) {
    LARGE_INTEGER timeout;
    timeout.QuadPart = -(LONGLONG)TP_IDLE_MS * 10000;
    RtlEnterCriticalSection(&pool->lock);
    ++pool->idle;
    RtlLeaveCriticalSection(&pool->lock);
    const NTSTATUS status = NtWaitForSingleObject(pool->semaphore, FALSE, &timeout);
    RtlEnterCriticalSection(&pool->lock);
    --pool->idle;
    TP_TASK_ *task = pool->head, *before = NULL;
    if (task && task->object == pool->last) {
      for (TP_TASK_ *t = task->next, *p = task; t; p = t, t = t->next) if (t->object != pool->last) { task = t; before = p; break; }
    }
    if (task) {
      if (before) before->next = task->next; else pool->head = task->next;
      if (pool->tail == task) pool->tail = before;
      pool->last = task->object;
    }
    if (!task && ((status == TP_STATUS_TIMEOUT_ && pool->workers > pool->minimum) || pool->closing)) {      /* nothing to do for a while, or nobody to do it for: the thread goes */
      --pool->workers;
      RtlLeaveCriticalSection(&pool->lock);
      tp_pool_unref(pool);
      return 0;
    }
    RtlLeaveCriticalSection(&pool->lock);
    if (task) tp_run(task);
  }
}
static NTSTATUS tp_pool_start_worker(TP_POOL_ *pool) {
  RtlEnterCriticalSection(&pool->lock);
  if (pool->workers >= pool->maximum) { RtlLeaveCriticalSection(&pool->lock); return STATUS_SUCCESS; }
  ++pool->workers;
  __sync_fetch_and_add(&pool->refs, 1);      /* (the thread's own reference) */
  const SIZE_T stack = pool->stack_reserve;
  RtlLeaveCriticalSection(&pool->lock);
  const NTSTATUS status = tp_create_thread((PVOID)tp_worker, pool, stack);
  if (!NT_SUCCESS(status)) { RtlEnterCriticalSection(&pool->lock); --pool->workers; RtlLeaveCriticalSection(&pool->lock); tp_pool_unref(pool); }
  return status;
}

static volatile LONG g_tp_starved;
static void tp_wake_manager(void);
static void tp_wake_mgr(LONG index);
static LONG tp_processors(void) {
  const LONG n = (LONG)*(const DWORD *)((const BYTE *)NtCurrentPeb() + 0xB8);      /* PEB.NumberOfProcessors */
  return n < 2 ? 2 : n;
}
static void tp_object_ref(TP_OBJ_ *object) { __sync_fetch_and_add(&object->refs, 1); }
static void tp_object_unref(TP_OBJ_ *object) {
  if (__sync_sub_and_fetch(&object->refs, 1) != 0) return;
  if (object->pool) tp_pool_unref(object->pool);
  tp_free(object);
}

/* Queues a task of the object: false if it could not be made. The queue holds a reference on the object until the task has run. */
static BOOLEAN tp_post(TP_OBJ_ *object, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c) {
  TP_TASK_ *task = tp_alloc(sizeof(*task));
  if (!task) return FALSE;
  task->object = object;
  task->a = a; task->b = b; task->c = c;
  tp_object_ref(object);
  __sync_fetch_and_add(&object->outstanding, 1);
  __sync_fetch_and_add(&object->running, 1);
  TP_POOL_ *pool = object->pool;
  RtlEnterCriticalSection(&pool->lock);
  if (pool->tail) pool->tail->next = task; else pool->head = task;
  pool->tail = task;
  const LONG soft = pool->maximum < tp_processors() ? pool->maximum : tp_processors();      /* (more threads than processors are made only when the others stay busy: see the manager) */
  const BOOLEAN need_worker = pool->idle == 0 && pool->workers < soft;
  const BOOLEAN starved = pool->idle == 0 && pool->workers >= soft && pool->workers < pool->maximum;
  RtlLeaveCriticalSection(&pool->lock);
  if (need_worker) tp_pool_start_worker(pool);
  else if (starved) { g_tp_starved = 1; tp_wake_manager(); }
  NtReleaseSemaphore(pool->semaphore, 1, NULL);
  return TRUE;
}

/* Takes the queued callbacks of an object out of the pool's queue; how many there were. */
static LONG tp_cancel_queued(TP_OBJ_ *object) {
  TP_POOL_ *pool = object->pool;
  LONG removed = 0;
  RtlEnterCriticalSection(&pool->lock);
  TP_TASK_ *previous = NULL, *t = pool->head;
  while (t) {
    TP_TASK_ *next = t->next;
    if (t->object == object) {
      if (previous) previous->next = next; else pool->head = next;
      if (pool->tail == t) pool->tail = previous;
      tp_free(t);
      ++removed;
    } else previous = t;
    t = next;
  }
  RtlLeaveCriticalSection(&pool->lock);
  for (LONG i = 0; i < removed; ++i) {
    tp_callback_done(object);
    if (__sync_sub_and_fetch(&object->running, 1) == 0) RtlWakeAddressAll((PVOID)&object->running);
    tp_object_unref(object);
  }
  return removed;
}
static void tp_wait_idle(TP_OBJ_ *object) {
  for (;;) {
    LONG seen = object->outstanding;
    if (seen <= 0) return;
    RtlWaitOnAddress(&object->outstanding, &seen, sizeof(seen), NULL);
  }
}
static void tp_wait_finished(TP_OBJ_ *object) {      /* (also for the callbacks that let go of the object) */
  for (;;) {
    LONG seen = object->running;
    if (seen <= 0) return;
    RtlWaitOnAddress(&object->running, &seen, sizeof(seen), NULL);
  }
}

/* ---- the timers and waits ---- */

static void tp_list_add(TP_OBJ_ **head, TP_OBJ_ *o) { o->prev = NULL; o->next = *head; if (*head) (*head)->prev = o; *head = o; }
static void tp_list_remove(TP_OBJ_ **head, TP_OBJ_ *o) {
  if (o->prev) o->prev->next = o->next; else if (*head == o) *head = o->next;
  if (o->next) o->next->prev = o->prev;
  o->next = o->prev = NULL;
}

/* A wait or timer fires: its callback goes to the pool, or to the manager thread itself if the Rtl interface said so. The lock is held. */
static TP_OBJ_ *g_tp_inline[8];
static ULONG_PTR g_tp_inline_result[8];
static ULONG g_tp_inline_count;
static void tp_fire(TP_OBJ_ *object, ULONG_PTR result) {
  TP_RTL_ *r = object->rtl;
  if (r && (r->flags & (0x1u | 0x20u)) && g_tp_inline_count < 8) {
    tp_object_ref(object);
    __sync_fetch_and_add(&object->outstanding, 1);
    __sync_fetch_and_add(&object->running, 1);
    g_tp_inline[g_tp_inline_count] = object;
    g_tp_inline_result[g_tp_inline_count++] = result;
    return;
  }
  tp_post(object, result, 0, 0);
}
static DWORD NTAPI tp_manager(PVOID argument) {
  const LONG me = (LONG)(ULONG_PTR)argument;
  TP_MGR_ *mgr = &g_tp_mgr[me];
  for (;;) {
    HANDLE handles[TP_MAX_WAITS + 1];
    TP_OBJ_ *owners[TP_MAX_WAITS + 1];
    LARGE_INTEGER now, timeout, *timeout_pointer = NULL;
    ULONG count = 0;
    handles[count] = mgr->event;
    owners[count++] = NULL;
    RtlEnterCriticalSection(&g_tp_lock);
    NtQuerySystemTime(&now);
    LONGLONG next = 0;
    if (me == 0) for (TP_OBJ_ *t = g_tp_timers; t; t = t->next) if (t->armed && (next == 0 || t->due < next)) next = t->due;
    for (TP_OBJ_ *w = mgr->waits; w; w = w->next) {
      if (!w->armed || count > TP_MAX_WAITS) continue;
      if (w->handle == (HANDLE)(LONG_PTR)-1) {      /* (INVALID_HANDLE_VALUE: nothing to wait for but the time) */
        if (w->due != 0 && (next == 0 || w->due < next)) next = w->due;
        continue;
      }
      ULONG same = 1;      /* (a handle that several waits are for is waited for once: the kernel refuses the same object twice) */
      while (same < count && handles[same] != w->handle) ++same;
      if (same < count) {
        if (w->due != 0 && (next == 0 || w->due < next)) next = w->due;
        continue;
      }
      handles[count] = w->handle;
      tp_object_ref(w);      /* (the wait may be released while this thread waits for it) */
      owners[count++] = w;
      if (w->due != 0 && (next == 0 || w->due < next)) next = w->due;
    }
    RtlLeaveCriticalSection(&g_tp_lock);
    if (next) { timeout.QuadPart = next > now.QuadPart ? -(next - now.QuadPart) : 0; timeout_pointer = &timeout; }
    if (me == 0 && g_tp_starved && (!timeout_pointer || timeout.QuadPart < -1000000)) { timeout.QuadPart = -1000000; timeout_pointer = &timeout; }      /* 100 ms: look again whether a queue is stuck */
    const NTSTATUS status = NtWaitForMultipleObjects(count, handles, 1 /* WaitAny */, TRUE, timeout_pointer);      /* (alertable: a program can queue an APC to the thread that runs its I/O callbacks) */
    RtlEnterCriticalSection(&g_tp_lock);
    NtQuerySystemTime(&now);
    if (status > 0 && (ULONG)status < count && owners[status]) {      /* a handle was signalled: the waits for it are over and call back once each (while it stays signalled) */
      HANDLE signalled = handles[status];
      BOOLEAN first = TRUE;
      for (TP_OBJ_ *w = mgr->waits, *next_w; w; w = next_w) {
        next_w = w->next;
        if (!w->armed || w->handle != signalled) continue;
        if (!first) { LARGE_INTEGER zero = {0}; if (NtWaitForSingleObject(signalled, FALSE, &zero) != STATUS_SUCCESS) continue; }
        first = FALSE;
        w->armed = FALSE; tp_list_remove(&mgr->waits, w); --mgr->count; tp_fire(w, 0 /* WAIT_OBJECT_0 */);
      }
    } else if (status < 0 && status != TP_STATUS_TIMEOUT_) {      /* a handle that is not one (closed early): the waits for it are given up, not waited for in a loop */
      for (TP_OBJ_ *w = mgr->waits, *next_w; w; w = next_w) {
        next_w = w->next;
        LARGE_INTEGER zero = {0};
        if (!w->armed || NtWaitForSingleObject(w->handle, FALSE, &zero) != STATUS_INVALID_HANDLE) continue;
        w->armed = FALSE; tp_list_remove(&mgr->waits, w); --mgr->count;
      }
    }
    for (ULONG i = 1; i < count; ++i) tp_object_unref(owners[i]);
    for (TP_OBJ_ *w = mgr->waits, *next_w; w; w = next_w) {
      next_w = w->next;
      if (w->armed && w->due != 0 && w->due <= now.QuadPart) { w->armed = FALSE; tp_list_remove(&mgr->waits, w); --mgr->count; tp_fire(w, 0x102 /* WAIT_TIMEOUT */); }
    }
    for (TP_OBJ_ *t = me == 0 ? g_tp_timers : NULL, *next_t; t; t = next_t) {
      next_t = t->next;
      if (!t->armed || t->due > now.QuadPart) continue;
      tp_fire(t, 0);
      if (t->period > 0) {
        const LONGLONG step = (LONGLONG)t->period * 10000;
        t->due += step;
        if (t->due <= now.QuadPart) t->due = now.QuadPart + step;      /* (late by more than a period: the missed ones are not made up) */
      } else { t->armed = FALSE; tp_list_remove(&g_tp_timers, t); }
    }
    if (me == 0 && g_tp_starved) {      /* queues that wait with all their threads busy get another thread (a callback that blocks on another must not stop the pool) */
      BOOLEAN more = FALSE;
      for (int i = 0; i < TP_MAX_POOLS; ++i) {
        TP_POOL_ *pool = g_tp_pools[i];
        if (!pool) continue;
        RtlEnterCriticalSection(&pool->lock);
        const BOOLEAN stuck = pool->head && pool->idle == 0 && pool->workers < pool->maximum;
        RtlLeaveCriticalSection(&pool->lock);
        if (stuck) { tp_pool_start_worker(pool); more = TRUE; }
      }
      if (!more) g_tp_starved = 0;
    }
    TP_OBJ_ *inline_objects[8];
    ULONG_PTR inline_results[8];
    const ULONG inline_count = g_tp_inline_count;
    for (ULONG i = 0; i < inline_count; ++i) { inline_objects[i] = g_tp_inline[i]; inline_results[i] = g_tp_inline_result[i]; }
    g_tp_inline_count = 0;
    RtlLeaveCriticalSection(&g_tp_lock);
    for (ULONG i = 0; i < inline_count; ++i) {      /* the callbacks that are for this thread (WT_EXECUTEINIOTHREAD, WT_EXECUTEINTIMERTHREAD) */
      TP_TASK_ *task = tp_alloc(sizeof(*task));
      if (!task) continue;
      task->object = inline_objects[i];
      task->a = inline_results[i];
      tp_run(task);
    }
  }
  return 0;
}
static NTSTATUS tp_start_manager(LONG index) {
  tp_init();
  if (g_tp_mgr[index].started == 2) return STATUS_SUCCESS;
  RtlEnterCriticalSection(&g_tp_lock);
  NTSTATUS status = STATUS_SUCCESS;
  if (!g_tp_mgr[index].started) {
    g_tp_mgr[index].started = 1;
    status = NtCreateEvent(&g_tp_mgr[index].event, 0x1F0003, NULL, 1 /* synchronization */, FALSE);
    if (NT_SUCCESS(status)) status = tp_create_thread((PVOID)tp_manager, (PVOID)(ULONG_PTR)index, 0);
    if (NT_SUCCESS(status)) g_tp_mgr[index].started = 2; else g_tp_mgr[index].started = 0;
  }
  RtlLeaveCriticalSection(&g_tp_lock);
  return status;
}
static void tp_wake_mgr(LONG index) { if (g_tp_mgr[index].event) NtSetEvent(g_tp_mgr[index].event, NULL); }
static void tp_wake_manager(void) { tp_wake_mgr(0); }

/* ---- making the objects ---- */

static NTSTATUS tp_new_object(ULONG kind, PVOID callback, PVOID context, const TP_ENVIRON_ *environment, TP_OBJ_ **out) {
  if (environment && (environment->version == 0 || environment->version > 3)) return STATUS_INVALID_PARAMETER;
  TP_POOL_ *pool = environment && environment->pool ? environment->pool : tp_default_pool();
  if (!pool) return STATUS_NO_MEMORY;
  TP_OBJ_ *object = tp_alloc(sizeof(*object));
  if (!object) return STATUS_NO_MEMORY;
  object->kind = kind;
  object->refs = 1;
  object->pool = pool;
  __sync_fetch_and_add(&pool->refs, 1);
  object->callback = callback;
  object->context = context;
  if (environment) {
    object->finalization = environment->finalization;
    TP_CLEANUP_ *group = environment->cleanup_group;
    if (group) {
      object->group = group;
      object->cancel_callback = environment->cancel_callback;
      tp_init();
      RtlEnterCriticalSection(&g_tp_lock);
      object->group_next = group->head;
      if (group->head) group->head->group_prev = object;
      group->head = object;
      RtlLeaveCriticalSection(&g_tp_lock);
    }
  }
  *out = object;
  return STATUS_SUCCESS;
}

/* The object is no more the program's: its timer or wait is cancelled, it leaves its cleanup group and goes when the last callback is over. */
static void tp_unset(TP_OBJ_ *object) {
  if (object->kind != TP_TIMER_ && object->kind != TP_WAIT_) return;
  tp_init();
  RtlEnterCriticalSection(&g_tp_lock);
  object->set = FALSE;
  if (object->armed) {
    object->armed = FALSE;
    if (object->kind == TP_TIMER_) tp_list_remove(&g_tp_timers, object);
    else { tp_list_remove(&g_tp_mgr[object->mgr].waits, object); --g_tp_mgr[object->mgr].count; }
  }
  RtlLeaveCriticalSection(&g_tp_lock);
  tp_wake_mgr(object->kind == TP_TIMER_ ? 0 : object->mgr);
}
static void tp_group_unlink(TP_OBJ_ *object) {
  if (!object->group) return;
  RtlEnterCriticalSection(&g_tp_lock);
  if (object->group) {
    if (object->group_prev) object->group_prev->group_next = object->group_next; else object->group->head = object->group_next;
    if (object->group_next) object->group_next->group_prev = object->group_prev;
    object->group = NULL;
    object->group_next = object->group_prev = NULL;
  }
  RtlLeaveCriticalSection(&g_tp_lock);
}
static void tp_release(TP_OBJ_ *object) {
  if (!object || __sync_lock_test_and_set(&object->released, 1)) return;
  tp_unset(object);
  tp_group_unlink(object);
  tp_object_unref(object);
}
/* Waits for the callbacks of the object: the ones that have not begun are dropped first when asked to. */
static void tp_wait_callbacks(TP_OBJ_ *object, BOOLEAN cancel_pending) {
  if (cancel_pending) tp_cancel_queued(object);
  tp_wait_idle(object);
}

EXPORT NTSTATUS NTAPI TpAllocPool(PVOID *out, PVOID reserved) {
  (void)reserved;
  TP_POOL_ *pool = NULL;
  const NTSTATUS status = tp_make_pool(&pool);
  if (NT_SUCCESS(status)) *out = pool;
  return status;
}
EXPORT void NTAPI TpReleasePool(PVOID pool_) {
  TP_POOL_ *pool = pool_;
  RtlEnterCriticalSection(&pool->lock);
  pool->closing = 1;
  const LONG workers = pool->workers;
  RtlLeaveCriticalSection(&pool->lock);
  if (workers > 0) NtReleaseSemaphore(pool->semaphore, workers, NULL);      /* (each idle thread wakes, finds the queue empty and goes) */
  tp_pool_unref(pool);
}
EXPORT void NTAPI TpSetPoolMaxThreads(PVOID pool_, ULONG maximum) {
  TP_POOL_ *pool = pool_;
  RtlEnterCriticalSection(&pool->lock);
  pool->maximum = maximum ? (LONG)maximum : 1;
  if (pool->minimum > pool->maximum) pool->minimum = pool->maximum;
  RtlLeaveCriticalSection(&pool->lock);
}
EXPORT NTSTATUS NTAPI TpSetPoolMinThreads(PVOID pool_, ULONG minimum) {
  TP_POOL_ *pool = pool_;
  if (minimum > 0x7FFFFFFFu) return STATUS_INVALID_PARAMETER;
  RtlEnterCriticalSection(&pool->lock);
  if ((LONG)minimum > pool->maximum) { RtlLeaveCriticalSection(&pool->lock); return STATUS_INVALID_PARAMETER; }
  pool->minimum = (LONG)minimum;
  RtlLeaveCriticalSection(&pool->lock);
  for (;;) {
    RtlEnterCriticalSection(&pool->lock);
    const BOOLEAN more = pool->workers < pool->minimum;
    RtlLeaveCriticalSection(&pool->lock);
    if (!more) return STATUS_SUCCESS;
    const NTSTATUS status = tp_pool_start_worker(pool);
    if (!NT_SUCCESS(status)) return status;
  }
}
EXPORT void NTAPI TpSetPoolMaxThreadsSoftLimit(PVOID pool, ULONG maximum) { TpSetPoolMaxThreads(pool, maximum); }
/* The stack of a pool's threads is what the program's image asks for, unless the program set it. */
static void tp_image_stack(SIZE_T *reserve, SIZE_T *commit) {
  const BYTE *image = *(BYTE **)((BYTE *)NtCurrentPeb() + 0x10);
  const BYTE *nt = image + *(const DWORD *)(image + 0x3C);
  *reserve = *(const ULONGLONG *)(nt + 0x18 + 0x48);
  *commit = *(const ULONGLONG *)(nt + 0x18 + 0x50);
}
EXPORT NTSTATUS NTAPI TpQueryPoolStackInformation(PVOID pool_, SIZE_T *information) {
  TP_POOL_ *pool = pool_;
  SIZE_T reserve, commit;
  tp_image_stack(&reserve, &commit);
  information[0] = pool->stack_reserve ? pool->stack_reserve : reserve;
  information[1] = pool->stack_commit ? pool->stack_commit : commit;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI TpSetPoolStackInformation(PVOID pool_, SIZE_T *information) {
  TP_POOL_ *pool = pool_;
  pool->stack_reserve = information[0];
  pool->stack_commit = information[1];
  return STATUS_SUCCESS;
}
EXPORT void NTAPI TpSetDefaultPoolMaxThreads(ULONG maximum) {
  g_tp_default_maximum = maximum ? (LONG)maximum : 1;
  TP_POOL_ *pool = tp_default_pool();
  if (pool) TpSetPoolMaxThreads(pool, maximum);
}
EXPORT NTSTATUS NTAPI TpSetDefaultPoolStackInformation(SIZE_T *information) {
  TP_POOL_ *pool = tp_default_pool();
  return pool ? TpSetPoolStackInformation(pool, information) : STATUS_NO_MEMORY;
}
EXPORT NTSTATUS NTAPI TpSetPoolThreadBasePriority(PVOID pool, ULONG priority) { (void)pool; (void)priority; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI TpSetPoolThreadCpuSets(PVOID pool, PVOID sets, ULONG count) { (void)pool; (void)sets; (void)count; return STATUS_SUCCESS; }
EXPORT void NTAPI TpSetPoolWorkerThreadIdleTimeout(PVOID pool, LONGLONG timeout) { (void)pool; (void)timeout; }
EXPORT void NTAPI TpTrimPools(void) {}
EXPORT void NTAPI TpPoolFreeUnusedNodes(void) {}
EXPORT NTSTATUS NTAPI TpCheckTerminateWorker(HANDLE thread) { (void)thread; return STATUS_SUCCESS; }
EXPORT void NTAPI TpDisablePoolCallbackChecks(PVOID pool) { (void)pool; }
EXPORT void NTAPI TpDbgDumpHeapUsage(PVOID a, PVOID b, PVOID c) { (void)a; (void)b; (void)c; }
EXPORT void NTAPI TpDbgSetLogRoutine(PVOID routine) { (void)routine; }
EXPORT void NTAPI TpDbgGetFreeInfo(PVOID a) { (void)a; }
EXPORT void NTAPI TpCallbackDetectedUnrecoverableError(PVOID instance) { (void)instance; RtlExitUserProcess(0xC0000409); }
EXPORT PVOID NTAPI TpCaptureCaller(ULONG type) { (void)type; return NULL; }

EXPORT NTSTATUS NTAPI TpAllocCleanupGroup(PVOID *out) {
  TP_CLEANUP_ *group = tp_alloc(sizeof(*group));
  if (!group) return STATUS_NO_MEMORY;
  tp_init();
  *out = group;
  return STATUS_SUCCESS;
}
/* Waits for the members of the group, the ones whose callbacks have not begun are dropped when asked to; each member that its owner has not released goes to the cancel callback. */
EXPORT void NTAPI TpReleaseCleanupGroupMembers(PVOID group_, BOOLEAN cancel_pending, PVOID cleanup_context) {
  TP_CLEANUP_ *group = group_;
  for (;;) {
    RtlEnterCriticalSection(&g_tp_lock);
    TP_OBJ_ *object = group->head;
    if (object) {
      group->head = object->group_next;
      if (object->group_next) object->group_next->group_prev = NULL;
      object->group = NULL;
      object->group_next = object->group_prev = NULL;
      tp_object_ref(object);
    }
    RtlLeaveCriticalSection(&g_tp_lock);
    if (!object) return;
    tp_unset(object);
    if (cancel_pending) tp_cancel_queued(object);
    tp_wait_finished(object);
    if (object->cancel_callback && !object->released) ((TP_CLEANUP_CANCEL_CALLBACK_)object->cancel_callback)(object->context, cleanup_context);
    tp_release(object);
    tp_object_unref(object);
  }
}
EXPORT void NTAPI TpReleaseCleanupGroup(PVOID group_) {
  TP_CLEANUP_ *group = group_;
  RtlEnterCriticalSection(&g_tp_lock);
  for (TP_OBJ_ *o = group->head, *next; o; o = next) { next = o->group_next; o->group = NULL; o->group_next = o->group_prev = NULL; }
  group->head = NULL;
  RtlLeaveCriticalSection(&g_tp_lock);
  tp_free(group);
}

EXPORT NTSTATUS NTAPI TpAllocWork(PVOID *out, PVOID callback, PVOID context, PVOID environment) {
  TP_OBJ_ *object = NULL;
  const NTSTATUS status = tp_new_object(TP_WORK_, callback, context, environment, &object);
  if (NT_SUCCESS(status)) *out = object;
  return status;
}
EXPORT void NTAPI TpPostWork(PVOID work) { tp_post(work, 0, 0, 0); }
EXPORT void NTAPI TpWaitForWork(PVOID work, BOOLEAN cancel_pending) { tp_wait_callbacks(work, cancel_pending); }
EXPORT void NTAPI TpReleaseWork(PVOID work) { tp_release(work); }

EXPORT NTSTATUS NTAPI TpSimpleTryPost(PVOID callback, PVOID context, PVOID environment) {
  TP_OBJ_ *object = NULL;
  NTSTATUS status = tp_new_object(TP_SIMPLE_, callback, context, environment, &object);
  if (!NT_SUCCESS(status)) return status;
  if (!tp_post(object, 0, 0, 0)) { status = STATUS_NO_MEMORY; tp_release(object); }
  return status;
}

EXPORT NTSTATUS NTAPI TpAllocTimer(PVOID *out, PVOID callback, PVOID context, PVOID environment) {
  TP_OBJ_ *object = NULL;
  const NTSTATUS status = tp_new_object(TP_TIMER_, callback, context, environment, &object);
  if (NT_SUCCESS(status)) *out = object;
  return status;
}
/* due: negative = from now, positive = a system time, null = cancel the timer. period in milliseconds (0: once). */
EXPORT void NTAPI TpSetTimer(PVOID timer, PLARGE_INTEGER due, LONG period, LONG window) {
  TP_OBJ_ *object = timer;
  (void)window;
  if (!due) { tp_unset(object); return; }
  if (tp_start_manager(0) != STATUS_SUCCESS) return;
  LARGE_INTEGER now;
  NtQuerySystemTime(&now);
  RtlEnterCriticalSection(&g_tp_lock);
  object->due = due->QuadPart < 0 ? now.QuadPart - due->QuadPart : (due->QuadPart == 0 ? now.QuadPart : due->QuadPart);
  object->period = period;
  object->set = TRUE;
  if (!object->armed) { object->armed = TRUE; tp_list_add(&g_tp_timers, object); }
  RtlLeaveCriticalSection(&g_tp_lock);
  tp_wake_manager();
}
EXPORT BOOLEAN NTAPI TpSetTimerEx(PVOID timer, PLARGE_INTEGER due, LONG period, LONG window) {      /* TRUE if the timer was set before */
  const BOOLEAN was = ((TP_OBJ_ *)timer)->set;
  TpSetTimer(timer, due, period, window);
  return was;
}
EXPORT BOOLEAN NTAPI TpIsTimerSet(PVOID timer) { return ((TP_OBJ_ *)timer)->set; }
EXPORT ULONG NTAPI TpTimerOutstandingCallbackCount(PVOID timer) { return (ULONG)((TP_OBJ_ *)timer)->outstanding; }
EXPORT void NTAPI TpWaitForTimer(PVOID timer, BOOLEAN cancel_pending) { tp_wait_callbacks(timer, cancel_pending); }
EXPORT void NTAPI TpReleaseTimer(PVOID timer) { tp_release(timer); }

EXPORT NTSTATUS NTAPI TpAllocWait(PVOID *out, PVOID callback, PVOID context, PVOID environment) {
  TP_OBJ_ *object = NULL;
  const NTSTATUS status = tp_new_object(TP_WAIT_, callback, context, environment, &object);
  if (NT_SUCCESS(status)) *out = object;
  return status;
}
/* Waits for the handle once (or for the timeout: negative = from now, positive = system time, null = no end); null handle: cancels the wait. */
static NTSTATUS tp_set_wait(TP_OBJ_ *object, HANDLE handle, PLARGE_INTEGER timeout) {
  if (!handle) { tp_unset(object); return STATUS_SUCCESS; }
  tp_init();
  LARGE_INTEGER now;
  NtQuerySystemTime(&now);
  RtlEnterCriticalSection(&g_tp_lock);
  LONG index = object->armed ? object->mgr : -1;
  if (index < 0) for (LONG i = 0; i < TP_MAX_MANAGERS; ++i) if (g_tp_mgr[i].count < TP_MAX_WAITS) { index = i; break; }
  RtlLeaveCriticalSection(&g_tp_lock);
  if (index < 0) return (NTSTATUS)0xC000009A;      /* STATUS_INSUFFICIENT_RESOURCES: 1008 waits are set already */
  NTSTATUS status = tp_start_manager(index);
  if (!NT_SUCCESS(status)) return status;
  if (index != 0) status = tp_start_manager(0);      /* (manager 0 looks after the timers: it is there for any callback environment) */
  if (!NT_SUCCESS(status)) return status;
  RtlEnterCriticalSection(&g_tp_lock);
  if (!object->armed && g_tp_mgr[index].count >= TP_MAX_WAITS) { RtlLeaveCriticalSection(&g_tp_lock); return (NTSTATUS)0xC000009A; }
  object->handle = handle;
  object->due = !timeout ? 0 : (timeout->QuadPart < 0 ? now.QuadPart - timeout->QuadPart : (timeout->QuadPart == 0 ? now.QuadPart : timeout->QuadPart));
  object->set = TRUE;
  if (!object->armed) { object->armed = TRUE; object->mgr = index; tp_list_add(&g_tp_mgr[index].waits, object); ++g_tp_mgr[index].count; }
  RtlLeaveCriticalSection(&g_tp_lock);
  tp_wake_mgr(index);
  return STATUS_SUCCESS;
}
EXPORT void NTAPI TpSetWait(PVOID wait, HANDLE handle, PLARGE_INTEGER timeout) { tp_set_wait(wait, handle, timeout); }
EXPORT NTSTATUS NTAPI TpSetWaitEx(PVOID wait, HANDLE handle, PLARGE_INTEGER timeout, PVOID reserved) { (void)reserved; return tp_set_wait(wait, handle, timeout); }
EXPORT void NTAPI TpWaitForWait(PVOID wait, BOOLEAN cancel_pending) { tp_wait_callbacks(wait, cancel_pending); }
EXPORT void NTAPI TpReleaseWait(PVOID wait) { tp_release(wait); }

/* I/O completion: the file is tied to the port of the pool, a thread takes the packets off it and calls back; the key is the object. */
static DWORD NTAPI tp_port_thread(PVOID parameter) {
  TP_POOL_ *pool = parameter;
  for (;;) {
    ULONG_PTR key = 0, context = 0;
    IO_STATUS_BLOCK iosb;
    memset(&iosb, 0, sizeof(iosb));
    const NTSTATUS status = NtRemoveIoCompletion(pool->port, &key, &context, &iosb, NULL);
    if (!NT_SUCCESS(status) || !key) continue;
    TP_OBJ_ *object = (TP_OBJ_ *)key;
    if (object->started <= 0) continue;      /* (cancelled, or never announced with TpStartAsyncIoOperation) */
    __sync_fetch_and_sub(&object->started, 1);
    tp_post(object, context, (ULONG_PTR)iosb.Status, iosb.Information);
  }
  return 0;
}
EXPORT NTSTATUS NTAPI TpAllocIoCompletion(PVOID *out, HANDLE file, PVOID callback, PVOID context, PVOID environment) {
  TP_OBJ_ *object = NULL;
  NTSTATUS status = tp_new_object(TP_IO_, callback, context, environment, &object);
  if (!NT_SUCCESS(status)) return status;
  TP_POOL_ *pool = object->pool;
  RtlEnterCriticalSection(&pool->lock);
  if (!pool->port) status = NtCreateIoCompletion(&pool->port, 0x1F0003, NULL, 0);
  const BOOLEAN start_thread = NT_SUCCESS(status) && !pool->port_thread;
  if (start_thread) pool->port_thread = 1;
  RtlLeaveCriticalSection(&pool->lock);
  if (NT_SUCCESS(status) && start_thread) status = tp_create_thread((PVOID)tp_port_thread, pool, 0);
  if (NT_SUCCESS(status)) {
    struct { HANDLE port; PVOID key; } info = {pool->port, object};
    IO_STATUS_BLOCK iosb;
    status = NtSetInformationFile(file, &iosb, &info, sizeof(info), 30 /* FileCompletionInformation */);
  }
  if (!NT_SUCCESS(status)) { tp_release(object); return status; }
  object->handle = file;
  *out = object;
  return STATUS_SUCCESS;
}
/* The program is about to start an operation on the file: its completion is a callback. */
EXPORT void NTAPI TpStartAsyncIoOperation(PVOID io) { __sync_fetch_and_add(&((TP_OBJ_ *)io)->started, 1); }
EXPORT void NTAPI TpCancelAsyncIoOperation(PVOID io) { __sync_fetch_and_sub(&((TP_OBJ_ *)io)->started, 1); }
EXPORT void NTAPI TpWaitForIoCompletion(PVOID io, BOOLEAN cancel_pending) {
  TP_OBJ_ *object = io;
  if (cancel_pending) __sync_lock_test_and_set(&object->started, 0);      /* (the operations that have not completed will not call back; those under way are not cancelled) */
  while (object->started > 0) {      /* operations that were started and have not reached the port yet */
    LARGE_INTEGER step;
    step.QuadPart = -10000;
    NtDelayExecution(FALSE, &step);
  }
  tp_wait_callbacks(object, cancel_pending);
}
EXPORT void NTAPI TpReleaseIoCompletion(PVOID io) { tp_release(io); }

/* Objects for the notifications of ALPC ports and jobs: they are made and released, no notification ever reaches them. */
EXPORT NTSTATUS NTAPI TpAllocAlpcCompletion(PVOID *out, HANDLE port, PVOID callback, PVOID context, PVOID environment) {
  (void)port;
  TP_OBJ_ *object = NULL;
  const NTSTATUS status = tp_new_object(TP_OTHER_, callback, context, environment, &object);
  if (NT_SUCCESS(status)) *out = object;
  return status;
}
EXPORT NTSTATUS NTAPI TpAllocAlpcCompletionEx(PVOID *out, HANDLE port, PVOID callback, PVOID context, PVOID environment) { return TpAllocAlpcCompletion(out, port, callback, context, environment); }
EXPORT void NTAPI TpReleaseAlpcCompletion(PVOID object) { tp_release(object); }
EXPORT void NTAPI TpWaitForAlpcCompletion(PVOID object) { tp_wait_idle(object); }
EXPORT NTSTATUS NTAPI TpAlpcRegisterCompletionList(PVOID list) { (void)list; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI TpAlpcUnregisterCompletionList(PVOID list) { (void)list; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI TpAllocJobNotification(PVOID *out, HANDLE job, PVOID callback, PVOID context, PVOID environment) {
  (void)job;
  TP_OBJ_ *object = NULL;
  const NTSTATUS status = tp_new_object(TP_OTHER_, callback, context, environment, &object);
  if (NT_SUCCESS(status)) *out = object;
  return status;
}
EXPORT void NTAPI TpReleaseJobNotification(PVOID object) { tp_release(object); }
EXPORT void NTAPI TpWaitForJobNotification(PVOID object, BOOLEAN cancel_pending) { tp_wait_callbacks(object, cancel_pending); }

/* ---- what a callback can ask for ---- */

EXPORT void NTAPI TpCallbackSetEventOnCompletion(PVOID instance, HANDLE event) { ((TP_INSTANCE_ *)instance)->event = event; }
EXPORT void NTAPI TpCallbackReleaseSemaphoreOnCompletion(PVOID instance, HANDLE semaphore, ULONG count) { ((TP_INSTANCE_ *)instance)->semaphore = semaphore; ((TP_INSTANCE_ *)instance)->semaphore_count = (LONG)count; }
EXPORT void NTAPI TpCallbackReleaseMutexOnCompletion(PVOID instance, HANDLE mutex) { ((TP_INSTANCE_ *)instance)->mutex = mutex; }
EXPORT void NTAPI TpCallbackLeaveCriticalSectionOnCompletion(PVOID instance, PRTL_CRITICAL_SECTION section) { ((TP_INSTANCE_ *)instance)->section = section; }
EXPORT void NTAPI TpCallbackUnloadDllOnCompletion(PVOID instance, PVOID library) { ((TP_INSTANCE_ *)instance)->library = library; }
EXPORT NTSTATUS NTAPI TpCallbackMayRunLong(PVOID instance) {
  TP_OBJ_ *object = ((TP_INSTANCE_ *)instance)->object;
  if (!object || !object->pool) return STATUS_SUCCESS;
  TP_POOL_ *pool = object->pool;
  RtlEnterCriticalSection(&pool->lock);
  const BOOLEAN room = pool->idle > 0 || pool->workers < pool->maximum;
  const BOOLEAN more = pool->idle == 0 && pool->workers < pool->maximum;
  RtlLeaveCriticalSection(&pool->lock);
  if (more) tp_pool_start_worker(pool);      /* (the thread that runs the callback is lost to the others for a while: another takes its place) */
  return room ? STATUS_SUCCESS : (NTSTATUS)0xC0000129 /* STATUS_TOO_MANY_THREADS */;
}
EXPORT void NTAPI TpCallbackIndependent(PVOID instance) { (void)instance; }
EXPORT void NTAPI TpCallbackSendAlpcMessageOnCompletion(PVOID instance, PVOID a, PVOID b, PVOID c) { (void)instance; (void)a; (void)b; (void)c; }
EXPORT NTSTATUS NTAPI TpCallbackSendPendingAlpcMessage(PVOID instance) { (void)instance; return STATUS_NOT_IMPLEMENTED; }
/* The callback is no longer the object's: whoever waits for the object does not wait for it, and what it asked for on completion is done now. */
EXPORT void NTAPI TpDisassociateCallback(PVOID instance_) {
  TP_INSTANCE_ *instance = instance_;
  TP_OBJ_ *object = instance->object;
  if (!object || instance->disassociated) return;
  instance->disassociated = TRUE;
  tp_callback_done(object);
}

/* ---- the Rtl interfaces over the pool: work items, timer queues and registered waits ---- */

typedef VOID (NTAPI *WAITORTIMERCALLBACK_)(PVOID context, BOOLEAN timer_or_wait_fired);
typedef ULONG (NTAPI *WORKER_FUNCTION_)(PVOID context);
#define WT_EXECUTEONLYONCE_ 0x8u
#define WT_EXECUTEINTIMERTHREAD_ 0x20u

/* An object that came from RtlQueueWorkItem, RtlCreateTimer or RtlRegisterWait: its callback is one of these, run by tp_run(). */
#define TP_RTL_WORK_ 1
#define TP_RTL_TIMER_ 2
#define TP_RTL_WAIT_ 3
static void tp_rtl_callback(TP_OBJ_ *object, TP_TASK_ *task) {
  TP_RTL_ *r = object->rtl;
  switch (r->kind) {
    case TP_RTL_WORK_: ((WORKER_FUNCTION_)r->function)(object->context); break;
    case TP_RTL_TIMER_: ((WAITORTIMERCALLBACK_)r->function)(object->context, TRUE); break;
    case TP_RTL_WAIT_: {
      ((WAITORTIMERCALLBACK_)r->function)(object->context, task->a == 0x102);
      if (!(r->flags & WT_EXECUTEONLYONCE_) && !object->released) {      /* a wait that is not for once is set again, with the same timeout */
        LARGE_INTEGER timeout;
        timeout.QuadPart = -(LONGLONG)(LONG)(ULONG_PTR)object->period * 10000;
        tp_set_wait(object, object->handle, (ULONG)object->period == 0xFFFFFFFFu ? NULL : &timeout);
      }
      break;
    }
  }
}

EXPORT NTSTATUS NTAPI RtlQueueWorkItem(PVOID function, PVOID context, ULONG flags) {
  (void)flags;
  TP_OBJ_ *object = NULL;
  NTSTATUS status = tp_new_object(TP_SIMPLE_, function, context, NULL, &object);
  if (!NT_SUCCESS(status)) return status;
  TP_RTL_ *r = tp_alloc(sizeof(*r));
  if (!r) { tp_release(object); return STATUS_NO_MEMORY; }
  r->kind = TP_RTL_WORK_;
  r->function = function;
  object->rtl = r;
  if (!tp_post(object, 0, 0, 0)) { status = STATUS_NO_MEMORY; tp_release(object); }
  return status;
}
EXPORT NTSTATUS NTAPI RtlSetIoCompletionCallback(HANDLE file, PVOID function, ULONG flags) {
  (void)flags;
  static HANDLE port;
  static volatile LONG started;
  tp_init();
  RtlEnterCriticalSection(&g_tp_lock);
  NTSTATUS status = STATUS_SUCCESS;
  if (!port) status = NtCreateIoCompletion(&port, 0x1F0003, NULL, 0);
  RtlLeaveCriticalSection(&g_tp_lock);
  if (!NT_SUCCESS(status)) return status;
  if (!__sync_lock_test_and_set(&started, 1)) {
    TP_POOL_ *pool = tp_default_pool();
    pool->port = port;
    status = tp_create_thread((PVOID)tp_port_thread, pool, 0);
    if (!NT_SUCCESS(status)) return status;
  }
  struct { HANDLE port; PVOID key; } info = {port, function};
  IO_STATUS_BLOCK iosb;
  return NtSetInformationFile(file, &iosb, &info, sizeof(info), 30);
}

/* Timer queues: a queue is the list of its timers (the default queue is the handle NULL). */
typedef struct TP_QUEUE_ { TP_OBJ_ *timers; } TP_QUEUE_;
static TP_QUEUE_ g_tp_default_queue;
EXPORT NTSTATUS NTAPI RtlCreateTimerQueue(PHANDLE out) {
  tp_init();
  TP_QUEUE_ *queue = tp_alloc(sizeof(*queue));
  if (!queue) return STATUS_NO_MEMORY;
  *out = queue;
  return STATUS_SUCCESS;
}
static LARGE_INTEGER tp_milliseconds(ULONG ms) { LARGE_INTEGER t; t.QuadPart = -(LONGLONG)ms * 10000; return t; }
EXPORT NTSTATUS NTAPI RtlCreateTimer(HANDLE queue_, PHANDLE out, PVOID function, PVOID context, ULONG due, ULONG period, ULONG flags) {
  TP_QUEUE_ *queue = queue_ ? queue_ : &g_tp_default_queue;
  TP_OBJ_ *object = NULL;
  NTSTATUS status = tp_new_object(TP_TIMER_, NULL, context, NULL, &object);
  if (!NT_SUCCESS(status)) return status;
  TP_RTL_ *r = tp_alloc(sizeof(*r));
  if (!r) { tp_release(object); return STATUS_NO_MEMORY; }
  r->kind = TP_RTL_TIMER_;
  r->function = function;
  r->flags = flags;
  object->rtl = r;
  tp_init();
  RtlEnterCriticalSection(&g_tp_lock);
  object->handle = queue;      /* (which queue: the timer is found again by its handle) */
  RtlLeaveCriticalSection(&g_tp_lock);
  LARGE_INTEGER first = tp_milliseconds(due);
  TpSetTimer(object, &first, (flags & WT_EXECUTEONLYONCE_) ? 0 : (LONG)period, 0);
  *out = object;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlUpdateTimer(HANDLE queue, HANDLE timer, ULONG due, ULONG period) {
  (void)queue;
  LARGE_INTEGER first = tp_milliseconds(due);
  TpSetTimer(timer, &first, (LONG)period, 0);
  return STATUS_SUCCESS;
}
/* Ends a timer. completion: NULL = do not wait (STATUS_PENDING if a callback runs), INVALID_HANDLE_VALUE = wait for the callbacks, else an event set when they are over. */
static NTSTATUS tp_end(TP_OBJ_ *object, HANDLE completion) {
  tp_unset(object);
  NTSTATUS status = STATUS_SUCCESS;
  if (completion == (HANDLE)(LONG_PTR)-1) tp_wait_idle(object);
  else if (object->outstanding > 0) {
    if (completion) {
      object->completion_event = completion;
      if (object->outstanding == 0) { HANDLE e = __sync_lock_test_and_set(&object->completion_event, NULL); if (e) NtSetEvent(e, NULL); }
    }
    status = STATUS_PENDING;
  } else if (completion) NtSetEvent(completion, NULL);
  TP_RTL_ *r = object->rtl;
  tp_release(object);
  (void)r;
  return status;
}
EXPORT NTSTATUS NTAPI RtlDeleteTimer(HANDLE queue, HANDLE timer, HANDLE completion) {
  (void)queue;
  if (!timer) return (NTSTATUS)0xC00000EF;      /* STATUS_INVALID_PARAMETER_1 */
  return tp_end(timer, completion);
}
EXPORT NTSTATUS NTAPI RtlDeleteTimerQueueEx(HANDLE queue_, HANDLE completion) {
  /* every timer of the queue is ended: the ones the program has not deleted are found by their handles in the list */
  TP_QUEUE_ *queue = queue_ ? queue_ : &g_tp_default_queue;
  tp_init();
  for (;;) {
    TP_OBJ_ *found = NULL;
    RtlEnterCriticalSection(&g_tp_lock);
    for (TP_OBJ_ *t = g_tp_timers; t; t = t->next) if (t->rtl && t->handle == (HANDLE)queue && !t->released) { found = t; break; }
    if (found) tp_object_ref(found);
    RtlLeaveCriticalSection(&g_tp_lock);
    if (!found) break;
    tp_end(found, (HANDLE)(LONG_PTR)-1);
    tp_object_unref(found);
  }
  NTSTATUS status = STATUS_SUCCESS;
  if (completion && completion != (HANDLE)(LONG_PTR)-1) NtSetEvent(completion, NULL);
  if (queue != &g_tp_default_queue) tp_free(queue);
  return status;
}
EXPORT NTSTATUS NTAPI RtlDeleteTimerQueue(HANDLE queue) { return RtlDeleteTimerQueueEx(queue, NULL); }
EXPORT NTSTATUS NTAPI RtlCancelTimer(HANDLE queue, HANDLE timer) { (void)queue; tp_unset(timer); return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlSetTimer(HANDLE queue, PHANDLE out, PVOID function, PVOID context, ULONG due, ULONG period, ULONG flags) { return RtlCreateTimer(queue, out, function, context, due, period, flags); }

/* Registered waits: a wait on the object that calls back each time it is signalled (or the time is up) until it is deregistered, unless it is for once. */
EXPORT NTSTATUS NTAPI RtlRegisterWait(PHANDLE out, HANDLE handle, PVOID function, PVOID context, ULONG timeout, ULONG flags) {
  TP_OBJ_ *object = NULL;
  NTSTATUS status = tp_new_object(TP_WAIT_, NULL, context, NULL, &object);
  if (!NT_SUCCESS(status)) return status;
  TP_RTL_ *r = tp_alloc(sizeof(*r));
  if (!r) { tp_release(object); return STATUS_NO_MEMORY; }
  r->kind = TP_RTL_WAIT_;
  r->function = function;
  r->flags = flags;
  object->rtl = r;
  object->period = (LONG)timeout;      /* (kept for setting the wait again; INFINITE is -1) */
  LARGE_INTEGER limit = tp_milliseconds(timeout);
  status = tp_set_wait(object, handle, timeout == 0xFFFFFFFFu ? NULL : &limit);
  if (!NT_SUCCESS(status)) { tp_release(object); return status; }
  *out = object;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlDeregisterWaitEx(HANDLE wait, HANDLE completion) {
  if (!wait || wait == (HANDLE)(LONG_PTR)-1) return STATUS_INVALID_HANDLE;
  return tp_end(wait, completion);
}
EXPORT NTSTATUS NTAPI RtlDeregisterWait(HANDLE wait) { return RtlDeregisterWaitEx(wait, NULL); }
