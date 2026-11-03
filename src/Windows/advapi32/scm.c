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

/* advapi32.dll: the service control manager's client side and the service side (StartServiceCtrlDispatcher and the control handlers). There is no services.exe: the service
 * database is the registry (HKLM\SYSTEM\CurrentControlSet\Services\<name>, the values Windows keeps there), and what the running services are doing is in the registry too, under
 * HKLM\SYSTEM\CurrentControlSet\Control\TcosServiceState\<name> - which, like all of this registry, is gone at the next boot, so every service starts out stopped.
 *
 *   StartService      starts the services it depends on, writes the state "start pending", puts the service's name in the environment of the new process and creates it from ImagePath.
 *   the service       calls StartServiceCtrlDispatcher, which finds its name there, claims the state (its process id) and runs ServiceMain on a thread.
 *   SetServiceStatus  writes the status into the state key.
 *   ControlService    writes the control code into the state key and signals the event "TcosSvcCtl_<name>", which a thread of the service waits on, calls the handler and signals
 *                     "TcosSvcAck_<name>" back.
 * A service whose process ended without saying "stopped" is stopped (ERROR_PROCESS_ABORTED). Drivers cannot be loaded. */
#include "security.h"
#include <winsvc.h>

#define SERVICES_KEY L"SYSTEM\\CurrentControlSet\\Services"
#define STATE_KEY L"SYSTEM\\CurrentControlSet\\Control\\TcosServiceState"
#define LOCK_KEY L"SYSTEM\\CurrentControlSet\\Control\\TcosServiceState\\.lock"
#define MAX_NAME 256
typedef struct { DWORD dwLaunchProtected; } SERVICE_LAUNCH_PROTECTED_INFO;
typedef struct { DWORD cTriggers; PVOID pTriggers; PBYTE pReserved; } SERVICE_TRIGGER_INFO;
typedef struct { USHORT usPreferredNode; BOOLEAN fDelete; } SERVICE_PREFERRED_NODE_INFO;
#define MAX_START_WAIT_MS 30000

/* ---- registry helpers -------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL valid_service_name(LPCWSTR name)
{
    size_t n = 0;
    if (!name) return FALSE;
    for (; name[n]; ++n) if (name[n] == '\\' || name[n] == '/' || n >= MAX_NAME) return FALSE;
    return n != 0;
}
static void service_path(LPCWSTR base, LPCWSTR name, WCHAR *out)
{
    wcscpy(out, base);
    wcscat(out, L"\\");
    wcscat(out, name);
}
static LONG open_service_key(LPCWSTR name, REGSAM access, HKEY *key)
{
    WCHAR path[600];
    service_path(SERVICES_KEY, name, path);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, access, key);
}
static LONG open_state_key(LPCWSTR name, BOOL create, HKEY *key)
{
    WCHAR path[600];
    service_path(STATE_KEY, name, path);
    if (create) return RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, key, NULL);
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_ALL_ACCESS, key);
}
static DWORD get_dword(HKEY key, LPCWSTR name, DWORD fallback)
{
    DWORD type = 0, value = 0, size = sizeof(value);
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&value, &size) != ERROR_SUCCESS || type != REG_DWORD || size != sizeof(value)) return fallback;
    return value;
}
static void set_dword(HKEY key, LPCWSTR name, DWORD value) { RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value)); }
/* A string value (REG_SZ or REG_EXPAND_SZ) in a new block, or NULL. */
static WCHAR *get_string(HKEY key, LPCWSTR name, DWORD *type_out)
{
    DWORD type = 0, size = 0;
    WCHAR *text;
    if (RegQueryValueExW(key, name, NULL, &type, NULL, &size) != ERROR_SUCCESS) return NULL;
    if (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ) return NULL;
    if (!(text = malloc((size_t)size + 8))) return NULL;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)text, &size) != ERROR_SUCCESS) { free(text); return NULL; }
    text[size / 2] = 0;
    text[size / 2 + 1] = 0;
    if (type_out) *type_out = type;
    return text;
}
static void set_string(HKEY key, LPCWSTR name, LPCWSTR text, DWORD type)
{
    if (!text) { RegDeleteValueW(key, name); return; }
    RegSetValueExW(key, name, 0, type, (const BYTE *)text, (DWORD)(wcslen(text) + 1) * 2);
}
/* A MULTI_SZ made from a list of names (or NULL); the length in characters with both NULs. */
static DWORD multi_length(LPCWSTR list)
{
    DWORD n = 0;
    if (!list) return 0;
    while (list[n]) n += (DWORD)wcslen(list + n) + 1;
    return n + 1;
}
static void set_multi(HKEY key, LPCWSTR name, LPCWSTR list)
{
    if (!list || !*list) { RegDeleteValueW(key, name); return; }
    RegSetValueExW(key, name, 0, REG_MULTI_SZ, (const BYTE *)list, multi_length(list) * 2);
}
static WCHAR *dup_wide(LPCWSTR s)
{
    WCHAR *r;
    if (!s) return NULL;
    if ((r = malloc((wcslen(s) + 1) * 2))) wcscpy(r, s);
    return r;
}

/* ---- handles ------------------------------------------------------------------------------------------------------------------------------------------------ */

enum { H_FREE, H_MANAGER, H_SERVICE, H_LOCK };
typedef struct { DWORD kind, access; WCHAR name[MAX_NAME + 1]; } ScHandle;
#define SC_SLOTS 256
#define SC_BASE 0x53430000u
static ScHandle g_handles[SC_SLOTS];
static volatile LONG g_handle_lock;

static void handles_lock(void) { while (InterlockedCompareExchange(&g_handle_lock, 1, 0)) Sleep(0); }
static void handles_unlock(void) { InterlockedExchange(&g_handle_lock, 0); }
static SC_HANDLE new_handle(DWORD kind, DWORD access, LPCWSTR name)
{
    DWORD i;
    SC_HANDLE result = NULL;
    handles_lock();
    for (i = 0; i < SC_SLOTS; ++i)
        if (g_handles[i].kind == H_FREE) {
            g_handles[i].kind = kind;
            g_handles[i].access = access;
            g_handles[i].name[0] = 0;
            if (name) { wcsncpy(g_handles[i].name, name, MAX_NAME); g_handles[i].name[MAX_NAME] = 0; }
            result = (SC_HANDLE)(ULONG_PTR)(SC_BASE + i + 1);
            break;
        }
    handles_unlock();
    if (!result) SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return result;
}
/* The slot a handle names, if it is of the kind; with the access asked for. Sets the error if not. */
static ScHandle *find_handle(SC_HANDLE h, DWORD kind, DWORD access)
{
    ULONG_PTR v = (ULONG_PTR)h;
    ScHandle *s;
    if (v <= SC_BASE || v > SC_BASE + SC_SLOTS) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    s = &g_handles[v - SC_BASE - 1];
    if (s->kind != kind) { SetLastError(ERROR_INVALID_HANDLE); return NULL; }
    if (access && (s->access & access) != access) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    return s;
}

/* ---- the state of a service -------------------------------------------------------------------------------------------------------------------------------- */

static BOOL process_alive(DWORD pid)
{
    HANDLE h;
    BOOL alive;
    if (pid == 0) return FALSE;
    if (pid == GetCurrentProcessId()) return TRUE;
    h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return FALSE;
    alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

static DWORD service_type_of(LPCWSTR name)
{
    HKEY key;
    DWORD type = SERVICE_WIN32_OWN_PROCESS;
    if (open_service_key(name, KEY_READ, &key) == ERROR_SUCCESS) { type = get_dword(key, L"Type", SERVICE_WIN32_OWN_PROCESS); RegCloseKey(key); }
    return type;
}

/* The status of a service, as the service last said it; stopped if there is none or its process is gone. */
static void read_status(LPCWSTR name, SERVICE_STATUS_PROCESS *status)
{
    HKEY key;
    DWORD size = sizeof(*status), type = 0;
    memset(status, 0, sizeof(*status));
    status->dwServiceType = service_type_of(name);
    status->dwCurrentState = SERVICE_STOPPED;
    if (open_state_key(name, FALSE, &key) != ERROR_SUCCESS) return;
    {
        SERVICE_STATUS_PROCESS stored;
        if (RegQueryValueExW(key, L"Status", NULL, &type, (BYTE *)&stored, &size) == ERROR_SUCCESS && type == REG_BINARY && size == sizeof(stored)) {
            *status = stored;
            status->dwServiceType = service_type_of(name) | (stored.dwServiceType & SERVICE_INTERACTIVE_PROCESS);
        }
    }
    RegCloseKey(key);
    if (status->dwCurrentState != SERVICE_STOPPED && status->dwProcessId != 0 && !process_alive(status->dwProcessId)) {
        status->dwCurrentState = SERVICE_STOPPED;
        status->dwControlsAccepted = 0;
        status->dwWin32ExitCode = ERROR_PROCESS_ABORTED;
        status->dwCheckPoint = status->dwWaitHint = 0;
        status->dwProcessId = 0;
    }
}
static void write_status(LPCWSTR name, const SERVICE_STATUS_PROCESS *status)
{
    HKEY key;
    if (open_state_key(name, TRUE, &key) != ERROR_SUCCESS) return;
    RegSetValueExW(key, L"Status", 0, REG_BINARY, (const BYTE *)status, sizeof(*status));
    RegCloseKey(key);
}

/* ---- the manager and the services: open, create, delete ----------------------------------------------------------------------------------------------------- */

__declspec(dllexport) SC_HANDLE WINAPI OpenSCManagerW(LPCWSTR machine, LPCWSTR database, DWORD access)
{
    if (machine && *machine) {
        WCHAR name[64];
        DWORD n = 64;
        LPCWSTR m = machine;
        if (m[0] == '\\' && m[1] == '\\') m += 2;
        if (!GetComputerNameW(name, &n) || (wcsicmp(m, name) && wcsicmp(m, L"localhost"))) { SetLastError(ERROR_BAD_NETPATH); return NULL; }
    }
    if (database && wcsicmp(database, L"ServicesActive")) {
        SetLastError(!wcsicmp(database, L"ServicesFailed") ? ERROR_DATABASE_DOES_NOT_EXIST : ERROR_INVALID_NAME);
        return NULL;
    }
    if (access & GENERIC_ALL) access = SC_MANAGER_ALL_ACCESS;
    if (access & GENERIC_READ) access |= SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_QUERY_LOCK_STATUS | STANDARD_RIGHTS_READ;
    if (access & GENERIC_WRITE) access |= SC_MANAGER_CREATE_SERVICE | SC_MANAGER_MODIFY_BOOT_CONFIG | STANDARD_RIGHTS_WRITE;
    if (access & GENERIC_EXECUTE) access |= SC_MANAGER_CONNECT | SC_MANAGER_LOCK;
    return new_handle(H_MANAGER, access | SC_MANAGER_CONNECT, NULL);
}
__declspec(dllexport) SC_HANDLE WINAPI OpenSCManagerA(LPCSTR machine, LPCSTR database, DWORD access)
{
    WCHAR *m = strdupAW(machine), *d = strdupAW(database);
    SC_HANDLE r = OpenSCManagerW(m, d, access);
    free(m);
    free(d);
    return r;
}
__declspec(dllexport) BOOL WINAPI CloseServiceHandle(SC_HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    BOOL ok = FALSE;
    if (v > SC_BASE && v <= SC_BASE + SC_SLOTS) {
        ScHandle *s = &g_handles[v - SC_BASE - 1];
        handles_lock();
        if (s->kind == H_LOCK) { HKEY key; if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) { RegDeleteTreeW(key, L".lock"); RegCloseKey(key); } }
        if (s->kind != H_FREE) { s->kind = H_FREE; ok = TRUE; }
        handles_unlock();
    }
    if (!ok) SetLastError(ERROR_INVALID_HANDLE);
    return ok;
}

