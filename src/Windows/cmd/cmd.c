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

/* cmd.exe for TheoryCraftOS (C:\Windows\System32\cmd.exe, what %ComSpec% names): runs batch files and command lines.
 *   cmd /c <command>   runs the command (a batch file, a program, a built-in command) and ends with its exit code;
 *   cmd /k <command>   runs it, then reads commands from the standard input;
 *   cmd                reads commands from the standard input until EXIT or its end.
 * /q starts with ECHO OFF; /d /s /a /u /t: /e: /f: /v: are accepted (/s only changes how quotes are read, as in cmd). The interpreter is batch.inc
 * (what it does and does not do is said there); this file gives it the Windows it works on: files, folders and the environment through kernel32,
 * programs through CreateProcessW, the console through the standard handles. The desktop's Start menu starts a shortcut to a batch file with
 * "cmd.exe /c <file>" (src/Desktop/InstalledApps.cpp). Built freestanding over kernel32.dll and ntdll.dll. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

__asm__(".text\n.globl ___chkstk_ms\n___chkstk_ms:\n ret\n");

#include "batch.inc"

#define CMD_MAX_SCRIPT (16u << 20)      /* the largest batch file read */
#define CMD_STREAMS 16                  /* files open for redirection at one time */

static HANDLE g_heap;
static HANDLE g_streams[CMD_STREAMS];   /* stream 3 + i */

static void *host_alloc(void *context, unsigned long bytes) { (void)context; return HeapAlloc(g_heap, HEAP_ZERO_MEMORY, bytes); }
static void host_release(void *context, void *block) { (void)context; if (block) HeapFree(g_heap, 0, block); }

static int host_get_variable(void *context, const WCHAR *name, WCHAR *out, int capacity) {
  (void)context;
  SetLastError(0);
  const DWORD n = GetEnvironmentVariableW(name, out, (DWORD)capacity);
  if (n == 0) { out[0] = 0; return GetLastError() == ERROR_ENVVAR_NOT_FOUND ? -1 : 0; }
  if (n >= (DWORD)capacity) return -1;      /* (too long: the size it needs) */
  return (int)n;
}
static int host_set_variable(void *context, const WCHAR *name, const WCHAR *value) { (void)context; return SetEnvironmentVariableW(name, value) != 0; }
static int host_current_directory(void *context, WCHAR *out, int capacity) {
  (void)context;
  const DWORD n = GetCurrentDirectoryW((DWORD)capacity, out);
  return n < (DWORD)capacity ? (int)n : 0;
}
static int host_change_directory(void *context, const WCHAR *path) { (void)context; return SetCurrentDirectoryW(path) != 0; }
static int host_full_path(void *context, const WCHAR *path, WCHAR *out, int capacity) {
  (void)context;
  const DWORD n = GetFullPathNameW(path, (DWORD)capacity, out, NULL);
  return n < (DWORD)capacity ? (int)n : 0;
}
static int host_file_kind(void *context, const WCHAR *path) {
  (void)context;
  const DWORD a = GetFileAttributesW(path);
  if (a == INVALID_FILE_ATTRIBUTES) return BATCH_KIND_NONE;
  return (a & FILE_ATTRIBUTE_DIRECTORY) ? BATCH_KIND_FOLDER : BATCH_KIND_FILE;
}

static HANDLE stream_handle(int stream) {
  if (stream == BATCH_IN) return GetStdHandle(STD_INPUT_HANDLE);
  if (stream == BATCH_OUT) return GetStdHandle(STD_OUTPUT_HANDLE);
  if (stream == BATCH_ERR) return GetStdHandle(STD_ERROR_HANDLE);
  if (stream >= 3 && stream < 3 + CMD_STREAMS) return g_streams[stream - 3];
  return INVALID_HANDLE_VALUE;
}

static void host_write(void *context, int stream, const WCHAR *text, int length) {
  (void)context;
  const HANDLE h = stream_handle(stream);
  if (h == INVALID_HANDLE_VALUE || h == NULL || length <= 0) return;
  DWORD mode, written;
  if (GetConsoleMode(h, &mode)) { WriteConsoleW(h, text, (DWORD)length, &written, NULL); return; }
  char bytes[1024];      /* (in pieces, in the console's code page, as cmd writes to a file or a pipe) */
  const UINT code_page = GetConsoleOutputCP() ? GetConsoleOutputCP() : CP_OEMCP;
  while (length > 0) {
    const int piece = length > 256 ? 256 : length;
    const int n = WideCharToMultiByte(code_page, 0, text, piece, bytes, (int)sizeof(bytes), NULL, NULL);
    if (n > 0) WriteFile(h, bytes, (DWORD)n, &written, NULL);
    text += piece;
    length -= piece;
  }
}

