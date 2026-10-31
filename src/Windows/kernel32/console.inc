/* The console of kernel32 (included by kernel32.c where the console functions go). The terminal a program is attached to is a stream of bytes: lines come in, text goes out.
 * On top of that this keeps what a Windows console has: screen buffers of cells (characters and colours), a cursor, a window, the input and output modes, key events made from
 * the typed text, control handlers and the title. Text written to the console is also sent to the terminal (without ANSI sequences, which the model interprets when the output
 * mode has ENABLE_VIRTUAL_TERMINAL_PROCESSING); cell and cursor changes made with the other functions stay in the model, since the terminal cannot show them. */

typedef struct { WCHAR ch; WORD attr; } CHAR_INFO_;
typedef struct { DWORD size; BOOL visible; } CONSOLE_CURSOR_INFO_;
typedef struct { BOOL bKeyDown; WORD wRepeatCount, wVirtualKeyCode, wVirtualScanCode; WCHAR uChar; DWORD dwControlKeyState; } KEY_EVENT_RECORD_;
typedef struct { WORD EventType; union { KEY_EVENT_RECORD_ KeyEvent; BYTE raw[16]; } Event; } INPUT_RECORD_;
typedef struct { ULONG cbSize; COORD dwSize, dwCursorPosition; WORD wAttributes; SMALL_RECT srWindow; COORD dwMaximumWindowSize; WORD wPopupAttributes; BOOL bFullscreenSupported; DWORD ColorTable[16]; } CONSOLE_SCREEN_BUFFER_INFOEX_;
typedef struct { DWORD nFont; COORD dwFontSize; } CONSOLE_FONT_INFO_;
typedef struct { ULONG cbSize; DWORD nFont; COORD dwFontSize; UINT FontFamily, FontWeight; WCHAR FaceName[32]; } CONSOLE_FONT_INFOEX_;
typedef struct { ULONG cbSize, HistoryBufferSize, NumberOfHistoryBuffers, dwFlags; } CONSOLE_HISTORY_INFO_;

#define CON_MAX_BUFFERS 8
#define CON_MAX_CELLS (512 * 1024)
#define CON_VT_OUT 4
#define CON_NO_AUTO_RETURN 8
typedef struct {
  BOOL used;
  HANDLE handle;      /* NULL: the original buffer (the standard output handle) */
  DWORD access;       /* what the handle of a made buffer was asked for */
  COORD size, cursor;
  WORD attr, popup;      /* popup: 0 is the default one */
  void *from;         /* the buffer that was shown before this one was made the one that is: it is shown again when this one is closed */
  SMALL_RECT window;
  CONSOLE_CURSOR_INFO_ cursor_info;
  DWORD out_mode;
  CHAR_INFO_ *cells;
  int esc;            /* 0: text, 1: after ESC, 2: in a CSI sequence, 3: in an OSC string, 4: after ESC inside an OSC string */
  int params[8], nparams;
  BOOL esc_private;
  WCHAR osc[130];
  int osc_length;
} ScreenBuffer;

static BOOL con_buffer_or_main(HANDLE h);
static ScreenBuffer g_local_screens[CON_MAX_BUFFERS];
static ScreenBuffer *g_active_screen;
static BOOL con_buffer_alive(HANDLE h);
static CRITICAL_SECTION g_con_lock;
static volatile LONG g_con_ready;
static PHANDLER_ROUTINE g_ctrl_handlers[32];
static int g_ctrl_handler_count;
static BOOL g_ctrl_ignored;
#define CON_DEFAULT_ATTR 7

/* ---- the console of a process, shared by the processes that belong to it ------------------------------------------------------------------------------------ *
 * The main screen buffer, the title, the modes and the list of processes of a console live in a section named after the console (the number in ConsoleHandle of the process
 * parameters), mapped at the same address in every process of it, so the pointers inside are good for all. The other screen buffers and the input stay with the process. */
#ifndef FILE_MAP_ALL_ACCESS
#define FILE_MAP_ALL_ACCESS 0xF001Fu
#endif
#define CON_HOST_BASE ((PVOID)0x700000000000ull)
#define CON_MAX_MEMBERS 64
#define CON_HOST_CELLS (128 * 1024)      /* the largest main screen buffer (two copies are kept so that a resize can copy) */
typedef struct {
  volatile LONG lock;
  DWORD magic, in_mode, history_size, history_buffers, history_flags, member_count, members[CON_MAX_MEMBERS], input_cp, output_cp;      /* (the code pages: 0 is the default, UTF-8) */
  struct { DWORD pid, group, ignored; } ctrl[CON_MAX_MEMBERS];      /* for each process: its process group and whether it ignores Ctrl+C (what GenerateConsoleCtrlEvent chooses by) */
  WCHAR title[260], original_title[260];
  ScreenBuffer main;
  LONG flip;
  CHAR_INFO_ cells[2][CON_HOST_CELLS];
} ConHost;
static HANDLE g_host_mapping;
static ConHost *g_locked_host;     /* the console whose lock this process holds */
static ConHost *g_host;            /* NULL: no console (never attached, or freed) */
static BOOL g_host_tried;
static WCHAR g_detached_title[260];
static DWORD g_detached_mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
static DWORD g_detached_history[3] = {50, 4, 0};
static DWORD g_detached_cp[2];

/* The lock of a console is a spin lock; one that its holder never gives back (the holder ended in the middle of a console call) is taken after a while. */
static void con_host_lock(ConHost *h) {
  for (unsigned spins = 0; InterlockedCompareExchange(&h->lock, 1, 0) != 0; ++spins) {
    if (spins > 20000) { LARGE_INTEGER t; t.QuadPart = -10000; NtDelayExecution(FALSE, &t); }      /* (1 ms) */
    else NtYieldExecution();
    if (spins > 20000 + 3000) { InterlockedExchange(&h->lock, 0); spins = 0; }
  }
}
static void con_host_unlock(ConHost *h) { InterlockedExchange(&h->lock, 0); }
static void con_host_ctrl_set(ConHost *h, DWORD pid, DWORD group, DWORD ignored) {      /* (the lock of the console is held) */
  int free_at = -1;
  for (int i = 0; i < CON_MAX_MEMBERS; ++i) {
    if (h->ctrl[i].pid == pid) { h->ctrl[i].group = group; h->ctrl[i].ignored = ignored; return; }
    if (!h->ctrl[i].pid && free_at < 0) free_at = i;
  }
  if (free_at >= 0) { h->ctrl[free_at].pid = pid; h->ctrl[free_at].group = group; h->ctrl[free_at].ignored = ignored; }
}
static void con_host_add_member(ConHost *h, DWORD pid) {
  for (DWORD i = 0; i < h->member_count; ++i) if (h->members[i] == pid) return;
  if (h->member_count < CON_MAX_MEMBERS) h->members[h->member_count++] = pid;
}
/* A handle that is not one of the small numbers the standard handles (4, 8, 12) are known by: a program closing its console handles must not close this one. */
static HANDLE con_keep_handle(HANDLE h) {
  HANDLE low[4];
  int lows = 0;
  while ((ULONG_PTR)h <= 12 && lows < 4) {
    HANDLE copy;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, FALSE, 2 /* DUPLICATE_SAME_ACCESS */)) break;
    low[lows++] = h;
    h = copy;
  }
  for (int i = 0; i < lows; ++i) CloseHandle(low[i]);
  return h;
}
/* Opens (or creates) the console with an identifier and joins it. */
static BOOL con_host_join(ULONG_PTR id) {
  WCHAR name[40] = {'T', 'c', 'o', 's', 'C', 'o', 'n', 's', 'o', 'l', 'e', '_'};
  int n = 12;
  for (int shift = 60; shift >= 0; shift -= 4) { const int d = (int)((id >> shift) & 15); if (d || n > 12 || shift == 0) name[n++] = (WCHAR)(d < 10 ? '0' + d : 'a' + d - 10); }
  name[n] = 0;
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(ConHost), name);
  if (!mapping) return FALSE;
  ConHost *h = MapViewOfFileEx(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0, CON_HOST_BASE);
  if (!h) h = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);      /* (another address: the pointers inside the console are then good in this process only) */
  if (!h) { CloseHandle(mapping); return FALSE; }
  g_host_mapping = mapping = con_keep_handle(mapping);      /* (kept open: the section lives as long as a handle to it does) */
  con_host_lock(h);
  if (h->magic == 0) {      /* the first process of the console makes it */
    h->magic = 0x434F4E48;
    h->in_mode = ENABLE_PROCESSED_INPUT | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT;
    if (id & CON_ID_PSEUDO) h->in_mode |= 0x10 | 0x20 | 0x40 | 0x80 | 0x100;      /* (mouse, insert, quick edit, extended flags, auto position: what a pseudo console starts with) */
    h->history_size = 50; h->history_buffers = 4;
    const UNICODE_STRING *title = &parameters()->WindowTitle;
    DWORD k = 0;
    for (; title->Buffer && k < title->Length / 2 && k < 259; ++k) h->title[k] = title->Buffer[k];
    h->title[k] = 0;
    memcpy(h->original_title, h->title, sizeof(h->title));
  }
  con_host_add_member(h, GetCurrentProcessId());
  con_host_ctrl_set(h, GetCurrentProcessId(), *(ULONG *)((BYTE *)parameters() + 0x408), parameters()->ConsoleFlags & 1);
  con_host_unlock(h);
  g_host = h;
  return TRUE;
}
static void con_host_leave(void) {
  ConHost *h = g_host;
  if (!h) return;
  con_host_lock(h);
  const DWORD pid = GetCurrentProcessId();
  for (DWORD i = 0; i < h->member_count; ++i) if (h->members[i] == pid) { h->members[i] = h->members[--h->member_count]; break; }
  con_host_unlock(h);
  g_host = NULL;
  g_active_screen = NULL;
  UnmapViewOfFile(h);
  if (g_host_mapping) CloseHandle(g_host_mapping);
  g_host_mapping = NULL;
}
static void con_directory_set(DWORD pid, ULONG_PTR id);
static void con_attach_if_needed(void) {      /* (the local lock is held) */
  if (g_host_tried) return;
  g_host_tried = TRUE;
  const ULONG_PTR id = (ULONG_PTR)parameters()->ConsoleHandle;
  if (id && con_host_join(id)) con_directory_set(GetCurrentProcessId(), id);
}
static WCHAR *con_title_ptr(void) { return g_host ? g_host->title : g_detached_title; }
static DWORD *con_in_mode_ptr(void) { return g_host ? &g_host->in_mode : &g_detached_mode; }
static DWORD *con_history_ptr(void) { return g_host ? &g_host->history_size : g_detached_history; }      /* size, buffers, flags in a row */
#define g_con_title (con_title_ptr())
#define g_con_in_mode (*con_in_mode_ptr())
static ScreenBuffer *con_screen(int i) { return i == 0 && g_host ? &g_host->main : &g_local_screens[i]; }

static void con_register_waits(void);
static void con_lock(void) {
  if (g_con_ready != 2) {
    if (InterlockedCompareExchange(&g_con_ready, 1, 0) == 0) { RtlInitializeCriticalSection(&g_con_lock); g_con_ready = 2; con_register_waits(); }
    else while (g_con_ready != 2) NtYieldExecution();
  }
  EnterCriticalSection(&g_con_lock);
  if (g_con_lock.RecursionCount == 1) {      /* (the lock of the console is taken once, not again by a nested call) */
    con_attach_if_needed();
    g_locked_host = g_host;
    if (g_host) con_host_lock(g_host);
  }
}
static void con_unlock(void) {
  if (g_con_lock.RecursionCount == 1 && g_locked_host) { ConHost *h = g_locked_host; g_locked_host = NULL; con_host_unlock(h); }
  LeaveCriticalSection(&g_con_lock);
}

static BOOL con_alloc_cells(ScreenBuffer *sb, COORD size) {
  if (size.X <= 0 || size.Y <= 0 || (long)size.X * size.Y > CON_MAX_CELLS) return FALSE;
  const BOOL shared = g_host && sb == &g_host->main;
  if (shared && (long)size.X * size.Y > CON_HOST_CELLS) return FALSE;
  CHAR_INFO_ *cells = shared ? g_host->cells[g_host->flip ^ 1] : RtlAllocateHeap(heap(), 0, (SIZE_T)size.X * size.Y * sizeof(CHAR_INFO_));
  if (!cells) return FALSE;
  for (long i = 0; i < (long)size.X * size.Y; ++i) { cells[i].ch = ' '; cells[i].attr = sb->attr; }
  if (sb->cells) {      /* the old text stays where it is, cut or padded */
    for (SHORT y = 0; y < size.Y && y < sb->size.Y; ++y)
      for (SHORT x = 0; x < size.X && x < sb->size.X; ++x) cells[(long)y * size.X + x] = sb->cells[(long)y * sb->size.X + x];
    if (!shared) RtlFreeHeap(heap(), 0, sb->cells);
  }
  if (shared) g_host->flip ^= 1;
  sb->cells = cells;
  sb->size = size;
  return TRUE;
}
static ScreenBuffer *con_main(void) {      /* (the lock is held) */
  ScreenBuffer *sb = con_screen(0);
  if (!sb->used) {
    sb->used = TRUE; sb->handle = NULL;
    sb->attr = CON_DEFAULT_ATTR;
    sb->out_mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT;
    sb->cursor_info.size = 25; sb->cursor_info.visible = TRUE;
    COORD size = {80, 25};
    const ULONG_PTR id = (ULONG_PTR)parameters()->ConsoleHandle;
    if ((id & CON_ID_PSEUDO) && CON_ID_PSEUDO_X(id) > 0 && CON_ID_PSEUDO_Y(id) > 0) { size.X = (SHORT)CON_ID_PSEUDO_X(id); size.Y = (SHORT)CON_ID_PSEUDO_Y(id); }
    if (!con_alloc_cells(sb, size)) { sb->used = FALSE; return NULL; }
    sb->window.Right = (SHORT)(size.X - 1); sb->window.Bottom = (SHORT)(size.Y - 1);
  }
  if (!g_active_screen) g_active_screen = sb;
  return sb;
}

