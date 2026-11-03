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

/* advapi32.dll: Event Tracing for Windows - providers (EventRegister, EventWrite), trace sessions (StartTrace, EnableTraceEx2, ControlTrace ...) and consumers (OpenTrace,
 * ProcessTrace). There is no kernel logger: everything is done by the processes that take part, through the registry and files.
 *
 *   a session     is a key HKLM\SYSTEM\CurrentControlSet\Control\TcosEtw\Sessions\<name> (its properties and the providers enabled in it, with level and keywords), and a log file
 *                 (the LogFileName, or <name>.etl in C:\Windows\System32\winevt\Logs for a real-time session). Every change bumps the value "Generation" of TcosEtw.
 *   a provider    that registers looks at the sessions, and a thread of the process watches the generation: when a session enables or disables the provider, its callback is called.
 *   EventWrite    appends the event to the file of every session that has enabled the provider for this level and these keywords (a named mutex orders the writers).
 *   ProcessTrace  reads the file from the start and calls the callbacks; for a real-time session it keeps waiting for more until the session is stopped or CloseTrace is called.
 * The file is this OS's own: a header, then records of an EVENT_HEADER followed by the user data. */
#include "security.h"
#include <evntrace.h>
#include <evntprov.h>
#include <evntcons.h>
#include <wmistr.h>
#include <stdarg.h>

#define ETW_KEY L"SYSTEM\\CurrentControlSet\\Control\\TcosEtw"
#define SESSIONS_KEY L"SYSTEM\\CurrentControlSet\\Control\\TcosEtw\\Sessions"
#define LOG_DIR L"C:\\Windows\\System32\\winevt\\Logs"
#define FILE_MAGIC 0x46455443u      /* "CTEF" */
#define MAX_PROVIDERS 64
#define MAX_SESSIONS 64
#define SESSION_HANDLE_BASE 0x54430000ull
#define MAX_EVENT_BYTES 0x10000

/* ---- small helpers ------------------------------------------------------------------------------------------------------------------------------------------ */

static volatile LONG g_lock;
static void lock(void) { while (InterlockedCompareExchange(&g_lock, 1, 0)) Sleep(0); }
static void unlock(void) { InterlockedExchange(&g_lock, 0); }

