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

/* advapi32.dll: the classic event log (OpenEventLog, ReportEvent, ReadEventLog ...) and the Elf* functions below it. There is no eventlog service: a log is a file,
 * C:\Windows\System32\winevt\Logs\<Log>.evt, holding a header (the number of the oldest record and the next one) and then the EVENTLOGRECORDs themselves, one after the other,
 * in the layout ReadEventLog hands out. Processes take a named mutex while they change a log. A source belongs to the log whose registry key
 * (HKLM\SYSTEM\CurrentControlSet\Services\EventLog\<Log>\<Source>) names it; an unknown source writes to "Application", like the real one. A log keeps at most 512 KB:
 * the oldest records go first. */
#include "security.h"

#define LOG_DIR L"C:\\Windows\\System32\\winevt\\Logs"
#define EVENTLOG_KEY L"SYSTEM\\CurrentControlSet\\Services\\EventLog"
#define LOG_MAGIC 0x4C455443u      /* "CTEL" */
#define LOG_MAX_BYTES (512u * 1024)
#define LOG_FILE_MAX (4u * 1024 * 1024)
#define HANDLE_MAGIC 0x45564C47u
#define MAX_HANDLES 64
#define RECORD_MAGIC 0x654c664cu      /* "LfLe" */

typedef struct { DWORD magic, oldest, next, reserved; } LogHeader;

typedef struct {
    DWORD magic;
    BOOL writer, backup;
    WCHAR log[64], source[128], path[300];
    DWORD last_read;      /* sequential reads: the record number to continue from (0: the start) */
    BOOL read_started;
} LogHandle;
static LogHandle g_logs[MAX_HANDLES];
static volatile LONG g_log_lock;
static void lock_logs(void) { while (InterlockedCompareExchange(&g_log_lock, 1, 0)) Sleep(0); }
static void unlock_logs(void) { InterlockedExchange(&g_log_lock, 0); }

static LogHandle *handle_of(HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    LogHandle *l;
    if (v < 0x45560001u || v > 0x45560000u + MAX_HANDLES) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    l = &g_logs[v - 0x45560001u];
    if (l->magic != HANDLE_MAGIC) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    return l;
}
static HANDLE new_handle(void)
{
    DWORD i;
    HANDLE r = NULL;
    lock_logs();
    for (i = 0; i < MAX_HANDLES; ++i)
        if (!g_logs[i].magic) { memset(&g_logs[i], 0, sizeof(g_logs[i])); g_logs[i].magic = HANDLE_MAGIC; r = (HANDLE)(ULONG_PTR)(0x45560001u + i); break; }
    unlock_logs();
    if (!r) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return r;
}

/* ---- the log files ------------------------------------------------------------------------------------------------------------------------------------------ */

static void log_path(LPCWSTR log, WCHAR *out)
{
    wcscpy(out, LOG_DIR);
    wcscat(out, L"\\");
    wcscat(out, log);
    wcscat(out, L".evt");
}
static void ensure_dirs(void)
{
    CreateDirectoryW(L"C:\\Windows\\System32\\winevt", NULL);
    CreateDirectoryW(LOG_DIR, NULL);
}
static HANDLE named_lock(LPCWSTR path)
{
    WCHAR name[128] = L"TcosEventLog_";
    size_t at = wcslen(name);
    LPCWSTR base = wcsrchr(path, '\\');
    base = base ? base + 1 : path;
    while (*base && at < 120) name[at++] = *base++;
    name[at] = 0;
    return CreateMutexW(NULL, FALSE, name);
}

/* Reads a whole log (file and header) into a block: NULL with the error set if it cannot be read. An empty or missing file is an empty log. */
static BYTE *read_log(LPCWSTR path, DWORD *size, BOOL must_exist)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD n, got = 0;
    BYTE *data;
    if (f == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND && !must_exist) {
            if (!(data = calloc(1, sizeof(LogHeader) + 16))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
            ((LogHeader *)data)->magic = LOG_MAGIC;
            ((LogHeader *)data)->oldest = ((LogHeader *)data)->next = 1;
            *size = sizeof(LogHeader);
            return data;
        }
        return NULL;
    }
    n = GetFileSize(f, NULL);
    if (n == INVALID_FILE_SIZE || n > LOG_FILE_MAX) { CloseHandle(f); SetLastError(ERROR_EVENTLOG_FILE_CORRUPT); return NULL; }
    if (!(data = calloc(1, n + sizeof(LogHeader) + 16))) { CloseHandle(f); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
    if (n && !ReadFile(f, data, n, &got, NULL)) { CloseHandle(f); free(data); return NULL; }
    CloseHandle(f);
    if (n == 0) {
        ((LogHeader *)data)->magic = LOG_MAGIC;
        ((LogHeader *)data)->oldest = ((LogHeader *)data)->next = 1;
        *size = sizeof(LogHeader);
        return data;
    }
    if (n < sizeof(LogHeader) || ((LogHeader *)data)->magic != LOG_MAGIC) { free(data); SetLastError(ERROR_EVENTLOG_FILE_CORRUPT); return NULL; }
    *size = n;
    return data;
}
static BOOL write_log(LPCWSTR path, const BYTE *data, DWORD size)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD done = 0;
    BOOL ok;
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    ok = WriteFile(f, data, size, &done, NULL) && done == size;
    CloseHandle(f);
    return ok;
}
/* Walks the records of a log image: the offsets must chain exactly; false if they do not. */
static BOOL valid_records(const BYTE *data, DWORD size)
{
    DWORD at = sizeof(LogHeader);
    while (at < size) {
        const EVENTLOGRECORD *r = (const EVENTLOGRECORD *)(data + at);
        if (size - at < sizeof(EVENTLOGRECORD) + 4 || r->Length < sizeof(EVENTLOGRECORD) + 4 || r->Length > size - at || r->Length % 4 || r->Reserved != RECORD_MAGIC) return FALSE;
        if (*(const DWORD *)(data + at + r->Length - 4) != r->Length) return FALSE;
        at += r->Length;
    }
    return at == size;
}