static DWORD map_service_access(DWORD access)
{
    if (access & GENERIC_ALL) access = SERVICE_ALL_ACCESS;
    if (access & GENERIC_READ) access |= SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_INTERROGATE | SERVICE_ENUMERATE_DEPENDENTS | READ_CONTROL;
    if (access & GENERIC_WRITE) access |= SERVICE_CHANGE_CONFIG | WRITE_DAC;
    if (access & GENERIC_EXECUTE) access |= SERVICE_START | SERVICE_STOP | SERVICE_PAUSE_CONTINUE | SERVICE_USER_DEFINED_CONTROL;
    return access;
}
__declspec(dllexport) SC_HANDLE WINAPI OpenServiceW(SC_HANDLE manager, LPCWSTR name, DWORD access)
{
    HKEY key;
    LONG r;
    if (!find_handle(manager, H_MANAGER, SC_MANAGER_CONNECT)) return NULL;
    if (!name) { SetLastError(ERROR_INVALID_ADDRESS); return NULL; }
    if (!valid_service_name(name)) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    r = open_service_key(name, KEY_READ, &key);
    if (r == ERROR_SUCCESS) {
        BOOL is_service = get_dword(key, L"Type", 0) != 0;
        RegCloseKey(key);
        if (!is_service) r = ERROR_FILE_NOT_FOUND;
    }
    if (r != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return NULL; }
    return new_handle(H_SERVICE, map_service_access(access), name);
}
__declspec(dllexport) SC_HANDLE WINAPI OpenServiceA(SC_HANDLE manager, LPCSTR name, DWORD access)
{
    WCHAR *w = strdupAW(name);
    SC_HANDLE r = OpenServiceW(manager, w, access);
    free(w);
    return r;
}

/* A display name and a key name are different services' if they match another's key name or display name. */
static BOOL name_in_use(LPCWSTR name, BOOL as_display, LPCWSTR except_key)
{
    HKEY services;
    DWORD i;
    WCHAR key_name[MAX_NAME + 2];
    BOOL found = FALSE;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY, 0, KEY_READ, &services) != ERROR_SUCCESS) return FALSE;
    for (i = 0; !found; ++i) {
        DWORD n = MAX_NAME + 1;
        HKEY key;
        if (RegEnumKeyExW(services, i, key_name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (except_key && !wcsicmp(key_name, except_key)) continue;
        if (!as_display && !wcsicmp(key_name, name)) { found = TRUE; break; }
        if (RegOpenKeyExW(services, key_name, 0, KEY_READ, &key) == ERROR_SUCCESS) {
            if (get_dword(key, L"Type", 0)) {
                WCHAR *display = get_string(key, L"DisplayName", NULL);
                if (display && !wcsicmp(display, name)) found = TRUE;
                free(display);
            }
            RegCloseKey(key);
        }
    }
    RegCloseKey(services);
    return found;
}

static BOOL valid_service_type(DWORD type)
{
    switch (type & ~SERVICE_INTERACTIVE_PROCESS) {
    case SERVICE_WIN32_OWN_PROCESS: case SERVICE_WIN32_SHARE_PROCESS: case SERVICE_KERNEL_DRIVER: case SERVICE_FILE_SYSTEM_DRIVER: case SERVICE_ADAPTER: case SERVICE_RECOGNIZER_DRIVER:
        return TRUE;
    default: return FALSE;
    }
}

__declspec(dllexport) SC_HANDLE WINAPI CreateServiceW(SC_HANDLE manager, LPCWSTR name, LPCWSTR display, DWORD access, DWORD type, DWORD start, DWORD error_control, LPCWSTR binary,
                                                     LPCWSTR group, LPDWORD tag, LPCWSTR dependencies, LPCWSTR account, LPCWSTR password)
{
    HKEY key;
    WCHAR path[600];
    DWORD disposition = 0;
    BOOL driver;
    (void)tag; (void)password;
    if (!find_handle(manager, H_MANAGER, SC_MANAGER_CREATE_SERVICE)) return NULL;
    if (!name || !*name) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    if (!valid_service_name(name)) { SetLastError(ERROR_INVALID_NAME); return NULL; }
    if (!valid_service_type(type)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (start > SERVICE_DISABLED || error_control > SERVICE_ERROR_CRITICAL) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    driver = (type & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER | SERVICE_ADAPTER | SERVICE_RECOGNIZER_DRIVER)) != 0;
    if (!binary && !driver) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!driver && start == SERVICE_BOOT_START) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if ((type & SERVICE_INTERACTIVE_PROCESS) && account && wcsicmp(account, L"LocalSystem")) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (dependencies && dependencies[0] == '/') { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    service_path(SERVICES_KEY, name, path);
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, &disposition) != ERROR_SUCCESS) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    if (disposition == REG_OPENED_EXISTING_KEY && get_dword(key, L"Type", 0)) {
        RegCloseKey(key);
        SetLastError(get_dword(key, L"DeleteFlag", 0) ? ERROR_SERVICE_MARKED_FOR_DELETE : ERROR_SERVICE_EXISTS);
        return NULL;
    }
    if (display && *display && name_in_use(display, TRUE, name)) { RegCloseKey(key); RegDeleteKeyW(HKEY_LOCAL_MACHINE, path); SetLastError(ERROR_DUPLICATE_SERVICE_NAME); return NULL; }
    set_dword(key, L"Type", type);
    set_dword(key, L"Start", start);
    set_dword(key, L"ErrorControl", error_control);
    if (binary) set_string(key, L"ImagePath", binary, REG_EXPAND_SZ);
    if (display && *display) set_string(key, L"DisplayName", display, REG_SZ);
    if (group && *group) set_string(key, L"Group", group, REG_SZ);
    set_multi(key, L"DependOnService", dependencies);
    if (account && *account) set_string(key, L"ObjectName", account, REG_SZ);
    else if (!driver) set_string(key, L"ObjectName", L"LocalSystem", REG_SZ);
    RegCloseKey(key);
    return new_handle(H_SERVICE, map_service_access(access), name);
}
__declspec(dllexport) SC_HANDLE WINAPI CreateServiceA(SC_HANDLE manager, LPCSTR name, LPCSTR display, DWORD access, DWORD type, DWORD start, DWORD error_control, LPCSTR binary,
                                                     LPCSTR group, LPDWORD tag, LPCSTR dependencies, LPCSTR account, LPCSTR password)
{
    WCHAR *wn = strdupAW(name), *wd = strdupAW(display), *wb = strdupAW(binary), *wg = strdupAW(group), *wa = strdupAW(account), *wp = strdupAW(password), *deps = NULL;
    SC_HANDLE r;
    if (dependencies) {      /* a list of names: each one converted, with the final NUL */
        size_t chars = 0, at = 0;
        const char *p;
        for (p = dependencies; *p; p += strlen(p) + 1) chars += MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
        if ((deps = malloc((chars + 2) * 2))) {
            for (p = dependencies; *p; p += strlen(p) + 1) at += MultiByteToWideChar(CP_ACP, 0, p, -1, deps + at, (int)(chars + 1 - at));
            deps[at] = 0;
        }
    }
    r = CreateServiceW(manager, wn, wd, access, type, start, error_control, wb, wg, tag, deps, wa, wp);
    free(wn); free(wd); free(wb); free(wg); free(wa); free(wp); free(deps);
    return r;
}

__declspec(dllexport) BOOL WINAPI DeleteService(SC_HANDLE h)
{
    ScHandle *s = find_handle(h, H_SERVICE, DELETE);
    HKEY key;
    SERVICE_STATUS_PROCESS status;
    WCHAR path[600];
    if (!s) return FALSE;
    if (open_service_key(s->name, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE); return FALSE; }
    if (get_dword(key, L"DeleteFlag", 0)) { RegCloseKey(key); SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE); return FALSE; }
    RegCloseKey(key);
    read_status(s->name, &status);
    if (status.dwCurrentState != SERVICE_STOPPED) {      /* a running service goes when it stops: marked for now */
        if (open_service_key(s->name, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) { set_dword(key, L"DeleteFlag", 1); RegCloseKey(key); }
        return TRUE;
    }
    service_path(SERVICES_KEY, s->name, path);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY, 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) {
        LONG r = RegDeleteTreeW(key, s->name);
        RegCloseKey(key);
        if (r != ERROR_SUCCESS) { SetLastError(r); return FALSE; }
    }
    service_path(STATE_KEY, s->name, path);
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) { RegDeleteTreeW(key, s->name); RegCloseKey(key); }
    return TRUE;
}

/* A service that was marked for deletion while it ran goes once it is stopped (seen by whoever looks next). */
static void reap_deleted(LPCWSTR name)
{
    HKEY key;
    BOOL marked = FALSE;
    if (open_service_key(name, KEY_READ, &key) == ERROR_SUCCESS) { marked = get_dword(key, L"DeleteFlag", 0) != 0; RegCloseKey(key); }
    if (marked) {
        SERVICE_STATUS_PROCESS status;
        read_status(name, &status);
        if (status.dwCurrentState == SERVICE_STOPPED && RegOpenKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY, 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) { RegDeleteTreeW(key, name); RegCloseKey(key); }
    }
}

/* ---- status ------------------------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) BOOL WINAPI QueryServiceStatusEx(SC_HANDLE h, SC_STATUS_TYPE level, LPBYTE buffer, DWORD size, LPDWORD needed)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS status;
    if (!s) return FALSE;
    if (level != SC_STATUS_PROCESS_INFO) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    *needed = sizeof(status);
    if (size < sizeof(status)) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    reap_deleted(s->name);
    read_status(s->name, &status);
    memcpy(buffer, &status, sizeof(status));
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI QueryServiceStatus(SC_HANDLE h, LPSERVICE_STATUS out)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_QUERY_STATUS);
    SERVICE_STATUS_PROCESS status;
    if (!s) return FALSE;
    if (!out) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    read_status(s->name, &status);
    memcpy(out, &status, sizeof(*out));
    return TRUE;
}

/* ---- configuration ------------------------------------------------------------------------------------------------------------------------------------------ */

/* The strings of a QUERY_SERVICE_CONFIG are packed after the structure; the size is what all of it needs. */
typedef struct { DWORD type, start, error_control; WCHAR *binary, *group, *deps, *account, *display; DWORD tag; } Config;
static void free_config(Config *c) { free(c->binary); free(c->group); free(c->deps); free(c->account); free(c->display); memset(c, 0, sizeof(*c)); }
static BOOL load_config(LPCWSTR name, Config *c)
{
    HKEY key;
    memset(c, 0, sizeof(*c));
    if (open_service_key(name, KEY_READ, &key) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    c->type = get_dword(key, L"Type", 0);
    c->start = get_dword(key, L"Start", SERVICE_DEMAND_START);
    c->error_control = get_dword(key, L"ErrorControl", SERVICE_ERROR_NORMAL);
    c->tag = get_dword(key, L"Tag", 0);
    c->binary = get_string(key, L"ImagePath", NULL);
    c->group = get_string(key, L"Group", NULL);
    c->deps = get_string(key, L"DependOnService", NULL);
    c->account = get_string(key, L"ObjectName", NULL);
    c->display = get_string(key, L"DisplayName", NULL);
    RegCloseKey(key);
    return TRUE;
}
static DWORD wsize(const WCHAR *s) { return s ? (DWORD)(wcslen(s) + 1) * 2 : 2; }
static WCHAR *put_wide(BYTE **at, const WCHAR *s)
{
    WCHAR *r = (WCHAR *)*at;
    if (s) memcpy(r, s, wsize(s)); else r[0] = 0;
    *at += wsize(s);
    return r;
}

__declspec(dllexport) BOOL WINAPI QueryServiceConfigW(SC_HANDLE h, LPQUERY_SERVICE_CONFIGW out, DWORD size, LPDWORD needed)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_QUERY_CONFIG);
    Config c;
    DWORD deps_chars, total;
    BYTE *at;
    if (!s) return FALSE;
    if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (!load_config(s->name, &c)) return FALSE;
    deps_chars = c.deps ? multi_length(c.deps) : 1;
    total = (DWORD)sizeof(QUERY_SERVICE_CONFIGW) + wsize(c.binary) + wsize(c.group) + deps_chars * 2 + wsize(c.account) + wsize(c.display);
    *needed = total;
    if (size < total || !out) { free_config(&c); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    out->dwServiceType = c.type;
    out->dwStartType = c.start;
    out->dwErrorControl = c.error_control;
    out->dwTagId = c.tag;
    at = (BYTE *)(out + 1);
    out->lpBinaryPathName = put_wide(&at, c.binary);
    out->lpLoadOrderGroup = put_wide(&at, c.group);
    out->lpDependencies = (WCHAR *)at;
    if (c.deps) memcpy(at, c.deps, deps_chars * 2); else ((WCHAR *)at)[0] = 0;
    at += deps_chars * 2;
    out->lpServiceStartName = put_wide(&at, c.account);
    out->lpDisplayName = put_wide(&at, c.display ? c.display : s->name);
    free_config(&c);
    return TRUE;
}