static void guid_text(const GUID *g, WCHAR *out)
{
    static const WCHAR hex[] = L"0123456789ABCDEF";
    const BYTE *b = (const BYTE *)g;
    DWORD i, at = 0;
    out[at++] = '{';
    for (i = 0; i < 16; ++i) {
        static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
        if (i == 4 || i == 6 || i == 8 || i == 10) out[at++] = '-';
        out[at++] = hex[b[order[i]] >> 4];
        out[at++] = hex[b[order[i]] & 15];
    }
    out[at++] = '}';
    out[at] = 0;
}
static void bump_generation(void)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, ETW_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) == ERROR_SUCCESS) {
        DWORD v = 0, size = sizeof(v);
        RegQueryValueExW(key, L"Generation", NULL, NULL, (BYTE *)&v, &size);
        ++v;
        RegSetValueExW(key, L"Generation", 0, REG_DWORD, (BYTE *)&v, sizeof(v));
        RegCloseKey(key);
    }
}
static DWORD read_generation(void)
{
    HKEY key;
    DWORD v = 0, size = sizeof(v);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, ETW_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) { RegQueryValueExW(key, L"Generation", NULL, NULL, (BYTE *)&v, &size); RegCloseKey(key); }
    return v;
}
static DWORD qdword(HKEY key, LPCWSTR name, DWORD fallback)
{
    DWORD type = 0, v = 0, size = sizeof(v);
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) return fallback;
    return v;
}
static ULONGLONG qqword(HKEY key, LPCWSTR name)
{
    DWORD type = 0, size = sizeof(ULONGLONG);
    ULONGLONG v = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) != ERROR_SUCCESS || type != REG_QWORD) return 0;
    return v;
}
static void sdword(HKEY key, LPCWSTR name, DWORD v) { RegSetValueExW(key, name, 0, REG_DWORD, (BYTE *)&v, sizeof(v)); }
static void sqword(HKEY key, LPCWSTR name, ULONGLONG v) { RegSetValueExW(key, name, 0, REG_QWORD, (BYTE *)&v, sizeof(v)); }
static void sstring(HKEY key, LPCWSTR name, LPCWSTR text) { RegSetValueExW(key, name, 0, REG_SZ, (const BYTE *)text, (DWORD)(wcslen(text) + 1) * 2); }
static BOOL qstring(HKEY key, LPCWSTR name, WCHAR *out, DWORD chars)
{
    DWORD type = 0, size = chars * 2;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)out, &size) != ERROR_SUCCESS || type != REG_SZ) return FALSE;
    out[size / 2 < chars ? size / 2 : chars - 1] = 0;
    return TRUE;
}
static ULONGLONG now_filetime(void)
{
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    return (ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
}

/* ---- sessions ----------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
    WCHAR name[128], file[300];
    DWORD id, buffer_size, min_buffers, max_buffers, max_file_size, mode, flush_timer, enable_flags, clock_type;
    GUID guid;
    ULONGLONG start_time;
} SessionInfo;

static BOOL session_key(LPCWSTR name, REGSAM access, BOOL create, HKEY *key)
{
    WCHAR path[420];
    wcscpy(path, SESSIONS_KEY);
    wcscat(path, L"\\");
    wcscat(path, name);
    if (create) return RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, access, NULL, key, NULL) == ERROR_SUCCESS;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, access, key) == ERROR_SUCCESS;
}
static void load_session(HKEY key, LPCWSTR name, SessionInfo *s)
{
    DWORD size = sizeof(s->guid), type = 0;
    memset(s, 0, sizeof(*s));
    wcsncpy(s->name, name, 127);
    s->id = qdword(key, L"Id", 0);
    s->buffer_size = qdword(key, L"BufferSize", 64);
    s->min_buffers = qdword(key, L"MinimumBuffers", 2);
    s->max_buffers = qdword(key, L"MaximumBuffers", 22);
    s->max_file_size = qdword(key, L"MaximumFileSize", 0);
    s->mode = qdword(key, L"LogFileMode", 0);
    s->flush_timer = qdword(key, L"FlushTimer", 0);
    s->enable_flags = qdword(key, L"EnableFlags", 0);
    s->clock_type = qdword(key, L"ClockType", 1);
    s->start_time = qqword(key, L"StartTime");
    qstring(key, L"LogFileName", s->file, 300);
    RegQueryValueExW(key, L"Guid", NULL, &type, (BYTE *)&s->guid, &size);
}
static BOOL find_session_by_handle(TRACEHANDLE h, SessionInfo *out)
{
    HKEY root;
    DWORD i;
    BOOL found = FALSE;
    if (h < SESSION_HANDLE_BASE || h > SESSION_HANDLE_BASE + 0xFFFF) return FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SESSIONS_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS) return FALSE;
    for (i = 0; !found; ++i) {
        WCHAR name[128];
        DWORD n = 128;
        HKEY key;
        if (RegEnumKeyExW(root, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        if (qdword(key, L"Id", 0) + SESSION_HANDLE_BASE == h) { load_session(key, name, out); found = TRUE; }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return found;
}
static BOOL find_session_by_name(LPCWSTR name, SessionInfo *out)
{
    HKEY key;
    if (!name || !session_key(name, KEY_READ, FALSE, &key)) return FALSE;
    load_session(key, name, out);
    RegCloseKey(key);
    return TRUE;
}
static void session_file(const SessionInfo *s, WCHAR *path)
{
    if (s->file[0]) { wcscpy(path, s->file); return; }
    wcscpy(path, LOG_DIR);
    wcscat(path, L"\\");
    wcscat(path, s->name);
    wcscat(path, L".rt.etl");
}
static HANDLE session_lock(const SessionInfo *s)
{
    WCHAR name[40] = L"TcosEtw_";
    WCHAR digits[12];
    DWORD v = s->id, n = 0, at = 8;
    do { digits[n++] = (WCHAR)('0' + v % 10); v /= 10; } while (v);
    while (n) name[at++] = digits[--n];
    name[at] = 0;
    return CreateMutexW(NULL, FALSE, name);
}
static void ensure_dirs(void)
{
    CreateDirectoryW(L"C:\\Windows\\System32\\winevt", NULL);
    CreateDirectoryW(LOG_DIR, NULL);
}

/* Fills the properties of a caller from a session (the names go after the structure, as StartTrace documents). */
static void fill_properties(const SessionInfo *s, PEVENT_TRACE_PROPERTIES p, TRACEHANDLE h)
{
    DWORD name_bytes = (DWORD)(wcslen(s->name) + 1) * 2, file_bytes = (DWORD)(wcslen(s->file) + 1) * 2;
    p->Wnode.HistoricalContext = h;
    p->Wnode.Guid = s->guid;
    p->BufferSize = s->buffer_size;
    p->MinimumBuffers = s->min_buffers;
    p->MaximumBuffers = s->max_buffers;
    p->MaximumFileSize = s->max_file_size;
    p->LogFileMode = s->mode;
    p->FlushTimer = s->flush_timer;
    p->EnableFlags = s->enable_flags;
    p->NumberOfBuffers = s->min_buffers;
    p->FreeBuffers = s->min_buffers;
    p->LoggerThreadId = (HANDLE)(ULONG_PTR)s->id;
    p->Wnode.ClientContext = s->clock_type;
    if (p->Wnode.BufferSize >= sizeof(*p) + name_bytes + file_bytes) {
        p->LoggerNameOffset = sizeof(*p);
        memcpy((BYTE *)p + p->LoggerNameOffset, s->name, name_bytes);
        p->LogFileNameOffset = sizeof(*p) + name_bytes;
        memcpy((BYTE *)p + p->LogFileNameOffset, s->file, file_bytes);
    }
}

static ULONG start_trace(PTRACEHANDLE handle, LPCWSTR name, PEVENT_TRACE_PROPERTIES p)
{
    HKEY key, root;
    DWORD i, used[MAX_SESSIONS + 1] = {0}, id = 0, existing = 0;
    WCHAR file[300] = L"";
    SessionInfo info;
    DWORD need;
    if (!handle || !name || !p) return ERROR_INVALID_PARAMETER;
    if (!*name || wcslen(name) > 127 || wcschr(name, '\\')) return ERROR_INVALID_PARAMETER;
    need = sizeof(*p) + ((DWORD)wcslen(name) + 1) * 2;
    if (p->Wnode.BufferSize < sizeof(*p) || p->Wnode.BufferSize < need) return ERROR_BAD_LENGTH;
    if (!(p->Wnode.Flags & WNODE_FLAG_TRACED_GUID)) return ERROR_INVALID_PARAMETER;
    if ((p->LogFileMode & EVENT_TRACE_FILE_MODE_NEWFILE) && (p->LogFileMode & EVENT_TRACE_FILE_MODE_CIRCULAR)) return ERROR_INVALID_PARAMETER;
    if (p->LogFileNameOffset) {
        if (p->LogFileNameOffset > p->Wnode.BufferSize - 2) return ERROR_INVALID_PARAMETER;
        wcsncpy(file, (WCHAR *)((BYTE *)p + p->LogFileNameOffset), 299);
        file[299] = 0;
    }
    if (!file[0] && !(p->LogFileMode & EVENT_TRACE_REAL_TIME_MODE)) return ERROR_BAD_PATHNAME;
    if (find_session_by_name(name, &info)) return ERROR_ALREADY_EXISTS;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, SESSIONS_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &root, NULL) != ERROR_SUCCESS) return ERROR_ACCESS_DENIED;
    for (i = 0;; ++i) {
        WCHAR other[128];
        DWORD n = 128;
        HKEY k;
        if (RegEnumKeyExW(root, i, other, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        existing++;
        if (RegOpenKeyExW(root, other, 0, KEY_READ, &k) == ERROR_SUCCESS) { DWORD oid = qdword(k, L"Id", 0); if (oid <= MAX_SESSIONS) used[oid] = 1; RegCloseKey(k); }
    }
    RegCloseKey(root);
    if (existing >= MAX_SESSIONS) return ERROR_NO_SYSTEM_RESOURCES;
    for (id = 1; id <= MAX_SESSIONS && used[id]; ++id) {}
    if (!session_key(name, KEY_ALL_ACCESS, TRUE, &key)) return ERROR_ACCESS_DENIED;
    {
        GUID guid = p->Wnode.Guid;
        sdword(key, L"Id", id);
        sdword(key, L"BufferSize", p->BufferSize ? p->BufferSize : 64);
        sdword(key, L"MinimumBuffers", p->MinimumBuffers ? p->MinimumBuffers : 2);
        sdword(key, L"MaximumBuffers", p->MaximumBuffers ? p->MaximumBuffers : 22);
        sdword(key, L"MaximumFileSize", p->MaximumFileSize);
        sdword(key, L"LogFileMode", p->LogFileMode);
        sdword(key, L"FlushTimer", p->FlushTimer);
        sdword(key, L"EnableFlags", p->EnableFlags);
        sdword(key, L"ClockType", p->Wnode.ClientContext ? p->Wnode.ClientContext : 1);
        sqword(key, L"StartTime", now_filetime());
        RegSetValueExW(key, L"Guid", 0, REG_BINARY, (BYTE *)&guid, sizeof(guid));
        if (file[0]) sstring(key, L"LogFileName", file);
    }
    RegCloseKey(key);
    if (!find_session_by_name(name, &info)) return ERROR_ACCESS_DENIED;
    ensure_dirs();
    {
        WCHAR path[300];
        HANDLE f;
        DWORD done;
        DWORD header[8] = {FILE_MAGIC, 1, 0, 0, 0, 0, 0, 0};
        ULONGLONG t = now_filetime();
        session_file(&info, path);
        f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, (p->LogFileMode & EVENT_TRACE_FILE_MODE_APPEND) ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (session_key(name, KEY_ALL_ACCESS, FALSE, &key)) { RegCloseKey(key); }
            { HKEY r; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SESSIONS_KEY, 0, KEY_ALL_ACCESS, &r) == ERROR_SUCCESS) { RegDeleteTreeW(r, name); RegCloseKey(r); } }
            return e == ERROR_FILE_NOT_FOUND ? ERROR_PATH_NOT_FOUND : e;
        }
        header[2] = (DWORD)t; header[3] = (DWORD)(t >> 32); header[4] = 8;
        if (GetFileSize(f, NULL) == 0) WriteFile(f, header, sizeof(header), &done, NULL);
        CloseHandle(f);
    }
    bump_generation();
    *handle = SESSION_HANDLE_BASE + id;
    fill_properties(&info, p, *handle);
    return ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WMIAPI StartTraceW(PTRACEHANDLE handle, LPCWSTR name, PEVENT_TRACE_PROPERTIES p) { return start_trace(handle, name, p); }
__declspec(dllexport) ULONG WMIAPI StartTraceA(PTRACEHANDLE handle, LPCSTR name, PEVENT_TRACE_PROPERTIES p)
{
    WCHAR *w = strdupAW(name);
    ULONG r;
    if (name && !w) return ERROR_NOT_ENOUGH_MEMORY;
    if (p && p->LogFileNameOffset && p->LogFileNameOffset < p->Wnode.BufferSize) {      /* an ANSI log file name is converted in place when it fits */
        char *a = (char *)p + p->LogFileNameOffset;
        WCHAR *wf = strdupAW(a);
        DWORD n = wf ? (DWORD)wcslen(wf) + 1 : 0;
        if (wf && p->LogFileNameOffset + n * 2 <= p->Wnode.BufferSize) memcpy(a, wf, n * 2);
        free(wf);
    }
    r = start_trace(handle, w, p);
    free(w);
    return r;
}

/* ---- control ------------------------------------------------------------------------------------------------------------------------------------------------ */