/* ---- handles that belong to the console but to no standard handle ------------------------------------------------------------------------------------------- *
 * What NtCreateFile of "\Device\ConDrv\Input" or "\Output" gives (ntdll.dll: a named event, TcosConI_ or TcosConO_ and the number of the console that made it), and the handle of a
 * screen buffer. The owner of the console keeps such an event set while the console has input waiting (con_sync_signals); a process it was duplicated into finds out what the handle
 * is from its name. */
typedef struct { HANDLE handle; ULONG_PTR console; LONG kind; } ConEvent;      /* kind: 1 input, 2 output (ntdll.dll keeps the list) */
IMPORT int NTAPI RtlTcosConsoleEvents(ConEvent *out, int max);
IMPORT void NTAPI RtlTcosConsoleEventNote(HANDLE h, ULONG_PTR console, LONG kind);
IMPORT void NTAPI RtlTcosConsoleEventForget(HANDLE h);
#define CON_ORPHAN (~(ULONG_PTR)0)      /* the console of an event that was given up (the console it belonged to is gone): it stays set */
static struct { HANDLE h; LONG kind; } g_unbound_cache[64];
static int g_unbound_next;
/* 1 for a handle to the input of the console, 2 for one to the output, 0 for any other; asked of the kernel by the name once for each handle. */
static LONG con_unbound_kind(HANDLE h) {
  if (!h || h == INVALID_HANDLE_VALUE) return 0;
  for (int i = 0; i < 64; ++i) if (g_unbound_cache[i].h == h) return g_unbound_cache[i].kind;
  LONG kind = 0;
  ConEvent own[64];
  const int n = RtlTcosConsoleEvents(own, 64);
  for (int i = 0; i < n && !kind; ++i) if (own[i].handle == h) kind = own[i].kind;
  if (!kind) {
    WCHAR buffer[16 + 64];
    ULONG got = 0;
    if (NT_SUCCESS(NtQueryObject(h, 1, buffer, sizeof(buffer), &got))) {
      const USHORT bytes = ((UNICODE_STRING *)buffer)->Length;
      const WCHAR *name = (const WCHAR *)((BYTE *)buffer + 16);
      static const WCHAR prefix[] = {'\\', 'B', 'a', 's', 'e', 'N', 'a', 'm', 'e', 'd', 'O', 'b', 'j', 'e', 'c', 't', 's', '\\', 'T', 'c', 'o', 's', 'C', 'o', 'n'};
      if (bytes / 2 > sizeof(prefix) / 2 + 1) {
        BOOL same = TRUE;
        for (unsigned i = 0; i < sizeof(prefix) / 2; ++i) if (name[i] != prefix[i]) same = FALSE;
        if (same && name[sizeof(prefix) / 2 + 1] == '_') kind = name[sizeof(prefix) / 2] == 'I' ? 1 : name[sizeof(prefix) / 2] == 'O' ? 2 : 0;
      }
    }
  }
  g_unbound_cache[g_unbound_next].h = h; g_unbound_cache[g_unbound_next].kind = kind;
  g_unbound_next = (g_unbound_next + 1) % 64;
  return kind;
}
static void con_unbound_forget(HANDLE h) {
  for (int i = 0; i < 64; ++i) if (g_unbound_cache[i].h == h) g_unbound_cache[i].h = NULL;
}