/* The ANSI form: the strings are converted into the tail of the caller's buffer. */
static char *put_ansi(BYTE **at, const WCHAR *s)
{
    char *r = (char *)*at;
    int n = s ? WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL) : 1;
    if (s) WideCharToMultiByte(CP_ACP, 0, s, -1, r, n, NULL, NULL); else r[0] = 0;
    *at += n;
    return r;
}
static DWORD asize(const WCHAR *s) { return s ? (DWORD)WideCharToMultiByte(CP_ACP, 0, s, -1, NULL, 0, NULL, NULL) : 1; }
__declspec(dllexport) BOOL WINAPI QueryServiceConfigA(SC_HANDLE h, LPQUERY_SERVICE_CONFIGA out, DWORD size, LPDWORD needed)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_QUERY_CONFIG);
    Config c;
    DWORD deps_bytes = 1, total;
    char *deps_a = NULL;
    BYTE *at;
    if (!s) return FALSE;
    if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (!load_config(s->name, &c)) return FALSE;
    if (c.deps) {
        const WCHAR *p;
        deps_bytes = 0;
        for (p = c.deps; *p; p += wcslen(p) + 1) deps_bytes += asize(p);
        deps_bytes += 1;
    }
    total = (DWORD)sizeof(QUERY_SERVICE_CONFIGA) + asize(c.binary) + asize(c.group) + deps_bytes + asize(c.account) + asize(c.display ? c.display : s->name);
    *needed = total;
    if (size < total || !out) { free_config(&c); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    out->dwServiceType = c.type;
    out->dwStartType = c.start;
    out->dwErrorControl = c.error_control;
    out->dwTagId = c.tag;
    at = (BYTE *)(out + 1);
    out->lpBinaryPathName = put_ansi(&at, c.binary);
    out->lpLoadOrderGroup = put_ansi(&at, c.group);
    deps_a = (char *)at;
    if (c.deps) {
        const WCHAR *p;
        char *w = deps_a;
        for (p = c.deps; *p; p += wcslen(p) + 1) { int n = WideCharToMultiByte(CP_ACP, 0, p, -1, w, 512, NULL, NULL); w += n; }
        *w = 0;
    } else deps_a[0] = 0;
    out->lpDependencies = deps_a;
    at += deps_bytes;
    out->lpServiceStartName = put_ansi(&at, c.account);
    out->lpDisplayName = put_ansi(&at, c.display ? c.display : s->name);
    free_config(&c);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI ChangeServiceConfigW(SC_HANDLE h, DWORD type, DWORD start, DWORD error_control, LPCWSTR binary, LPCWSTR group, LPDWORD tag, LPCWSTR dependencies,
                                                      LPCWSTR account, LPCWSTR password, LPCWSTR display)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_CHANGE_CONFIG);
    HKEY key;
    DWORD current_type;
    (void)password;
    if (!s) return FALSE;
    if (open_service_key(s->name, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE); return FALSE; }
    if (get_dword(key, L"DeleteFlag", 0)) { RegCloseKey(key); SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE); return FALSE; }
    current_type = get_dword(key, L"Type", 0);
    if (type != SERVICE_NO_CHANGE && !valid_service_type(type)) { RegCloseKey(key); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (start != SERVICE_NO_CHANGE && start > SERVICE_DISABLED) { RegCloseKey(key); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (error_control != SERVICE_NO_CHANGE && error_control > SERVICE_ERROR_CRITICAL) { RegCloseKey(key); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (type != SERVICE_NO_CHANGE && ((type ^ current_type) & (SERVICE_KERNEL_DRIVER | SERVICE_FILE_SYSTEM_DRIVER)) && FALSE) { RegCloseKey(key); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (display && *display && name_in_use(display, TRUE, s->name)) { RegCloseKey(key); SetLastError(ERROR_DUPLICATE_SERVICE_NAME); return FALSE; }
    if (type != SERVICE_NO_CHANGE) set_dword(key, L"Type", type);
    if (start != SERVICE_NO_CHANGE) set_dword(key, L"Start", start);
    if (error_control != SERVICE_NO_CHANGE) set_dword(key, L"ErrorControl", error_control);
    if (binary) set_string(key, L"ImagePath", binary, REG_EXPAND_SZ);
    if (group) set_string(key, L"Group", *group ? group : NULL, REG_SZ);
    if (dependencies) set_multi(key, L"DependOnService", dependencies);
    if (account) set_string(key, L"ObjectName", account, REG_SZ);
    if (display) set_string(key, L"DisplayName", *display ? display : NULL, REG_SZ);
    if (tag) *tag = 0;
    RegCloseKey(key);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI ChangeServiceConfigA(SC_HANDLE h, DWORD type, DWORD start, DWORD error_control, LPCSTR binary, LPCSTR group, LPDWORD tag, LPCSTR dependencies,
                                                      LPCSTR account, LPCSTR password, LPCSTR display)
{
    WCHAR *wb = strdupAW(binary), *wg = strdupAW(group), *wa = strdupAW(account), *wp = strdupAW(password), *wd = strdupAW(display), *deps = NULL;
    BOOL r;
    if (dependencies) {
        size_t chars = 0, at = 0;
        const char *p;
        for (p = dependencies; *p; p += strlen(p) + 1) chars += MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
        if ((deps = malloc((chars + 2) * 2))) {
            for (p = dependencies; *p; p += strlen(p) + 1) at += MultiByteToWideChar(CP_ACP, 0, p, -1, deps + at, (int)(chars + 1 - at));
            deps[at] = 0;
        }
    }
    r = ChangeServiceConfigW(h, type, start, error_control, wb, wg, tag, deps, wa, wp, wd);
    free(wb); free(wg); free(wa); free(wp); free(wd); free(deps);
    return r;
}

/* ---- the optional configuration (levels of ChangeServiceConfig2 / QueryServiceConfig2) ---------------------------------------------------------------------- */

__declspec(dllexport) BOOL WINAPI ChangeServiceConfig2W(SC_HANDLE h, DWORD level, LPVOID info)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_CHANGE_CONFIG);
    HKEY key;
    if (!s) return FALSE;
    if (!info) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (open_service_key(s->name, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE); return FALSE; }
    switch (level) {
    case SERVICE_CONFIG_DESCRIPTION: {
        SERVICE_DESCRIPTIONW *d = info;
        if (!d->lpDescription || !*d->lpDescription) RegDeleteValueW(key, L"Description"); else set_string(key, L"Description", d->lpDescription, REG_SZ);
        break;
    }
    case SERVICE_CONFIG_FAILURE_ACTIONS: {
        SERVICE_FAILURE_ACTIONSW *f = info;
        DWORD i, blob_size = 12 + f->cActions * 8, *blob;
        if (f->cActions && !f->lpsaActions) { RegCloseKey(key); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        if (f->dwResetPeriod != 0xFFFFFFFFu || f->cActions || f->lpsaActions) {
            if (!(blob = calloc(1, blob_size))) { RegCloseKey(key); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
            blob[0] = f->dwResetPeriod;
            blob[1] = 0;
            blob[2] = f->cActions;
            for (i = 0; i < f->cActions; ++i) { blob[3 + 2 * i] = f->lpsaActions[i].Type; blob[4 + 2 * i] = f->lpsaActions[i].Delay; }
            RegSetValueExW(key, L"FailureActions", 0, REG_BINARY, (BYTE *)blob, blob_size);
            free(blob);
        }
        if (f->lpRebootMsg) set_string(key, L"RebootMessage", *f->lpRebootMsg ? f->lpRebootMsg : NULL, REG_SZ);
        if (f->lpCommand) set_string(key, L"FailureCommand", *f->lpCommand ? f->lpCommand : NULL, REG_SZ);
        break;
    }
    case SERVICE_CONFIG_DELAYED_AUTO_START_INFO: set_dword(key, L"DelayedAutostart", ((SERVICE_DELAYED_AUTO_START_INFO *)info)->fDelayedAutostart ? 1 : 0); break;
    case SERVICE_CONFIG_FAILURE_ACTIONS_FLAG: set_dword(key, L"FailureActionsOnNonCrashFailures", ((SERVICE_FAILURE_ACTIONS_FLAG *)info)->fFailureActionsOnNonCrashFailures ? 1 : 0); break;
    case SERVICE_CONFIG_SERVICE_SID_INFO: {
        DWORD v = ((SERVICE_SID_INFO *)info)->dwServiceSidType;
        if (v > SERVICE_SID_TYPE_RESTRICTED) { RegCloseKey(key); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        set_dword(key, L"ServiceSidType", v);
        break;
    }
    case SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO: set_multi(key, L"RequiredPrivileges", ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)info)->pmszRequiredPrivileges); break;
    case SERVICE_CONFIG_PRESHUTDOWN_INFO: set_dword(key, L"PreshutdownTimeout", ((SERVICE_PRESHUTDOWN_INFO *)info)->dwPreshutdownTimeout); break;
    case SERVICE_CONFIG_TRIGGER_INFO: break;      /* (triggers start a service on an event; there is no event source) */
    case SERVICE_CONFIG_PREFERRED_NODE: break;
    case SERVICE_CONFIG_LAUNCH_PROTECTED: set_dword(key, L"LaunchProtected", ((SERVICE_LAUNCH_PROTECTED_INFO *)info)->dwLaunchProtected); break;
    default: RegCloseKey(key); SetLastError(ERROR_INVALID_LEVEL); return FALSE;
    }
    RegCloseKey(key);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI ChangeServiceConfig2A(SC_HANDLE h, DWORD level, LPVOID info)
{
    if (info && level == SERVICE_CONFIG_DESCRIPTION) {
        SERVICE_DESCRIPTIONA *a = info;
        SERVICE_DESCRIPTIONW w;
        BOOL r;
        w.lpDescription = strdupAW(a->lpDescription);
        r = ChangeServiceConfig2W(h, level, &w);
        free(w.lpDescription);
        return r;
    }
    if (info && level == SERVICE_CONFIG_FAILURE_ACTIONS) {
        SERVICE_FAILURE_ACTIONSA *a = info;
        SERVICE_FAILURE_ACTIONSW w;
        BOOL r;
        w.dwResetPeriod = a->dwResetPeriod;
        w.lpRebootMsg = strdupAW(a->lpRebootMsg);
        w.lpCommand = strdupAW(a->lpCommand);
        w.cActions = a->cActions;
        w.lpsaActions = a->lpsaActions;
        r = ChangeServiceConfig2W(h, level, &w);
        free(w.lpRebootMsg);
        free(w.lpCommand);
        return r;
    }
    if (info && level == SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO) {
        SERVICE_REQUIRED_PRIVILEGES_INFOA *a = info;
        SERVICE_REQUIRED_PRIVILEGES_INFOW w;
        size_t chars = 0, at = 0;
        const char *p;
        BOOL r;
        w.pmszRequiredPrivileges = NULL;
        if (a->pmszRequiredPrivileges) {
            for (p = a->pmszRequiredPrivileges; *p; p += strlen(p) + 1) chars += MultiByteToWideChar(CP_ACP, 0, p, -1, NULL, 0);
            if ((w.pmszRequiredPrivileges = malloc((chars + 2) * 2))) {
                for (p = a->pmszRequiredPrivileges; *p; p += strlen(p) + 1) at += MultiByteToWideChar(CP_ACP, 0, p, -1, w.pmszRequiredPrivileges + at, (int)(chars + 1 - at));
                w.pmszRequiredPrivileges[at] = 0;
            }
        }
        r = ChangeServiceConfig2W(h, level, &w);
        free(w.pmszRequiredPrivileges);
        return r;
    }
    return ChangeServiceConfig2W(h, level, info);
}

__declspec(dllexport) BOOL WINAPI QueryServiceConfig2W(SC_HANDLE h, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_QUERY_CONFIG);
    HKEY key;
    DWORD total = 0;
    BYTE *at;
    if (!s) return FALSE;
    if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (open_service_key(s->name, KEY_READ, &key) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    switch (level) {
    case SERVICE_CONFIG_DESCRIPTION: {
        WCHAR *text = get_string(key, L"Description", NULL);
        total = sizeof(SERVICE_DESCRIPTIONW) + (text ? wsize(text) : 0);
        *needed = total;
        if (size < total) { free(text); RegCloseKey(key); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        ((SERVICE_DESCRIPTIONW *)buffer)->lpDescription = NULL;
        if (text) { at = buffer + sizeof(SERVICE_DESCRIPTIONW); ((SERVICE_DESCRIPTIONW *)buffer)->lpDescription = put_wide(&at, text); }
        free(text);
        break;
    }
    case SERVICE_CONFIG_FAILURE_ACTIONS: {
        WCHAR *reboot = get_string(key, L"RebootMessage", NULL), *command = get_string(key, L"FailureCommand", NULL);
        DWORD blob_size = 0, *blob = NULL, count = 0, i;
        SERVICE_FAILURE_ACTIONSW *out = (SERVICE_FAILURE_ACTIONSW *)buffer;
        if (RegQueryValueExW(key, L"FailureActions", NULL, NULL, NULL, &blob_size) == ERROR_SUCCESS && blob_size >= 12 && (blob = malloc(blob_size + 8))) {
            if (RegQueryValueExW(key, L"FailureActions", NULL, NULL, (BYTE *)blob, &blob_size) == ERROR_SUCCESS) { count = blob[2]; if (count > (blob_size - 12) / 8) count = (blob_size - 12) / 8; }
        }
        total = sizeof(*out) + count * sizeof(SC_ACTION) + (reboot ? wsize(reboot) : 0) + (command ? wsize(command) : 0);
        *needed = total;
        if (size < total) { free(reboot); free(command); free(blob); RegCloseKey(key); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        out->dwResetPeriod = blob ? blob[0] : 0;
        out->cActions = count;
        out->lpsaActions = count ? (SC_ACTION *)(buffer + sizeof(*out)) : NULL;
        for (i = 0; i < count; ++i) { out->lpsaActions[i].Type = blob[3 + 2 * i]; out->lpsaActions[i].Delay = blob[4 + 2 * i]; }
        at = buffer + sizeof(*out) + count * sizeof(SC_ACTION);
        out->lpRebootMsg = reboot ? put_wide(&at, reboot) : NULL;
        out->lpCommand = command ? put_wide(&at, command) : NULL;
        free(reboot); free(command); free(blob);
        break;
    }
    case SERVICE_CONFIG_DELAYED_AUTO_START_INFO: total = sizeof(SERVICE_DELAYED_AUTO_START_INFO); *needed = total; if (size < total) goto small;
        ((SERVICE_DELAYED_AUTO_START_INFO *)buffer)->fDelayedAutostart = get_dword(key, L"DelayedAutostart", 0) != 0; break;
    case SERVICE_CONFIG_FAILURE_ACTIONS_FLAG: total = sizeof(SERVICE_FAILURE_ACTIONS_FLAG); *needed = total; if (size < total) goto small;
        ((SERVICE_FAILURE_ACTIONS_FLAG *)buffer)->fFailureActionsOnNonCrashFailures = get_dword(key, L"FailureActionsOnNonCrashFailures", 0) != 0; break;
    case SERVICE_CONFIG_SERVICE_SID_INFO: total = sizeof(SERVICE_SID_INFO); *needed = total; if (size < total) goto small;
        ((SERVICE_SID_INFO *)buffer)->dwServiceSidType = get_dword(key, L"ServiceSidType", SERVICE_SID_TYPE_NONE); break;
    case SERVICE_CONFIG_REQUIRED_PRIVILEGES_INFO: {
        WCHAR *list = get_string(key, L"RequiredPrivileges", NULL);
        DWORD chars = list ? multi_length(list) : 1;
        total = sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW) + chars * 2;
        *needed = total;
        if (size < total) { free(list); RegCloseKey(key); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        ((SERVICE_REQUIRED_PRIVILEGES_INFOW *)buffer)->pmszRequiredPrivileges = (WCHAR *)(buffer + sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW));
        if (list) memcpy(buffer + sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW), list, chars * 2); else ((WCHAR *)(buffer + sizeof(SERVICE_REQUIRED_PRIVILEGES_INFOW)))[0] = 0;
        free(list);
        break;
    }
    case SERVICE_CONFIG_PRESHUTDOWN_INFO: total = sizeof(SERVICE_PRESHUTDOWN_INFO); *needed = total; if (size < total) goto small;
        ((SERVICE_PRESHUTDOWN_INFO *)buffer)->dwPreshutdownTimeout = get_dword(key, L"PreshutdownTimeout", 180000); break;
    case SERVICE_CONFIG_TRIGGER_INFO: total = sizeof(SERVICE_TRIGGER_INFO); *needed = total; if (size < total) goto small;
        memset(buffer, 0, total); break;
    case SERVICE_CONFIG_LAUNCH_PROTECTED: total = sizeof(SERVICE_LAUNCH_PROTECTED_INFO); *needed = total; if (size < total) goto small;
        ((SERVICE_LAUNCH_PROTECTED_INFO *)buffer)->dwLaunchProtected = get_dword(key, L"LaunchProtected", 0); break;
    case SERVICE_CONFIG_PREFERRED_NODE: total = sizeof(SERVICE_PREFERRED_NODE_INFO); *needed = total; if (size < total) goto small;
        memset(buffer, 0, total); break;
    default: RegCloseKey(key); SetLastError(ERROR_INVALID_LEVEL); return FALSE;
    }
    RegCloseKey(key);
    return TRUE;
small:
    RegCloseKey(key);
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI QueryServiceConfig2A(SC_HANDLE h, DWORD level, LPBYTE buffer, DWORD size, LPDWORD needed)
{
    if (level == SERVICE_CONFIG_DESCRIPTION || level == SERVICE_CONFIG_FAILURE_ACTIONS) {
        DWORD wide_size = 0, total;
        BYTE *wide;
        BOOL ok;
        if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
        QueryServiceConfig2W(h, level, NULL, 0, &wide_size);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !(wide = malloc(wide_size + 8))) return FALSE;
        ok = QueryServiceConfig2W(h, level, wide, wide_size, &wide_size);
        if (ok && level == SERVICE_CONFIG_DESCRIPTION) {
            WCHAR *text = ((SERVICE_DESCRIPTIONW *)wide)->lpDescription;
            total = sizeof(SERVICE_DESCRIPTIONA) + (text ? asize(text) : 0);
            *needed = total;
            if (size < total) { ok = FALSE; SetLastError(ERROR_INSUFFICIENT_BUFFER); }
            else { BYTE *at = buffer + sizeof(SERVICE_DESCRIPTIONA); ((SERVICE_DESCRIPTIONA *)buffer)->lpDescription = text ? put_ansi(&at, text) : NULL; }
        } else if (ok) {
            SERVICE_FAILURE_ACTIONSW *w = (SERVICE_FAILURE_ACTIONSW *)wide;
            SERVICE_FAILURE_ACTIONSA *a = (SERVICE_FAILURE_ACTIONSA *)buffer;
            total = sizeof(*a) + w->cActions * sizeof(SC_ACTION) + (w->lpRebootMsg ? asize(w->lpRebootMsg) : 0) + (w->lpCommand ? asize(w->lpCommand) : 0);
            *needed = total;
            if (size < total) { ok = FALSE; SetLastError(ERROR_INSUFFICIENT_BUFFER); }
            else {
                BYTE *at = buffer + sizeof(*a) + w->cActions * sizeof(SC_ACTION);
                a->dwResetPeriod = w->dwResetPeriod;
                a->cActions = w->cActions;
                a->lpsaActions = w->cActions ? (SC_ACTION *)(buffer + sizeof(*a)) : NULL;
                if (w->cActions) memcpy(a->lpsaActions, w->lpsaActions, w->cActions * sizeof(SC_ACTION));
                a->lpRebootMsg = w->lpRebootMsg ? put_ansi(&at, w->lpRebootMsg) : NULL;
                a->lpCommand = w->lpCommand ? put_ansi(&at, w->lpCommand) : NULL;
            }
        } else *needed = wide_size;
        free(wide);
        return ok;
    }
    return QueryServiceConfig2W(h, level, buffer, size, needed);
}

/* ---- names -------------------------------------------------------------------------------------------------------------------------------------------------- */

static WCHAR *display_name_of(LPCWSTR key_name)
{
    HKEY key;
    WCHAR *display = NULL;
    if (open_service_key(key_name, KEY_READ, &key) == ERROR_SUCCESS) { display = get_string(key, L"DisplayName", NULL); RegCloseKey(key); }
    return display ? display : dup_wide(key_name);
}
__declspec(dllexport) BOOL WINAPI GetServiceDisplayNameW(SC_HANDLE manager, LPCWSTR name, LPWSTR out, LPDWORD size)
{
    WCHAR *display;
    HKEY key;
    DWORD n;
    if (!find_handle(manager, H_MANAGER, SC_MANAGER_CONNECT)) return FALSE;
    if (!name || !size) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (!valid_service_name(name) || open_service_key(name, KEY_READ, &key) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    RegCloseKey(key);
    display = display_name_of(name);
    if (!display) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    n = (DWORD)wcslen(display);
    if (!out || *size <= n) { *size = n; free(display); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    wcscpy(out, display);
    *size = n;
    free(display);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI GetServiceDisplayNameA(SC_HANDLE manager, LPCSTR name, LPSTR out, LPDWORD size)
{
    WCHAR *wn = strdupAW(name), wide[MAX_NAME * 2 + 2];
    DWORD n = MAX_NAME * 2;
    BOOL ok = GetServiceDisplayNameW(manager, wn, wide, &n);
    free(wn);
    if (!ok && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return FALSE;
    if (!ok) { *size = n; return FALSE; }
    n = (DWORD)WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
    if (!out || *size < n) { *size = n - 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    WideCharToMultiByte(CP_ACP, 0, wide, -1, out, n, NULL, NULL);
    *size = n - 1;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI GetServiceKeyNameW(SC_HANDLE manager, LPCWSTR display, LPWSTR out, LPDWORD size)
{
    HKEY services;
    DWORD i;
    WCHAR key_name[MAX_NAME + 2], found[MAX_NAME + 2];
    BOOL ok = FALSE;
    if (!find_handle(manager, H_MANAGER, SC_MANAGER_CONNECT)) return FALSE;
    if (!display || !size) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY, 0, KEY_READ, &services) != ERROR_SUCCESS) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    for (i = 0; !ok; ++i) {
        DWORD n = MAX_NAME + 1;
        HKEY key;
        if (RegEnumKeyExW(services, i, key_name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(services, key_name, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        if (get_dword(key, L"Type", 0)) {
            WCHAR *text = get_string(key, L"DisplayName", NULL);
            if (!wcsicmp(text ? text : key_name, display)) { wcscpy(found, key_name); ok = TRUE; }
            free(text);
        }
        RegCloseKey(key);
    }
    RegCloseKey(services);
    if (!ok) { SetLastError(ERROR_SERVICE_DOES_NOT_EXIST); return FALSE; }
    if (!out || *size <= wcslen(found)) { *size = (DWORD)wcslen(found); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    wcscpy(out, found);
    *size = (DWORD)wcslen(found);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI GetServiceKeyNameA(SC_HANDLE manager, LPCSTR display, LPSTR out, LPDWORD size)
{
    WCHAR *wd = strdupAW(display), wide[MAX_NAME + 2];
    DWORD n = MAX_NAME + 1, bytes;
    BOOL ok = GetServiceKeyNameW(manager, wd, wide, &n);
    free(wd);
    if (!ok && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return FALSE;
    if (!ok) { *size = n; return FALSE; }
    bytes = (DWORD)WideCharToMultiByte(CP_ACP, 0, wide, -1, NULL, 0, NULL, NULL);
    if (!out || *size < bytes) { *size = bytes - 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    WideCharToMultiByte(CP_ACP, 0, wide, -1, out, bytes, NULL, NULL);
    *size = bytes - 1;
    return TRUE;
}

/* ---- starting ----------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL start_one(LPCWSTR name, DWORD argc, LPCWSTR *argv, DWORD depth);

/* The services a service needs running first (its DependOnService), started one after the other. */
static BOOL start_dependencies(LPCWSTR name, DWORD depth)
{
    HKEY key;
    WCHAR *deps;
    const WCHAR *d;
    BOOL ok = TRUE;
    if (open_service_key(name, KEY_READ, &key) != ERROR_SUCCESS) return TRUE;
    deps = get_string(key, L"DependOnService", NULL);
    RegCloseKey(key);
    for (d = deps; ok && d && *d; d += wcslen(d) + 1) {
        SERVICE_STATUS_PROCESS status;
        if (depth > 8) { ok = FALSE; break; }
        if (*d == '+') continue;      /* (a group) */
        reap_deleted(d);
        read_status(d, &status);
        if (status.dwCurrentState == SERVICE_RUNNING) continue;
        if (!start_one(d, 0, NULL, depth + 1) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) ok = FALSE;
    }
    free(deps);
    if (!ok) SetLastError(ERROR_SERVICE_DEPENDENCY_FAIL);
    return ok;
}

static BOOL start_one(LPCWSTR name, DWORD argc, LPCWSTR *argv, DWORD depth)
{
    HKEY key;
    Config c;
    SERVICE_STATUS_PROCESS status;
    WCHAR *command = NULL, *expanded = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    BOOL created;
    DWORD waited = 0;
    reap_deleted(name);
    if (!load_config(name, &c)) return FALSE;
    if (c.start == SERVICE_DISABLED) { free_config(&c); SetLastError(ERROR_SERVICE_DISABLED); return FALSE; }
    if (!(c.type & (SERVICE_WIN32_OWN_PROCESS | SERVICE_WIN32_SHARE_PROCESS))) { free_config(&c); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }      /* drivers cannot be loaded */
    if (!c.binary) { free_config(&c); SetLastError(ERROR_PATH_NOT_FOUND); return FALSE; }
    read_status(name, &status);
    if (status.dwCurrentState != SERVICE_STOPPED) { free_config(&c); SetLastError(ERROR_SERVICE_ALREADY_RUNNING); return FALSE; }
    if (!start_dependencies(name, depth)) { free_config(&c); return FALSE; }
    {
        DWORD n = ExpandEnvironmentStringsW(c.binary, NULL, 0);
        if (n == 0 || !(expanded = malloc((n + 1) * 2)) || !ExpandEnvironmentStringsW(c.binary, expanded, n + 1)) { free(expanded); free_config(&c); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    }
    if (!(command = dup_wide(expanded))) { free(expanded); free_config(&c); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    /* the state: start pending, nobody has claimed it; the arguments of StartService for the service's ServiceMain */
    if (open_state_key(name, TRUE, &key) == ERROR_SUCCESS) {
        WCHAR *list = NULL;
        DWORD i, chars = 0, at = 0;
        for (i = 0; i < argc; ++i) if (argv && argv[i]) chars += (DWORD)wcslen(argv[i]) + 1;
        if (chars && (list = malloc((chars + 2) * 2))) {
            for (i = 0; i < argc; ++i) if (argv && argv[i]) { wcscpy(list + at, argv[i]); at += (DWORD)wcslen(argv[i]) + 1; }
            list[at] = 0;
            RegSetValueExW(key, L"Args", 0, REG_MULTI_SZ, (BYTE *)list, (at + 1) * 2);
            free(list);
        } else RegDeleteValueW(key, L"Args");
        RegDeleteValueW(key, L"Control");
        RegCloseKey(key);
    }
    memset(&status, 0, sizeof(status));
    status.dwServiceType = c.type;
    status.dwCurrentState = SERVICE_START_PENDING;
    status.dwWaitHint = MAX_START_WAIT_MS;
    write_status(name, &status);
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    SetEnvironmentVariableW(L"TCOS_SERVICE_NAME", name);
    created = CreateProcessW(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &startup, &process);
    SetEnvironmentVariableW(L"TCOS_SERVICE_NAME", NULL);
    free(command); free(expanded); free_config(&c);
    if (!created) {
        DWORD error = GetLastError();
        status.dwCurrentState = SERVICE_STOPPED;
        status.dwWin32ExitCode = error;
        write_status(name, &status);
        SetLastError(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ERROR_FILE_NOT_FOUND : error);
        return FALSE;
    }
    CloseHandle(process.hThread);
    for (;;) {      /* until the service has taken its state (its process id is in it) */
        read_status(name, &status);
        if (status.dwProcessId != 0 && status.dwCurrentState != SERVICE_START_PENDING) break;
        if (status.dwProcessId != 0 && status.dwCurrentState == SERVICE_START_PENDING && waited > 200) break;
        if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
            read_status(name, &status);
            if (status.dwCurrentState == SERVICE_STOPPED || status.dwProcessId == 0) {
                DWORD code = 0;
                GetExitCodeProcess(process.hProcess, &code);
                status.dwCurrentState = SERVICE_STOPPED;
                status.dwWin32ExitCode = ERROR_SERVICE_REQUEST_TIMEOUT;
                status.dwProcessId = 0;
                write_status(name, &status);
                CloseHandle(process.hProcess);
                SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
                return FALSE;
            }
            break;
        }
        if (waited >= MAX_START_WAIT_MS) { CloseHandle(process.hProcess); SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT); return FALSE; }
        Sleep(20);
        waited += 20;
    }
    CloseHandle(process.hProcess);
    return TRUE;
}

__declspec(dllexport) BOOL WINAPI StartServiceW(SC_HANDLE h, DWORD argc, LPCWSTR *argv)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_START);
    if (!s) return FALSE;
    if (argc && !argv) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    return start_one(s->name, argc, argv, 0);
}
__declspec(dllexport) BOOL WINAPI StartServiceA(SC_HANDLE h, DWORD argc, LPCSTR *argv)
{
    LPCWSTR *wide = NULL;
    DWORD i;
    BOOL r;
    if (argc && !argv) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (argc) {
        if (!(wide = calloc(argc, sizeof(*wide)))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        for (i = 0; i < argc; ++i) wide[i] = strdupAW(argv[i]);
    }
    r = StartServiceW(h, argc, wide);
    for (i = 0; i < argc; ++i) free((void *)wide[i]);
    free(wide);
    return r;
}

/* ---- controlling -------------------------------------------------------------------------------------------------------------------------------------------- */

static void control_names(LPCWSTR name, WCHAR *event, WCHAR *ack, WCHAR *mutex)
{
    wcscpy(event, L"TcosSvcCtl_"); wcscat(event, name);
    wcscpy(ack, L"TcosSvcAck_"); wcscat(ack, name);
    wcscpy(mutex, L"TcosSvcLck_"); wcscat(mutex, name);
}

static BOOL deliver_control(LPCWSTR name, DWORD control, DWORD event_type, const BYTE *data, DWORD data_size, SERVICE_STATUS_PROCESS *after)
{
    WCHAR en[MAX_NAME + 20], an[MAX_NAME + 20], mn[MAX_NAME + 20];
    HANDLE event, ack, lock;
    HKEY key;
    DWORD wait;
    control_names(name, en, an, mn);
    event = CreateEventW(NULL, FALSE, FALSE, en);
    ack = CreateEventW(NULL, FALSE, FALSE, an);
    lock = CreateMutexW(NULL, FALSE, mn);
    if (!event || !ack || !lock) { if (event) CloseHandle(event); if (ack) CloseHandle(ack); if (lock) CloseHandle(lock); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    WaitForSingleObject(lock, INFINITE);
    if (open_state_key(name, FALSE, &key) != ERROR_SUCCESS) { ReleaseMutex(lock); CloseHandle(event); CloseHandle(ack); CloseHandle(lock); SetLastError(ERROR_SERVICE_NOT_ACTIVE); return FALSE; }
    set_dword(key, L"Control", control);
    set_dword(key, L"ControlEventType", event_type);
    if (data && data_size) RegSetValueExW(key, L"ControlData", 0, REG_BINARY, data, data_size); else RegDeleteValueW(key, L"ControlData");
    RegCloseKey(key);
    ResetEvent(ack);
    SetEvent(event);
    wait = WaitForSingleObject(ack, 30000);
    ReleaseMutex(lock);
    CloseHandle(event); CloseHandle(ack); CloseHandle(lock);
    read_status(name, after);
    if (wait != WAIT_OBJECT_0) {
        if (after->dwCurrentState == SERVICE_STOPPED) { SetLastError(ERROR_SERVICE_NOT_ACTIVE); return FALSE; }
        SetLastError(ERROR_SERVICE_REQUEST_TIMEOUT);
        return FALSE;
    }
    return TRUE;
}

static BOOL control_service(SC_HANDLE h, DWORD control, DWORD level, PVOID info_out)
{
    ScHandle *s;
    SERVICE_STATUS_PROCESS status, after;
    DWORD needed_access, bit;
    if (!(s = find_handle(h, H_SERVICE, 0))) return FALSE;
    switch (control) {
    case SERVICE_CONTROL_STOP: needed_access = SERVICE_STOP; bit = SERVICE_ACCEPT_STOP; break;
    case SERVICE_CONTROL_PAUSE: case SERVICE_CONTROL_CONTINUE: needed_access = SERVICE_PAUSE_CONTINUE; bit = SERVICE_ACCEPT_PAUSE_CONTINUE; break;
    case SERVICE_CONTROL_INTERROGATE: needed_access = SERVICE_INTERROGATE; bit = 0; break;
    case SERVICE_CONTROL_PARAMCHANGE: needed_access = SERVICE_PAUSE_CONTINUE; bit = SERVICE_ACCEPT_PARAMCHANGE; break;
    case SERVICE_CONTROL_NETBINDADD: case SERVICE_CONTROL_NETBINDREMOVE: case SERVICE_CONTROL_NETBINDENABLE: case SERVICE_CONTROL_NETBINDDISABLE:
        needed_access = SERVICE_PAUSE_CONTINUE; bit = SERVICE_ACCEPT_NETBINDCHANGE; break;
    default:
        if (control >= 128 && control <= 255) { needed_access = SERVICE_USER_DEFINED_CONTROL; bit = 0; break; }
        SetLastError(ERROR_INVALID_SERVICE_CONTROL);
        return FALSE;
    }
    if ((s->access & needed_access) != needed_access) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    reap_deleted(s->name);
    read_status(s->name, &status);
    if (status.dwCurrentState == SERVICE_STOPPED) { SetLastError(ERROR_SERVICE_NOT_ACTIVE); return FALSE; }
    if (control == SERVICE_CONTROL_STOP && status.dwCurrentState == SERVICE_STOP_PENDING) { SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL); memcpy(info_out, &status, level ? sizeof(status) : sizeof(SERVICE_STATUS)); return FALSE; }
    if (status.dwCurrentState == SERVICE_START_PENDING || status.dwCurrentState == SERVICE_STOP_PENDING) { if (control != SERVICE_CONTROL_INTERROGATE) { SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL); return FALSE; } }
    if (bit && !(status.dwControlsAccepted & bit)) { SetLastError(ERROR_INVALID_SERVICE_CONTROL); return FALSE; }
    if (control == SERVICE_CONTROL_PAUSE && status.dwCurrentState == SERVICE_PAUSED) { SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL); return FALSE; }
    if (control == SERVICE_CONTROL_CONTINUE && status.dwCurrentState == SERVICE_RUNNING) { SetLastError(ERROR_SERVICE_CANNOT_ACCEPT_CTRL); return FALSE; }
    if (!deliver_control(s->name, control, 0, NULL, 0, &after)) {
        if (info_out) memcpy(info_out, &after, level ? sizeof(after) : sizeof(SERVICE_STATUS));
        return FALSE;
    }
    if (info_out) memcpy(info_out, &after, level ? sizeof(after) : sizeof(SERVICE_STATUS));
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI ControlService(SC_HANDLE h, DWORD control, LPSERVICE_STATUS out)
{
    if (!out) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    return control_service(h, control, 0, out);
}
__declspec(dllexport) BOOL WINAPI ControlServiceExW(SC_HANDLE h, DWORD control, DWORD level, PVOID info)
{
    if (!info) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (level != SERVICE_CONTROL_STATUS_REASON_INFO) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    {
        SERVICE_CONTROL_STATUS_REASON_PARAMSW *p = info;
        return control_service(h, control, 1, &p->ServiceStatus);
    }
}
__declspec(dllexport) BOOL WINAPI ControlServiceExA(SC_HANDLE h, DWORD control, DWORD level, PVOID info) { return ControlServiceExW(h, control, level, info); }

/* ---- enumeration -------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { WCHAR *name, *display; SERVICE_STATUS_PROCESS status; } Entry;

static void free_entries(Entry *e, DWORD n) { DWORD i; for (i = 0; i < n; ++i) { free(e[i].name); free(e[i].display); } free(e); }

/* The services that match a type mask, a state (SERVICE_ACTIVE / _INACTIVE / _STATE_ALL) and, if given, a load order group ("" = no group). Sorted by key name. */
static BOOL collect_services(DWORD type_mask, DWORD state, LPCWSTR group, Entry **list, DWORD *count)
{
    HKEY services;
    DWORD i, capacity = 16;
    Entry *entries = calloc(capacity, sizeof(Entry));
    *list = NULL;
    *count = 0;
    if (!entries) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, SERVICES_KEY, 0, KEY_READ, &services) != ERROR_SUCCESS) { *list = entries; return TRUE; }
    for (i = 0;; ++i) {
        WCHAR name[MAX_NAME + 2];
        DWORD n = MAX_NAME + 1, type;
        HKEY key;
        SERVICE_STATUS_PROCESS status;
        BOOL active;
        if (RegEnumKeyExW(services, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (name[0] == '.') continue;
        if (RegOpenKeyExW(services, name, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        type = get_dword(key, L"Type", 0);
        if (type && group) {
            WCHAR *g = get_string(key, L"Group", NULL);
            BOOL match = *group ? (g && !wcsicmp(g, group)) : (!g || !*g);
            free(g);
            if (!match) type = 0;
        }
        RegCloseKey(key);
        if (!type || !(type & type_mask)) continue;
        reap_deleted(name);
        read_status(name, &status);
        active = status.dwCurrentState != SERVICE_STOPPED;
        if (!((state & SERVICE_ACTIVE) && active) && !((state & SERVICE_INACTIVE) && !active)) continue;
        if (*count == capacity) {
            Entry *bigger = realloc(entries, capacity * 2 * sizeof(Entry));
            if (!bigger) { free_entries(entries, *count); RegCloseKey(services); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
            entries = bigger;
            memset(entries + capacity, 0, capacity * sizeof(Entry));
            capacity *= 2;
        }
        entries[*count].name = dup_wide(name);
        entries[*count].display = display_name_of(name);
        entries[*count].status = status;
        ++*count;
    }
    RegCloseKey(services);
    *list = entries;
    return TRUE;
}

/* Fills `buffer` with as many ENUM_SERVICE_STATUS(_PROCESS)W records as fit (the records first, the strings after them from the end), from `first`. */
static BOOL fill_enum(Entry *e, DWORD count, DWORD first, BOOL process_form, BYTE *buffer, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume)
{
    const DWORD record = process_form ? sizeof(ENUM_SERVICE_STATUS_PROCESSW) : sizeof(ENUM_SERVICE_STATUSW);
    DWORD used = 0, strings = 0, i, fit = 0, all = 0;
    BYTE *tail = buffer + size;
    for (i = first; i < count; ++i) all += record + wsize(e[i].name) + wsize(e[i].display);
    *returned = 0;
    for (i = first; i < count; ++i) {
        const DWORD need = record + wsize(e[i].name) + wsize(e[i].display);
        if (!buffer || used + strings + need > size) break;
        {
            WCHAR *name, *display;
            ENUM_SERVICE_STATUSW *r = (ENUM_SERVICE_STATUSW *)(buffer + used);
            tail -= wsize(e[i].display); display = (WCHAR *)tail; memcpy(display, e[i].display, wsize(e[i].display));
            tail -= wsize(e[i].name); name = (WCHAR *)tail; memcpy(name, e[i].name, wsize(e[i].name));
            if (process_form) {
                ENUM_SERVICE_STATUS_PROCESSW *p = (ENUM_SERVICE_STATUS_PROCESSW *)r;
                p->lpServiceName = name; p->lpDisplayName = display; p->ServiceStatusProcess = e[i].status;
            } else {
                r->lpServiceName = name; r->lpDisplayName = display; memcpy(&r->ServiceStatus, &e[i].status, sizeof(SERVICE_STATUS));
            }
        }
        used += record;
        strings += wsize(e[i].name) + wsize(e[i].display);
        ++fit;
    }
    *returned = fit;
    if (first + fit < count) {
        *needed = all - (used + strings);
        if (resume) *resume = first + fit;
        SetLastError(ERROR_MORE_DATA);
        return FALSE;
    }
    *needed = 0;
    if (resume) *resume = 0;
    (void)fit;
    return TRUE;
}

static BOOL enum_services(SC_HANDLE h, DWORD type, DWORD state, BOOL process_form, BYTE *buffer, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume, LPCWSTR group)
{
    Entry *list;
    DWORD count, first = resume ? *resume : 0;
    BOOL ok;
    if (!find_handle(h, H_MANAGER, SC_MANAGER_ENUMERATE_SERVICE)) return FALSE;
    if (!needed || !returned) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (!(type & (SERVICE_DRIVER | SERVICE_WIN32)) || (type & ~(SERVICE_DRIVER | SERVICE_WIN32 | SERVICE_INTERACTIVE_PROCESS))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!state || state > SERVICE_STATE_ALL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!collect_services(type, state, group, &list, &count)) return FALSE;
    if (first > count) first = count;
    if (size && !buffer) { free_entries(list, count); SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    ok = fill_enum(list, count, first, process_form, buffer, size, needed, returned, resume);
    free_entries(list, count);
    return ok;
}
__declspec(dllexport) BOOL WINAPI EnumServicesStatusW(SC_HANDLE h, DWORD type, DWORD state, LPENUM_SERVICE_STATUSW out, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume)
{
    return enum_services(h, type, state, FALSE, (BYTE *)out, size, needed, returned, resume, NULL);
}
__declspec(dllexport) BOOL WINAPI EnumServicesStatusExW(SC_HANDLE h, SC_ENUM_TYPE level, DWORD type, DWORD state, LPBYTE out, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume,
                                                       LPCWSTR group)
{
    if (level != SC_ENUM_PROCESS_INFO) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    return enum_services(h, type, state, TRUE, out, size, needed, returned, resume, group);
}

/* The ANSI forms: the wide records are converted into the caller's buffer, strings at its end. */
static BOOL enum_services_a(SC_HANDLE h, DWORD type, DWORD state, BOOL process_form, BYTE *buffer, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume, LPCSTR group)
{
    Entry *list;
    DWORD count, first = resume ? *resume : 0, record = process_form ? sizeof(ENUM_SERVICE_STATUS_PROCESSA) : sizeof(ENUM_SERVICE_STATUSA), i, used = 0, strings = 0, all = 0, fit = 0;
    WCHAR *wgroup = group ? strdupAW(group) : NULL;
    BYTE *tail = buffer + size;
    if (!find_handle(h, H_MANAGER, SC_MANAGER_ENUMERATE_SERVICE)) { free(wgroup); return FALSE; }
    if (!needed || !returned) { free(wgroup); SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (!(type & (SERVICE_DRIVER | SERVICE_WIN32)) || !state || state > SERVICE_STATE_ALL) { free(wgroup); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!collect_services(type, state, wgroup, &list, &count)) { free(wgroup); return FALSE; }
    free(wgroup);
    if (first > count) first = count;
    for (i = first; i < count; ++i) all += record + asize(list[i].name) + asize(list[i].display);
    *returned = 0;
    for (i = first; i < count; ++i) {
        DWORD need = record + asize(list[i].name) + asize(list[i].display);
        char *name, *display;
        if (!buffer || used + strings + need > size) break;
        tail -= asize(list[i].display); display = (char *)tail; WideCharToMultiByte(CP_ACP, 0, list[i].display, -1, display, asize(list[i].display), NULL, NULL);
        tail -= asize(list[i].name); name = (char *)tail; WideCharToMultiByte(CP_ACP, 0, list[i].name, -1, name, asize(list[i].name), NULL, NULL);
        if (process_form) {
            ENUM_SERVICE_STATUS_PROCESSA *p = (ENUM_SERVICE_STATUS_PROCESSA *)(buffer + used);
            p->lpServiceName = name; p->lpDisplayName = display; p->ServiceStatusProcess = list[i].status;
        } else {
            ENUM_SERVICE_STATUSA *p = (ENUM_SERVICE_STATUSA *)(buffer + used);
            p->lpServiceName = name; p->lpDisplayName = display; memcpy(&p->ServiceStatus, &list[i].status, sizeof(SERVICE_STATUS));
        }
        used += record;
        strings += asize(list[i].name) + asize(list[i].display);
        ++fit;
    }
    *returned = fit;
    free_entries(list, count);
    if (first + fit < count) { *needed = all - (used + strings); if (resume) *resume = first + fit; SetLastError(ERROR_MORE_DATA); return FALSE; }
    *needed = 0;
    if (resume) *resume = 0;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI EnumServicesStatusA(SC_HANDLE h, DWORD type, DWORD state, LPENUM_SERVICE_STATUSA out, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume)
{
    return enum_services_a(h, type, state, FALSE, (BYTE *)out, size, needed, returned, resume, NULL);
}
__declspec(dllexport) BOOL WINAPI EnumServicesStatusExA(SC_HANDLE h, SC_ENUM_TYPE level, DWORD type, DWORD state, LPBYTE out, DWORD size, LPDWORD needed, LPDWORD returned, LPDWORD resume,
                                                       LPCSTR group)
{
    if (level != SC_ENUM_PROCESS_INFO) { SetLastError(ERROR_INVALID_LEVEL); return FALSE; }
    return enum_services_a(h, type, state, TRUE, out, size, needed, returned, resume, group);
}

/* The active or stopped services that need this one. */
static BOOL depends_on(LPCWSTR candidate, LPCWSTR name)
{
    HKEY key;
    WCHAR *deps;
    const WCHAR *d;
    BOOL yes = FALSE;
    if (open_service_key(candidate, KEY_READ, &key) != ERROR_SUCCESS) return FALSE;
    deps = get_string(key, L"DependOnService", NULL);
    RegCloseKey(key);
    for (d = deps; d && *d; d += wcslen(d) + 1) if (!wcsicmp(d, name)) { yes = TRUE; break; }
    free(deps);
    return yes;
}
static BOOL enum_dependents(SC_HANDLE h, DWORD state, BYTE *buffer, DWORD size, LPDWORD needed, LPDWORD returned, BOOL ansi)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_ENUMERATE_DEPENDENTS);
    Entry *list, *picked;
    DWORD count, i, n = 0;
    BOOL ok;
    if (!s) return FALSE;
    if (!needed || !returned) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (!state || state > SERVICE_STATE_ALL) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!collect_services(SERVICE_DRIVER | SERVICE_WIN32, state, NULL, &list, &count)) return FALSE;
    picked = calloc(count + 1, sizeof(Entry));
    if (!picked) { free_entries(list, count); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0; i < count; ++i) {      /* direct dependents, then theirs: a breadth-first walk from the service */
        if (depends_on(list[i].name, s->name)) picked[n++] = list[i];
    }
    for (i = 0; i < n; ++i) {
        DWORD j, k;
        for (j = 0; j < count && n < count; ++j) {
            BOOL have = FALSE;
            if (!depends_on(list[j].name, picked[i].name)) continue;
            for (k = 0; k < n; ++k) if (picked[k].name == list[j].name) have = TRUE;
            if (!have && wcsicmp(list[j].name, s->name)) picked[n++] = list[j];
        }
    }
    if (ansi) {
        /* converted like EnumServicesStatusA does, over the picked entries */
        DWORD used = 0, strings = 0, all = 0, fit = 0, record = sizeof(ENUM_SERVICE_STATUSA);
        BYTE *tail = buffer + size;
        for (i = 0; i < n; ++i) all += record + asize(picked[i].name) + asize(picked[i].display);
        for (i = 0; i < n; ++i) {
            DWORD need = record + asize(picked[i].name) + asize(picked[i].display);
            ENUM_SERVICE_STATUSA *p;
            if (!buffer || used + strings + need > size) break;
            p = (ENUM_SERVICE_STATUSA *)(buffer + used);
            tail -= asize(picked[i].display); p->lpDisplayName = (char *)tail; WideCharToMultiByte(CP_ACP, 0, picked[i].display, -1, p->lpDisplayName, asize(picked[i].display), NULL, NULL);
            tail -= asize(picked[i].name); p->lpServiceName = (char *)tail; WideCharToMultiByte(CP_ACP, 0, picked[i].name, -1, p->lpServiceName, asize(picked[i].name), NULL, NULL);
            memcpy(&p->ServiceStatus, &picked[i].status, sizeof(SERVICE_STATUS));
            used += record; strings += asize(picked[i].name) + asize(picked[i].display); ++fit;
        }
        *returned = fit;
        ok = fit == n;
        *needed = ok ? 0 : all - (used + strings);
    } else {
        ok = fill_enum(picked, n, 0, FALSE, buffer, size, needed, returned, NULL);
        if (ok) SetLastError(ERROR_SUCCESS);
    }
    free(picked);      /* (the names belong to `list`) */
    free_entries(list, count);
    if (!ok) SetLastError(ERROR_MORE_DATA);
    return ok;
}
__declspec(dllexport) BOOL WINAPI EnumDependentServicesW(SC_HANDLE h, DWORD state, LPENUM_SERVICE_STATUSW out, DWORD size, LPDWORD needed, LPDWORD returned)
{
    return enum_dependents(h, state, (BYTE *)out, size, needed, returned, FALSE);
}
__declspec(dllexport) BOOL WINAPI EnumDependentServicesA(SC_HANDLE h, DWORD state, LPENUM_SERVICE_STATUSA out, DWORD size, LPDWORD needed, LPDWORD returned)
{
    return enum_dependents(h, state, (BYTE *)out, size, needed, returned, TRUE);
}

/* ---- the database lock, security ---------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) SC_LOCK WINAPI LockServiceDatabase(SC_HANDLE manager)
{
    HKEY key;
    DWORD owner = 0;
    SC_HANDLE lock;
    if (!find_handle(manager, H_MANAGER, SC_MANAGER_LOCK)) return NULL;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, LOCK_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) != ERROR_SUCCESS) { SetLastError(ERROR_ACCESS_DENIED); return NULL; }
    owner = get_dword(key, L"Owner", 0);
    if (owner && process_alive(owner)) { RegCloseKey(key); SetLastError(ERROR_SERVICE_DATABASE_LOCKED); return NULL; }
    if (!(lock = new_handle(H_LOCK, 0, NULL))) { RegCloseKey(key); return NULL; }
    set_dword(key, L"Owner", GetCurrentProcessId());
    set_dword(key, L"Since", (DWORD)(GetTickCount64() / 1000));
    RegCloseKey(key);
    return (SC_LOCK)lock;
}
__declspec(dllexport) BOOL WINAPI UnlockServiceDatabase(SC_LOCK lock)
{
    if (!find_handle((SC_HANDLE)lock, H_LOCK, 0)) { SetLastError(ERROR_INVALID_SERVICE_LOCK); return FALSE; }
    return CloseServiceHandle((SC_HANDLE)lock);
}
__declspec(dllexport) BOOL WINAPI QueryServiceLockStatusW(SC_HANDLE manager, LPQUERY_SERVICE_LOCK_STATUSW out, DWORD size, LPDWORD needed)
{
    HKEY key;
    DWORD owner = 0, since = 0, total;
    WCHAR who[32] = L"";
    BYTE *at;
    if (!find_handle(manager, H_MANAGER, SC_MANAGER_QUERY_LOCK_STATUS)) return FALSE;
    if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, LOCK_KEY, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        owner = get_dword(key, L"Owner", 0);
        since = get_dword(key, L"Since", 0);
        RegCloseKey(key);
    }
    if (owner && !process_alive(owner)) owner = 0;
    if (owner) wcscpy(who, L"NT AUTHORITY\\SYSTEM");
    total = sizeof(QUERY_SERVICE_LOCK_STATUSW) + wsize(who);
    *needed = total;
    if (size < total || !out) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    out->fIsLocked = owner != 0;
    out->dwLockDuration = owner ? (DWORD)(GetTickCount64() / 1000) - since : 0;
    at = (BYTE *)(out + 1);
    out->lpLockOwner = put_wide(&at, who);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI QueryServiceLockStatusA(SC_HANDLE manager, LPQUERY_SERVICE_LOCK_STATUSA out, DWORD size, LPDWORD needed)
{
    BYTE wide[256];
    DWORD n = 0;
    LPQUERY_SERVICE_LOCK_STATUSW w = (LPQUERY_SERVICE_LOCK_STATUSW)wide;
    BYTE *at;
    DWORD total;
    if (!QueryServiceLockStatusW(manager, w, sizeof(wide), &n)) { if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed) *needed = n; return FALSE; }
    total = sizeof(QUERY_SERVICE_LOCK_STATUSA) + asize(w->lpLockOwner);
    *needed = total;
    if (size < total || !out) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    out->fIsLocked = w->fIsLocked;
    out->dwLockDuration = w->dwLockDuration;
    at = (BYTE *)(out + 1);
    out->lpLockOwner = put_ansi(&at, w->lpLockOwner);
    return TRUE;
}

#define DEFAULT_SERVICE_SDDL L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)"
__declspec(dllexport) BOOL WINAPI QueryServiceObjectSecurity(SC_HANDLE h, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd, DWORD size, LPDWORD needed)
{
    ScHandle *s = find_handle(h, H_SERVICE, 0);
    HKEY key;
    DWORD blob_size = 0;
    BYTE *blob = NULL;
    BOOL ok = FALSE;
    if (!s) return FALSE;
    if (!needed) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (((info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION)) && !(s->access & READ_CONTROL)) ||
        ((info & SACL_SECURITY_INFORMATION) && !(s->access & ACCESS_SYSTEM_SECURITY))) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    if (open_service_key(s->name, KEY_READ, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"Security", NULL, NULL, NULL, &blob_size) == ERROR_SUCCESS && (blob = malloc(blob_size + 8)))
            if (RegQueryValueExW(key, L"Security", NULL, NULL, blob, &blob_size) != ERROR_SUCCESS) { free(blob); blob = NULL; }
        RegCloseKey(key);
    }
    if (!blob) {      /* never set: the default of a service */
        PSECURITY_DESCRIPTOR made = NULL;
        ULONG made_size = 0;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(DEFAULT_SERVICE_SDDL, SDDL_REVISION_1, &made, &made_size)) {
            if ((blob = malloc(made_size + 8))) { memcpy(blob, made, made_size); blob_size = made_size; }
            LocalFree(made);
        }
    }
    if (blob) {      /* only the parts that were asked for */
        DWORD out_size = size;
        SECURITY_DESCRIPTOR_CONTROL control;
        DWORD revision;
        BOOL present, defaulted;
        PSID owner = NULL, group = NULL;
        PACL dacl = NULL, sacl = NULL;
        SECURITY_DESCRIPTOR abs;
        BYTE *copy = blob;
        InitializeSecurityDescriptor(&abs, SECURITY_DESCRIPTOR_REVISION);
        (void)control; (void)revision;
        if ((info & OWNER_SECURITY_INFORMATION) && GetSecurityDescriptorOwner(copy, &owner, &defaulted) && owner) SetSecurityDescriptorOwner(&abs, owner, defaulted);
        if ((info & GROUP_SECURITY_INFORMATION) && GetSecurityDescriptorGroup(copy, &group, &defaulted) && group) SetSecurityDescriptorGroup(&abs, group, defaulted);
        if ((info & DACL_SECURITY_INFORMATION) && GetSecurityDescriptorDacl(copy, &present, &dacl, &defaulted) && present) SetSecurityDescriptorDacl(&abs, TRUE, dacl, defaulted);
        if ((info & SACL_SECURITY_INFORMATION) && GetSecurityDescriptorSacl(copy, &present, &sacl, &defaulted) && present) SetSecurityDescriptorSacl(&abs, TRUE, sacl, defaulted);
        *needed = 0;
        ok = MakeSelfRelativeSD(&abs, sd, &out_size);
        if (!ok) *needed = out_size;
        else *needed = out_size;
        free(blob);
        if (!ok) SetLastError(ERROR_INSUFFICIENT_BUFFER);
        return ok;
    }
    SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI SetServiceObjectSecurity(SC_HANDLE h, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd)
{
    ScHandle *s = find_handle(h, H_SERVICE, 0);
    HKEY key;
    DWORD size;
    BYTE *relative;
    if (!s) return FALSE;
    if (!sd || !IsValidSecurityDescriptor(sd)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (((info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION)) && !(s->access & WRITE_OWNER)) || ((info & DACL_SECURITY_INFORMATION) && !(s->access & WRITE_DAC)) ||
        ((info & SACL_SECURITY_INFORMATION) && !(s->access & ACCESS_SYSTEM_SECURITY))) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    size = GetSecurityDescriptorLength(sd);
    if (!(relative = malloc(size + 64))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    size += 64;
    if (!MakeSelfRelativeSD(sd, relative, &size)) { free(relative); return FALSE; }
    if (open_service_key(s->name, KEY_ALL_ACCESS, &key) != ERROR_SUCCESS) { free(relative); SetLastError(ERROR_SERVICE_MARKED_FOR_DELETE); return FALSE; }
    RegSetValueExW(key, L"Security", 0, REG_BINARY, relative, size);
    RegCloseKey(key);
    free(relative);
    return TRUE;
}

/* ---- notification of a change of state ------------------------------------------------------------------------------------------------------------------------ */

typedef struct { WCHAR name[MAX_NAME + 1]; DWORD mask; PSERVICE_NOTIFYW notify; HANDLE thread; DWORD state; } Watcher;

static VOID CALLBACK notify_apc(ULONG_PTR parameter)
{
    PSERVICE_NOTIFYW n = (PSERVICE_NOTIFYW)parameter;
    if (n->pfnNotifyCallback) n->pfnNotifyCallback(n);
}
static DWORD WINAPI watcher_thread(LPVOID parameter)
{
    Watcher *w = parameter;
    HANDLE target = w->thread;
    for (;;) {
        SERVICE_STATUS_PROCESS status;
        DWORD seen = 0;
        reap_deleted(w->name);
        read_status(w->name, &status);
        if (status.dwCurrentState != w->state) {
            switch (status.dwCurrentState) {
            case SERVICE_STOPPED: seen = SERVICE_NOTIFY_STOPPED; break;
            case SERVICE_START_PENDING: seen = SERVICE_NOTIFY_START_PENDING; break;
            case SERVICE_STOP_PENDING: seen = SERVICE_NOTIFY_STOP_PENDING; break;
            case SERVICE_RUNNING: seen = SERVICE_NOTIFY_RUNNING; break;
            case SERVICE_CONTINUE_PENDING: seen = SERVICE_NOTIFY_CONTINUE_PENDING; break;
            case SERVICE_PAUSE_PENDING: seen = SERVICE_NOTIFY_PAUSE_PENDING; break;
            case SERVICE_PAUSED: seen = SERVICE_NOTIFY_PAUSED; break;
            }
            w->state = status.dwCurrentState;
            if (seen & w->mask) {
                w->notify->dwNotificationStatus = ERROR_SUCCESS;
                w->notify->ServiceStatus = status;
                w->notify->dwNotificationTriggered = seen;
                w->notify->pszServiceNames = NULL;
                QueueUserAPC(notify_apc, target, (ULONG_PTR)w->notify);
                break;
            }
        }
        Sleep(50);
    }
    CloseHandle(target);
    free(w);
    return 0;
}
__declspec(dllexport) DWORD WINAPI NotifyServiceStatusChangeW(SC_HANDLE h, DWORD mask, PSERVICE_NOTIFYW notify)
{
    ScHandle *s = find_handle(h, H_SERVICE, SERVICE_QUERY_STATUS);
    Watcher *w;
    HANDLE self = NULL;
    SERVICE_STATUS_PROCESS status;
    if (!s) return GetLastError();
    if (!notify || notify->dwVersion < SERVICE_NOTIFY_STATUS_CHANGE || !notify->pfnNotifyCallback || !mask) return ERROR_INVALID_PARAMETER;
    if (!(w = calloc(1, sizeof(*w)))) return ERROR_NOT_ENOUGH_MEMORY;
    wcscpy(w->name, s->name);
    w->mask = mask;
    w->notify = notify;
    read_status(s->name, &status);
    w->state = status.dwCurrentState;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self, 0, FALSE, DUPLICATE_SAME_ACCESS)) { free(w); return GetLastError(); }
    w->thread = self;
    {
        HANDLE t = CreateThread(NULL, 0, watcher_thread, w, 0, NULL);
        if (!t) { DWORD e = GetLastError(); CloseHandle(self); free(w); return e; }
        CloseHandle(t);
    }
    return ERROR_SUCCESS;
}
__declspec(dllexport) DWORD WINAPI NotifyServiceStatusChangeA(SC_HANDLE h, DWORD mask, PSERVICE_NOTIFYA notify) { return NotifyServiceStatusChangeW(h, mask, (PSERVICE_NOTIFYW)notify); }

__declspec(dllexport) BOOL WINAPI QueryServiceDynamicInformation(SERVICE_STATUS_HANDLE h, DWORD level, PVOID *info)
{
    (void)h; (void)level; (void)info;
    SetLastError(ERROR_NOT_SUPPORTED);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI SetServiceBits(SERVICE_STATUS_HANDLE h, DWORD mask, BOOL set, BOOL update)
{
    HKEY key;
    DWORD bits;
    (void)h; (void)update;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, STATE_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, &key, NULL) != ERROR_SUCCESS) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    bits = get_dword(key, L"ServiceBits", 0);
    bits = set ? (bits | mask) : (bits & ~mask);
    set_dword(key, L"ServiceBits", bits);
    RegCloseKey(key);
    return TRUE;
}

/* ---- the service's side ------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct ServiceSlot {
    DWORD magic;
    WCHAR name[MAX_NAME + 1];
    LPHANDLER_FUNCTION handler;
    LPHANDLER_FUNCTION_EX handler_ex;
    LPVOID context;
    HANDLE control_event, ack_event, stopped_event, thread;
    volatile LONG stop;
    SERVICE_STATUS_PROCESS status;
    LPSERVICE_MAIN_FUNCTIONW main_w;
    LPSERVICE_MAIN_FUNCTIONA main_a;
    LPCWSTR *argv;
    DWORD argc;
} ServiceSlot;
#define SLOT_MAGIC 0x5356434Cu
#define MAX_SLOTS 16
static ServiceSlot g_slots[MAX_SLOTS];
static volatile LONG g_slot_lock;

static ServiceSlot *slot_of(SERVICE_STATUS_HANDLE h)
{
    ServiceSlot *s = (ServiceSlot *)h;
    if (s < g_slots || s >= g_slots + MAX_SLOTS || ((ULONG_PTR)s - (ULONG_PTR)g_slots) % sizeof(ServiceSlot) || s->magic != SLOT_MAGIC) return NULL;
    return s;
}
static ServiceSlot *slot_named(LPCWSTR name)
{
    DWORD i;
    for (i = 0; i < MAX_SLOTS; ++i) if (g_slots[i].magic == SLOT_MAGIC && !wcsicmp(g_slots[i].name, name)) return &g_slots[i];
    return NULL;
}

/* The thread that hears of controls: it waits for the control event, reads the code, runs the handler and says so. */
static DWORD WINAPI control_thread(LPVOID parameter)
{
    ServiceSlot *s = parameter;
    while (!s->stop) {
        HKEY key;
        DWORD control = 0, event_type = 0, data_size = 0;
        BYTE *data = NULL;
        if (WaitForSingleObject(s->control_event, 200) != WAIT_OBJECT_0) continue;
        if (open_state_key(s->name, FALSE, &key) == ERROR_SUCCESS) {
            control = get_dword(key, L"Control", 0);
            event_type = get_dword(key, L"ControlEventType", 0);
            if (RegQueryValueExW(key, L"ControlData", NULL, NULL, NULL, &data_size) == ERROR_SUCCESS && data_size && (data = malloc(data_size))) RegQueryValueExW(key, L"ControlData", NULL, NULL, data, &data_size);
            RegCloseKey(key);
        }
        if (control) {
            if (s->handler_ex) s->handler_ex(control, event_type, data, s->context);
            else if (s->handler) s->handler(control);
        }
        free(data);
        SetEvent(s->ack_event);
    }
    return 0;
}

static SERVICE_STATUS_HANDLE register_handler(LPCWSTR name, LPHANDLER_FUNCTION handler, LPHANDLER_FUNCTION_EX handler_ex, LPVOID context)
{
    ServiceSlot *s;
    WCHAR en[MAX_NAME + 20], an[MAX_NAME + 20], mn[MAX_NAME + 20];
    if (!name || (!handler && !handler_ex)) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
    if (!(s = slot_named(name))) { SetLastError(ERROR_SERVICE_NOT_IN_EXE); return NULL; }
    s->handler = handler;
    s->handler_ex = handler_ex;
    s->context = context;
    if (!s->control_event) {
        control_names(s->name, en, an, mn);
        s->control_event = CreateEventW(NULL, FALSE, FALSE, en);
        s->ack_event = CreateEventW(NULL, FALSE, FALSE, an);
        if (!s->control_event || !s->ack_event) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return NULL; }
        s->thread = CreateThread(NULL, 0, control_thread, s, 0, NULL);
    }
    return (SERVICE_STATUS_HANDLE)s;
}
__declspec(dllexport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerExW(LPCWSTR name, LPHANDLER_FUNCTION_EX handler, LPVOID context) { return register_handler(name, NULL, handler, context); }
__declspec(dllexport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerW(LPCWSTR name, LPHANDLER_FUNCTION handler) { return register_handler(name, handler, NULL, NULL); }
__declspec(dllexport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerExA(LPCSTR name, LPHANDLER_FUNCTION_EX handler, LPVOID context)
{
    WCHAR *w = strdupAW(name);
    SERVICE_STATUS_HANDLE r = register_handler(w, NULL, handler, context);
    free(w);
    return r;
}
__declspec(dllexport) SERVICE_STATUS_HANDLE WINAPI RegisterServiceCtrlHandlerA(LPCSTR name, LPHANDLER_FUNCTION handler)
{
    WCHAR *w = strdupAW(name);
    SERVICE_STATUS_HANDLE r = register_handler(w, handler, NULL, NULL);
    free(w);
    return r;
}

__declspec(dllexport) BOOL WINAPI SetServiceStatus(SERVICE_STATUS_HANDLE h, LPSERVICE_STATUS status)
{
    ServiceSlot *s = slot_of(h);
    if (!s) { SetLastError(ERROR_INVALID_HANDLE); return FALSE; }
    if (!status) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    if (status->dwCurrentState < SERVICE_STOPPED || status->dwCurrentState > SERVICE_PAUSED) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    if (status->dwServiceType & ~(SERVICE_WIN32 | SERVICE_INTERACTIVE_PROCESS | 0x60) || !status->dwServiceType) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    memcpy(&s->status, status, sizeof(*status));
    s->status.dwProcessId = GetCurrentProcessId();
    s->status.dwServiceFlags = 0;
    write_status(s->name, &s->status);
    if (status->dwCurrentState == SERVICE_STOPPED && s->stopped_event) SetEvent(s->stopped_event);
    return TRUE;
}

typedef struct { ServiceSlot *slot; } MainArgs;
static DWORD WINAPI service_main_thread(LPVOID parameter)
{
    ServiceSlot *s = parameter;
    if (s->main_w) s->main_w(s->argc, (LPWSTR *)s->argv);
    else if (s->main_a) {
        DWORD i;
        char **argv = calloc(s->argc + 1, sizeof(char *));
        if (argv) {
            for (i = 0; i < s->argc; ++i) {
                int n = WideCharToMultiByte(CP_ACP, 0, s->argv[i], -1, NULL, 0, NULL, NULL);
                argv[i] = malloc(n + 1);
                if (argv[i]) WideCharToMultiByte(CP_ACP, 0, s->argv[i], -1, argv[i], n, NULL, NULL);
            }
            s->main_a(s->argc, argv);
        }
    }
    return 0;
}

/* The one service this process was started for: its name is in the environment; its ServiceMain runs on a thread, and the dispatcher returns when it has said it stopped. */
static BOOL dispatch(const SERVICE_TABLE_ENTRYW *table, const SERVICE_TABLE_ENTRYA *table_a)
{
    WCHAR name[MAX_NAME + 1], *args = NULL;
    HKEY key;
    DWORD i, argc = 1, count = 0;
    ServiceSlot *s;
    const WCHAR *a;
    HANDLE main_thread;
    SERVICE_STATUS_PROCESS status;
    if ((!table && !table_a)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!GetEnvironmentVariableW(L"TCOS_SERVICE_NAME", name, MAX_NAME + 1)) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return FALSE; }
    for (i = 0; ; ++i) {
        WCHAR *entry_name = table ? (WCHAR *)table[i].lpServiceName : (table_a[i].lpServiceName ? strdupAW(table_a[i].lpServiceName) : NULL);
        BOOL last = table ? (table[i].lpServiceName == NULL && table[i].lpServiceProc == NULL) : (table_a[i].lpServiceName == NULL && table_a[i].lpServiceProc == NULL);
        BOOL match;
        if (last) break;
        /* an own-process service may leave its name out or write anything: it is the one entry */
        match = (entry_name && !wcsicmp(entry_name, name)) || (service_type_of(name) & SERVICE_WIN32_OWN_PROCESS);
        if (!table && entry_name) free(entry_name);
        if (match && !count) {
            if (!(s = slot_named(name))) {
                for (count = 0; count < MAX_SLOTS && g_slots[count].magic == SLOT_MAGIC; ++count) {}
                if (count >= MAX_SLOTS) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
                s = &g_slots[count];
                memset(s, 0, sizeof(*s));
                s->magic = SLOT_MAGIC;
                wcscpy(s->name, name);
            }
            if (table) s->main_w = table[i].lpServiceProc; else s->main_a = table_a[i].lpServiceProc;
            count = 1;
        }
    }
    if (!count) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return FALSE; }
    s = slot_named(name);
    /* claim the state: only a service that is waiting to be started is ours */
    read_status(name, &status);
    if (status.dwCurrentState != SERVICE_START_PENDING || status.dwProcessId != 0) { SetLastError(ERROR_FAILED_SERVICE_CONTROLLER_CONNECT); return FALSE; }
    s->status = status;
    s->status.dwProcessId = GetCurrentProcessId();
    write_status(name, &s->status);
    s->stopped_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    /* ServiceMain(argc, argv): the name and the arguments of StartService */
    if (open_state_key(name, FALSE, &key) == ERROR_SUCCESS) { args = get_string(key, L"Args", NULL); RegCloseKey(key); }
    for (a = args; a && *a; a += wcslen(a) + 1) ++argc;
    s->argv = calloc(argc + 1, sizeof(LPCWSTR));
    s->argc = argc;
    if (!s->argv) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    s->argv[0] = s->name;
    for (a = args, i = 1; a && *a; a += wcslen(a) + 1) s->argv[i++] = a;
    main_thread = CreateThread(NULL, 0, service_main_thread, s, 0, NULL);
    if (!main_thread) { SetLastError(ERROR_SERVICE_NO_THREAD); return FALSE; }
    WaitForSingleObject(s->stopped_event, INFINITE);
    WaitForSingleObject(main_thread, 5000);
    CloseHandle(main_thread);
    s->stop = 1;
    if (s->thread) { WaitForSingleObject(s->thread, 1000); CloseHandle(s->thread); s->thread = NULL; }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI StartServiceCtrlDispatcherW(const SERVICE_TABLE_ENTRYW *table) { return dispatch(table, NULL); }
__declspec(dllexport) BOOL WINAPI StartServiceCtrlDispatcherA(const SERVICE_TABLE_ENTRYA *table) { return dispatch(NULL, table); }