static ULONG resolve_session(TRACEHANDLE h, LPCWSTR name, SessionInfo *s)
{
    if (name && *name) return find_session_by_name(name, s) ? ERROR_SUCCESS : ERROR_WMI_INSTANCE_NOT_FOUND;
    if (h) return find_session_by_handle(h, s) ? ERROR_SUCCESS : ERROR_INVALID_HANDLE;
    return ERROR_INVALID_PARAMETER;
}
__declspec(dllexport) ULONG WMIAPI ControlTraceW(TRACEHANDLE h, LPCWSTR name, PEVENT_TRACE_PROPERTIES p, ULONG code)
{
    SessionInfo s;
    ULONG r;
    HKEY key;
    if (!p) return ERROR_INVALID_PARAMETER;
    if (p->Wnode.BufferSize < sizeof(*p)) return ERROR_BAD_LENGTH;
    if ((r = resolve_session(h, name, &s))) return r;
    switch (code) {
    case EVENT_TRACE_CONTROL_QUERY:
    case EVENT_TRACE_CONTROL_FLUSH:
        fill_properties(&s, p, SESSION_HANDLE_BASE + s.id);
        return ERROR_SUCCESS;
    case EVENT_TRACE_CONTROL_STOP: {
        HKEY root;
        fill_properties(&s, p, SESSION_HANDLE_BASE + s.id);
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SESSIONS_KEY, 0, KEY_ALL_ACCESS, &root) != ERROR_SUCCESS) return ERROR_ACCESS_DENIED;
        r = RegDeleteTreeW(root, s.name);
        RegCloseKey(root);
        if (r) return r;
        bump_generation();
        return ERROR_SUCCESS;
    }
    case EVENT_TRACE_CONTROL_UPDATE:
        if (!session_key(s.name, KEY_ALL_ACCESS, FALSE, &key)) return ERROR_ACCESS_DENIED;
        if (p->MaximumBuffers && p->MaximumBuffers < p->MinimumBuffers) { RegCloseKey(key); return ERROR_INVALID_PARAMETER; }
        if (p->FlushTimer) sdword(key, L"FlushTimer", p->FlushTimer);
        if (p->EnableFlags) sdword(key, L"EnableFlags", p->EnableFlags);
        if (p->MaximumFileSize) sdword(key, L"MaximumFileSize", p->MaximumFileSize);
        if (p->LogFileNameOffset && p->LogFileNameOffset < p->Wnode.BufferSize) {
            WCHAR *file = (WCHAR *)((BYTE *)p + p->LogFileNameOffset);
            if (*file) sstring(key, L"LogFileName", file);
        }
        RegCloseKey(key);
        bump_generation();
        if (find_session_by_name(s.name, &s)) fill_properties(&s, p, SESSION_HANDLE_BASE + s.id);
        return ERROR_SUCCESS;
    case EVENT_TRACE_CONTROL_INCREMENT_FILE:
        return (s.mode & EVENT_TRACE_FILE_MODE_NEWFILE) ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
    default: return ERROR_INVALID_PARAMETER;
    }
}
__declspec(dllexport) ULONG WMIAPI StopTraceW(TRACEHANDLE h, LPCWSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceW(h, name, p, EVENT_TRACE_CONTROL_STOP); }
__declspec(dllexport) ULONG WMIAPI QueryTraceW(TRACEHANDLE h, LPCWSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceW(h, name, p, EVENT_TRACE_CONTROL_QUERY); }
__declspec(dllexport) ULONG WMIAPI UpdateTraceW(TRACEHANDLE h, LPCWSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceW(h, name, p, EVENT_TRACE_CONTROL_UPDATE); }
__declspec(dllexport) ULONG WMIAPI FlushTraceW(TRACEHANDLE h, LPCWSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceW(h, name, p, EVENT_TRACE_CONTROL_FLUSH); }
__declspec(dllexport) ULONG WMIAPI ControlTraceA(TRACEHANDLE h, LPCSTR name, PEVENT_TRACE_PROPERTIES p, ULONG code)
{
    WCHAR *w = strdupAW(name);
    ULONG r;
    if (name && !w) return ERROR_NOT_ENOUGH_MEMORY;
    r = ControlTraceW(h, w, p, code);
    free(w);
    return r;
}
__declspec(dllexport) ULONG WMIAPI StopTraceA(TRACEHANDLE h, LPCSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceA(h, name, p, EVENT_TRACE_CONTROL_STOP); }
__declspec(dllexport) ULONG WMIAPI QueryTraceA(TRACEHANDLE h, LPCSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceA(h, name, p, EVENT_TRACE_CONTROL_QUERY); }
__declspec(dllexport) ULONG WMIAPI UpdateTraceA(TRACEHANDLE h, LPCSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceA(h, name, p, EVENT_TRACE_CONTROL_UPDATE); }
__declspec(dllexport) ULONG WMIAPI FlushTraceA(TRACEHANDLE h, LPCSTR name, PEVENT_TRACE_PROPERTIES p) { return ControlTraceA(h, name, p, EVENT_TRACE_CONTROL_FLUSH); }