static int host_open_stream(void *context, const WCHAR *path, int mode) {
  (void)context;
  int slot = 0;
  while (slot < CMD_STREAMS && g_streams[slot]) ++slot;
  if (slot == CMD_STREAMS) return -1;
  SECURITY_ATTRIBUTES inherit;
  inherit.nLength = sizeof(inherit);
  inherit.lpSecurityDescriptor = NULL;
  inherit.bInheritHandle = TRUE;      /* (a program it is given to inherits it) */
  HANDLE h;
  if (mode == BATCH_READ) h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  else h = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, &inherit, mode == BATCH_APPEND ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return -1;
  if (mode == BATCH_APPEND) SetFilePointer(h, 0, NULL, FILE_END);
  g_streams[slot] = h;
  return 3 + slot;
}
static void host_close_stream(void *context, int stream) {
  (void)context;
  if (stream < 3 || stream >= 3 + CMD_STREAMS || !g_streams[stream - 3]) return;
  CloseHandle(g_streams[stream - 3]);
  g_streams[stream - 3] = NULL;
}

/* A file's bytes (at most CMD_MAX_SCRIPT) in a block of the heap. */
static char *read_file_bytes(const WCHAR *path, DWORD *size) {
  const HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;
  LARGE_INTEGER length;
  char *bytes = NULL;
  if (GetFileSizeEx(h, &length) && length.QuadPart >= 0 && length.QuadPart <= CMD_MAX_SCRIPT) {
    bytes = (char *)HeapAlloc(g_heap, 0, (SIZE_T)length.QuadPart + 1);
    DWORD got = 0, at = 0;
    while (bytes && at < (DWORD)length.QuadPart && ReadFile(h, bytes + at, (DWORD)length.QuadPart - at, &got, NULL) && got > 0) at += got;
    if (bytes) *size = at;
  }
  CloseHandle(h);
  return bytes;
}

/* Text of a file as UTF-16: UTF-8 (with or without its byte order mark) when it is that, else the OEM code page, as cmd reads a batch file. */
static WCHAR *decode_text(const char *bytes, DWORD size, int *length) {
  UINT code_page = CP_UTF8;
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (size >= 3 && (unsigned char)bytes[0] == 0xEF && (unsigned char)bytes[1] == 0xBB && (unsigned char)bytes[2] == 0xBF) { bytes += 3; size -= 3; }
  int n = size == 0 ? 0 : MultiByteToWideChar(code_page, flags, bytes, (int)size, NULL, 0);
  if (size > 0 && n <= 0) { code_page = CP_OEMCP; flags = 0; n = MultiByteToWideChar(code_page, flags, bytes, (int)size, NULL, 0); }
  if (size > 0 && n <= 0) return NULL;
  WCHAR *text = (WCHAR *)HeapAlloc(g_heap, HEAP_ZERO_MEMORY, ((SIZE_T)n + 1) * sizeof(WCHAR));
  if (!text) return NULL;
  if (n > 0) MultiByteToWideChar(code_page, flags, bytes, (int)size, text, n);
  *length = n;
  return text;
}

static int host_load_script(void *context, const WCHAR *path, WCHAR **text, int *length) {
  (void)context;
  DWORD size = 0;
  char *bytes = read_file_bytes(path, &size);
  if (!bytes) return 0;
  *text = decode_text(bytes, size, length);
  HeapFree(g_heap, 0, bytes);
  return *text != NULL;
}

static int host_type_file(void *context, const WCHAR *path, int stream) {
  int length = 0;
  WCHAR *text = NULL;
  if (!host_load_script(context, path, &text, &length)) return 0;
  host_write(context, stream, text, length);
  HeapFree(g_heap, 0, text);
  return 1;
}

static int host_read_line(void *context, WCHAR *out, int capacity) {
  (void)context;
  const HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
  char bytes[BATCH_LINE + 1];
  int n = 0;
  DWORD got = 0;
  char c;
  int any = 0;
  while (ReadFile(h, &c, 1, &got, NULL) && got == 1) {      /* (a byte at a time: what follows the line is the next command's or a program's) */
    any = 1;
    if (c == '\n') break;
    if (c != '\r' && n < BATCH_LINE) bytes[n++] = c;
  }
  if (!any) return -1;
  int w = n == 0 ? 0 : MultiByteToWideChar(GetConsoleCP() ? GetConsoleCP() : CP_OEMCP, 0, bytes, n, out, capacity - 1);
  if (w < 0) w = 0;
  out[w] = 0;
  return w;
}