/* Which log a source writes to. */
static void log_of_source(LPCWSTR source, WCHAR *log)
{
    HKEY root;
    DWORD i;
    wcscpy(log, L"Application");
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, EVENTLOG_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS) return;
    for (i = 0;; ++i) {
        WCHAR name[64];
        DWORD n = 64;
        HKEY sub, found;
        if (RegEnumKeyExW(root, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (!wcsicmp(name, source)) { wcscpy(log, name); break; }      /* (a log is its own source) */
        if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
        if (RegOpenKeyExW(sub, source, 0, KEY_READ, &found) == ERROR_SUCCESS) { RegCloseKey(found); RegCloseKey(sub); wcscpy(log, name); break; }
        RegCloseKey(sub);
    }
    RegCloseKey(root);
}
static BOOL log_exists(LPCWSTR log)
{
    HKEY root, k;
    BOOL yes = FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, EVENTLOG_KEY, 0, KEY_READ, &root) != ERROR_SUCCESS) return FALSE;
    if (RegOpenKeyExW(root, log, 0, KEY_READ, &k) == ERROR_SUCCESS) { RegCloseKey(k); yes = TRUE; }
    RegCloseKey(root);
    return yes;
}
static BOOL local_machine(LPCWSTR server)
{
    WCHAR name[64];
    DWORD n = 64;
    if (!server || !*server) return TRUE;
    if (server[0] == '\\' && server[1] == '\\') server += 2;
    return GetComputerNameW(name, &n) && (!wcsicmp(server, name) || !wcsicmp(server, L"localhost"));
}
static void make_default_logs(void)
{
    static const WCHAR *const names[] = {L"Application", L"Security", L"System"};
    DWORD i;
    for (i = 0; i < 3; ++i) {
        WCHAR path[128];
        HKEY k;
        wcscpy(path, EVENTLOG_KEY);
        wcscat(path, L"\\");
        wcscat(path, names[i]);
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &k, NULL) == ERROR_SUCCESS) RegCloseKey(k);
    }
}

/* ---- opening and registering ------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) HANDLE WINAPI OpenEventLogW(LPCWSTR server, LPCWSTR source)
{
    HANDLE h;
    LogHandle *l;
    WCHAR log[64];
    if (!source) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!local_machine(server)) { SetLastError(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    make_default_logs();
    if (!log_exists(source)) log_of_source(source, log); else wcsncpy(log, source, 63), log[63] = 0;
    if (!*source) wcscpy(log, L"Application");
    if (!(h = new_handle())) return NULL;
    l = handle_of(h);
    wcscpy(l->log, log);
    wcsncpy(l->source, source, 127);
    log_path(log, l->path);
    return h;
}
__declspec(dllexport) HANDLE WINAPI OpenEventLogA(LPCSTR server, LPCSTR source)
{
    WCHAR *s = strdupAW(server), *n = strdupAW(source);
    HANDLE r = OpenEventLogW(s, n);
    free(s); free(n);
    return r;
}
__declspec(dllexport) HANDLE WINAPI OpenBackupEventLogW(LPCWSTR server, LPCWSTR file)
{
    HANDLE h;
    LogHandle *l;
    DWORD size;
    BYTE *data;
    if (!file) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!local_machine(server)) { SetLastError(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    if (!(data = read_log(file, &size, TRUE))) return NULL;
    if (!valid_records(data, size)) { free(data); SetLastError(ERROR_EVENTLOG_FILE_CORRUPT); return NULL; }
    free(data);
    if (!(h = new_handle())) return NULL;
    l = handle_of(h);
    l->backup = TRUE;
    wcsncpy(l->path, file, 299);
    wcscpy(l->log, L"Backup");
    return h;
}
__declspec(dllexport) HANDLE WINAPI OpenBackupEventLogA(LPCSTR server, LPCSTR file)
{
    WCHAR *s = strdupAW(server), *f = strdupAW(file);
    HANDLE r = OpenBackupEventLogW(s, f);
    free(s); free(f);
    return r;
}
__declspec(dllexport) HANDLE WINAPI RegisterEventSourceW(LPCWSTR server, LPCWSTR source)
{
    HANDLE h;
    LogHandle *l;
    WCHAR log[64];
    if (!source || !*source) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!local_machine(server)) { SetLastError(RPC_S_SERVER_UNAVAILABLE); return NULL; }
    make_default_logs();
    log_of_source(source, log);
    if (!(h = new_handle())) return NULL;
    l = handle_of(h);
    l->writer = TRUE;
    wcscpy(l->log, log);
    wcsncpy(l->source, source, 127);
    log_path(log, l->path);
    return h;
}
__declspec(dllexport) HANDLE WINAPI RegisterEventSourceA(LPCSTR server, LPCSTR source)
{
    WCHAR *s = strdupAW(server), *n = strdupAW(source);
    HANDLE r = RegisterEventSourceW(s, n);
    free(s); free(n);
    return r;
}
__declspec(dllexport) BOOL WINAPI CloseEventLog(HANDLE h)
{
    LogHandle *l = handle_of(h);
    if (!l) return FALSE;
    lock_logs();
    l->magic = 0;
    unlock_logs();
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI DeregisterEventSource(HANDLE h)
{
    LogHandle *l = handle_of(h);
    if (!l) return FALSE;
    if (!l->writer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    return CloseEventLog(h);
}

/* ---- writing ------------------------------------------------------------------------------------------------------------------------------------------------ */