static ULONG query_all(PEVENT_TRACE_PROPERTIES *array, ULONG count, PULONG returned)
{
    HKEY root;
    DWORD i, n = 0, total = 0;
    if (!returned) return ERROR_INVALID_PARAMETER;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SESSIONS_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS) { *returned = 0; return ERROR_SUCCESS; }
    for (i = 0;; ++i) {
        WCHAR name[128];
        DWORD len = 128;
        SessionInfo s;
        HKEY key;
        if (RegEnumKeyExW(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        load_session(key, name, &s);
        RegCloseKey(key);
        ++total;
        if (array && n < count && array[n]) {
            if (array[n]->Wnode.BufferSize < sizeof(EVENT_TRACE_PROPERTIES)) { RegCloseKey(root); *returned = total; return ERROR_BAD_LENGTH; }
            fill_properties(&s, array[n], SESSION_HANDLE_BASE + s.id);
            ++n;
        }
    }
    RegCloseKey(root);
    *returned = total;
    return total > count ? ERROR_MORE_DATA : ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WMIAPI QueryAllTracesW(PEVENT_TRACE_PROPERTIES *array, ULONG count, PULONG returned) { return query_all(array, count, returned); }
__declspec(dllexport) ULONG WMIAPI QueryAllTracesA(PEVENT_TRACE_PROPERTIES *array, ULONG count, PULONG returned) { return query_all(array, count, returned); }

/* ---- enabling providers in a session ---------------------------------------------------------------------------------------------------------------------------- */

static ULONG enable_provider(TRACEHANDLE h, const GUID *provider, ULONG code, UCHAR level, ULONGLONG any, ULONGLONG all)
{
    SessionInfo s;
    HKEY key, providers, pk;
    WCHAR text[48];
    if (!provider) return ERROR_INVALID_PARAMETER;
    if (!find_session_by_handle(h, &s)) return ERROR_INVALID_HANDLE;
    if (!session_key(s.name, KEY_ALL_ACCESS, FALSE, &key)) return ERROR_WMI_INSTANCE_NOT_FOUND;
    if (RegCreateKeyExW(key, L"Providers", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &providers, NULL) != ERROR_SUCCESS) { RegCloseKey(key); return ERROR_ACCESS_DENIED; }
    guid_text(provider, text);
    if (code == EVENT_CONTROL_CODE_DISABLE_PROVIDER) RegDeleteTreeW(providers, text);
    else if (code == EVENT_CONTROL_CODE_ENABLE_PROVIDER) {
        if (RegCreateKeyExW(providers, text, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &pk, NULL) == ERROR_SUCCESS) {
            sdword(pk, L"Level", level);
            sqword(pk, L"MatchAnyKeyword", any);
            sqword(pk, L"MatchAllKeyword", all);
            RegSetValueExW(pk, L"Guid", 0, REG_BINARY, (const BYTE *)provider, sizeof(GUID));
            RegCloseKey(pk);
        }
    } else if (code != EVENT_CONTROL_CODE_CAPTURE_STATE) { RegCloseKey(providers); RegCloseKey(key); return ERROR_INVALID_PARAMETER; }
    RegCloseKey(providers);
    RegCloseKey(key);
    bump_generation();
    return ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WMIAPI EnableTraceEx2(TRACEHANDLE h, LPCGUID provider, ULONG code, UCHAR level, ULONGLONG any, ULONGLONG all, ULONG timeout, PENABLE_TRACE_PARAMETERS parameters)
{
    ULONG r;
    (void)timeout;
    if (parameters && parameters->Version != ENABLE_TRACE_PARAMETERS_VERSION && parameters->Version != ENABLE_TRACE_PARAMETERS_VERSION_2) return ERROR_INVALID_PARAMETER;
    r = enable_provider(h, provider, code, level, any, all);
    return r;
}
__declspec(dllexport) ULONG WMIAPI EnableTraceEx(LPCGUID provider, LPCGUID source, TRACEHANDLE h, ULONG enable, UCHAR level, ULONGLONG any, ULONGLONG all, ULONG property, PEVENT_FILTER_DESCRIPTOR filter)
{
    (void)source; (void)property; (void)filter;
    return enable_provider(h, provider, enable ? EVENT_CONTROL_CODE_ENABLE_PROVIDER : EVENT_CONTROL_CODE_DISABLE_PROVIDER, level, any, all);
}
__declspec(dllexport) ULONG WMIAPI EnableTrace(ULONG enable, ULONG flags, ULONG level, LPCGUID control_guid, TRACEHANDLE h)
{
    return enable_provider(h, control_guid, enable ? EVENT_CONTROL_CODE_ENABLE_PROVIDER : EVENT_CONTROL_CODE_DISABLE_PROVIDER, (UCHAR)level, flags ? flags : ~0ull, 0);
}

/* ---- providers ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
    DWORD magic;
    GUID id;
    PENABLECALLBACK callback;
    PVOID context;
    UCHAR level;
    ULONGLONG any, all;
    BOOL enabled;
    BOOL legacy;
    WMIDPREQUEST request;
    TRACEHANDLE logger;
} Provider;
#define PROVIDER_MAGIC 0x45545750u
static Provider g_providers[MAX_PROVIDERS];
static volatile LONG g_watcher_started;
static DWORD g_seen_generation;

typedef struct { DWORD session; UCHAR level; ULONGLONG any, all; GUID session_guid; } Enabling;

/* The sessions that have enabled a provider; returns how many (up to `max`). */
static DWORD enablings_of(const GUID *id, Enabling *out, DWORD max)
{
    HKEY root;
    DWORD i, n = 0;
    WCHAR text[48];
    guid_text(id, text);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SESSIONS_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS) return 0;
    for (i = 0; n < max; ++i) {
        WCHAR name[128];
        DWORD len = 128;
        HKEY key, providers, pk;
        if (RegEnumKeyExW(root, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        if (RegOpenKeyExW(key, L"Providers", 0, KEY_READ, &providers) == ERROR_SUCCESS) {
            if (RegOpenKeyExW(providers, text, 0, KEY_READ, &pk) == ERROR_SUCCESS) {
                DWORD size = sizeof(GUID);
                out[n].session = qdword(key, L"Id", 0);
                out[n].level = (UCHAR)qdword(pk, L"Level", 0);
                out[n].any = qqword(pk, L"MatchAnyKeyword");
                out[n].all = qqword(pk, L"MatchAllKeyword");
                memset(&out[n].session_guid, 0, sizeof(GUID));
                RegQueryValueExW(key, L"Guid", NULL, NULL, (BYTE *)&out[n].session_guid, &size);
                ++n;
                RegCloseKey(pk);
            }
            RegCloseKey(providers);
        }
        RegCloseKey(key);
    }
    RegCloseKey(root);
    return n;
}

/* Looks at the sessions again and calls the callback of the provider if what is enabled changed. */
static void refresh_provider(Provider *p)
{
    Enabling e[MAX_SESSIONS];
    DWORD n = enablings_of(&p->id, e, MAX_SESSIONS), i;
    UCHAR level = 0;
    ULONGLONG any = 0, all = ~0ull;
    BOOL enabled = n != 0, changed;
    GUID source = {0};
    for (i = 0; i < n; ++i) {
        if (e[i].level > level) level = e[i].level;
        any |= e[i].any ? e[i].any : ~0ull;
        all &= e[i].all;
        if (i == 0) source = e[i].session_guid;
    }
    if (!enabled) { level = 0; any = 0; all = 0; }
    changed = enabled != p->enabled || level != p->level || any != p->any || all != p->all;
    p->enabled = enabled; p->level = level; p->any = any; p->all = all;
    if (changed) {
        if (p->callback) p->callback(&source, enabled ? EVENT_CONTROL_CODE_ENABLE_PROVIDER : EVENT_CONTROL_CODE_DISABLE_PROVIDER, level, any, all, NULL, p->context);
        else if (p->request) {      /* a legacy provider (RegisterTraceGuids) hears WMI_ENABLE_EVENTS / WMI_DISABLE_EVENTS with a WNODE_HEADER whose HistoricalContext is the logger */
            WNODE_HEADER header;
            ULONG size = sizeof(header);
            memset(&header, 0, sizeof(header));
            header.BufferSize = sizeof(header);
            header.Guid = p->id;
            header.HistoricalContext = enabled && n ? SESSION_HANDLE_BASE + e[0].session : 0;
            p->logger = header.HistoricalContext;
            p->request(enabled ? WMI_ENABLE_EVENTS : WMI_DISABLE_EVENTS, p->context, &size, &header);
        }
    }
}
static DWORD WINAPI generation_watcher(LPVOID unused)
{
    (void)unused;
    for (;;) {
        DWORD g = read_generation(), i;
        if (g != g_seen_generation) {
            g_seen_generation = g;
            for (i = 0; i < MAX_PROVIDERS; ++i) if (g_providers[i].magic == PROVIDER_MAGIC) refresh_provider(&g_providers[i]);
        }
        Sleep(50);
    }
    return 0;
}
static void start_watcher(void)
{
    if (InterlockedCompareExchange(&g_watcher_started, 1, 0) == 0) {
        HANDLE t;
        g_seen_generation = read_generation();
        t = CreateThread(NULL, 0, generation_watcher, NULL, 0, NULL);
        if (t) CloseHandle(t); else InterlockedExchange(&g_watcher_started, 0);
    }
}

static Provider *provider_of(REGHANDLE h)
{
    Provider *p = (Provider *)(ULONG_PTR)h;
    if (p < g_providers || p >= g_providers + MAX_PROVIDERS || ((ULONG_PTR)p - (ULONG_PTR)g_providers) % sizeof(Provider) || p->magic != PROVIDER_MAGIC) return NULL;
    return p;
}
__declspec(dllexport) ULONG EVNTAPI EventRegister(LPCGUID id, PENABLECALLBACK callback, PVOID context, PREGHANDLE out)
{
    DWORD i;
    Provider *p = NULL;
    if (!id || !out) return ERROR_INVALID_PARAMETER;
    lock();
    for (i = 0; i < MAX_PROVIDERS; ++i) if (!g_providers[i].magic) { p = &g_providers[i]; memset(p, 0, sizeof(*p)); p->magic = PROVIDER_MAGIC; break; }
    unlock();
    if (!p) return ERROR_NOT_ENOUGH_MEMORY;
    p->id = *id;
    p->callback = callback;
    p->context = context;
    *out = (REGHANDLE)(ULONG_PTR)p;
    start_watcher();
    refresh_provider(p);
    return ERROR_SUCCESS;
}
__declspec(dllexport) ULONG EVNTAPI EventUnregister(REGHANDLE h)
{
    Provider *p = provider_of(h);
    if (!p) return ERROR_INVALID_HANDLE;
    if (p->enabled && p->callback) p->callback(&p->id, EVENT_CONTROL_CODE_DISABLE_PROVIDER, 0, 0, 0, NULL, p->context);
    lock();
    p->magic = 0;
    unlock();
    return ERROR_SUCCESS;
}

/* Whether an event of this level and these keywords is wanted by some session (or by a provider state that is on). */
static BOOL wanted(const Provider *p, UCHAR level, ULONGLONG keyword)
{
    if (!p->enabled) return FALSE;
    if (level != 0 && p->level != 0 && level > p->level) return FALSE;
    if (keyword != 0 && p->any && !(keyword & p->any)) return FALSE;
    if (keyword != 0 && (keyword & p->all) != p->all) return FALSE;
    return TRUE;
}
__declspec(dllexport) BOOLEAN EVNTAPI EventEnabled(REGHANDLE h, PCEVENT_DESCRIPTOR d)
{
    Provider *p = provider_of(h);
    return p && d && wanted(p, d->Level, d->Keyword);
}
__declspec(dllexport) BOOLEAN EVNTAPI EventProviderEnabled(REGHANDLE h, UCHAR level, ULONGLONG keyword)
{
    Provider *p = provider_of(h);
    return p && wanted(p, level, keyword);
}
__declspec(dllexport) ULONG EVNTAPI EventSetInformation(REGHANDLE h, EVENT_INFO_CLASS c, PVOID info, ULONG length)
{
    (void)info; (void)length;
    if (!provider_of(h)) return ERROR_INVALID_HANDLE;
    if (c < 0 || c >= MaxEventInfo) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS;
}

/* ---- activity ids -------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { DWORD thread; GUID id; } ActivitySlot;
static ActivitySlot g_activity[64];
static ULONGLONG g_guid_counter;
static void new_guid(GUID *g)
{
    ULONGLONG t = now_filetime() ^ (GetTickCount64() << 20) ^ ((ULONGLONG)GetCurrentProcessId() << 40) ^ (++g_guid_counter * 0x9E3779B97F4A7C15ull);
    BYTE *b = (BYTE *)g;
    DWORD i;
    for (i = 0; i < 16; ++i) { t = t * 6364136223846793005ull + 1442695040888963407ull; b[i] = (BYTE)(t >> 56); }
    g->Data3 = (g->Data3 & 0x0FFF) | 0x4000;      /* a random (version 4) id */
    g->Data4[0] = (g->Data4[0] & 0x3F) | 0x80;
}
static ActivitySlot *activity_slot(BOOL create)
{
    DWORD tid = GetCurrentThreadId(), i;
    ActivitySlot *free_slot = NULL;
    for (i = 0; i < 64; ++i) {
        if (g_activity[i].thread == tid) return &g_activity[i];
        if (!g_activity[i].thread && !free_slot) free_slot = &g_activity[i];
    }
    if (create && free_slot) { free_slot->thread = tid; memset(&free_slot->id, 0, sizeof(GUID)); return free_slot; }
    return NULL;
}
__declspec(dllexport) ULONG EVNTAPI EventActivityIdControl(ULONG code, LPGUID id)
{
    ActivitySlot *s;
    GUID zero = {0};
    if (code != EVENT_ACTIVITY_CTRL_GET_ID && code != EVENT_ACTIVITY_CTRL_SET_ID && code != EVENT_ACTIVITY_CTRL_CREATE_ID && code != EVENT_ACTIVITY_CTRL_GET_SET_ID && code != EVENT_ACTIVITY_CTRL_CREATE_SET_ID)
        return ERROR_INVALID_FUNCTION;
    if (!id) return ERROR_INVALID_PARAMETER;
    lock();
    s = activity_slot(TRUE);
    switch (code) {
    case EVENT_ACTIVITY_CTRL_GET_ID: *id = s ? s->id : zero; break;
    case EVENT_ACTIVITY_CTRL_SET_ID: if (s) s->id = *id; break;
    case EVENT_ACTIVITY_CTRL_CREATE_ID: new_guid(id); break;
    case EVENT_ACTIVITY_CTRL_GET_SET_ID: { GUID old = s ? s->id : zero; if (s) s->id = *id; *id = old; break; }
    case EVENT_ACTIVITY_CTRL_CREATE_SET_ID: { GUID old = s ? s->id : zero; GUID fresh; new_guid(&fresh); if (s) s->id = fresh; *id = old; break; }
    }
    unlock();
    return ERROR_SUCCESS;
}

/* ---- writing events ----------------------------------------------------------------------------------------------------------------------------------------- */

/* The file format: a header (32 bytes), then records { DWORD size; EVENT_HEADER header (80 bytes); DWORD user_size; user data; padded to 8 }. */
static ULONG append_event(const SessionInfo *s, const BYTE *record, DWORD size)
{
    WCHAR path[300];
    HANDLE f, lk = session_lock(s);
    DWORD done;
    BOOL ok;
    session_file(s, path);
    if (lk) WaitForSingleObject(lk, INFINITE);
    f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { if (lk) { ReleaseMutex(lk); CloseHandle(lk); } return GetLastError(); }
    SetFilePointer(f, 0, NULL, FILE_END);
    if (s->max_file_size && GetFileSize(f, NULL) / 1048576 >= s->max_file_size && !(s->mode & EVENT_TRACE_FILE_MODE_CIRCULAR)) { CloseHandle(f); if (lk) { ReleaseMutex(lk); CloseHandle(lk); } return ERROR_DISK_FULL; }
    ok = WriteFile(f, record, size, &done, NULL) && done == size;
    CloseHandle(f);
    if (lk) { ReleaseMutex(lk); CloseHandle(lk); }
    return ok ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
}

static ULONG deliver(const GUID *provider, const EVENT_DESCRIPTOR *d, const GUID *activity, const GUID *related, const BYTE *user, DWORD user_size, USHORT flags, const Provider *only)
{
    Enabling e[MAX_SESSIONS];
    DWORD n, i, total;
    BYTE *record;
    EVENT_HEADER *h;
    if (user_size > MAX_EVENT_BYTES) return ERROR_ARITHMETIC_OVERFLOW;
    n = enablings_of(provider, e, MAX_SESSIONS);
    if (!n) return ERROR_SUCCESS;
    total = (4 + sizeof(EVENT_HEADER) + 4 + user_size + 7) & ~7u;
    if (!(record = calloc(1, total))) return ERROR_NOT_ENOUGH_MEMORY;
    *(DWORD *)record = total;
    h = (EVENT_HEADER *)(record + 4);
    h->Size = sizeof(EVENT_HEADER);
    h->HeaderType = 0;
    h->Flags = EVENT_HEADER_FLAG_64_BIT_HEADER | EVENT_HEADER_FLAG_PROCESSOR_INDEX | flags;
    h->ThreadId = GetCurrentThreadId();
    h->ProcessId = GetCurrentProcessId();
    h->TimeStamp.QuadPart = (LONGLONG)now_filetime();
    h->ProviderId = *provider;
    h->EventDescriptor = *d;
    if (activity) h->ActivityId = *activity;
    else { GUID cur = {0}; lock(); { ActivitySlot *s = activity_slot(FALSE); if (s) cur = s->id; } unlock(); h->ActivityId = cur; }
    (void)related;
    *(DWORD *)(record + 4 + sizeof(EVENT_HEADER)) = user_size;
    if (user_size) memcpy(record + 4 + sizeof(EVENT_HEADER) + 4, user, user_size);
    for (i = 0; i < n; ++i) {
        SessionInfo s;
        HKEY key;
        BOOL pass = TRUE;
        ULONGLONG keyword = d->Keyword;
        if (d->Level && e[i].level && d->Level > e[i].level) pass = FALSE;
        if (keyword && e[i].any && !(keyword & e[i].any)) pass = FALSE;
        if (keyword && (keyword & e[i].all) != e[i].all) pass = FALSE;
        if (!pass) continue;
        /* find the session by its id */
        {
            TRACEHANDLE th = SESSION_HANDLE_BASE + e[i].session;
            if (!find_session_by_handle(th, &s)) continue;
        }
        (void)key; (void)only;
        h->ProcessorTime = e[i].session;      /* (the logger it went to) */
        append_event(&s, record, total);
    }
    free(record);
    return ERROR_SUCCESS;
}

static ULONG write_event(REGHANDLE rh, const EVENT_DESCRIPTOR *d, const GUID *activity, const GUID *related, ULONG count, PEVENT_DATA_DESCRIPTOR data, USHORT flags)
{
    Provider *p = provider_of(rh);
    BYTE *user = NULL;
    DWORD size = 0, i;
    ULONG r;
    if (!p) return ERROR_INVALID_HANDLE;
    if (!d) return ERROR_INVALID_PARAMETER;
    if (count > 128 || (count && !data)) return ERROR_INVALID_PARAMETER;
    if (!wanted(p, d->Level, d->Keyword)) return ERROR_SUCCESS;
    for (i = 0; i < count; ++i) {
        if (data[i].Size > MAX_EVENT_BYTES || size + data[i].Size > MAX_EVENT_BYTES) return ERROR_ARITHMETIC_OVERFLOW;
        size += data[i].Size;
    }
    if (size && !(user = malloc(size))) return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0, size = 0; i < count; ++i) {
        if (data[i].Size) memcpy(user + size, (const void *)(ULONG_PTR)data[i].Ptr, data[i].Size);
        size += data[i].Size;
    }
    r = deliver(&p->id, d, activity, related, user, size, flags, p);
    free(user);
    return r;
}
__declspec(dllexport) ULONG EVNTAPI EventWrite(REGHANDLE h, PCEVENT_DESCRIPTOR d, ULONG count, PEVENT_DATA_DESCRIPTOR data) { return write_event(h, d, NULL, NULL, count, data, 0); }
__declspec(dllexport) ULONG EVNTAPI EventWriteTransfer(REGHANDLE h, PCEVENT_DESCRIPTOR d, LPCGUID activity, LPCGUID related, ULONG count, PEVENT_DATA_DESCRIPTOR data)
{
    return write_event(h, d, activity, related, count, data, 0);
}
__declspec(dllexport) ULONG EVNTAPI EventWriteEx(REGHANDLE h, PCEVENT_DESCRIPTOR d, ULONG64 filter, ULONG flags, LPCGUID activity, LPCGUID related, ULONG count, PEVENT_DATA_DESCRIPTOR data)
{
    (void)filter; (void)flags;
    return write_event(h, d, activity, related, count, data, 0);
}
__declspec(dllexport) ULONG EVNTAPI EventWriteString(REGHANDLE h, UCHAR level, ULONGLONG keyword, PCWSTR text)
{
    EVENT_DESCRIPTOR d;
    EVENT_DATA_DESCRIPTOR data;
    if (!text) return ERROR_INVALID_PARAMETER;
    memset(&d, 0, sizeof(d));
    d.Level = level;
    d.Keyword = keyword;
    EventDataDescCreate(&data, text, (ULONG)(wcslen(text) + 1) * 2);
    return write_event(h, &d, NULL, NULL, 1, &data, EVENT_HEADER_FLAG_STRING_ONLY);
}
__declspec(dllexport) ULONG EVNTAPI EventWriteStartScenario(REGHANDLE h, PCEVENT_DESCRIPTOR d, ULONG count, PEVENT_DATA_DESCRIPTOR data)
{
    EVENT_DESCRIPTOR copy;
    if (!d) return ERROR_INVALID_PARAMETER;
    copy = *d;
    copy.Opcode = EVENT_TRACE_TYPE_START;
    return write_event(h, &copy, NULL, NULL, count, data, 0);
}
__declspec(dllexport) ULONG EVNTAPI EventWriteEndScenario(REGHANDLE h, PCEVENT_DESCRIPTOR d, ULONG count, PEVENT_DATA_DESCRIPTOR data)
{
    EVENT_DESCRIPTOR copy;
    if (!d) return ERROR_INVALID_PARAMETER;
    copy = *d;
    copy.Opcode = EVENT_TRACE_TYPE_END;
    return write_event(h, &copy, NULL, NULL, count, data, 0);
}
__declspec(dllexport) LONG WINAPI CveEventWrite(PCWSTR cve, PCWSTR details)
{
    if (!cve) return ERROR_INVALID_PARAMETER;
    (void)details;
    return ERROR_SUCCESS;      /* (the Windows provider that hears these is never enabled here) */
}

/* ---- classic providers and the older writing calls ----------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) ULONG WMIAPI RegisterTraceGuidsW(WMIDPREQUEST request, PVOID context, LPCGUID control, ULONG count, PTRACE_GUID_REGISTRATION regs, LPCWSTR mof, LPCWSTR resource, PTRACEHANDLE out)
{
    DWORD i;
    Provider *p = NULL;
    (void)mof; (void)resource;
    if (!request || !control || !out || (count && !regs)) return ERROR_INVALID_PARAMETER;
    lock();
    for (i = 0; i < MAX_PROVIDERS; ++i) if (!g_providers[i].magic) { p = &g_providers[i]; memset(p, 0, sizeof(*p)); p->magic = PROVIDER_MAGIC; break; }
    unlock();
    if (!p) return ERROR_NOT_ENOUGH_MEMORY;
    for (i = 0; i < count; ++i) regs[i].RegHandle = (HANDLE)(ULONG_PTR)p;
    p->id = *control;
    p->request = request;
    p->context = context;
    p->legacy = TRUE;
    *out = (TRACEHANDLE)(ULONG_PTR)p;
    start_watcher();
    refresh_provider(p);
    return ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WMIAPI RegisterTraceGuidsA(WMIDPREQUEST request, PVOID context, LPCGUID control, ULONG count, PTRACE_GUID_REGISTRATION regs, LPCSTR mof, LPCSTR resource, PTRACEHANDLE out)
{
    (void)mof; (void)resource;
    return RegisterTraceGuidsW(request, context, control, count, regs, NULL, NULL, out);
}
__declspec(dllexport) ULONG WMIAPI UnregisterTraceGuids(TRACEHANDLE h)
{
    Provider *p = provider_of((REGHANDLE)h);
    if (!p || !p->legacy) return ERROR_INVALID_HANDLE;
    if (p->enabled && p->request) { WNODE_HEADER header; ULONG size = sizeof(header); memset(&header, 0, sizeof(header)); header.BufferSize = sizeof(header); header.Guid = p->id; p->request(WMI_DISABLE_EVENTS, p->context, &size, &header); }
    lock();
    p->magic = 0;
    unlock();
    return ERROR_SUCCESS;
}
__declspec(dllexport) TRACEHANDLE WMIAPI GetTraceLoggerHandle(PVOID buffer)
{
    WNODE_HEADER *w = buffer;
    if (!w) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    return w->HistoricalContext;
}
static BOOL enable_state_of(TRACEHANDLE h, UCHAR *level, ULONG *flags)
{
    DWORD i;
    for (i = 0; i < MAX_PROVIDERS; ++i)
        if (g_providers[i].magic == PROVIDER_MAGIC && g_providers[i].logger == h && g_providers[i].enabled) {
            *level = g_providers[i].level;
            *flags = (ULONG)g_providers[i].any;
            return TRUE;
        }
    return FALSE;
}
__declspec(dllexport) UCHAR WMIAPI GetTraceEnableLevel(TRACEHANDLE h)
{
    UCHAR level = 0;
    ULONG flags = 0;
    if (!enable_state_of(h, &level, &flags)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return level;
}
__declspec(dllexport) ULONG WMIAPI GetTraceEnableFlags(TRACEHANDLE h)
{
    UCHAR level = 0;
    ULONG flags = 0;
    if (!enable_state_of(h, &level, &flags)) { SetLastError(ERROR_INVALID_HANDLE); return 0; }
    return flags;
}

/* A session by its handle, and an event written to it directly (TraceEvent and the TraceMessage family do not go through a provider's enable state). */
static ULONG write_to_session(TRACEHANDLE h, const GUID *guid, UCHAR opcode, UCHAR version, const BYTE *user, DWORD size)
{
    SessionInfo s;
    DWORD total = (4 + sizeof(EVENT_HEADER) + 4 + size + 7) & ~7u;
    BYTE *record;
    EVENT_HEADER *eh;
    ULONG r;
    if (size > MAX_EVENT_BYTES) return ERROR_ARITHMETIC_OVERFLOW;
    if (!find_session_by_handle(h, &s)) return ERROR_INVALID_HANDLE;
    if (!(record = calloc(1, total))) return ERROR_NOT_ENOUGH_MEMORY;
    *(DWORD *)record = total;
    eh = (EVENT_HEADER *)(record + 4);
    eh->Size = sizeof(EVENT_HEADER);
    eh->Flags = EVENT_HEADER_FLAG_64_BIT_HEADER | EVENT_HEADER_FLAG_CLASSIC_HEADER;
    eh->ThreadId = GetCurrentThreadId();
    eh->ProcessId = GetCurrentProcessId();
    eh->TimeStamp.QuadPart = (LONGLONG)now_filetime();
    eh->ProviderId = *guid;
    eh->EventDescriptor.Opcode = opcode;
    eh->EventDescriptor.Version = version;
    eh->ProcessorTime = s.id;
    *(DWORD *)(record + 4 + sizeof(EVENT_HEADER)) = size;
    if (size) memcpy(record + 4 + sizeof(EVENT_HEADER) + 4, user, size);
    r = append_event(&s, record, total);
    free(record);
    return r;
}
__declspec(dllexport) ULONG WMIAPI TraceEvent(TRACEHANDLE h, PEVENT_TRACE_HEADER header)
{
    if (!header || header->Size < sizeof(EVENT_TRACE_HEADER)) return ERROR_INVALID_PARAMETER;
    return write_to_session(h, &header->Guid, header->Class.Type, header->Class.Version, (BYTE *)header + sizeof(EVENT_TRACE_HEADER), header->Size - sizeof(EVENT_TRACE_HEADER));
}
__declspec(dllexport) ULONG WMIAPI TraceEventInstance(TRACEHANDLE h, PEVENT_INSTANCE_HEADER header, PEVENT_INSTANCE_INFO instance, PEVENT_INSTANCE_INFO parent)
{
    Provider *p;
    (void)parent;
    if (!header || !instance || header->Size < sizeof(EVENT_INSTANCE_HEADER)) return ERROR_INVALID_PARAMETER;
    if (!(p = provider_of((REGHANDLE)(ULONG_PTR)instance->RegHandle))) return ERROR_INVALID_HANDLE;
    return write_to_session(h, &p->id, header->Class.Type, header->Class.Version, (BYTE *)header + sizeof(EVENT_INSTANCE_HEADER), header->Size - sizeof(EVENT_INSTANCE_HEADER));
}
__declspec(dllexport) ULONG WMIAPI CreateTraceInstanceId(HANDLE reg, PEVENT_INSTANCE_INFO info)
{
    static volatile LONG next = 1;
    if (!info || !reg) return ERROR_INVALID_PARAMETER;
    info->RegHandle = reg;
    info->InstanceId = (ULONG)InterlockedIncrement(&next);
    return ERROR_SUCCESS;
}
static ULONG trace_message(TRACEHANDLE h, ULONG flags, LPCGUID guid, USHORT number, va_list args)
{
    BYTE buffer[2048];
    DWORD size = 0;
    (void)flags;
    if (!guid) return ERROR_INVALID_PARAMETER;
    for (;;) {      /* pairs of (pointer, length), ended by a null pointer */
        void *p = va_arg(args, void *);
        size_t n;
        if (!p) break;
        n = va_arg(args, size_t);
        if (n > sizeof(buffer) - size) return ERROR_ARITHMETIC_OVERFLOW;
        memcpy(buffer + size, p, n);
        size += (DWORD)n;
    }
    return write_to_session(h, guid, (UCHAR)number, 0, buffer, size);
}
__declspec(dllexport) ULONG TraceMessageVa(TRACEHANDLE h, ULONG flags, LPCGUID guid, USHORT number, va_list args) { return trace_message(h, flags, guid, number, args); }
__declspec(dllexport) ULONG __cdecl TraceMessage(TRACEHANDLE h, ULONG flags, LPCGUID guid, USHORT number, ...)
{
    va_list args;
    ULONG r;
    va_start(args, number);
    r = trace_message(h, flags, guid, number, args);
    va_end(args);
    return r;
}
__declspec(dllexport) ULONG WMIAPI TraceSetInformation(TRACEHANDLE h, TRACE_INFO_CLASS c, PVOID info, ULONG length)
{
    SessionInfo s;
    (void)info; (void)length;
    if (!find_session_by_handle(h, &s)) return ERROR_INVALID_HANDLE;
    return (int)c >= 0 && c < 16 ? ERROR_SUCCESS : ERROR_INVALID_PARAMETER;
}
__declspec(dllexport) ULONG WMIAPI TraceQueryInformation(TRACEHANDLE h, TRACE_INFO_CLASS c, PVOID info, ULONG length, PULONG returned)
{
    SessionInfo s;
    if (!find_session_by_handle(h, &s)) return ERROR_INVALID_HANDLE;
    if (c == TraceSystemTraceEnableFlagsInfo) {
        if (returned) *returned = sizeof(ULONG);
        if (length < sizeof(ULONG)) return ERROR_INSUFFICIENT_BUFFER;
        *(ULONG *)info = s.enable_flags;
        return ERROR_SUCCESS;
    }
    return ERROR_INVALID_PARAMETER;
}
static ULONG enumerate_guids(PTRACE_GUID_PROPERTIES *array, ULONG count, PULONG returned)
{
    DWORD i, n = 0;
    if (!returned) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < MAX_PROVIDERS; ++i) {
        if (g_providers[i].magic != PROVIDER_MAGIC) continue;
        if (array && n < count && array[n]) {
            memset(array[n], 0, sizeof(*array[n]));
            array[n]->Guid = g_providers[i].id;
            array[n]->IsEnable = g_providers[i].enabled;
            array[n]->EnableLevel = g_providers[i].level;
            array[n]->EnableFlags = (ULONG)g_providers[i].any;
        }
        ++n;
    }
    *returned = n;
    return n > count ? ERROR_MORE_DATA : ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WMIAPI EnumerateTraceGuids(PTRACE_GUID_PROPERTIES *array, ULONG count, PULONG returned) { return enumerate_guids(array, count, returned); }
__declspec(dllexport) ULONG WMIAPI EnumerateTraceGuidsEx(TRACE_QUERY_INFO_CLASS c, PVOID in, ULONG in_size, PVOID out, ULONG out_size, PULONG returned)
{
    DWORD i, n = 0;
    GUID *list = out;
    (void)in; (void)in_size;
    if (!returned) return ERROR_INVALID_PARAMETER;
    if (c != TraceGuidQueryList) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < MAX_PROVIDERS; ++i) if (g_providers[i].magic == PROVIDER_MAGIC) { if ((n + 1) * sizeof(GUID) <= out_size && list) list[n] = g_providers[i].id; ++n; }
    *returned = n * sizeof(GUID);
    return n * sizeof(GUID) > out_size ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS;
}

/* ---- consumers ---------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
    DWORD magic;
    WCHAR file[300], logger[128];
    DWORD mode;
    PEVENT_RECORD_CALLBACK record_callback;
    PEVENT_CALLBACK event_callback;
    PEVENT_TRACE_BUFFER_CALLBACKW buffer_callback;
    PVOID context;
    volatile LONG closed;
    DWORD position;
} Consumer;
#define CONSUMER_MAGIC 0x43534E43u
#define MAX_CONSUMERS 32
static Consumer g_consumers[MAX_CONSUMERS];
typedef struct { GUID guid; PEVENT_CALLBACK callback; } GuidCallback;
static GuidCallback g_guid_callbacks[16];

static Consumer *consumer_of(TRACEHANDLE h)
{
    Consumer *c = (Consumer *)(ULONG_PTR)h;
    if (c < g_consumers || c >= g_consumers + MAX_CONSUMERS || ((ULONG_PTR)c - (ULONG_PTR)g_consumers) % sizeof(Consumer) || c->magic != CONSUMER_MAGIC) return NULL;
    return c;
}
__declspec(dllexport) TRACEHANDLE WMIAPI OpenTraceW(PEVENT_TRACE_LOGFILEW log)
{
    DWORD i;
    Consumer *c = NULL;
    if (!log) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    if (!log->LogFileName && !log->LoggerName) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    if (log->LogFileName && log->LoggerName) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    if (!(log->ProcessTraceMode & PROCESS_TRACE_MODE_EVENT_RECORD) && !log->EventCallback) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    if ((log->ProcessTraceMode & PROCESS_TRACE_MODE_EVENT_RECORD) && !log->EventRecordCallback) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    lock();
    for (i = 0; i < MAX_CONSUMERS; ++i) if (!g_consumers[i].magic) { c = &g_consumers[i]; memset(c, 0, sizeof(*c)); c->magic = CONSUMER_MAGIC; break; }
    unlock();
    if (!c) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    if (log->LogFileName) {
        HANDLE f;
        wcsncpy(c->file, log->LogFileName, 299);
        f = CreateFileW(c->file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f == INVALID_HANDLE_VALUE) { DWORD e = GetLastError(); c->magic = 0; SetLastError(e); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
        CloseHandle(f);
    } else {
        SessionInfo s;
        if (!(log->LogFileMode & EVENT_TRACE_REAL_TIME_MODE) && !(log->ProcessTraceMode & PROCESS_TRACE_MODE_REAL_TIME)) { c->magic = 0; SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
        wcsncpy(c->logger, log->LoggerName, 127);
        if (!find_session_by_name(c->logger, &s)) { c->magic = 0; SetLastError(ERROR_WMI_INSTANCE_NOT_FOUND); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
        session_file(&s, c->file);
    }
    c->mode = log->ProcessTraceMode;
    c->record_callback = log->EventRecordCallback;
    c->event_callback = log->EventCallback;
    c->buffer_callback = log->BufferCallback;
    c->context = log->Context;
    log->IsKernelTrace = FALSE;
    log->LogfileHeader.StartTime.QuadPart = 0;
    return (TRACEHANDLE)(ULONG_PTR)c;
}
__declspec(dllexport) TRACEHANDLE WMIAPI OpenTraceA(PEVENT_TRACE_LOGFILEA log)
{
    EVENT_TRACE_LOGFILEW w;
    WCHAR *file = NULL, *logger = NULL;
    TRACEHANDLE r;
    if (!log) { SetLastError(ERROR_INVALID_PARAMETER); return (TRACEHANDLE)INVALID_HANDLE_VALUE; }
    memcpy(&w, log, sizeof(w) < sizeof(*log) ? sizeof(w) : sizeof(*log));
    w.LogFileName = log->LogFileName ? (file = strdupAW(log->LogFileName)) : NULL;
    w.LoggerName = log->LoggerName ? (logger = strdupAW(log->LoggerName)) : NULL;
    w.BufferCallback = NULL;
    r = OpenTraceW(&w);
    free(file); free(logger);
    return r;
}
__declspec(dllexport) ULONG WMIAPI CloseTrace(TRACEHANDLE h)
{
    Consumer *c = consumer_of(h);
    if (!c) return ERROR_INVALID_HANDLE;
    InterlockedExchange(&c->closed, 1);
    return ERROR_CTX_CLOSE_PENDING;      /* (ProcessTrace calls it back out and the entry goes there) */
}