/* The standard handles that are consoles, found out once (and again if a handle changes). */
static struct { HANDLE h; int console; } g_con_cache[3];
static BOOL con_std_is_terminal(HANDLE h) {
  for (int i = 0; i < 3; ++i) if (g_con_cache[i].h == h && h) return g_con_cache[i].console;
  const int type = device_type(h);
  if (type < 0) return FALSE;      /* (not remembered: it may be a handle that will be closed) */
  if (type != 0x50 && con_unbound_kind(h)) return TRUE;
  for (int i = 0; i < 3; ++i) if (h == GetStdHandle(i == 0 ? STD_INPUT_HANDLE : i == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE)) { g_con_cache[i].h = h; g_con_cache[i].console = type == 0x50; return type == 0x50; }
  return type == 0x50;
}
/* Whether this process belongs to a console (the first question lets it join the one in its process parameters). */
static BOOL con_attached(void) { if (!g_host_tried) { con_lock(); con_unlock(); } return g_host != NULL; }
static BOOL con_std_is_console(HANDLE h) { return con_std_is_terminal(h) && con_attached(); }
/* The standard output of a console (the same handle as CONOUT$) is not something to read from. */
/* The handles CreateFile made for "CONIN$" and "CONOUT$" (more handles to the console input and output, which then are not the standard handles). */
static HANDLE g_con_alias[16];
static BOOL g_con_alias_input[16];
static HANDLE g_con_alias_bound[16];      /* for an output alias: the buffer that was shown when it was opened (its handle; NULL is the original buffer) */
static void con_alias_add(HANDLE h, BOOL input) {
  for (int i = 0; i < 16; ++i) if (g_con_alias[i] == h || !g_con_alias[i]) { g_con_alias[i] = h; g_con_alias_input[i] = input; return; }
}
static void con_alias_forget(HANDLE h) { for (int i = 0; i < 16; ++i) if (g_con_alias[i] == h) { g_con_alias[i] = NULL; g_con_alias_bound[i] = NULL; } }
static int con_table_alias_kind(HANDLE h) {      /* 1: an input alias, 2: an output alias, 0: none (a handle that was closed and is a file now is none either) */
  for (int i = 0; i < 16; ++i) if (g_con_alias[i] == h && h && device_type(h) == 0x50) return g_con_alias_input[i] ? 1 : 2;
  return 0;
}
static int con_alias_kind(HANDLE h) {
  const int table = con_table_alias_kind(h);
  if (table) return table;
  if (!con_unbound_kind(h)) return 0;
  return con_attached() ? (int)con_unbound_kind(h) : 0;      /* (without a console the handles of it are of no use) */
}
/* The terminal handles of this process (they are the standard handles until the console is freed; a new console opens new ones). */
static HANDLE g_term[3] = {(HANDLE)(ULONG_PTR)4, (HANDLE)(ULONG_PTR)8, (HANDLE)(ULONG_PTR)12};
static HANDLE con_open_terminal(int which) {      /* 0 input, 1 output, 2 error */
  static const WCHAR *const names[3] = {L"\\Device\\ConDrv\\TcosIn", L"\\Device\\ConDrv\\TcosOut", L"\\Device\\ConDrv\\TcosErr"};
  UNICODE_STRING name;
  unicode_string(&name, names[which]);
  OBJECT_ATTRIBUTES attributes = { sizeof(attributes), NULL, &name, 0x40, NULL, NULL };
  IO_STATUS_BLOCK iosb;
  HANDLE h = NULL;
  if (!NT_SUCCESS(NtCreateFile(&h, 0x100003 | 0x80, &attributes, &iosb, NULL, 0, 7, 1, 0x20, NULL, 0))) return NULL;
  return h;
}
static HANDLE con_open_alias(BOOL input, BOOL inherit) {      /* another handle to the console input or output (descriptors 0 and 1 are them) */
  HANDLE copy = NULL;
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }
  const NTSTATUS made = NtDuplicateObject(NtCurrentProcess(), g_term[input ? 0 : 1], NtCurrentProcess(), &copy, 0, inherit ? 2 : 0, 2 /* DUPLICATE_SAME_ACCESS */);
  if (!NT_SUCCESS(made)) { fail_nt(made); return INVALID_HANDLE_VALUE; }
  con_alias_add(copy, input);
  if (!input) {      /* an output handle opened as CONOUT$ is the buffer that is shown at that time */
    con_lock();
    HANDLE bound = g_active_screen && g_active_screen != con_screen(0) ? g_active_screen->handle : NULL;
    con_unlock();
    for (int i = 0; i < 16; ++i) if (g_con_alias[i] == copy) g_con_alias_bound[i] = bound;
  }
  return copy;
}
/* OpenConsoleW(name, access, inherit, creation): the console's "CONIN$" or "CONOUT$" and nothing else; the access and the creation disposition are checked and then not needed. */
EXPORT HANDLE WINAPI OpenConsoleW(LPCWSTR name, DWORD access, BOOL inherit, DWORD creation) {
  const BOOL input = name && equals_ignore_case(name, (const WCHAR *)L"CONIN$");
  if (!name || !name[0]) { set_error(ERROR_PATH_NOT_FOUND); return INVALID_HANDLE_VALUE; }
  if ((!input && !equals_ignore_case(name, (const WCHAR *)L"CONOUT$")) || creation > 5 || (access & 0x0C000000u)) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  return con_open_alias(input, inherit);
}
/* The name of the program the console's input history is kept for. */
static WCHAR g_input_exe[MAX_PATH];
static void con_input_exe_default(void) {
  if (g_input_exe[0]) return;
  WCHAR module[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(NULL, module, MAX_PATH * 2);
  DWORD start = n;
  while (start > 0 && module[start - 1] != '\\') --start;
  DWORD k = 0;
  for (; start + k < n && k + 1 < MAX_PATH; ++k) g_input_exe[k] = module[start + k];
  g_input_exe[k] = 0;
}
EXPORT BOOL WINAPI GetConsoleInputExeNameW(DWORD capacity, LPWSTR buffer) {
  con_input_exe_default();
  const DWORD n = (DWORD)wcslen(g_input_exe);
  if (capacity > n && buffer) memcpy(buffer, g_input_exe, ((size_t_)n + 1) * 2);
  else set_error(111 /* ERROR_BUFFER_OVERFLOW */);
  return TRUE;
}
EXPORT BOOL WINAPI GetConsoleInputExeNameA(DWORD capacity, LPSTR buffer) {
  con_input_exe_default();
  const DWORD n = (DWORD)wcslen(g_input_exe);
  const DWORD need = (DWORD)WideCharToMultiByte(CP_ACP, 0, g_input_exe, (int)n, NULL, 0, NULL, NULL);
  if (capacity > need && buffer) { WideCharToMultiByte(CP_ACP, 0, g_input_exe, (int)n, buffer, (int)capacity, NULL, NULL); buffer[need] = 0; }
  else set_error(111 /* ERROR_BUFFER_OVERFLOW */);
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleInputExeNameW(LPCWSTR name) {
  if (!name || !name[0] || wcslen(name) >= MAX_PATH) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  memcpy(g_input_exe, name, ((size_t_)wcslen(name) + 1) * 2);
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleInputExeNameA(LPCSTR name) {
  if (!name || !name[0]) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR wide[MAX_PATH];
  const int n = MultiByteToWideChar(CP_ACP, 0, name, -1, wide, MAX_PATH);
  if (n <= 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return SetConsoleInputExeNameW(wide);
}
static BOOL con_is_output_only(HANDLE h) { return h && ((h == GetStdHandle(STD_OUTPUT_HANDLE) && h != GetStdHandle(STD_INPUT_HANDLE) && con_std_is_console(h)) || con_alias_kind(h) == 2); }
static BOOL con_is_input(HANDLE h) { return h && ((h == GetStdHandle(STD_INPUT_HANDLE) && con_std_is_console(h)) || con_alias_kind(h) == 1); }
static BOOL con_buffer_alive(HANDLE h) { return SetEvent(h) || GetLastError() != ERROR_INVALID_HANDLE; }
static volatile LONG g_screens_made;      /* screen buffers have been made: CloseHandle has to look whether it closed one */
/* A handle was closed: the screen buffer it was the handle of goes (unless another copy of the handle keeps it), and the one shown before it is shown again. A handle number that
 * is used again soon after must not make a closed buffer look alive. */
static void con_screen_closed(HANDLE h) {
  if (!g_screens_made) return;
  con_lock();
  for (int i = 1; i < CON_MAX_BUFFERS; ++i) {
    ScreenBuffer *sb = con_screen(i);
    if (!sb->used || sb->handle != h || con_buffer_alive(h)) continue;
    ScreenBuffer *next = (ScreenBuffer *)sb->from;
    for (int k = 1; k < CON_MAX_BUFFERS; ++k) if (con_screen(k)->used && con_screen(k)->from == sb) con_screen(k)->from = next;      /* (what was shown before it is now before the next) */
    if (g_active_screen == sb) g_active_screen = next && next->used ? next : NULL;
    RtlFreeHeap(heap(), 0, sb->cells);
    memset(sb, 0, sizeof(*sb));
  }
  con_unlock();
}
/* The buffer that is shown (the lock is held): when the one that was is closed, the one before it is. */
static ScreenBuffer *con_active(void) {
  while (g_active_screen && g_active_screen != con_screen(0) && (!g_active_screen->used || !con_buffer_alive(g_active_screen->handle))) {
    ScreenBuffer *dead = g_active_screen;
    ScreenBuffer *next = (ScreenBuffer *)dead->from;
    if (dead->used) RtlFreeHeap(heap(), 0, dead->cells);
    memset(dead, 0, sizeof(*dead));
    g_active_screen = next && next->used ? next : NULL;
  }
  return g_active_screen ? g_active_screen : con_main();      /* (the main buffer of a console is made when it is first needed) */
}
/* The screen buffer a handle writes to (the lock is held), or NULL. */
static ScreenBuffer *con_buffer(HANDLE h) {
  for (int i = 1; i < CON_MAX_BUFFERS; ++i) {
    ScreenBuffer *sb = con_screen(i);
    if (sb->used && sb->handle == h) {
      if (!con_buffer_alive(h)) { RtlFreeHeap(heap(), 0, sb->cells); if (g_active_screen == sb) g_active_screen = con_screen(0); memset(sb, 0, sizeof(*sb)); return NULL; }
      return sb;
    }
  }
  if (!h || con_is_input(h) || !con_std_is_console(h)) return NULL;
  if (con_alias_kind(h) == 2) {      /* (a handle opened as CONOUT$ is the buffer that was shown when it was opened, while that is there; an unbound handle is the one shown) */
    for (int i = 0; i < 16; ++i) {
      if (g_con_alias[i] != h) continue;
      if (!g_con_alias_bound[i]) return con_main();
      for (int k = 1; k < CON_MAX_BUFFERS; ++k) if (con_screen(k)->used && con_screen(k)->handle == g_con_alias_bound[i] && con_buffer_alive(g_con_alias_bound[i])) return con_screen(k);
    }
    return con_active();
  }
  return con_main();
}
#define WITH_BUFFER(h, sb, failure) con_lock(); ScreenBuffer *sb = con_buffer(h); if (!sb) { con_unlock(); set_error(ERROR_INVALID_HANDLE); return failure; }

static void sb_follow(ScreenBuffer *sb) {      /* the window moves to show the cursor */
  const SHORT w = sb->window.Right - sb->window.Left, h = sb->window.Bottom - sb->window.Top;
  if (sb->cursor.Y > sb->window.Bottom) { sb->window.Bottom = sb->cursor.Y; sb->window.Top = (SHORT)(sb->cursor.Y - h); }
  if (sb->cursor.Y < sb->window.Top) { sb->window.Top = sb->cursor.Y; sb->window.Bottom = (SHORT)(sb->cursor.Y + h); }
  if (sb->cursor.X > sb->window.Right) { sb->window.Right = sb->cursor.X; sb->window.Left = (SHORT)(sb->cursor.X - w); }
  if (sb->cursor.X < sb->window.Left) { sb->window.Left = sb->cursor.X; sb->window.Right = (SHORT)(sb->cursor.X + w); }
}
static void sb_blank_row(ScreenBuffer *sb, SHORT y) { for (SHORT x = 0; x < sb->size.X; ++x) { CHAR_INFO_ *c = &sb->cells[(long)y * sb->size.X + x]; c->ch = ' '; c->attr = sb->attr; } }
static void sb_scroll_up(ScreenBuffer *sb) {
  memmove(sb->cells, sb->cells + sb->size.X, (size_t_)(sb->size.Y - 1) * sb->size.X * sizeof(CHAR_INFO_));
  sb_blank_row(sb, (SHORT)(sb->size.Y - 1));
}
static void sb_line_down(ScreenBuffer *sb) {
  if (sb->cursor.Y + 1 >= sb->size.Y) sb_scroll_up(sb); else ++sb->cursor.Y;
}
/* Without ENABLE_WRAP_AT_EOL_OUTPUT the character after the one in the last column goes back to the column the write began in (Windows keeps the last column as it is). */
static SHORT g_write_start;
static BOOL g_edge_written;
static void sb_put(ScreenBuffer *sb, WCHAR c) {
  if (g_edge_written) {
    g_edge_written = FALSE;
    if (!(sb->out_mode & ENABLE_WRAP_AT_EOL_OUTPUT)) sb->cursor.X = g_write_start < sb->size.X ? g_write_start : 0;
  }
  CHAR_INFO_ *cell = &sb->cells[(long)sb->cursor.Y * sb->size.X + sb->cursor.X];
  cell->ch = c; cell->attr = sb->attr;
  if (sb->cursor.X + 1 < sb->size.X) ++sb->cursor.X;
  else if (sb->out_mode & ENABLE_WRAP_AT_EOL_OUTPUT) { sb->cursor.X = 0; sb_line_down(sb); }
  else g_edge_written = TRUE;
}
static void sb_clear(ScreenBuffer *sb, COORD from, long count) {      /* count cells from `from`, in reading order, with the current attributes */
  long i = (long)from.Y * sb->size.X + from.X;
  for (; count > 0 && i < (long)sb->size.X * sb->size.Y; --count, ++i) { sb->cells[i].ch = ' '; sb->cells[i].attr = sb->attr; }
}
static WORD ansi_to_attr(int n, BOOL background) {      /* ANSI colour 0..7 to the bits of a Windows attribute */
  static const BYTE map[8] = {0, 4, 2, 6, 1, 5, 3, 7};
  return (WORD)(background ? map[n & 7] << 4 : map[n & 7]);
}
static void sb_sgr(ScreenBuffer *sb) {
  int count = sb->nparams ? sb->nparams : 1;
  for (int i = 0; i < count; ++i) {
    const int p = sb->nparams ? sb->params[i] : 0;
    if (p == 0) sb->attr = CON_DEFAULT_ATTR;
    else if (p == 1) sb->attr |= 8;
    else if (p == 22) sb->attr &= (WORD)~8;
    else if (p == 4) sb->attr |= 0x8000;
    else if (p == 24) sb->attr &= (WORD)~0x8000;
    else if (p == 7) sb->attr |= 0x4000;
    else if (p == 27) sb->attr &= (WORD)~0x4000;
    else if (p >= 30 && p <= 37) sb->attr = (WORD)((sb->attr & ~7) | ansi_to_attr(p - 30, FALSE));
    else if (p == 39) sb->attr = (WORD)((sb->attr & ~7) | 7);
    else if (p >= 40 && p <= 47) sb->attr = (WORD)((sb->attr & ~0x70) | ansi_to_attr(p - 40, TRUE));
    else if (p == 49) sb->attr &= (WORD)~0x70;
    else if (p >= 90 && p <= 97) sb->attr = (WORD)((sb->attr & ~0xF) | ansi_to_attr(p - 90, FALSE) | 8);
    else if (p >= 100 && p <= 107) sb->attr = (WORD)((sb->attr & ~0xF0) | ansi_to_attr(p - 100, TRUE) | 0x80);
    else if ((p == 38 || p == 48) && i + 1 < count) {      /* 256 colours or true colour: the nearest of the sixteen */
      const BOOL bg = p == 48;
      int index = 7;
      if (sb->params[i + 1] == 5 && i + 2 < count) { index = sb->params[i + 2]; i += 2; if (index >= 16) index = index >= 232 ? (index > 243 ? 7 : 0) : 7; }
      else if (sb->params[i + 1] == 2 && i + 4 < count) {
        const int r = sb->params[i + 2] > 127, g = sb->params[i + 3] > 127, b = sb->params[i + 4] > 127;
        index = r | (g << 1) | (b << 2); i += 4;
      } else break;
      const WORD bits = ansi_to_attr(index & 7, bg) | (index >= 8 && index < 16 ? (bg ? 0x80 : 8) : 0);
      sb->attr = (WORD)(bg ? ((sb->attr & ~0xF0) | bits) : ((sb->attr & ~0xF) | bits));
    }
  }
}
static int sb_param(ScreenBuffer *sb, int i, int fallback) { return i < sb->nparams && sb->params[i] > 0 ? sb->params[i] : fallback; }
static void sb_csi(ScreenBuffer *sb, WCHAR final) {
  if (sb->esc_private) {
    if (final == 'h' || final == 'l') for (int i = 0; i < sb->nparams; ++i) if (sb->params[i] == 25) sb->cursor_info.visible = final == 'h';
    return;
  }
  switch (final) {
    case 'A': sb->cursor.Y = (SHORT)(sb->cursor.Y - sb_param(sb, 0, 1)); if (sb->cursor.Y < 0) sb->cursor.Y = 0; break;
    case 'B': sb->cursor.Y = (SHORT)(sb->cursor.Y + sb_param(sb, 0, 1)); if (sb->cursor.Y >= sb->size.Y) sb->cursor.Y = (SHORT)(sb->size.Y - 1); break;
    case 'C': sb->cursor.X = (SHORT)(sb->cursor.X + sb_param(sb, 0, 1)); if (sb->cursor.X >= sb->size.X) sb->cursor.X = (SHORT)(sb->size.X - 1); break;
    case 'D': sb->cursor.X = (SHORT)(sb->cursor.X - sb_param(sb, 0, 1)); if (sb->cursor.X < 0) sb->cursor.X = 0; break;
    case 'E': sb->cursor.X = 0; sb->cursor.Y = (SHORT)(sb->cursor.Y + sb_param(sb, 0, 1)); if (sb->cursor.Y >= sb->size.Y) sb->cursor.Y = (SHORT)(sb->size.Y - 1); break;
    case 'F': sb->cursor.X = 0; sb->cursor.Y = (SHORT)(sb->cursor.Y - sb_param(sb, 0, 1)); if (sb->cursor.Y < 0) sb->cursor.Y = 0; break;
    case 'G': sb->cursor.X = (SHORT)(sb_param(sb, 0, 1) - 1); if (sb->cursor.X >= sb->size.X) sb->cursor.X = (SHORT)(sb->size.X - 1); break;
    case 'd': sb->cursor.Y = (SHORT)(sb->window.Top + sb_param(sb, 0, 1) - 1); if (sb->cursor.Y >= sb->size.Y) sb->cursor.Y = (SHORT)(sb->size.Y - 1); break;
    case 'H': case 'f': {      /* rows and columns count from the window's corner */
      int row = sb->window.Top + sb_param(sb, 0, 1) - 1, col = sb->window.Left + sb_param(sb, 1, 1) - 1;
      if (row >= sb->size.Y) row = sb->size.Y - 1;
      if (col >= sb->size.X) col = sb->size.X - 1;
      sb->cursor.Y = (SHORT)row; sb->cursor.X = (SHORT)col;
      break;
    }
    case 'J': {
      const int mode = sb->nparams ? sb->params[0] : 0;
      COORD zero = {0, 0};
      const long here = (long)sb->cursor.Y * sb->size.X + sb->cursor.X, total = (long)sb->size.X * sb->size.Y;
      if (mode == 0) sb_clear(sb, sb->cursor, total - here);
      else if (mode == 1) sb_clear(sb, zero, here + 1);
      else sb_clear(sb, zero, total);
      break;
    }
    case 'K': {
      const int mode = sb->nparams ? sb->params[0] : 0;
      COORD start = {0, sb->cursor.Y};
      if (mode == 0) sb_clear(sb, sb->cursor, sb->size.X - sb->cursor.X);
      else if (mode == 1) sb_clear(sb, start, sb->cursor.X + 1);
      else sb_clear(sb, start, sb->size.X);
      break;
    }
    case 'm': sb_sgr(sb); break;
    default: break;      /* (saving and restoring the cursor, scrolling regions: not kept) */
  }
}

/* Writes text into the model (the lock is held). The characters a terminal would show are copied to `host` (a program that wrote ANSI sequences does not send them to the terminal);
 * returns how many. */
static DWORD sb_write(ScreenBuffer *sb, const WCHAR *s, DWORD n, WCHAR *host) {
  DWORD out = 0;
  g_write_start = sb->cursor.X; g_edge_written = FALSE;
  const BOOL vt = (sb->out_mode & CON_VT_OUT) != 0, processed = (sb->out_mode & ENABLE_PROCESSED_OUTPUT) != 0;
  for (DWORD i = 0; i < n; ++i) {
    const WCHAR c = s[i];
    if (sb->esc != 0) {
      if (sb->esc == 1) {
        if (c == '[') { sb->esc = 2; sb->nparams = 0; sb->esc_private = FALSE; memset(sb->params, 0, sizeof(sb->params)); }
        else if (c == ']') { sb->esc = 3; sb->osc_length = 0; }
        else sb->esc = 0;
      } else if (sb->esc == 2) {
        if (c >= '0' && c <= '9') { if (sb->nparams == 0) sb->nparams = 1; int *p = &sb->params[sb->nparams - 1]; if (*p < 100000) *p = *p * 10 + (c - '0'); }
        else if (c == ';') { if (sb->nparams == 0) sb->nparams = 1; if (sb->nparams < 8) ++sb->nparams; }
        else if (c == '?') sb->esc_private = TRUE;
        else if (c >= 0x40 && c <= 0x7E) { sb_csi(sb, c); sb->esc = 0; }
        else if (c < 0x20 || c > 0x2F) sb->esc = 0;
      } else {      /* an OSC string ends with BEL or ESC \ */
        if (c == 7 || (sb->esc == 4 && c == '\\')) {
          if (sb->osc_length > 2 && (sb->osc[0] == '0' || sb->osc[0] == '2') && sb->osc[1] == ';') {
            int k = 0;
            for (; k + 2 < sb->osc_length && k < 259; ++k) g_con_title[k] = sb->osc[k + 2];
            g_con_title[k] = 0;
          }
          sb->esc = 0;
        } else if (c == 0x1B) sb->esc = 4;
        else { sb->esc = 3; if (sb->osc_length < 128) sb->osc[sb->osc_length++] = c; }
      }
      continue;
    }
    if (vt && c == 0x1B) { sb->esc = 1; continue; }
    host[out++] = c;
    if (processed && c < 0x20) {
      if (c != 7) g_edge_written = FALSE;
      switch (c) {
        case '\n': if (!(vt && (sb->out_mode & CON_NO_AUTO_RETURN))) sb->cursor.X = 0; sb_line_down(sb); break;
        case '\r': sb->cursor.X = 0; break;
        case '\b': if (sb->cursor.X > 0) --sb->cursor.X; break;
        case '\t': { const SHORT stop = (SHORT)((sb->cursor.X / 8 + 1) * 8); while (sb->cursor.X < stop && sb->cursor.X + 1 < sb->size.X) sb_put(sb, ' '); break; }
        case 7: break;
        default: sb_put(sb, c); break;
      }
    } else sb_put(sb, c);
  }
  sb_follow(sb);
  return out;
}

/* Writes the characters to a console handle: into the model and, if the buffer is the one on show, to the terminal. */
static BOOL con_write_wide(HANDLE h, const WCHAR *w, DWORD length) {
  DWORD done = 0;
  while (done < length) {
    DWORD chunk = length - done < 300 ? length - done : 300;
    if (chunk > 1 && w[done + chunk - 1] >= 0xD800 && w[done + chunk - 1] < 0xDC00) --chunk;      /* do not split a surrogate pair */
    WCHAR host[300];
    char utf8[1024];
    con_lock();
    ScreenBuffer *sb = con_buffer(h);
    if (!sb) { con_unlock(); set_error(ERROR_INVALID_HANDLE); return FALSE; }
    const DWORD n = sb_write(sb, w + done, chunk, host);
    const BOOL show = sb == g_active_screen || (g_active_screen == NULL && sb == con_screen(0));
    con_unlock();
    done += chunk;
    if (!show || n == 0) continue;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, host, (int)n, utf8, sizeof(utf8), NULL, NULL);
    IO_STATUS_BLOCK iosb;
    HANDLE terminal = h;      /* (the handle of a screen buffer that was made is no terminal: the text goes out by the console's own) */
    if (h != GetStdHandle(STD_OUTPUT_HANDLE) && h != GetStdHandle(STD_ERROR_HANDLE) && device_type(h) != 0x50) terminal = g_term[1];
    if (bytes > 0 && !NT_SUCCESS(NtWriteFile(terminal, NULL, NULL, NULL, &iosb, utf8, (ULONG)bytes, NULL, NULL))) return FALSE;
  }
  return TRUE;
}
/* WriteFile to a console: UTF-8 bytes (a character split between two writes is kept for the next). */
static BOOL console_write_hook(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, BOOL *handled) {
  static BYTE carry[4];
  static int carried;
  *handled = FALSE;
  if (h != GetStdHandle(STD_OUTPUT_HANDLE) && h != GetStdHandle(STD_ERROR_HANDLE) && con_alias_kind(h) != 2) return FALSE;
  if (!con_std_is_console(h)) return FALSE;
  *handled = TRUE;
  const BYTE *b = buffer;
  DWORD total = length;
  BYTE joined[1030];
  DWORD pos = 0;
  while (pos < total) {
    DWORD n = total - pos < 1024 ? total - pos : 1024;
    DWORD k = 0;
    for (int i = 0; i < carried; ++i) joined[k++] = carry[i];
    carried = 0;
    for (DWORD i = 0; i < n; ++i) joined[k++] = b[pos + i];
    pos += n;
    DWORD usable = k;      /* do not end inside a UTF-8 sequence */
    for (DWORD back = 1; back <= 3 && back <= k; ++back) {
      const BYTE c = joined[k - back];
      if ((c & 0xC0) == 0x80) continue;
      const DWORD need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      if (need > back) usable = k - back;
      break;
    }
    for (DWORD i = usable; i < k; ++i) carry[carried++] = joined[i];
    WCHAR wide[1030];
    const int units = MultiByteToWideChar(CP_UTF8, 0, (LPCSTR)joined, (int)usable, wide, 1030);
    if (units > 0 && !con_write_wide(h, wide, (DWORD)units)) return FALSE;
  }
  if (written) *written = length;
  return TRUE;
}

/* ---- input ----------------------------------------------------------------------------------------------------------------------------------------------- */

#define CON_TEXT_MAX 2048
#define CON_INJECT_MAX 64
static char g_in_text[CON_TEXT_MAX];
static DWORD g_in_len, g_in_pos;
static BOOL g_in_up, g_in_eof;
static INPUT_RECORD_ g_injected[CON_INJECT_MAX];
static DWORD g_inject_head, g_inject_count;

static BOOL con_input_ready_now(BOOL *at_end) {      /* could the standard input give something without waiting? */
  *at_end = FALSE;
  HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
  BYTE info[40];
  IO_STATUS_BLOCK iosb;
  memset(info, 0, sizeof(info));
  if (!NT_SUCCESS(NtQueryInformationFile(in, &iosb, info, sizeof(info), 24))) return TRUE;      /* (not a console: a read tells) */
  *at_end = info[24] != 0;
  return *(DWORD *)(info + 20) > 0 || *at_end;
}
/* Makes sure text is waiting in g_in_text: 1 if there is, 0 if not (without waiting), -1 at the end of input. */
static int con_fill_input(BOOL wait) {
  if (g_in_pos < g_in_len) return 1;
  if (g_in_eof) return -1;
  g_in_len = g_in_pos = 0;
  if (!wait) {
    BOOL at_end;
    if (!con_input_ready_now(&at_end)) return 0;
  }
  IO_STATUS_BLOCK iosb;
  iosb.Information = 0;
  const NTSTATUS status = NtReadFile(GetStdHandle(STD_INPUT_HANDLE), NULL, NULL, NULL, &iosb, g_in_text, 1024, NULL, NULL);
  if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && iosb.Information == 0)) { g_in_eof = TRUE; return -1; }
  if (!NT_SUCCESS(status)) return -1;
  g_in_len = (DWORD)iosb.Information;
  return 1;
}
/* Is there an event for a wait on the input handle? The end of the input is not one: a console has no end, so a wait goes on (a read at the end still ends). */
static BOOL con_input_ready(void) {
  con_lock();
  const BOOL ready = g_inject_count > 0 || g_in_pos < g_in_len;
  con_unlock();
  if (ready) return TRUE;
  BOOL at_end;
  return con_input_ready_now(&at_end) && !at_end;
}
/* The events of the console follow its input: set while there is something to read, reset when there is not. A process that has been given up by its console (FreeConsole, then a new
 * console) leaves the events of the old one set. Every call that changes the input (and every wait) brings them up to date. */
static void con_sync_signals(void) {
  if (RtlTcosConsoleEvents(NULL, 0) == 0) return;
  ConEvent ev[64];
  const int n = RtlTcosConsoleEvents(ev, 64);
  const ULONG_PTR current = con_attached() ? (ULONG_PTR)parameters()->ConsoleHandle : 0;
  int ready = -1;
  for (int i = 0; i < n; ++i) {
    if (!current || ev[i].console != current) continue;
    if (ready < 0) ready = con_input_ready() ? 1 : 0;
    if (ready) SetEvent(ev[i].handle); else ResetEvent(ev[i].handle);
  }
}
static void con_orphan_events(ULONG_PTR keep) {
  if (RtlTcosConsoleEvents(NULL, 0) == 0) return;
  ConEvent ev[64];
  const int n = RtlTcosConsoleEvents(ev, 64);
  for (int i = 0; i < n; ++i) {
    if (ev[i].console == keep || ev[i].console == CON_ORPHAN) continue;
    RtlTcosConsoleEventNote(ev[i].handle, CON_ORPHAN, ev[i].kind);
    SetEvent(ev[i].handle);
  }
}
/* Does a wait on the handle look at the input of the console here? The standard input and the aliases of the console (CONIN$, CONOUT$) have no state of their own: they are ready when
 * the input is. The events above have one, which the kernel looks at. */
static BOOL con_wait_polled(HANDLE h) { return h && ((h == GetStdHandle(STD_INPUT_HANDLE) && con_std_is_console(h)) || con_table_alias_kind(h) != 0); }
IMPORT void NTAPI RtlTcosSetConsoleWait(BOOL (*handle)(HANDLE), BOOL (*ready)(void));
static BOOL con_wait_ready(void) { return con_input_ready(); }
static void con_register_waits(void) { RtlTcosSetConsoleWait(con_wait_polled, con_wait_ready); }
/* A handle is closed. */
static void con_screen_closed(HANDLE h);
static void con_handle_closed(HANDLE h) {
  con_unbound_forget(h);
  RtlTcosConsoleEventForget(h);
  con_screen_closed(h);
}
static void key_for(WCHAR c, WORD *vk, WORD *scan, DWORD *state) {
  static const char rows[] = "QWERTYUIOP\0ASDFGHJKL\0ZXCVBNM";
  static const BYTE starts[3] = {0x10, 0x1E, 0x2C};
  *vk = 0; *scan = 0; *state = 0;
  WCHAR u = (c >= 'a' && c <= 'z') ? (WCHAR)(c - 32) : c;
  if (c >= 'A' && c <= 'Z') *state = 0x10;
  if (u >= 'A' && u <= 'Z') {
    *vk = u;
    int row = 0, col = 0;
    for (const char *p = rows; *p || p[1]; ++p) { if (!*p) { ++row; col = 0; continue; } if (*p == (char)u) { *scan = (WORD)(starts[row] + col); break; } ++col; }
    return;
  }
  if (c >= '0' && c <= '9') { *vk = c; *scan = c == '0' ? 0x0B : (WORD)(0x02 + c - '1'); return; }
  static const struct { WCHAR plain, shifted; WORD vk, scan; } keys[] = {
    {' ', 0, 0x20, 0x39}, {'\r', 0, 0x0D, 0x1C}, {'\t', 0, 0x09, 0x0F}, {'\b', 0, 0x08, 0x0E}, {0x1B, 0, 0x1B, 0x01},
    {';', ':', 0xBA, 0x27}, {'=', '+', 0xBB, 0x0D}, {',', '<', 0xBC, 0x33}, {'-', '_', 0xBD, 0x0C}, {'.', '>', 0xBE, 0x34}, {'/', '?', 0xBF, 0x35},
    {'`', '~', 0xC0, 0x29}, {'[', '{', 0xDB, 0x1A}, {'\\', '|', 0xDC, 0x2B}, {']', '}', 0xDD, 0x1B}, {'\'', '"', 0xDE, 0x28}};
  for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
    if (keys[i].plain == c) { *vk = keys[i].vk; *scan = keys[i].scan; return; }
    if (keys[i].shifted && keys[i].shifted == c) { *vk = keys[i].vk; *scan = keys[i].scan; *state = 0x10; return; }
  }
  static const struct { WCHAR c; WORD scan; } shifted_digits[] = {{'!', 2}, {'@', 3}, {'#', 4}, {'$', 5}, {'%', 6}, {'^', 7}, {'&', 8}, {'*', 9}, {'(', 10}, {')', 11}};
  for (unsigned i = 0; i < sizeof(shifted_digits) / sizeof(shifted_digits[0]); ++i) if (shifted_digits[i].c == c) { *vk = (WORD)(i == 9 ? '0' : '1' + i); *scan = shifted_digits[i].scan; *state = 0x10; return; }
}
/* The character at g_in_pos (a UTF-16 unit, '\r' for the end of a line) and the bytes it takes; skips a '\r' that comes before '\n'. */
static WCHAR con_peek_char(DWORD *bytes) {
  for (;;) {
    const BYTE c = (BYTE)g_in_text[g_in_pos];
    DWORD n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (g_in_pos + n > g_in_len) n = g_in_len - g_in_pos;
    *bytes = n;
    if (c == '\r' && g_in_pos + 1 < g_in_len && g_in_text[g_in_pos + 1] == '\n') { ++g_in_pos; continue; }
    if (c == '\n') return '\r';
    WCHAR w[2] = {'?', 0};
    if (MultiByteToWideChar(CP_UTF8, 0, g_in_text + g_in_pos, (int)n, w, 2) < 1) w[0] = '?';
    return w[0];
  }
}
static void make_key_record(INPUT_RECORD_ *r, WCHAR c, BOOL down) {
  memset(r, 0, sizeof(*r));
  r->EventType = 1;      /* KEY_EVENT */
  r->Event.KeyEvent.bKeyDown = down;
  r->Event.KeyEvent.wRepeatCount = 1;
  r->Event.KeyEvent.uChar = c;
  key_for(c, &r->Event.KeyEvent.wVirtualKeyCode, &r->Event.KeyEvent.wVirtualScanCode, &r->Event.KeyEvent.dwControlKeyState);
}
/* The next event: from WriteConsoleInput, else made from the typed text (a key down, then up). The lock is held. */
static BOOL con_next_record(INPUT_RECORD_ *r, BOOL consume) {
  if (g_inject_count > 0) {
    memcpy(r, &g_injected[g_inject_head], sizeof(*r));      /* (the whole record, the padding too: a program compares records as bytes) */
    if (consume) { g_inject_head = (g_inject_head + 1) % CON_INJECT_MAX; --g_inject_count; }
    return TRUE;
  }
  if (g_in_pos >= g_in_len) return FALSE;
  DWORD bytes;
  const WCHAR c = con_peek_char(&bytes);
  make_key_record(r, c, !g_in_up);
  if (consume) { if (g_in_up) { g_in_pos += bytes; g_in_up = FALSE; } else g_in_up = TRUE; }
  return TRUE;
}
static DWORD con_count_records(void) {
  DWORD count = g_inject_count;
  DWORD saved_pos = g_in_pos;
  BOOL up = g_in_up;
  while (g_in_pos < g_in_len) { DWORD b; con_peek_char(&b); if (g_in_pos >= g_in_len) break; count += up ? 1 : 2; up = FALSE; g_in_pos += b; }
  g_in_pos = saved_pos;
  return count;
}