static BOOL report(LogHandle *l, WORD type, WORD category, DWORD id, PSID sid, WORD count, DWORD data_size, LPCWSTR *strings, LPVOID data, LPCWSTR source_override)
{
    WCHAR computer[64];
    DWORD computer_size = 64, sid_len = 0, strings_chars = 0, header, total, i, size = 0, at;
    EVENTLOGRECORD *r;
    BYTE *buffer, *log;
    HANDLE lock;
    LPCWSTR source = source_override ? source_override : l->source;
    FILETIME ft;
    ULONGLONG now;
    LogHeader *h;
    if (!l->writer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (count > 0 && !strings) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (data_size > 0xF000 || count > 256) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!GetComputerNameW(computer, &computer_size)) wcscpy(computer, L"TCOS");
    if (sid) { if (!IsValidSid(sid)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; } sid_len = GetLengthSid(sid); }
    for (i = 0; i < count; ++i) strings_chars += (DWORD)(strings[i] ? wcslen(strings[i]) : 0) + 1;
    header = sizeof(EVENTLOGRECORD) + ((DWORD)wcslen(source) + 1) * 2 + ((DWORD)wcslen(computer) + 1) * 2;
    header = (header + 3) & ~3u;
    total = header + sid_len + strings_chars * 2 + data_size;
    total = ((total + 3) & ~3u) + 4;
    if (!(buffer = calloc(1, total + 8))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    GetSystemTimeAsFileTime(&ft);
    now = ((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10000000ull - 11644473600ull;
    r = (EVENTLOGRECORD *)buffer;
    r->Length = total;
    r->Reserved = RECORD_MAGIC;
    r->TimeGenerated = r->TimeWritten = (DWORD)now;
    r->EventID = id;
    r->EventType = type;
    r->NumStrings = count;
    r->EventCategory = category;
    at = sizeof(EVENTLOGRECORD);
    wcscpy((WCHAR *)(buffer + at), source); at += ((DWORD)wcslen(source) + 1) * 2;
    wcscpy((WCHAR *)(buffer + at), computer);
    at = header;
    r->UserSidLength = sid_len;
    r->UserSidOffset = sid_len ? at : 0;
    if (sid_len) { memcpy(buffer + at, sid, sid_len); at += sid_len; }
    r->StringOffset = at;
    for (i = 0; i < count; ++i) { DWORD n = strings[i] ? (DWORD)wcslen(strings[i]) : 0; if (n) memcpy(buffer + at, strings[i], n * 2); at += (n + 1) * 2; }
    r->DataLength = data_size;
    r->DataOffset = at;
    if (data_size && data) memcpy(buffer + at, data, data_size);
    *(DWORD *)(buffer + total - 4) = total;
    ensure_dirs();
    lock = named_lock(l->path);
    if (lock) WaitForSingleObject(lock, INFINITE);
    log = read_log(l->path, &size, FALSE);
    if (!log || !valid_records(log, size)) {
        if (log) free(log);
        if (!(log = calloc(1, sizeof(LogHeader) + 16))) { if (lock) { ReleaseMutex(lock); CloseHandle(lock); } free(buffer); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        ((LogHeader *)log)->magic = LOG_MAGIC;
        ((LogHeader *)log)->oldest = ((LogHeader *)log)->next = 1;
        size = sizeof(LogHeader);
    }
    h = (LogHeader *)log;
    r->RecordNumber = h->next;
    {
        BYTE *grown = realloc(log, size + total + 16);
        BOOL ok;
        if (!grown) { if (lock) { ReleaseMutex(lock); CloseHandle(lock); } free(log); free(buffer); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        log = grown;
        h = (LogHeader *)log;
        memcpy(log + size, buffer, total);
        size += total;
        h->next++;
        while (size > LOG_MAX_BYTES && size - sizeof(LogHeader) > total) {      /* the oldest record goes */
            EVENTLOGRECORD *first = (EVENTLOGRECORD *)(log + sizeof(LogHeader));
            DWORD len = first->Length;
            memmove(log + sizeof(LogHeader), log + sizeof(LogHeader) + len, size - sizeof(LogHeader) - len);
            size -= len;
            h->oldest++;
        }
        ok = write_log(l->path, log, size);
        free(log);
        if (lock) { ReleaseMutex(lock); CloseHandle(lock); }
        free(buffer);
        if (!ok) { SetLastError(ERROR_EVENTLOG_FILE_CORRUPT); return FALSE; }
    }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI ReportEventW(HANDLE h, WORD type, WORD category, DWORD id, PSID sid, WORD count, DWORD data_size, LPCWSTR *strings, LPVOID data)
{
    LogHandle *l = handle_of(h);
    if (!l) return FALSE;
    return report(l, type, category, id, sid, count, data_size, strings, data, NULL);
}
__declspec(dllexport) BOOL WINAPI ReportEventA(HANDLE h, WORD type, WORD category, DWORD id, PSID sid, WORD count, DWORD data_size, LPCSTR *strings, LPVOID data)
{
    LPCWSTR *wide = NULL;
    WORD i;
    BOOL r;
    if (count && !strings) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (count) {
        if (!(wide = calloc(count, sizeof(*wide)))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        for (i = 0; i < count; ++i) wide[i] = strings[i] ? strdupAW(strings[i]) : NULL;
    }
    r = ReportEventW(h, type, category, id, sid, count, data_size, wide, data);
    for (i = 0; i < count; ++i) free((void *)wide[i]);
    free(wide);
    return r;
}

/* ---- reading ------------------------------------------------------------------------------------------------------------------------------------------------ */

__declspec(dllexport) BOOL WINAPI GetNumberOfEventLogRecords(HANDLE h, PDWORD count)
{
    LogHandle *l = handle_of(h);
    DWORD size;
    BYTE *data;
    if (!l) return FALSE;
    if (!count) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (l->writer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!(data = read_log(l->path, &size, l->backup))) return FALSE;
    *count = ((LogHeader *)data)->next - ((LogHeader *)data)->oldest;
    free(data);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI GetOldestEventLogRecord(HANDLE h, PDWORD oldest)
{
    LogHandle *l = handle_of(h);
    DWORD size;
    BYTE *data;
    if (!l) return FALSE;
    if (!oldest) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (l->writer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!(data = read_log(l->path, &size, l->backup))) return FALSE;
    *oldest = ((LogHeader *)data)->next == ((LogHeader *)data)->oldest ? 0 : ((LogHeader *)data)->oldest;
    free(data);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI GetEventLogInformation(HANDLE h, DWORD level, LPVOID buffer, DWORD size, LPDWORD needed)
{
    LogHandle *l = handle_of(h);
    DWORD file_size;
    BYTE *data;
    if (!l) return FALSE;
    if (level != EVENTLOG_FULL_INFO) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    if (!needed) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *needed = sizeof(EVENTLOG_FULL_INFORMATION);
    if (size < sizeof(EVENTLOG_FULL_INFORMATION)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (!(data = read_log(l->path, &file_size, l->backup))) return FALSE;
    ((EVENTLOG_FULL_INFORMATION *)buffer)->dwFull = file_size + 4096 > LOG_MAX_BYTES;
    free(data);
    return TRUE;
}

#define READ_FLAGS_VALID (EVENTLOG_SEEK_READ | EVENTLOG_SEQUENTIAL_READ | EVENTLOG_FORWARDS_READ | EVENTLOG_BACKWARDS_READ)
static BOOL read_records(HANDLE h, DWORD flags, DWORD offset, LPVOID buffer, DWORD size, DWORD *bytes_read, DWORD *needed)
{
    LogHandle *l = handle_of(h);
    DWORD file_size, count = 0, start;
    BYTE *data;
    BOOL forward, ok = TRUE, seek;
    DWORD *offsets;
    DWORD i, n = 0, first, written = 0, first_size = 0;
    if (!l) return FALSE;
    if (!bytes_read || !needed || (!buffer && size)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (l->writer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if ((flags & ~READ_FLAGS_VALID) || ((flags & EVENTLOG_SEEK_READ) != 0) == ((flags & EVENTLOG_SEQUENTIAL_READ) != 0) ||
        ((flags & EVENTLOG_FORWARDS_READ) != 0) == ((flags & EVENTLOG_BACKWARDS_READ) != 0)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    forward = (flags & EVENTLOG_FORWARDS_READ) != 0;
    seek = (flags & EVENTLOG_SEEK_READ) != 0;
    *bytes_read = *needed = 0;
    if (size < sizeof(EVENTLOGRECORD)) {      /* the size of the first record it would give */
    }
    if (!(data = read_log(l->path, &file_size, l->backup))) return FALSE;
    if (!valid_records(data, file_size)) { free(data); SetLastError(ERROR_EVENTLOG_FILE_CORRUPT); return FALSE; }
    {
        LogHeader *hd = (LogHeader *)data;
        DWORD at = sizeof(LogHeader);
        count = hd->next - hd->oldest;
        offsets = calloc(count + 1, sizeof(DWORD));
        if (!offsets) { free(data); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        for (i = 0; i < count && at < file_size; ++i) { offsets[i] = at; at += ((EVENTLOGRECORD *)(data + at))->Length; }
        count = i;
        if (seek) {
            if (offset < hd->oldest || offset >= hd->oldest + count) { free(offsets); free(data); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
            start = offset - hd->oldest;
        } else if (!l->read_started) start = forward ? 0 : count ? count - 1 : 0;
        else {
            if (l->last_read < hd->oldest) l->last_read = hd->oldest;      /* (records that went since) */
            start = forward ? l->last_read + 1 - hd->oldest : l->last_read - 1 - hd->oldest + 0u;
            if (!forward && l->last_read <= hd->oldest) { free(offsets); free(data); SetLastError(ERROR_HANDLE_EOF); return FALSE; }
            if (forward && start >= count) { free(offsets); free(data); SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        }
        if (count == 0 || start >= count) { free(offsets); free(data); SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        first = start;
        for (i = start;; ) {
            EVENTLOGRECORD *r = (EVENTLOGRECORD *)(data + offsets[i]);
            if (n == 0) first_size = r->Length;
            if (written + r->Length > size) break;
            memcpy((BYTE *)buffer + written, r, r->Length);
            written += r->Length;
            ++n;
            l->last_read = r->RecordNumber;
            if (forward) { if (i + 1 >= count) break; ++i; } else { if (i == 0) break; --i; }
        }
        (void)first;
    }
    l->read_started = TRUE;
    free(offsets);
    free(data);
    if (n == 0) { *needed = first_size; SetLastError(ERROR_INSUFFICIENT_BUFFER); ok = FALSE; }
    *bytes_read = written;
    return ok;
}
__declspec(dllexport) BOOL WINAPI ReadEventLogW(HANDLE h, DWORD flags, DWORD offset, LPVOID buffer, DWORD size, DWORD *read, DWORD *needed)
{
    return read_records(h, flags, offset, buffer, size, read, needed);
}
/* The ANSI form: the same records, strings in the ANSI code page. They are the same bytes here as a record keeps its strings as wide characters; the A form converts. */
__declspec(dllexport) BOOL WINAPI ReadEventLogA(HANDLE h, DWORD flags, DWORD offset, LPVOID buffer, DWORD size, DWORD *read, DWORD *needed)
{
    BYTE *wide;
    DWORD got = 0, at = 0, out = 0;
    BOOL ok;
    if (!buffer || !read || !needed) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(wide = malloc(size * 2 + 4096))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    ok = read_records(h, flags, offset, wide, size * 2 + 4096, &got, needed);
    if (!ok) { free(wide); if (GetLastError() == ERROR_INSUFFICIENT_BUFFER) *needed = *needed / 2 + 64; return FALSE; }
    while (at < got) {
        EVENTLOGRECORD *w = (EVENTLOGRECORD *)(wide + at);
        const WCHAR *source = (const WCHAR *)(wide + at + sizeof(EVENTLOGRECORD));
        const WCHAR *computer = source + wcslen(source) + 1;
        const WCHAR *str;
        char *dest = (char *)buffer + out;
        EVENTLOGRECORD *a = (EVENTLOGRECORD *)dest;
        DWORD pos = sizeof(EVENTLOGRECORD), i, sid_len = w->UserSidLength, strings_size, record_size;
        int n;
        str = (const WCHAR *)(wide + at + w->StringOffset);
        strings_size = 0;
        for (i = 0; i < w->NumStrings; ++i) { DWORD len = (DWORD)wcslen(str); strings_size += WideCharToMultiByte(CP_ACP, 0, str, -1, NULL, 0, NULL, NULL); str += len + 1; }
        record_size = sizeof(EVENTLOGRECORD) + WideCharToMultiByte(CP_ACP, 0, source, -1, NULL, 0, NULL, NULL) + WideCharToMultiByte(CP_ACP, 0, computer, -1, NULL, 0, NULL, NULL);
        record_size = ((record_size + 3) & ~3u) + sid_len + strings_size + w->DataLength;
        record_size = ((record_size + 3) & ~3u) + 4;
        if (out + record_size > size) { if (out == 0) { *needed = record_size; SetLastError(ERROR_INSUFFICIENT_BUFFER); free(wide); return FALSE; } break; }
        memset(a, 0, record_size);
        *a = *w;
        n = WideCharToMultiByte(CP_ACP, 0, source, -1, dest + pos, 256, NULL, NULL); pos += n;
        n = WideCharToMultiByte(CP_ACP, 0, computer, -1, dest + pos, 256, NULL, NULL); pos += n;
        pos = (pos + 3) & ~3u;
        a->UserSidLength = sid_len;
        a->UserSidOffset = sid_len ? pos : 0;
        if (sid_len) { memcpy(dest + pos, wide + at + w->UserSidOffset, sid_len); pos += sid_len; }
        a->StringOffset = pos;
        str = (const WCHAR *)(wide + at + w->StringOffset);
        for (i = 0; i < w->NumStrings; ++i) { n = WideCharToMultiByte(CP_ACP, 0, str, -1, dest + pos, 4096, NULL, NULL); pos += n; str += wcslen(str) + 1; }
        a->DataOffset = pos;
        a->DataLength = w->DataLength;
        if (w->DataLength) memcpy(dest + pos, wide + at + w->DataOffset, w->DataLength);
        a->Length = record_size;
        *(DWORD *)(dest + record_size - 4) = record_size;
        out += record_size;
        at += w->Length;
    }
    *read = out;
    free(wide);
    return TRUE;
}

/* ---- clearing, backing up, waiting -------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) BOOL WINAPI BackupEventLogW(HANDLE h, LPCWSTR file)
{
    LogHandle *l = handle_of(h);
    DWORD size;
    BYTE *data;
    HANDLE out;
    DWORD done;
    BOOL ok;
    if (!l) return FALSE;
    if (!file) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (l->writer) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!(data = read_log(l->path, &size, l->backup))) return FALSE;
    out = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (out == INVALID_HANDLE_VALUE) { free(data); return FALSE; }
    ok = WriteFile(out, data, size, &done, NULL) && done == size;
    CloseHandle(out);
    free(data);
    return ok;
}
__declspec(dllexport) BOOL WINAPI BackupEventLogA(HANDLE h, LPCSTR file)
{
    WCHAR *w = strdupAW(file);
    BOOL r = BackupEventLogW(h, w);
    free(w);
    return r;
}
__declspec(dllexport) BOOL WINAPI ClearEventLogW(HANDLE h, LPCWSTR backup)
{
    LogHandle *l = handle_of(h);
    HANDLE lock;
    DWORD size;
    BYTE *data;
    LogHeader empty;
    BOOL ok;
    if (!l) return FALSE;
    if (l->writer || l->backup) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    ensure_dirs();
    lock = named_lock(l->path);
    if (lock) WaitForSingleObject(lock, INFINITE);
    if (backup && *backup) {
        BYTE *copy = read_log(l->path, &size, FALSE);
        HANDLE out;
        DWORD done;
        if (!copy) { if (lock) { ReleaseMutex(lock); CloseHandle(lock); } return FALSE; }
        out = CreateFileW(backup, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        ok = out != INVALID_HANDLE_VALUE && WriteFile(out, copy, size, &done, NULL) && done == size;
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        free(copy);
        if (!ok) { if (lock) { ReleaseMutex(lock); CloseHandle(lock); } return FALSE; }
    }
    data = read_log(l->path, &size, FALSE);
    memset(&empty, 0, sizeof(empty));
    empty.magic = LOG_MAGIC;
    empty.oldest = empty.next = data ? ((LogHeader *)data)->next : 1;      /* (numbers go on counting) */
    free(data);
    ok = write_log(l->path, (BYTE *)&empty, sizeof(empty));
    if (lock) { ReleaseMutex(lock); CloseHandle(lock); }
    l->read_started = FALSE;
    return ok;
}
__declspec(dllexport) BOOL WINAPI ClearEventLogA(HANDLE h, LPCSTR backup)
{
    WCHAR *w = strdupAW(backup);
    BOOL r = ClearEventLogW(h, w);
    free(w);
    return r;
}

typedef struct { LogHandle *log; HANDLE event; DWORD next; WCHAR path[300]; } LogWatch;
static DWORD WINAPI log_watch_thread(LPVOID parameter)
{
    LogWatch *w = parameter;
    for (;;) {
        DWORD size;
        BYTE *data = read_log(w->path, &size, FALSE);
        DWORD next;
        if (!data) { Sleep(200); continue; }
        next = ((LogHeader *)data)->next;
        free(data);
        if (next != w->next) { SetEvent(w->event); break; }
        Sleep(100);
    }
    free(w);
    return 0;
}
__declspec(dllexport) BOOL WINAPI NotifyChangeEventLog(HANDLE h, HANDLE event)
{
    LogHandle *l = handle_of(h);
    LogWatch *w;
    DWORD size;
    BYTE *data;
    HANDLE t;
    if (!l) return FALSE;
    if (!event) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(w = calloc(1, sizeof(*w)))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &w->event, 0, FALSE, DUPLICATE_SAME_ACCESS)) { free(w); return FALSE; }
    wcscpy(w->path, l->path);
    data = read_log(l->path, &size, FALSE);
    w->next = data ? ((LogHeader *)data)->next : 1;
    free(data);
    if (!(t = CreateThread(NULL, 0, log_watch_thread, w, 0, NULL))) { CloseHandle(w->event); free(w); return FALSE; }
    CloseHandle(t);
    return TRUE;
}

/* ---- Elf*: the native forms ---------------------------------------------------------------------------------------------------------------------------------- */

static NTSTATUS status_of(DWORD error)
{
    switch (error) {
    case ERROR_SUCCESS: return STATUS_SUCCESS;
    case ERROR_ACCESS_DENIED: return STATUS_ACCESS_DENIED;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND;
    case ERROR_INVALID_HANDLE: return STATUS_INVALID_HANDLE;
    case ERROR_INVALID_PARAMETER: return STATUS_INVALID_PARAMETER;
    case ERROR_INSUFFICIENT_BUFFER: return STATUS_BUFFER_TOO_SMALL;
    case ERROR_HANDLE_EOF: return STATUS_END_OF_FILE;
    case ERROR_NOT_ENOUGH_MEMORY: return STATUS_NO_MEMORY;
    case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return STATUS_OBJECT_NAME_COLLISION;
    case ERROR_EVENTLOG_FILE_CORRUPT: return STATUS_EVENTLOG_FILE_CORRUPT;
    default: return STATUS_UNSUCCESSFUL;
    }
}
static WCHAR *unicode_dup(const UNICODE_STRING *s)
{
    WCHAR *r;
    if (!s || !s->Buffer) return NULL;
    if ((r = malloc((size_t)s->Length + 2))) { memcpy(r, s->Buffer, s->Length); r[s->Length / 2] = 0; }
    return r;
}
__declspec(dllexport) NTSTATUS NTAPI ElfOpenEventLogW(PUNICODE_STRING server, PUNICODE_STRING source, PHANDLE out)
{
    WCHAR *s = unicode_dup(server), *n = unicode_dup(source);
    HANDLE h;
    NTSTATUS st;
    if (!out || !n) { free(s); free(n); return STATUS_INVALID_PARAMETER; }
    h = OpenEventLogW(s, n);
    st = h ? STATUS_SUCCESS : status_of(GetLastError());
    free(s); free(n);
    *out = h;
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfOpenEventLogA(PANSI_STRING server, PANSI_STRING source, PHANDLE out)
{
    UNICODE_STRING s = {0}, n = {0};
    NTSTATUS st;
    if (server && RtlAnsiStringToUnicodeString(&s, server, TRUE)) return STATUS_NO_MEMORY;
    if (source && RtlAnsiStringToUnicodeString(&n, source, TRUE)) return STATUS_NO_MEMORY;
    st = ElfOpenEventLogW(server ? &s : NULL, source ? &n : NULL, out);
    RtlFreeUnicodeString(&s); RtlFreeUnicodeString(&n);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfRegisterEventSourceW(PUNICODE_STRING server, PUNICODE_STRING source, PHANDLE out)
{
    WCHAR *s = unicode_dup(server), *n = unicode_dup(source);
    HANDLE h;
    NTSTATUS st;
    if (!out || !n) { free(s); free(n); return STATUS_INVALID_PARAMETER; }
    h = RegisterEventSourceW(s, n);
    st = h ? STATUS_SUCCESS : status_of(GetLastError());
    free(s); free(n);
    *out = h;
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfRegisterEventSourceA(PANSI_STRING server, PANSI_STRING source, PHANDLE out)
{
    UNICODE_STRING s = {0}, n = {0};
    NTSTATUS st;
    if (server && RtlAnsiStringToUnicodeString(&s, server, TRUE)) return STATUS_NO_MEMORY;
    if (source && RtlAnsiStringToUnicodeString(&n, source, TRUE)) return STATUS_NO_MEMORY;
    st = ElfRegisterEventSourceW(server ? &s : NULL, source ? &n : NULL, out);
    RtlFreeUnicodeString(&s); RtlFreeUnicodeString(&n);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfOpenBackupEventLogW(PUNICODE_STRING server, PUNICODE_STRING file, PHANDLE out)
{
    WCHAR *s = unicode_dup(server), *f = unicode_dup(file);
    HANDLE h;
    NTSTATUS st;
    if (!out || !f) { free(s); free(f); return STATUS_INVALID_PARAMETER; }
    h = OpenBackupEventLogW(s, f);
    st = h ? STATUS_SUCCESS : status_of(GetLastError());
    free(s); free(f);
    *out = h;
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfOpenBackupEventLogA(PANSI_STRING server, PANSI_STRING file, PHANDLE out)
{
    UNICODE_STRING s = {0}, f = {0};
    NTSTATUS st;
    if (server && RtlAnsiStringToUnicodeString(&s, server, TRUE)) return STATUS_NO_MEMORY;
    if (file && RtlAnsiStringToUnicodeString(&f, file, TRUE)) return STATUS_NO_MEMORY;
    st = ElfOpenBackupEventLogW(server ? &s : NULL, file ? &f : NULL, out);
    RtlFreeUnicodeString(&s); RtlFreeUnicodeString(&f);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfCloseEventLog(HANDLE h) { return CloseEventLog(h) ? STATUS_SUCCESS : status_of(GetLastError()); }
__declspec(dllexport) NTSTATUS NTAPI ElfDeregisterEventSource(HANDLE h) { return DeregisterEventSource(h) ? STATUS_SUCCESS : status_of(GetLastError()); }
__declspec(dllexport) NTSTATUS NTAPI ElfNumberOfRecords(HANDLE h, PULONG count) { return GetNumberOfEventLogRecords(h, count) ? STATUS_SUCCESS : status_of(GetLastError()); }
__declspec(dllexport) NTSTATUS NTAPI ElfOldestRecord(HANDLE h, PULONG oldest) { return GetOldestEventLogRecord(h, oldest) ? STATUS_SUCCESS : status_of(GetLastError()); }
__declspec(dllexport) NTSTATUS NTAPI ElfFlushEventLog(HANDLE h) { return handle_of(h) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE; }
__declspec(dllexport) NTSTATUS NTAPI ElfChangeNotify(HANDLE h, HANDLE event) { return NotifyChangeEventLog(h, event) ? STATUS_SUCCESS : status_of(GetLastError()); }
__declspec(dllexport) NTSTATUS NTAPI ElfClearEventLogFileW(HANDLE h, PUNICODE_STRING backup)
{
    WCHAR *b = unicode_dup(backup);
    NTSTATUS st = ClearEventLogW(h, b) ? STATUS_SUCCESS : status_of(GetLastError());
    free(b);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfClearEventLogFileA(HANDLE h, PANSI_STRING backup)
{
    UNICODE_STRING b = {0};
    NTSTATUS st;
    if (backup && RtlAnsiStringToUnicodeString(&b, backup, TRUE)) return STATUS_NO_MEMORY;
    st = ElfClearEventLogFileW(h, backup ? &b : NULL);
    RtlFreeUnicodeString(&b);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfBackupEventLogFileW(HANDLE h, PUNICODE_STRING file)
{
    WCHAR *f = unicode_dup(file);
    NTSTATUS st = BackupEventLogW(h, f) ? STATUS_SUCCESS : status_of(GetLastError());
    free(f);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfBackupEventLogFileA(HANDLE h, PANSI_STRING file)
{
    UNICODE_STRING f = {0};
    NTSTATUS st;
    if (file && RtlAnsiStringToUnicodeString(&f, file, TRUE)) return STATUS_NO_MEMORY;
    st = ElfBackupEventLogFileW(h, file ? &f : NULL);
    RtlFreeUnicodeString(&f);
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfReadEventLogW(HANDLE h, ULONG flags, ULONG offset, PVOID buffer, ULONG size, PULONG read, PULONG needed)
{
    return ReadEventLogW(h, flags, offset, buffer, size, read, needed) ? STATUS_SUCCESS : status_of(GetLastError());
}
__declspec(dllexport) NTSTATUS NTAPI ElfReadEventLogA(HANDLE h, ULONG flags, ULONG offset, PVOID buffer, ULONG size, PULONG read, PULONG needed)
{
    return ReadEventLogA(h, flags, offset, buffer, size, read, needed) ? STATUS_SUCCESS : status_of(GetLastError());
}
/* ElfReportEventW(LogHandle, Time, EventType, EventCategory, EventID, NumStrings, DataSize, ComputerName, UserSid, Strings, Data, Flags, RecordNumber, TimeWritten) */
__declspec(dllexport) NTSTATUS NTAPI ElfReportEventW(HANDLE h, USHORT type, USHORT category, ULONG id, PSID sid, USHORT count, ULONG data_size, PUNICODE_STRING *strings, PVOID data, USHORT flags,
                                                     PULONG record_number, PULONG time_written)
{
    LPCWSTR *wide = NULL;
    USHORT i;
    NTSTATUS st;
    LogHandle *l = handle_of(h);
    (void)flags;
    if (!l) return STATUS_INVALID_HANDLE;
    if (count && !strings) return STATUS_INVALID_PARAMETER;
    if (count && !(wide = calloc(count, sizeof(*wide)))) return STATUS_NO_MEMORY;
    for (i = 0; i < count; ++i) wide[i] = unicode_dup(strings[i]);
    st = report(l, type, category, id, sid, count, data_size, wide, data, NULL) ? STATUS_SUCCESS : status_of(GetLastError());
    for (i = 0; i < count; ++i) free((void *)wide[i]);
    free(wide);
    if (record_number) *record_number = 0;
    if (time_written) *time_written = 0;
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfReportEventAndSourceW(HANDLE h, ULONG time, USHORT type, USHORT category, ULONG id, PUNICODE_STRING source, PSID sid, USHORT count, ULONG data_size,
                                                              PUNICODE_STRING *strings, PVOID data, USHORT flags, PULONG record_number, PULONG time_written)
{
    LPCWSTR *wide = NULL;
    USHORT i;
    NTSTATUS st;
    WCHAR *src = unicode_dup(source);
    LogHandle *l = handle_of(h);
    (void)time; (void)flags;
    if (!l) { free(src); return STATUS_INVALID_HANDLE; }
    if (count && !strings) { free(src); return STATUS_INVALID_PARAMETER; }
    if (count && !(wide = calloc(count, sizeof(*wide)))) { free(src); return STATUS_NO_MEMORY; }
    for (i = 0; i < count; ++i) wide[i] = unicode_dup(strings[i]);
    st = report(l, type, category, id, sid, count, data_size, wide, data, src) ? STATUS_SUCCESS : status_of(GetLastError());
    for (i = 0; i < count; ++i) free((void *)wide[i]);
    free(wide);
    free(src);
    if (record_number) *record_number = 0;
    if (time_written) *time_written = 0;
    return st;
}
__declspec(dllexport) NTSTATUS NTAPI ElfReportEventA(HANDLE h, USHORT type, USHORT category, ULONG id, PSID sid, USHORT count, ULONG data_size, PANSI_STRING *strings, PVOID data, USHORT flags,
                                                     PULONG record_number, PULONG time_written)
{
    UNICODE_STRING *wide = NULL;
    PUNICODE_STRING *list = NULL;
    USHORT i;
    NTSTATUS st;
    if (count && !strings) return STATUS_INVALID_PARAMETER;
    if (count) {
        wide = calloc(count, sizeof(*wide));
        list = calloc(count, sizeof(*list));
        if (!wide || !list) { free(wide); free(list); return STATUS_NO_MEMORY; }
        for (i = 0; i < count; ++i) { if (strings[i] && RtlAnsiStringToUnicodeString(&wide[i], strings[i], TRUE)) { free(wide); free(list); return STATUS_NO_MEMORY; } list[i] = &wide[i]; }
    }
    st = ElfReportEventW(h, type, category, id, sid, count, data_size, list, data, flags, record_number, time_written);
    for (i = 0; i < count; ++i) RtlFreeUnicodeString(&wide[i]);
    free(wide); free(list);
    return st;
}