static DWORD file_size_of(LPCWSTR path)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n;
    if (f == INVALID_HANDLE_VALUE) return 0;
    n = GetFileSize(f, NULL);
    CloseHandle(f);
    return n == INVALID_FILE_SIZE ? 0 : n;
}
/* Gives one stored event to the consumer's callback. */
static void give_event(Consumer *c, const BYTE *record, DWORD total)
{
    const EVENT_HEADER *h = (const EVENT_HEADER *)(record + 4);
    DWORD user_size = *(const DWORD *)(record + 4 + sizeof(EVENT_HEADER));
    const BYTE *user = record + 4 + sizeof(EVENT_HEADER) + 4;
    (void)total;
    if (c->mode & PROCESS_TRACE_MODE_EVENT_RECORD) {
        EVENT_RECORD r;
        memset(&r, 0, sizeof(r));
        r.EventHeader = *h;
        r.BufferContext.LoggerId = (USHORT)h->ProcessorTime;
        r.UserDataLength = (USHORT)user_size;
        r.UserData = (PVOID)user;
        r.UserContext = c->context;
        c->record_callback(&r);
    } else {
        EVENT_TRACE t;
        DWORD i;
        GuidCallback *gc = NULL;
        memset(&t, 0, sizeof(t));
        t.Header.Size = sizeof(EVENT_TRACE_HEADER);
        t.Header.Class.Type = h->EventDescriptor.Opcode;
        t.Header.Class.Level = h->EventDescriptor.Level;
        t.Header.Class.Version = h->EventDescriptor.Version;
        t.Header.ThreadId = h->ThreadId;
        t.Header.ProcessId = h->ProcessId;
        t.Header.TimeStamp = h->TimeStamp;
        t.Header.Guid = h->ProviderId;
        t.MofData = (PVOID)user;
        t.MofLength = user_size;
        t.ClientContext = (ULONG)h->ProcessorTime;
        for (i = 0; i < 16; ++i) if (g_guid_callbacks[i].callback && !memcmp(&g_guid_callbacks[i].guid, &h->ProviderId, sizeof(GUID))) gc = &g_guid_callbacks[i];
        if (gc) gc->callback(&t);
        else if (c->event_callback) c->event_callback(&t);
    }
}
__declspec(dllexport) ULONG WMIAPI ProcessTrace(PTRACEHANDLE handles, ULONG count, LPFILETIME start, LPFILETIME end)
{
    ULONG i;
    ULONGLONG from = start ? ((ULONGLONG)start->dwHighDateTime << 32 | start->dwLowDateTime) : 0, to = end ? ((ULONGLONG)end->dwHighDateTime << 32 | end->dwLowDateTime) : ~0ull;
    BOOL again = TRUE, closed = FALSE;
    if (!handles || !count || count > MAX_CONSUMERS) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < count; ++i) if (!consumer_of(handles[i])) return ERROR_INVALID_HANDLE;
    for (i = 0; i < count; ++i) consumer_of(handles[i])->position = 32;
    while (again) {
        again = FALSE;
        for (i = 0; i < count; ++i) {
            Consumer *c = consumer_of(handles[i]);
            HANDLE f;
            DWORD size, got;
            if (!c) continue;
            if (c->closed) { closed = TRUE; continue; }
            size = file_size_of(c->file);
            f = CreateFileW(c->file, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (f == INVALID_HANDLE_VALUE) continue;
            while (c->position + 8 <= size && !c->closed) {
                DWORD total = 0;
                BYTE *record;
                SetFilePointer(f, (LONG)c->position, NULL, FILE_BEGIN);
                if (!ReadFile(f, &total, 4, &got, NULL) || got != 4 || total < 4 + sizeof(EVENT_HEADER) + 4 || total > MAX_EVENT_BYTES + 256 || c->position + total > size) break;
                if (!(record = malloc(total))) break;
                SetFilePointer(f, (LONG)c->position, NULL, FILE_BEGIN);
                if (!ReadFile(f, record, total, &got, NULL) || got != total) { free(record); break; }
                c->position += total;
                {
                    const EVENT_HEADER *h = (const EVENT_HEADER *)(record + 4);
                    ULONGLONG t = (ULONGLONG)h->TimeStamp.QuadPart;
                    if (t >= from && t <= to) give_event(c, record, total);
                }
                free(record);
            }
            CloseHandle(f);
            if (c->buffer_callback) {
                EVENT_TRACE_LOGFILEW lf;
                memset(&lf, 0, sizeof(lf));
                lf.LogFileName = c->file;
                lf.Context = c->context;
                if (!c->buffer_callback(&lf)) c->closed = 1;
            }
            if (c->logger[0] && !c->closed) {      /* real time: more may come until the session is stopped */
                SessionInfo s;
                if (find_session_by_name(c->logger, &s)) again = TRUE;
                else if (c->position + 8 <= file_size_of(c->file)) again = TRUE;
            }
        }
        if (again) Sleep(50);
    }
    for (i = 0; i < count; ++i) { Consumer *c = consumer_of(handles[i]); if (c) { lock(); c->magic = 0; unlock(); } }
    return closed ? ERROR_CANCELLED : ERROR_SUCCESS;
}
__declspec(dllexport) ULONG WMIAPI SetTraceCallback(LPCGUID guid, PEVENT_CALLBACK callback)
{
    DWORD i;
    if (!guid || !callback) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < 16; ++i) if (!g_guid_callbacks[i].callback || !memcmp(&g_guid_callbacks[i].guid, guid, sizeof(GUID))) { g_guid_callbacks[i].guid = *guid; g_guid_callbacks[i].callback = callback; return ERROR_SUCCESS; }
    return ERROR_NOT_ENOUGH_MEMORY;
}
__declspec(dllexport) ULONG WMIAPI RemoveTraceCallback(LPCGUID guid)
{
    DWORD i;
    if (!guid) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < 16; ++i) if (g_guid_callbacks[i].callback && !memcmp(&g_guid_callbacks[i].guid, guid, sizeof(GUID))) { g_guid_callbacks[i].callback = NULL; return ERROR_SUCCESS; }
    return ERROR_WMI_GUID_NOT_FOUND;
}
__declspec(dllexport) ULONG WMIAPI QueryTraceProcessingHandle(TRACEHANDLE h, ETW_PROCESS_HANDLE_INFO_TYPE c, PVOID in, ULONG in_size, PVOID out, ULONG out_size, PULONG returned)
{
    (void)in; (void)in_size; (void)out; (void)out_size; (void)returned;
    if (!consumer_of(h)) return ERROR_INVALID_HANDLE;
    return c == EtwQueryPartitionInformation ? ERROR_NOT_SUPPORTED : ERROR_INVALID_PARAMETER;
}