static int host_run(void *context, const BatchRun *run, unsigned *exit_code) {
  (void)context;
  const int length = bw_len(run->command_line);
  WCHAR *line = (WCHAR *)HeapAlloc(g_heap, 0, ((SIZE_T)length + 1) * sizeof(WCHAR));      /* (CreateProcessW may write into it) */
  if (!line) return 0;
  for (int i = 0; i <= length; ++i) line[i] = run->command_line[i];
  STARTUPINFOW startup;
  PROCESS_INFORMATION process;
  BYTE *p = (BYTE *)&startup;
  for (unsigned i = 0; i < sizeof(startup); ++i) p[i] = 0;
  startup.cb = sizeof(startup);
  HANDLE nul = INVALID_HANDLE_VALUE;
  BOOL inherit = FALSE;
  if (run->input != BATCH_IN || run->output != BATCH_OUT || run->error != BATCH_ERR) {      /* redirected: the child gets these three */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (run->input == BATCH_NUL || run->output == BATCH_NUL || run->error == BATCH_NUL)
      nul = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = run->input == BATCH_NUL ? nul : stream_handle(run->input);
    startup.hStdOutput = run->output == BATCH_NUL ? nul : stream_handle(run->output);
    startup.hStdError = run->error == BATCH_NUL ? nul : stream_handle(run->error);
    inherit = TRUE;
  }
  const BOOL started = CreateProcessW(run->program, line, NULL, NULL, inherit, 0, NULL, run->directory, &startup, &process);
  HeapFree(g_heap, 0, line);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!started) return 0;
  *exit_code = 0;
  if (run->wait) {
    DWORD code = 0;
    WaitForSingleObject(process.hProcess, INFINITE);
    if (GetExitCodeProcess(process.hProcess, &code)) *exit_code = code;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return 1;
}

/* What follows /c or /k, as cmd takes it: without /s, a line that begins with a quote, has exactly two, no special characters between them, a blank
   between them, and names a file between them keeps its quotes; otherwise the first quote and the last one are taken away. */
static BView command_text(BView rest, int s_option) {
  rest = bv_skip_blanks(rest);
  if (rest.n == 0 || rest.p[0] != '"') return rest;
  int quotes = 0, last = -1, special = 0, blank = 0;
  for (int i = 0; i < rest.n; ++i) {
    if (rest.p[i] == '"') { ++quotes; last = i; continue; }
    if (quotes == 1) {
      const WCHAR c = rest.p[i];
      special |= c == '&' || c == '<' || c == '>' || c == '(' || c == ')' || c == '@' || c == '^' || c == '|';
      blank |= b_blank(c);
    }
  }
  if (!s_option && quotes == 2 && !special && blank) {
    WCHAR name[BATCH_PATH];
    int end = 1;
    while (end < rest.n && rest.p[end] != '"') ++end;
    if (bv_copy(bv(rest.p + 1, end - 1), name, BATCH_PATH)) {
      const DWORD a = GetFileAttributesW(name);
      if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY)) return rest;
      WCHAR found[BATCH_PATH];      /* (a program named without its extension or folder counts too) */
      if (SearchPathW(NULL, name, L".exe", BATCH_PATH, found, NULL)) return rest;
    }
  }
  /* the first quote and the last one go */
  static WCHAR stripped[BATCH_LINE + 1];
  BText t;
  bt_init(&t, stripped, BATCH_LINE + 1);
  for (int i = 1; i < rest.n; ++i) if (i != last) bt_put(&t, rest.p[i]);
  return bv(t.data, t.length);
}

static BatchHost g_host = {
  NULL, host_alloc, host_release, host_get_variable, host_set_variable, host_current_directory, host_change_directory, host_full_path,
  host_file_kind, host_run, host_write, host_open_stream, host_close_stream, host_load_script, host_read_line, host_type_file,
};
static BatchShell g_shell;

void __stdcall cmd_main(void) {
  g_heap = GetProcessHeap();
  batch_shell_init(&g_shell, &g_host);
  const WCHAR *line = GetCommandLineW();
  BView rest = bv_str(line ? line : L"");
  /* the program's own name: in quotes, or up to a blank */
  rest = bv_skip_blanks(rest);
  if (rest.n > 0 && rest.p[0] == '"') {
    int end = 1;
    while (end < rest.n && rest.p[end] != '"') ++end;
    rest = bv(rest.p + end + (end < rest.n), rest.n - end - (end < rest.n));
  } else {
    int end = 0;
    while (end < rest.n && !b_blank(rest.p[end]) && rest.p[end] != '/') ++end;
    rest = bv(rest.p + end, rest.n - end);
  }
  int run_once = 0, keep = 0, s_option = 0;
  for (;;) {
    rest = bv_skip_blanks(rest);
    if (rest.n < 2 || rest.p[0] != '/') break;
    const WCHAR option = b_lower(rest.p[1]);
    if (option == 'c' || option == 'k') { run_once = option == 'c'; keep = option == 'k'; rest = bv(rest.p + 2, rest.n - 2); break; }
    if (option == 'q') g_shell.echo = 0;
    if (option == 's') s_option = 1;
    int end = 2;      /* (/d /a /u /t:fg /e:on /f:off /v:on: the option runs to a blank or the next /) */
    while (end < rest.n && !b_blank(rest.p[end]) && rest.p[end] != '/') ++end;
    rest = bv(rest.p + end, rest.n - end);
  }
  unsigned code = 0;
  if (run_once || keep) {
    const BView command = command_text(rest, s_option);
    code = batch_run_command(&g_shell, command);
  }
  if (!run_once && !g_shell.exit_all) code = batch_interactive(&g_shell);
  for (int i = 0; i < CMD_STREAMS; ++i) if (g_streams[i]) CloseHandle(g_streams[i]);
  ExitProcess(code);
}