static BOOL con_read_events(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read, BOOL consume, BOOL block) {
  if (read) *read = 0;
  if (!con_is_input(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (!out && length) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  DWORD n = 0;
  for (;;) {
    con_lock();
    if (g_inject_count == 0 && g_in_pos >= g_in_len && !g_in_eof) {
      con_unlock();
      const int got = con_fill_input(block && n == 0);
      con_lock();
      (void)got;
    }
    /* (a peek walks the events as a read does and puts the queue back as it was) */
    const DWORD saved_head = g_inject_head, saved_count = g_inject_count;
    const DWORD saved_pos = g_in_pos;
    const BOOL saved_up = g_in_up;
    while (n < length && con_next_record(&out[n], TRUE)) ++n;
    if (!consume) { g_inject_head = saved_head; g_inject_count = saved_count; g_in_pos = saved_pos; g_in_up = saved_up; }
    const BOOL eof = g_in_eof && g_in_pos >= g_in_len && g_inject_count == 0;
    con_unlock();
    if (n > 0 || !block || eof) break;
  }
  if (read) *read = n;
  if (consume && n > 0) con_sync_signals();
  return TRUE;
}
EXPORT BOOL WINAPI ReadConsoleInputW(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read) { return con_read_events(h, out, length, read, TRUE, TRUE); }
EXPORT BOOL WINAPI ReadConsoleInputA(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read) { return con_read_events(h, out, length, read, TRUE, TRUE); }
/* flags: CONSOLE_READ_NOREMOVE 1 (the events stay), CONSOLE_READ_NOWAIT 2 (an empty queue is answered with none). */
EXPORT BOOL WINAPI ReadConsoleInputExW(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read, USHORT flags) { return con_read_events(h, out, length, read, !(flags & 1), !(flags & 2)); }
EXPORT BOOL WINAPI ReadConsoleInputExA(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read, USHORT flags) { return con_read_events(h, out, length, read, !(flags & 1), !(flags & 2)); }
EXPORT BOOL WINAPI PeekConsoleInputW(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read) { return con_read_events(h, out, length, read, FALSE, FALSE); }
EXPORT BOOL WINAPI PeekConsoleInputA(HANDLE h, INPUT_RECORD_ *out, DWORD length, LPDWORD read) { return con_read_events(h, out, length, read, FALSE, FALSE); }
EXPORT BOOL WINAPI GetNumberOfConsoleInputEvents(HANDLE h, LPDWORD count) {
  if (!con_is_input(h) || !count) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  con_fill_input(FALSE);
  con_lock();
  *count = con_count_records();
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI WriteConsoleInputW(HANDLE h, const INPUT_RECORD_ *in, DWORD length, LPDWORD written) {
  if (!written) { set_error(12); return FALSE; }
  if (!h && length && !in) { set_error(ERROR_NOACCESS); return FALSE; }
  if (!con_is_input(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (length && !in) { set_error(ERROR_NOACCESS); return FALSE; }
  DWORD n = 0;
  con_lock();
  /* a single mouse-move record written behind a queued mouse-move record replaces it */
  if (length == 1 && in[0].EventType == 2 && g_inject_count > 0 && in[0].Event.raw[12] == 1) {
    INPUT_RECORD_ *last = &g_injected[(g_inject_head + g_inject_count - 1) % CON_INJECT_MAX];
    if (last->EventType == 2 && last->Event.raw[12] == 1) { *last = in[0]; con_unlock(); if (written) *written = 1; return TRUE; }
  }
  for (; n < length && g_inject_count < CON_INJECT_MAX; ++n) memcpy(&g_injected[(g_inject_head + g_inject_count++) % CON_INJECT_MAX], &in[n], sizeof(in[n]));
  con_unlock();
  if (written) *written = n;
  con_sync_signals();
  return TRUE;
}
EXPORT BOOL WINAPI WriteConsoleInputA(HANDLE h, const INPUT_RECORD_ *in, DWORD length, LPDWORD written) { return WriteConsoleInputW(h, in, length, written); }
EXPORT BOOL WINAPI FlushConsoleInputBuffer(HANDLE h) {
  if (!con_is_input(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  con_lock();
  g_inject_count = 0; g_in_pos = g_in_len = 0; g_in_up = FALSE; g_in_eof = FALSE;      /* (the end of the input is found again by the next read) */
  con_unlock();
  con_sync_signals();
  return TRUE;
}

/* Reads text for ReadConsole: a line (to its "\n", written as "\r\n") when the input mode has ENABLE_LINE_INPUT, else what is there (an Enter is "\r"). */
static BOOL con_read_text(char *out, DWORD capacity, DWORD *got) {
  *got = 0;
  for (;;) {
    con_lock();
    if (g_in_pos < g_in_len) break;
    con_unlock();
    if (con_fill_input(TRUE) < 0) return TRUE;
  }
  const BOOL line = (g_con_in_mode & ENABLE_LINE_INPUT) != 0;
  DWORD n = 0;
  g_in_up = FALSE;
  while (g_in_pos < g_in_len) {
    const char c = g_in_text[g_in_pos];
    DWORD bytes = (BYTE)c >= 0xF0 ? 4 : (BYTE)c >= 0xE0 ? 3 : (BYTE)c >= 0xC0 ? 2 : 1;
    if (g_in_pos + bytes > g_in_len) bytes = g_in_len - g_in_pos;
    if (c == '\n') {
      if (line) { if (n + 2 > capacity) break; out[n++] = '\r'; out[n++] = '\n'; ++g_in_pos; break; }
      if (n + 1 > capacity) break;
      out[n++] = '\r'; ++g_in_pos;
      continue;
    }
    if (n + bytes > capacity) break;
    for (DWORD i = 0; i < bytes; ++i) out[n++] = g_in_text[g_in_pos + i];
    g_in_pos += bytes;
  }
  con_unlock();
  *got = n;
  return TRUE;
}
EXPORT BOOL WINAPI ReadConsoleA(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPVOID control) {
  (void)control;
  if (con_is_output_only(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (length > 0x7FFFFFFFu) { set_error(ERROR_NOACCESS); return FALSE; }
  if (read) *read = 0;
  if (!con_is_input(h)) {      /* (a redirected input is read as it is) */
    DWORD got = 0;
    if (!ReadFile(h, buffer, length, &got, NULL)) return FALSE;
    if (read) *read = got;
    return TRUE;
  }
  DWORD got;
  if (!con_read_text(buffer, length, &got)) return FALSE;
  if (read) *read = got;
  return TRUE;
}
EXPORT BOOL WINAPI ReadConsoleW(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, LPVOID control) {
  char utf8[1024];
  DWORD got = 0;
  if (length > 0x7FFFFFFFu) { set_error(ERROR_NOACCESS); return FALSE; }
  if (read) *read = 0;
  if (!ReadConsoleA(h, utf8, length < 340 ? length * 3 : 1020, &got, control)) return FALSE;
  const int units = got ? MultiByteToWideChar(CP_UTF8, 0, utf8, (int)got, buffer, (int)length) : 0;
  if (read) *read = units > 0 ? (DWORD)units : 0;
  return TRUE;
}
/* ReadFile on the console input takes what ReadConsoleInput or PeekConsoleInput already read from the terminal first. */
static BOOL console_read_hook(HANDLE h, LPVOID buffer, DWORD length, LPDWORD read, BOOL *handled) {
  *handled = FALSE;
  if (h != GetStdHandle(STD_INPUT_HANDLE) || g_in_pos >= g_in_len) return FALSE;
  *handled = TRUE;
  con_lock();
  DWORD n = 0;
  char *out = buffer;
  while (g_in_pos < g_in_len && n < length) { const char c = g_in_text[g_in_pos++]; out[n++] = c; if (c == '\n') break; }
  g_in_up = FALSE;
  con_unlock();
  if (read) *read = n;
  return TRUE;
}

/* ---- code pages of the console (shared by its processes) ------------------------------------------------------------------------------------------------- */

static UINT con_get_cp(int output) {
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return 0; }      /* (no console: no code page of it) */
  con_lock();
  const DWORD cp = g_host ? (output ? g_host->output_cp : g_host->input_cp) : g_detached_cp[output];
  con_unlock();
  return cp ? cp : CP_UTF8;
}
static BOOL con_set_cp(int output, UINT cp) {
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  if (cp == 0 || cp >= 0x10000u || !IsValidCodePage(cp)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  con_lock();
  DWORD *where = g_host ? (output ? &g_host->output_cp : &g_host->input_cp) : &g_detached_cp[output];
  *where = cp;
  con_unlock();
  return TRUE;
}
EXPORT UINT WINAPI GetConsoleCP(void) { return con_get_cp(0); }
EXPORT UINT WINAPI GetConsoleOutputCP(void) { return con_get_cp(1); }
EXPORT BOOL WINAPI SetConsoleCP(UINT cp) { return con_set_cp(0, cp); }
EXPORT BOOL WINAPI SetConsoleOutputCP(UINT cp) { return con_set_cp(1, cp); }

/* The code page the A functions of the console use for characters, or 0 for the default (the bytes are UTF-8 for text written, and the characters below 256 as they are for cells). */
static UINT con_a_cp(void) { return g_host && g_host->output_cp && g_host->output_cp != CP_UTF8 ? g_host->output_cp : 0; }

/* ---- modes, titles, control events --------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI GetConsoleMode(HANDLE h, LPDWORD mode) {
  if (!mode) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (con_is_input(h)) { *mode = g_con_in_mode; return TRUE; }
  WITH_BUFFER(h, sb, FALSE)
  *mode = sb->out_mode;
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleMode(HANDLE h, DWORD mode) {
  if (con_is_input(h)) {
    if (mode & ~0x3FFu) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
    g_con_in_mode = mode;
    return TRUE;
  }
  WITH_BUFFER(h, sb, FALSE)
  if (mode & ~0x1Fu) { con_unlock(); set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  sb->out_mode = mode;
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleCtrlHandler(PHANDLER_ROUTINE handler, BOOL add) {
  con_lock();
  BOOL ok = TRUE;
  if (!handler) {
    g_ctrl_ignored = add;
    parameters()->ConsoleFlags = (parameters()->ConsoleFlags & ~1u) | (add ? 1 : 0);
    if (g_host) con_host_ctrl_set(g_host, GetCurrentProcessId(), *(ULONG *)((BYTE *)parameters() + 0x408), add ? 1 : 0);
  }
  else if (add) { if (g_ctrl_handler_count < 32) g_ctrl_handlers[g_ctrl_handler_count++] = handler; else ok = FALSE; }
  else {
    ok = FALSE;
    for (int i = g_ctrl_handler_count - 1; i >= 0; --i) if (g_ctrl_handlers[i] == handler) {
      for (int j = i; j + 1 < g_ctrl_handler_count; ++j) g_ctrl_handlers[j] = g_ctrl_handlers[j + 1];
      --g_ctrl_handler_count; ok = TRUE; break;
    }
  }
  con_unlock();
  if (!ok) set_error(add ? ERROR_NOT_ENOUGH_MEMORY : ERROR_INVALID_PARAMETER);
  return ok;
}
EXPORT HANDLE WINAPI CreateRemoteThread(HANDLE process, LPSECURITY_ATTRIBUTES sa, SIZE_T stack, LPTHREAD_START_ROUTINE start, LPVOID parameter, DWORD flags, LPDWORD id);
/* The handlers of this process run for a control event, newest first; none that takes it ends the program. */
static void con_run_ctrl_handlers(DWORD event) {
  PHANDLER_ROUTINE handlers[32];
  con_lock();
  const int count = g_ctrl_handler_count;
  for (int i = 0; i < count; ++i) handlers[i] = g_ctrl_handlers[i];
  con_unlock();
  for (int i = count - 1; i >= 0; --i) if (handlers[i](event)) return;
  RtlExitUserProcess((NTSTATUS)0xC000013A);      /* STATUS_CONTROL_C_EXIT */
}
/* What a thread started in another process of the console runs to give it the event. */
static DWORD WINAPI con_ctrl_routine(LPVOID event) { con_run_ctrl_handlers((DWORD)(ULONG_PTR)event); return 0; }
/* Ctrl+C typed at the terminal ends the program (the kernel does it). A program can send an event to the processes of its console: group 0 is all of them, another number the
 * processes of that group; a process that ignores Ctrl+C is left out of a Ctrl+C. This process runs its handlers at once, the others in a thread made in them. */
EXPORT BOOL WINAPI GenerateConsoleCtrlEvent(DWORD event, DWORD group) {
  if (event > 1) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  DWORD targets[CON_MAX_MEMBERS], count = 0;
  const DWORD me = GetCurrentProcessId();
  BOOL local = FALSE;
  con_lock();
  for (DWORD i = 0; i < g_host->member_count; ++i) {
    const DWORD pid = g_host->members[i];
    DWORD member_group = 0, ignored = 0;
    for (int k = 0; k < CON_MAX_MEMBERS; ++k) if (g_host->ctrl[k].pid == pid) { member_group = g_host->ctrl[k].group; ignored = g_host->ctrl[k].ignored; break; }
    if ((group != 0 && member_group != group) || (event == 0 && ignored)) continue;
    if (pid == me) local = TRUE; else targets[count++] = pid;
  }
  con_unlock();
  for (DWORD i = 0; i < count; ++i) {
    HANDLE process = OpenProcess(0x1F0FFF /* PROCESS_ALL_ACCESS */, FALSE, targets[i]);
    if (!process) continue;      /* (it has ended) */
    HANDLE thread = CreateRemoteThread(process, NULL, 0, con_ctrl_routine, (LPVOID)(ULONG_PTR)event, 0, NULL);
    if (thread) CloseHandle(thread);
    CloseHandle(process);
  }
  if (local) con_run_ctrl_handlers(event);
  return TRUE;
}
EXPORT DWORD WINAPI GetConsoleTitleW(LPWSTR title, DWORD size) {
  if (size == 0) return 0;
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return 0; }
  con_lock();
  const DWORD n = (DWORD)wcslen(g_con_title);
  if (size > 0) { const DWORD k = n < size - 1 ? n : size - 1; memcpy(title, g_con_title, (size_t_)k * 2); title[k] = 0; }
  con_unlock();
  return n;
}
EXPORT DWORD WINAPI GetConsoleTitleA(LPSTR title, DWORD size) {
  WCHAR wide[260];
  if (size == 0) return 0;
  const DWORD n = GetConsoleTitleW(wide, 260);
  if (size > 0) { const DWORD k = n < size - 1 ? n : size - 1; for (DWORD i = 0; i < k; ++i) title[i] = (char)wide[i]; title[k] = 0; }
  return n;
}
EXPORT DWORD WINAPI GetConsoleOriginalTitleW(LPWSTR title, DWORD size) {
  if (size == 0) return 0;
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return 0; }
  con_lock();
  const WCHAR *original = g_host ? g_host->original_title : g_detached_title;
  const DWORD n = (DWORD)wcslen(original);
  const DWORD k = n < size - 1 ? n : size - 1;
  memcpy(title, original, (size_t_)k * 2);
  title[k] = 0;
  con_unlock();
  return n;
}
EXPORT DWORD WINAPI GetConsoleOriginalTitleA(LPSTR title, DWORD size) {
  WCHAR wide[260];
  if (size == 0) return 0;
  const DWORD n = GetConsoleOriginalTitleW(wide, 260);
  const DWORD k = n < size - 1 ? n : size - 1;
  for (DWORD i = 0; i < k; ++i) title[i] = (char)wide[i];
  title[k] = 0;
  return n;
}
EXPORT BOOL WINAPI SetConsoleTitleW(LPCWSTR title) {
  if (!title) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  con_lock();
  int k = 0;
  for (; title[k] && k < 259; ++k) g_con_title[k] = title[k];
  g_con_title[k] = 0;
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleTitleA(LPCSTR title) {
  if (!title) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WCHAR wide[260];
  const int n = MultiByteToWideChar(CP_UTF8, 0, title, -1, wide, 260);
  if (n <= 0) wide[0] = 0;
  wide[259] = 0;
  return SetConsoleTitleW(wide);
}
/* ---- console membership: AllocConsole, FreeConsole, AttachConsole, GetConsoleProcessList ------------------------------------------------------------------ */

/* Which console each process is in, in a section all of them can open (the numbers of the consoles are in the process parameters of the processes). */
#define CON_DIR_SLOTS 256
typedef struct { volatile LONG lock; struct { DWORD pid; ULONG_PTR id; } slot[CON_DIR_SLOTS]; } ConDirectory;
static ConDirectory *con_directory(void) {
  static ConDirectory *dir;
  if (dir) return dir;
  static const WCHAR name[] = {'T', 'c', 'o', 's', 'C', 'o', 'n', 's', 'o', 'l', 'e', 'D', 'i', 'r', 'e', 'c', 't', 'o', 'r', 'y', 0};
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, (DWORD)sizeof(ConDirectory), name);
  if (!mapping) return NULL;
  mapping = con_keep_handle(mapping);
  dir = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0);      /* (the handle stays open: the section lives as long as a handle to it does) */
  return dir;
}
static void con_directory_set(DWORD pid, ULONG_PTR id) {
  ConDirectory *d = con_directory();
  if (!d) return;
  while (InterlockedCompareExchange(&d->lock, 1, 0) != 0) NtYieldExecution();
  int free_slot = -1, found = -1;
  for (int i = 0; i < CON_DIR_SLOTS; ++i) {
    if (d->slot[i].pid == pid) found = i;
    else if (free_slot < 0 && d->slot[i].pid == 0) free_slot = i;
  }
  if (found < 0) found = free_slot;
  if (found >= 0) { d->slot[found].pid = id ? pid : 0; d->slot[found].id = id; }
  InterlockedExchange(&d->lock, 0);
}
static ULONG_PTR con_directory_find(DWORD pid) {
  ConDirectory *d = con_directory();
  ULONG_PTR id = 0;
  if (!d) return 0;
  while (InterlockedCompareExchange(&d->lock, 1, 0) != 0) NtYieldExecution();
  for (int i = 0; i < CON_DIR_SLOTS; ++i) if (d->slot[i].pid == pid) id = d->slot[i].id;
  InterlockedExchange(&d->lock, 0);
  return id;
}
static BOOL con_process_alive(DWORD pid) {
  if (pid == GetCurrentProcessId()) return TRUE;
  const DWORD saved = GetLastError();      /* (asking leaves the last error of the caller as it was) */
  HANDLE h = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, FALSE, pid);      /* (a process that has ended cannot be opened) */
  if (h) CloseHandle(h);
  set_error(saved);
  return h != NULL;
}
typedef struct { LONG_PTR ExitStatus; PVOID PebBaseAddress; ULONG_PTR AffinityMask; LONG_PTR BasePriority; ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId; } PROCESS_BASIC_INFORMATION_;
/* A child that shares the console of this process is one of its members from the start. */
static void con_note_child(DWORD pid, HANDLE console, DWORD group, DWORD ignored) {
  if (!console || !con_attached() || console != parameters()->ConsoleHandle) return;
  con_lock();
  con_host_add_member(g_host, pid);
  con_host_ctrl_set(g_host, pid, group, ignored);
  con_unlock();
}
/* A new console for this process: a number nobody has. */
static ULONG_PTR con_new_id(void) { return (ULONG_PTR)console_for_new_process(0x10, FALSE, NULL); }
/* The standard handles of a process that joins a console become the console's (those it was started with stay if it was started with STARTF_USESTDHANDLES and has them). */
static void con_bind_std_handles(void) {
  RTL_USER_PROCESS_PARAMETERS *pp = parameters();
  const BOOL keep = (pp->WindowFlags & 0x100) != 0;
  HANDLE *std[3] = {&pp->StandardInput, &pp->StandardOutput, &pp->StandardError};
  for (int i = 0; i < 3; ++i) {
    DWORD flags;
    if (keep && *std[i] && GetHandleInformation(*std[i], &flags)) continue;
    HANDLE terminal = con_open_terminal(i);
    if (terminal) { *std[i] = terminal; g_term[i] = terminal; }
    else *std[i] = g_term[i];
  }
}
static void con_reset_handlers(void) { con_lock(); g_ctrl_handler_count = 0; g_ctrl_ignored = FALSE; con_unlock(); }
static BOOL con_join(ULONG_PTR id) {
  parameters()->ConsoleHandle = (HANDLE)id;
  g_host_tried = FALSE;
  const BOOL joined = con_attached();
  if (joined) { con_directory_set(GetCurrentProcessId(), id); con_orphan_events(id); }
  return joined;
}
EXPORT BOOL WINAPI FreeConsole(void) {
  if (!con_attached()) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  con_lock();
  ConHost *h = g_host;
  const DWORD pid = GetCurrentProcessId();
  for (DWORD i = 0; i < h->member_count; ++i) if (h->members[i] == pid) { h->members[i] = h->members[--h->member_count]; break; }
  g_host = NULL;      /* (con_unlock still lets go of the lock of the console it holds) */
  g_active_screen = NULL;
  g_host_tried = TRUE;
  con_unlock();
  UnmapViewOfFile(h);
  if (g_host_mapping) CloseHandle(g_host_mapping);
  g_host_mapping = NULL;
  parameters()->ConsoleHandle = NULL;
  con_directory_set(pid, 0);
  /* the standard handles of the console go with it (those that are something else stay) */
  RTL_USER_PROCESS_PARAMETERS *pp = parameters();
  HANDLE *std[3] = {&pp->StandardInput, &pp->StandardOutput, &pp->StandardError};
  for (int i = 0; i < 3; ++i) {
    HANDLE h = *std[i];
    BOOL again = FALSE;
    for (int k = 0; k < i; ++k) if (*std[k] == h) again = TRUE;
    if (h && !again && device_type(h) == 0x50) CloseHandle(h);
  }
  return TRUE;
}
EXPORT BOOL WINAPI AllocConsole(void) {
  if (con_attached()) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  if (!con_join(con_new_id())) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  con_bind_std_handles();
  con_reset_handlers();
  return TRUE;
}
EXPORT BOOL WINAPI AttachConsole(DWORD pid) {
  if (con_attached()) { set_error(ERROR_ACCESS_DENIED); return FALSE; }
  if (pid == (DWORD)-1) {      /* ATTACH_PARENT_PROCESS */
    PROCESS_BASIC_INFORMATION_ info;
    ULONG length = 0;
    if (NtQueryInformationProcess((HANDLE)(LONG_PTR)-1, 0, &info, sizeof(info), &length) != 0 || !info.InheritedFromUniqueProcessId) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    pid = (DWORD)info.InheritedFromUniqueProcessId;
  }
  const ULONG_PTR id = con_directory_find(pid);
  if (!id) { set_error(pid == GetCurrentProcessId() ? ERROR_ACCESS_DENIED : ERROR_INVALID_HANDLE); return FALSE; }
  if (!con_join(id)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  con_bind_std_handles();
  con_reset_handlers();
  return TRUE;
}
/* The window of the console: a number made from the console's, the same in every process of it; none when the console was made without one (CREATE_NO_WINDOW) or there is no console.
 * user32.dll knows it as the one window that exists (IsWindow, GetClientRect). */
EXPORT HANDLE WINAPI GetConsoleWindow(void) {
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return NULL; }
  const ULONG_PTR id = (ULONG_PTR)parameters()->ConsoleHandle;
  if (id & CON_ID_NO_WINDOW) return NULL;
  return (HANDLE)(ULONG_PTR)(0x10000 + (((id >> 8) ^ id) & 0xFFFF) * 4);
}
/* (for user32.dll) The size of the window of the screen buffer that is shown, in cells. */
EXPORT BOOL WINAPI TcosConsoleWindowCells(LONG *columns, LONG *rows) {
  if (!con_attached()) return FALSE;
  con_lock();
  ScreenBuffer *sb = g_active_screen ? g_active_screen : con_main();
  const BOOL ok = sb != NULL;
  if (ok) { *columns = sb->window.Right - sb->window.Left + 1; *rows = sb->window.Bottom - sb->window.Top + 1; }
  con_unlock();
  return ok;
}
/* The processes of the console (those that have ended are dropped from the list); the list is written only if it is big enough. */
EXPORT DWORD WINAPI GetConsoleProcessList(LPDWORD list, DWORD count) {
  if (!list || !count) { set_error(ERROR_INVALID_PARAMETER); return 0; }
  if (!con_attached()) return 0;
  con_lock();
  ConHost *h = g_host;
  for (DWORD i = 0; i < h->member_count;) {
    if (con_process_alive(h->members[i])) ++i; else h->members[i] = h->members[--h->member_count];
  }
  const DWORD n = h->member_count;
  if (n <= count) for (DWORD i = 0; i < n; ++i) list[i] = h->members[i];
  con_unlock();
  return n;
}
EXPORT BOOL WINAPI GetNumberOfConsoleMouseButtons(LPDWORD n) { if (!n) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *n = 3; return TRUE; }
EXPORT BOOL WINAPI GetConsoleDisplayMode(LPDWORD mode) { if (!mode) { set_error(ERROR_INVALID_PARAMETER); return FALSE; } *mode = 0; return TRUE; }
EXPORT BOOL WINAPI SetConsoleDisplayMode(HANDLE h, DWORD flags, COORD *size) { (void)h; (void)flags; (void)size; set_error(ERROR_INVALID_PARAMETER); return FALSE; }
EXPORT BOOL WINAPI GetConsoleHistoryInfo(CONSOLE_HISTORY_INFO_ *info) {
  if (!info || info->cbSize != sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  info->HistoryBufferSize = con_history_ptr()[0]; info->NumberOfHistoryBuffers = con_history_ptr()[1]; info->dwFlags = con_history_ptr()[2];
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleHistoryInfo(CONSOLE_HISTORY_INFO_ *info) {
  if (!info || info->cbSize != sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  con_history_ptr()[0] = info->HistoryBufferSize; con_history_ptr()[1] = info->NumberOfHistoryBuffers; con_history_ptr()[2] = info->dwFlags;
  return TRUE;
}
EXPORT BOOL WINAPI GetConsoleSelectionInfo(LPVOID info) { if (info) memset(info, 0, 16); return TRUE; }

/* ---- screen buffers ---------------------------------------------------------------------------------------------------------------------------------- */

EXPORT BOOL WINAPI WriteConsoleA(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPVOID reserved) {
  (void)reserved;
  if (written) *written = 0;
  BOOL handled_ = FALSE;
  /* (the bytes go through the same path as WriteFile, which keeps a split UTF-8 character) */
  con_lock();
  const BOOL ours = con_buffer(h) != NULL;
  con_unlock();
  if (!ours) {
    const int type = device_type(h);
    if (type < 0 || (type != 0x50 && type != 0x11) /* a file or the null device is no console */) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
    return WriteFile(h, buffer, length, written, NULL);
  }
  WCHAR wide[1024];
  DWORD done = 0;
  const BYTE *b = buffer;
  while (done < length) {
    DWORD n = length - done < 1000 ? length - done : 1000;
    while (n > 1 && done + n < length && (b[done + n] & 0xC0) == 0x80) --n;      /* (not inside a character) */
    const UINT cp = con_a_cp();
    const int units = MultiByteToWideChar(cp ? cp : CP_UTF8, 0, (LPCSTR)b + done, (int)n, wide, 1024);
    if (units > 0 && !con_write_wide(h, wide, (DWORD)units)) return FALSE;
    done += n;
  }
  (void)handled_;
  if (written) *written = length;
  return TRUE;
}
EXPORT BOOL WINAPI WriteConsoleW(HANDLE h, LPCVOID buffer, DWORD length, LPDWORD written, LPVOID reserved) {
  (void)reserved;
  if (written) *written = 0;
  if (!con_write_wide(h, buffer, length)) return FALSE;
  if (written) *written = length;
  return TRUE;
}
/* The console window sits on the desktop (src/Windows/user32: the work area is 1024 x 728, a title bar is 23 high) and shows cells of the one font, 8 x 16: the window is never larger than
 * that, nor than the buffer. */
#define CON_FONT_W 8
#define CON_FONT_H 16
#define CON_WORK_W 1024
#define CON_WORK_H 728
#define CON_CAPTION_H 23
static COORD con_largest_window(void) { COORD c; c.X = CON_WORK_W / CON_FONT_W; c.Y = (CON_WORK_H - CON_CAPTION_H) / CON_FONT_H; return c; }
static COORD con_max_window(COORD size) { COORD c = con_largest_window(); if (size.X < c.X) c.X = size.X; if (size.Y < c.Y) c.Y = size.Y; return c; }
EXPORT BOOL WINAPI GetConsoleScreenBufferInfo(HANDLE h, PCONSOLE_SCREEN_BUFFER_INFO info) {
  if (!info) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WITH_BUFFER(h, sb, FALSE)
  info->dwSize = sb->size; info->dwCursorPosition = sb->cursor; info->wAttributes = sb->attr; info->srWindow = sb->window;
  info->dwMaximumWindowSize = con_max_window(sb->size);
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI GetConsoleScreenBufferInfoEx(HANDLE h, CONSOLE_SCREEN_BUFFER_INFOEX_ *info) {
  if (!info || info->cbSize != sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WITH_BUFFER(h, sb, FALSE)
  static const DWORD colors[16] = {0x000000, 0x800000, 0x008000, 0x808000, 0x000080, 0x800080, 0x008080, 0xC0C0C0, 0x808080, 0xFF0000, 0x00FF00, 0xFFFF00, 0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF};
  info->dwSize = sb->size; info->dwCursorPosition = sb->cursor; info->wAttributes = sb->attr; info->srWindow = sb->window;
  info->dwMaximumWindowSize = con_max_window(sb->size); info->wPopupAttributes = sb->popup ? sb->popup : 0xF5; info->bFullscreenSupported = FALSE;
  for (int i = 0; i < 16; ++i) info->ColorTable[i] = colors[i];
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleScreenBufferInfoEx(HANDLE h, CONSOLE_SCREEN_BUFFER_INFOEX_ *info) {
  if (!info || info->cbSize != sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WITH_BUFFER(h, sb, FALSE)
  BOOL ok = TRUE;
  if (info->dwSize.X != sb->size.X || info->dwSize.Y != sb->size.Y) ok = con_alloc_cells(sb, info->dwSize);
  if (ok) {
    sb->attr = info->wAttributes;
    sb->popup = info->wPopupAttributes;
    sb->window = info->srWindow;
    sb->cursor = info->dwCursorPosition;
    if (sb->cursor.X >= sb->size.X) sb->cursor.X = (SHORT)(sb->size.X - 1);
    if (sb->cursor.Y >= sb->size.Y) sb->cursor.Y = (SHORT)(sb->size.Y - 1);
  }
  con_unlock();
  if (!ok) set_error(ERROR_NOT_ENOUGH_MEMORY);
  return ok;
}
EXPORT BOOL WINAPI SetConsoleScreenBufferSize(HANDLE h, COORD size) {
  WITH_BUFFER(h, sb, FALSE)
  const BOOL ok = size.X > 0 && size.Y > 0 && size.X >= sb->window.Right - sb->window.Left + 1 && size.Y >= sb->window.Bottom - sb->window.Top + 1 && con_alloc_cells(sb, size);
  if (ok) {
    if (sb->cursor.X >= size.X) sb->cursor.X = (SHORT)(size.X - 1);
    if (sb->cursor.Y >= size.Y) sb->cursor.Y = (SHORT)(size.Y - 1);
    if (sb->window.Right >= size.X) { sb->window.Left = (SHORT)(sb->window.Left - (sb->window.Right - size.X + 1)); sb->window.Right = (SHORT)(size.X - 1); }
    if (sb->window.Bottom >= size.Y) { sb->window.Top = (SHORT)(sb->window.Top - (sb->window.Bottom - size.Y + 1)); sb->window.Bottom = (SHORT)(size.Y - 1); }
    if (sb->window.Left < 0) sb->window.Left = 0;
    if (sb->window.Top < 0) sb->window.Top = 0;
  }
  con_unlock();
  if (!ok) set_error(ERROR_INVALID_PARAMETER);
  return ok;
}
EXPORT BOOL WINAPI SetConsoleCursorPosition(HANDLE h, COORD pos) {
  WITH_BUFFER(h, sb, FALSE)
  const BOOL ok = pos.X >= 0 && pos.Y >= 0 && pos.X < sb->size.X && pos.Y < sb->size.Y;
  if (ok) { sb->cursor = pos; sb_follow(sb); }
  con_unlock();
  if (!ok) set_error(ERROR_INVALID_PARAMETER);
  return ok;
}
EXPORT BOOL WINAPI GetConsoleCursorInfo(HANDLE h, CONSOLE_CURSOR_INFO_ *info) {
  WITH_BUFFER(h, sb, FALSE)
  if (!info) { con_unlock(); set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  *info = sb->cursor_info;
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleCursorInfo(HANDLE h, const CONSOLE_CURSOR_INFO_ *info) {
  if (!info || info->size < 1 || info->size > 100) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WITH_BUFFER(h, sb, FALSE)
  sb->cursor_info = *info;
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleTextAttribute(HANDLE h, WORD attr) {
  WITH_BUFFER(h, sb, FALSE)
  sb->attr = attr;
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI SetConsoleWindowInfo(HANDLE h, BOOL absolute, const SMALL_RECT *rect) {
  if (!rect) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WITH_BUFFER(h, sb, FALSE)
  SMALL_RECT r = *rect;
  if (!absolute) { r.Left = (SHORT)(r.Left + sb->window.Left); r.Right = (SHORT)(r.Right + sb->window.Right); r.Top = (SHORT)(r.Top + sb->window.Top); r.Bottom = (SHORT)(r.Bottom + sb->window.Bottom); }
  const BOOL ok = r.Left >= 0 && r.Top >= 0 && r.Right < sb->size.X && r.Bottom < sb->size.Y && r.Left <= r.Right && r.Top <= r.Bottom;
  if (ok) sb->window = r;
  con_unlock();
  if (!ok) set_error(ERROR_INVALID_PARAMETER);
  return ok;
}
EXPORT COORD WINAPI GetLargestConsoleWindowSize(HANDLE h) {
  COORD c = {0, 0};
  con_lock();
  if (con_buffer(h)) c = con_largest_window(); else set_error(ERROR_INVALID_HANDLE);
  con_unlock();
  return c;
}
EXPORT BOOL WINAPI GetCurrentConsoleFont(HANDLE h, BOOL maximum, CONSOLE_FONT_INFO_ *info) {
  if (!info || !con_buffer_or_main(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  info->nFont = 0; info->dwFontSize.X = CON_FONT_W; info->dwFontSize.Y = CON_FONT_H;
  if (maximum) {      /* (the size of the largest window, which is what Windows answers here) */
    con_lock();
    ScreenBuffer *sb = con_buffer(h);
    info->dwFontSize = con_max_window(sb ? sb->size : con_largest_window());
    con_unlock();
  }
  return TRUE;
}
EXPORT DWORD WINAPI GetNumberOfConsoleFonts(void) { return 1; }
EXPORT COORD WINAPI GetConsoleFontSize(HANDLE h, DWORD font) {
  COORD c = {0, 0};
  if (!con_buffer_or_main(h)) { set_error(ERROR_INVALID_HANDLE); return c; }
  if (font >= 1) { set_error(ERROR_INVALID_PARAMETER); return c; }      /* (there is one font) */
  c.X = 8; c.Y = 16;
  return c;
}
EXPORT BOOL WINAPI GetCurrentConsoleFontEx(HANDLE h, BOOL maximum, CONSOLE_FONT_INFOEX_ *info) {
  if (!info || info->cbSize != sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!con_buffer_or_main(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  info->nFont = 0; info->dwFontSize.X = CON_FONT_W; info->dwFontSize.Y = CON_FONT_H; info->FontFamily = 0x36; info->FontWeight = 400;
  static const WCHAR name[] = {'T', 'e', 'r', 'm', 'i', 'n', 'a', 'l', 0};
  memcpy(info->FaceName, name, sizeof(name));
  if (maximum) {
    con_lock();
    ScreenBuffer *sb = con_buffer(h);
    info->dwFontSize = con_max_window(sb ? sb->size : con_largest_window());
    con_unlock();
  }
  return TRUE;
}
EXPORT BOOL WINAPI SetCurrentConsoleFontEx(HANDLE h, BOOL maximum, CONSOLE_FONT_INFOEX_ *info) {
  (void)maximum;
  if (!info || info->cbSize != sizeof(*info)) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  if (!con_buffer_or_main(h)) { set_error(ERROR_INVALID_HANDLE); return FALSE; }
  return TRUE;
}

EXPORT HANDLE WINAPI CreateConsoleScreenBuffer(DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD flags, LPVOID data) {
  (void)share; (void)sa; (void)data;
  if (flags != 1) { set_error(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE; }
  if (!con_attached()) { set_error(ERROR_INVALID_HANDLE); return INVALID_HANDLE_VALUE; }      /* (a screen buffer belongs to a console) */
  con_lock();
  ScreenBuffer *main = con_main();
  for (int i = 1; i < CON_MAX_BUFFERS; ++i) if ((*con_screen(i)).used && !con_buffer_alive((*con_screen(i)).handle)) { RtlFreeHeap(heap(), 0, (*con_screen(i)).cells); memset(con_screen(i), 0, sizeof((*con_screen(i)))); }
  ScreenBuffer *sb = NULL;
  for (int i = 1; i < CON_MAX_BUFFERS && !sb; ++i) if (!(*con_screen(i)).used) sb = con_screen(i);
  HANDLE handle = INVALID_HANDLE_VALUE;
  if (!main || !sb) set_error(ERROR_NOT_ENOUGH_MEMORY);
  else {
    handle = CreateEventW(NULL, TRUE, FALSE, NULL);      /* the buffer's handle: closing it is what the program does when it is done */
    for (int i = 1; handle && i < CON_MAX_BUFFERS; ++i) {      /* (an entry for a closed buffer whose handle value is used again is dropped) */
      if (!(*con_screen(i)).used || (*con_screen(i)).handle != handle) continue;
      if (g_active_screen == con_screen(i)) g_active_screen = con_screen(0);
      RtlFreeHeap(heap(), 0, (*con_screen(i)).cells);
      memset(con_screen(i), 0, sizeof((*con_screen(i))));
    }
    if (handle) {
      memset(sb, 0, sizeof(*sb));
      g_screens_made = 1;
      sb->used = TRUE; sb->handle = handle; sb->access = access; sb->attr = sb->popup = g_active_screen ? g_active_screen->attr : main->attr; sb->out_mode = main->out_mode;      /* (a new buffer takes the colours in use) */
      sb->cursor_info.size = 25; sb->cursor_info.visible = TRUE;
      if (!con_alloc_cells(sb, main->size)) { CloseHandle(handle); memset(sb, 0, sizeof(*sb)); handle = INVALID_HANDLE_VALUE; set_error(ERROR_NOT_ENOUGH_MEMORY); }
      else sb->window = main->window;
    } else handle = INVALID_HANDLE_VALUE;
  }
  con_unlock();
  if (handle != INVALID_HANDLE_VALUE) { RtlTcosConsoleEventNote(handle, (ULONG_PTR)parameters()->ConsoleHandle, 2); con_sync_signals(); }
  return handle;
}
EXPORT BOOL WINAPI SetConsoleActiveScreenBuffer(HANDLE h) {
  WITH_BUFFER(h, sb, FALSE)
  if (sb->handle && !(sb->access & (0x40000000u | 0x10000000u | 0x2))) { con_unlock(); set_error(ERROR_INVALID_HANDLE); return FALSE; }      /* (a buffer made for reading only cannot be shown) */
  if (sb != con_active()) sb->from = g_active_screen;
  g_active_screen = sb;
  con_unlock();
  return TRUE;
}

/* ---- reading and writing cells ----------------------------------------------------------------------------------------------------------------------------- */

static BOOL con_buffer_or_main(HANDLE h) { con_lock(); const BOOL ok = con_buffer(h) != NULL; con_unlock(); return ok; }

/* Cells in reading order from `at`, `count` of them to the end of the buffer at most. The lock is held; returns how many. */
static DWORD cells_from(ScreenBuffer *sb, COORD at, DWORD count) {
  if (at.X < 0 || at.Y < 0 || at.X >= sb->size.X || at.Y >= sb->size.Y) return 0;
  const long left = (long)sb->size.X * sb->size.Y - ((long)at.Y * sb->size.X + at.X);
  return count < (DWORD)left ? count : (DWORD)left;
}
static BOOL fill_cells(HANDLE h, COORD at, DWORD count, LPDWORD done, WCHAR ch, WORD attr, BOOL set_char) {
  if (done) *done = 0;
  WITH_BUFFER(h, sb, FALSE)
  const DWORD n = cells_from(sb, at, count);
  long i = (long)at.Y * sb->size.X + at.X;
  for (DWORD k = 0; k < n; ++k, ++i) { if (set_char) sb->cells[i].ch = ch; else sb->cells[i].attr = attr; }
  con_unlock();
  if (done) *done = n;
  if (n == 0 && count > 0) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  return TRUE;
}
EXPORT BOOL WINAPI FillConsoleOutputCharacterW(HANDLE h, WCHAR ch, DWORD count, COORD at, LPDWORD done) { return fill_cells(h, at, count, done, ch, 0, TRUE); }
EXPORT BOOL WINAPI FillConsoleOutputCharacterA(HANDLE h, CHAR ch, DWORD count, COORD at, LPDWORD done) { return fill_cells(h, at, count, done, (WCHAR)(BYTE)ch, 0, TRUE); }
EXPORT BOOL WINAPI FillConsoleOutputAttribute(HANDLE h, WORD attr, DWORD count, COORD at, LPDWORD done) { return fill_cells(h, at, count, done, 0, attr, FALSE); }
static BOOL read_cells(HANDLE h, COORD at, DWORD count, LPDWORD done, WCHAR *chars, WORD *attrs) {
  if (done) *done = 0;
  WITH_BUFFER(h, sb, FALSE)
  const DWORD n = cells_from(sb, at, count);
  long i = (long)at.Y * sb->size.X + at.X;
  for (DWORD k = 0; k < n; ++k, ++i) { if (chars) chars[k] = sb->cells[i].ch; else attrs[k] = sb->cells[i].attr; }
  con_unlock();
  if (done) *done = n;
  return TRUE;
}
EXPORT BOOL WINAPI ReadConsoleOutputCharacterW(HANDLE h, LPWSTR out, DWORD count, COORD at, LPDWORD done) { return read_cells(h, at, count, done, out, NULL); }
EXPORT BOOL WINAPI ReadConsoleOutputCharacterA(HANDLE h, LPSTR out, DWORD count, COORD at, LPDWORD done) {
  if (done) *done = 0;
  WCHAR *w = RtlAllocateHeap(heap(), 0, ((SIZE_T)count + 1) * 2);
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  DWORD n = 0;
  const BOOL ok = read_cells(h, at, count, &n, w, NULL);
  if (ok) {
    const UINT cp = con_a_cp();
    for (DWORD i = 0; i < n; ++i) {
      if (cp) { char c = '?'; BOOL lossy = FALSE; if (WideCharToMultiByte(cp, 0, &w[i], 1, &c, 1, NULL, &lossy) != 1) c = '?'; out[i] = c; }
      else out[i] = w[i] < 0x100 ? (char)w[i] : '?';
    }
    if (done) *done = n;
  }
  RtlFreeHeap(heap(), 0, w);
  return ok;
}
EXPORT BOOL WINAPI ReadConsoleOutputAttribute(HANDLE h, LPWORD out, DWORD count, COORD at, LPDWORD done) { return read_cells(h, at, count, done, NULL, out); }
static BOOL write_cells(HANDLE h, COORD at, DWORD count, LPDWORD done, const WCHAR *chars, const WORD *attrs) {
  if (done) *done = 0;
  WITH_BUFFER(h, sb, FALSE)
  const DWORD n = cells_from(sb, at, count);
  long i = (long)at.Y * sb->size.X + at.X;
  for (DWORD k = 0; k < n; ++k, ++i) { if (chars) sb->cells[i].ch = chars[k]; else sb->cells[i].attr = attrs[k]; }
  con_unlock();
  if (done) *done = n;
  return TRUE;
}
EXPORT BOOL WINAPI WriteConsoleOutputCharacterW(HANDLE h, LPCWSTR chars, DWORD count, COORD at, LPDWORD done) { return write_cells(h, at, count, done, chars, NULL); }
EXPORT BOOL WINAPI WriteConsoleOutputCharacterA(HANDLE h, LPCSTR chars, DWORD count, COORD at, LPDWORD done) {
  WCHAR *w = RtlAllocateHeap(heap(), 0, ((SIZE_T)count + 1) * 2);
  if (!w) { set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  const UINT cp = con_a_cp();
  for (DWORD i = 0; i < count; ++i) { if (cp) { WCHAR c = (BYTE)chars[i]; if (MultiByteToWideChar(cp, 0, &chars[i], 1, &c, 1) != 1) c = (BYTE)chars[i]; w[i] = c; } else w[i] = (BYTE)chars[i]; }
  const BOOL ok = write_cells(h, at, count, done, w, NULL);
  RtlFreeHeap(heap(), 0, w);
  return ok;
}
EXPORT BOOL WINAPI WriteConsoleOutputAttribute(HANDLE h, const WORD *attrs, DWORD count, COORD at, LPDWORD done) { return write_cells(h, at, count, done, NULL, attrs); }

/* A rectangle of a CHAR_INFO array of `bufsize` (the program's) and of the screen buffer; ascii: the characters are bytes. */
static BOOL block_io(HANDLE h, CHAR_INFO_ *data, COORD bufsize, COORD bufcoord, SMALL_RECT *region, BOOL write, BOOL ascii) {
  if (!region || !data) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  /* the rectangle is first cut to what the program's array holds (that much is done before the console is asked, so the rectangle comes back cut even when the handle is no good);
   * one that is empty after that is an error (a write says the parameter is wrong, a read says the function is) */
  int width = region->Right - region->Left + 1, height = region->Bottom - region->Top + 1;
  if (bufsize.X - bufcoord.X < width) width = bufsize.X - bufcoord.X;
  if (bufsize.Y - bufcoord.Y < height) height = bufsize.Y - bufcoord.Y;
  if (width <= 0 || height <= 0) {
    if (!write) { region->Right = (SHORT)(region->Left + width - 1); region->Bottom = (SHORT)(region->Top + height - 1); }
    set_error(write ? ERROR_INVALID_PARAMETER : ERROR_INVALID_FUNCTION);
    return FALSE;
  }
  region->Right = (SHORT)(region->Left + width - 1); region->Bottom = (SHORT)(region->Top + height - 1);
  WITH_BUFFER(h, sb, FALSE)
  SMALL_RECT r = *region;
  if (bufcoord.X < 0 || bufcoord.Y < 0) { con_unlock(); set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  /* cut to the screen buffer */
  if (r.Left < 0) { bufcoord.X = (SHORT)(bufcoord.X - r.Left); r.Left = 0; }
  if (r.Top < 0) { bufcoord.Y = (SHORT)(bufcoord.Y - r.Top); r.Top = 0; }
  if (r.Right >= sb->size.X) r.Right = (SHORT)(sb->size.X - 1);
  if (r.Bottom >= sb->size.Y) r.Bottom = (SHORT)(sb->size.Y - 1);
  const int w = r.Right - r.Left + 1, hh = r.Bottom - r.Top + 1;
  if (w <= 0 || hh <= 0 || bufcoord.X >= bufsize.X || bufcoord.Y >= bufsize.Y) {      /* a rectangle outside the screen buffer: a write has nothing to do (the rectangle stays), a read has no cells to give */
    con_unlock();
    if (!write) set_error(ERROR_INVALID_PARAMETER);
    return write;
  }
  for (int y = 0; y < hh; ++y)
    for (int x = 0; x < w; ++x) {
      CHAR_INFO_ *cell = &sb->cells[(long)(r.Top + y) * sb->size.X + r.Left + x];
      CHAR_INFO_ *mine = &data[(long)(bufcoord.Y + y) * bufsize.X + bufcoord.X + x];
      if (write) { cell->ch = ascii ? (WCHAR)(BYTE)mine->ch : mine->ch; cell->attr = mine->attr; }
      else { mine->ch = ascii ? (WCHAR)(cell->ch < 0x100 ? cell->ch : '?') : cell->ch; mine->attr = cell->attr; }
    }
  region->Left = r.Left; region->Top = r.Top; region->Right = (SHORT)(r.Left + w - 1); region->Bottom = (SHORT)(r.Top + hh - 1);
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI ReadConsoleOutputW(HANDLE h, CHAR_INFO_ *data, COORD size, COORD at, SMALL_RECT *region) { return block_io(h, data, size, at, region, FALSE, FALSE); }
EXPORT BOOL WINAPI ReadConsoleOutputA(HANDLE h, CHAR_INFO_ *data, COORD size, COORD at, SMALL_RECT *region) { return block_io(h, data, size, at, region, FALSE, TRUE); }
EXPORT BOOL WINAPI WriteConsoleOutputW(HANDLE h, const CHAR_INFO_ *data, COORD size, COORD at, SMALL_RECT *region) { return block_io(h, (CHAR_INFO_ *)data, size, at, region, TRUE, FALSE); }
EXPORT BOOL WINAPI WriteConsoleOutputA(HANDLE h, const CHAR_INFO_ *data, COORD size, COORD at, SMALL_RECT *region) { return block_io(h, (CHAR_INFO_ *)data, size, at, region, TRUE, TRUE); }

static BOOL scroll_block(HANDLE h, const SMALL_RECT *scroll, const SMALL_RECT *clip, COORD dest, const CHAR_INFO_ *fill, BOOL ascii) {
  if (!scroll || !fill) { set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  WITH_BUFFER(h, sb, FALSE)
  SMALL_RECT src = *scroll, bounds = {0, 0, (SHORT)(sb->size.X - 1), (SHORT)(sb->size.Y - 1)};
  if (clip) { if (clip->Left > bounds.Left) bounds.Left = clip->Left; if (clip->Top > bounds.Top) bounds.Top = clip->Top; if (clip->Right < bounds.Right) bounds.Right = clip->Right; if (clip->Bottom < bounds.Bottom) bounds.Bottom = clip->Bottom; }
  if (src.Left < 0) src.Left = 0;
  if (src.Top < 0) src.Top = 0;
  if (src.Right >= sb->size.X) src.Right = (SHORT)(sb->size.X - 1);
  if (src.Bottom >= sb->size.Y) src.Bottom = (SHORT)(sb->size.Y - 1);
  const int w = src.Right - src.Left + 1, hh = src.Bottom - src.Top + 1;
  if (w <= 0 || hh <= 0) { con_unlock(); set_error(ERROR_INVALID_PARAMETER); return FALSE; }
  CHAR_INFO_ *copy = RtlAllocateHeap(heap(), 0, (SIZE_T)w * hh * sizeof(CHAR_INFO_));
  if (!copy) { con_unlock(); set_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
  for (int y = 0; y < hh; ++y) for (int x = 0; x < w; ++x) copy[(long)y * w + x] = sb->cells[(long)(src.Top + y) * sb->size.X + src.Left + x];
  const CHAR_INFO_ blank = {ascii ? (WCHAR)(BYTE)fill->ch : fill->ch, fill->attr};
  for (int y = src.Top; y <= src.Bottom; ++y)      /* what is left of the source is blanked, then the copy is put down */
    for (int x = src.Left; x <= src.Right; ++x) if (x >= bounds.Left && x <= bounds.Right && y >= bounds.Top && y <= bounds.Bottom) sb->cells[(long)y * sb->size.X + x] = blank;
  for (int y = 0; y < hh; ++y)
    for (int x = 0; x < w; ++x) {
      const int dx = dest.X + x, dy = dest.Y + y;
      if (dx >= bounds.Left && dx <= bounds.Right && dy >= bounds.Top && dy <= bounds.Bottom) sb->cells[(long)dy * sb->size.X + dx] = copy[(long)y * w + x];
    }
  RtlFreeHeap(heap(), 0, copy);
  con_unlock();
  return TRUE;
}
EXPORT BOOL WINAPI ScrollConsoleScreenBufferW(HANDLE h, const SMALL_RECT *scroll, const SMALL_RECT *clip, COORD dest, const CHAR_INFO_ *fill) { return scroll_block(h, scroll, clip, dest, fill, FALSE); }
EXPORT BOOL WINAPI ScrollConsoleScreenBufferA(HANDLE h, const SMALL_RECT *scroll, const SMALL_RECT *clip, COORD dest, const CHAR_INFO_ *fill) { return scroll_block(h, scroll, clip, dest, fill, TRUE); }

/* Windows 10 no longer has these: they fail with E_NOTIMPL (the low word of it is the error). */
EXPORT BOOL WINAPI GetConsoleFontInfo(HANDLE h, BOOL maximum, DWORD count, LPVOID info) { (void)h; (void)maximum; (void)count; (void)info; set_error(0x4001); return FALSE; }
EXPORT BOOL WINAPI SetConsoleFont(HANDLE h, DWORD font) { (void)h; (void)font; set_error(0x4001); return FALSE; }

/* DuplicateConsoleHandle / CloseConsoleHandle: the handles of a console are handles; these are the old names for duplicating and closing them. */
EXPORT HANDLE WINAPI DuplicateConsoleHandle(HANDLE h, DWORD access, BOOL inherit, DWORD options) {
  HANDLE copy = NULL;
  if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, access, inherit, options)) return INVALID_HANDLE_VALUE;
  return copy;
}
EXPORT BOOL WINAPI CloseConsoleHandle(HANDLE h) { return CloseHandle(h); }
/* The handle to wait on for the console's input: the standard input handle. */
EXPORT HANDLE WINAPI GetConsoleInputWaitHandle(void) { return GetStdHandle(STD_INPUT_HANDLE); }
