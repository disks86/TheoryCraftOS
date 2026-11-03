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

/* advapi32.dll: the policy database of the local security authority and the audit policy. It is kept in the registry, under HKLM\SECURITY\Policy:
 *   Accounts\<SID>\    Rights (MULTI_SZ: privileges and logon rights), SystemAccess (DWORD), Quotas (QUOTA_LIMITS)
 *   Secrets\<name>\    CurrVal, OldVal, CurrTime, OldTime
 *   PrivateData\<key>  the data of LsaStorePrivateData
 *   Audit\<subcategory GUID>     the system audit policy of a subcategory (DWORD of POLICY_AUDIT_EVENT_* flags)
 *   AuditUsers\<SID>\<GUID>      the per-user audit policy
 * A machine that is not in a domain has no trusted domains: the functions that make or change one answer STATUS_NOT_SUPPORTED. */
#include "security.h"
#include <wchar.h>

NTSTATUS WINAPI LsaLookupPrivilegeName(LSA_HANDLE handle, LUID *luid, LSA_UNICODE_STRING **name);
const WCHAR *get_wellknown_privilege_name(const LUID *);

#define POLICY_KEY L"SECURITY\\Policy"
#define POLICY_HANDLE ((LSA_HANDLE)0xcafe)
#define STATUS_NOT_SUPPORTED_ ((NTSTATUS)0xC00000BB)
#define STATUS_NO_SUCH_PRIVILEGE_ ((NTSTATUS)0xC0000060)
#define STATUS_OBJECT_NAME_NOT_FOUND_ ((NTSTATUS)0xC0000034)
#define STATUS_OBJECT_NAME_EXISTS_ ((NTSTATUS)0x40000000)
#define STATUS_PRIVILEGE_NOT_HELD_ ((NTSTATUS)0xC0000061)
#define STATUS_INVALID_HANDLE_ ((NTSTATUS)0xC0000008)

static void wcsncat_(WCHAR *dest, const WCHAR *src, size_t n) { dest += wcslen(dest); while (n-- && *src) *dest++ = *src++; *dest = 0; }
static NTSTATUS nt_of(LONG r)
{
    switch (r) {
    case ERROR_SUCCESS: return STATUS_SUCCESS;
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return STATUS_OBJECT_NAME_NOT_FOUND_;
    case ERROR_ACCESS_DENIED: return STATUS_ACCESS_DENIED;
    case ERROR_NOT_ENOUGH_MEMORY: return STATUS_NO_MEMORY;
    default: return STATUS_UNSUCCESSFUL;
    }
}
static BOOL privilege_enabled(ULONG low)
{
    PRIVILEGE_SET set;
    HANDLE token;
    BOOL held = FALSE;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    memset(&set, 0, sizeof(set));
    set.PrivilegeCount = 1;
    set.Privilege[0].Luid.LowPart = low;
    if (!PrivilegeCheck(token, &set, &held)) held = FALSE;
    CloseHandle(token);
    return held;
}
static BOOL open_policy(LPCWSTR sub, BOOL create, REGSAM access, HKEY *key)
{
    WCHAR path[600];
    wcscpy(path, POLICY_KEY);
    if (sub && *sub) { wcscat(path, L"\\"); wcscat(path, sub); }
    if (create) return RegCreateKeyExW(HKEY_LOCAL_MACHINE, path, 0, NULL, 0, access, NULL, key, NULL) == ERROR_SUCCESS;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, access, key) == ERROR_SUCCESS;
}
static WCHAR *dup_unicode(const LSA_UNICODE_STRING *s)
{
    WCHAR *r;
    if (!s || !s->Buffer) return NULL;
    if ((r = malloc((size_t)s->Length + 2))) { memcpy(r, s->Buffer, s->Length); r[s->Length / 2] = 0; }
    return r;
}

/* ---- the privileges and logon rights ----------------------------------------------------------------------------------------------------------------------------- */

static const WCHAR *const g_logon_rights[] = {L"SeInteractiveLogonRight", L"SeNetworkLogonRight", L"SeBatchLogonRight", L"SeServiceLogonRight", L"SeDenyInteractiveLogonRight",
                                              L"SeDenyNetworkLogonRight", L"SeDenyBatchLogonRight", L"SeDenyServiceLogonRight", L"SeRemoteInteractiveLogonRight", L"SeDenyRemoteInteractiveLogonRight"};
static const DWORD g_logon_right_bits[] = {1, 2, 4, 0x10, 0x80, 0x100, 0x40, 0x20, 0x400, 0x800};      /* SECURITY_ACCESS_* */

static BOOL privilege_luid(LPCWSTR name, LUID *luid)
{
    DWORD low;
    for (low = 2; low < 64; ++low) {
        LUID l = {low, 0};
        const WCHAR *n = get_wellknown_privilege_name(&l);
        if (n && !wcsicmp(n, name)) { if (luid) *luid = l; return TRUE; }
    }
    return FALSE;
}
static BOOL known_right(LPCWSTR name)
{
    DWORD i;
    if (privilege_luid(name, NULL)) return TRUE;
    for (i = 0; i < sizeof(g_logon_rights) / sizeof(g_logon_rights[0]); ++i) if (!wcsicmp(g_logon_rights[i], name)) return TRUE;
    return FALSE;
}
static const WCHAR *canonical_right(LPCWSTR name)
{
    DWORD i, low;
    for (low = 2; low < 64; ++low) {
        LUID l = {low, 0};
        const WCHAR *n = get_wellknown_privilege_name(&l);
        if (n && !wcsicmp(n, name)) return n;
    }
    for (i = 0; i < sizeof(g_logon_rights) / sizeof(g_logon_rights[0]); ++i) if (!wcsicmp(g_logon_rights[i], name)) return g_logon_rights[i];
    return NULL;
}
static BOOL is_privilege_name(LPCWSTR name) { return privilege_luid(name, NULL); }

/* ---- accounts --------------------------------------------------------------------------------------------------------------------------------------------------- */

static WCHAR *sid_text(PSID sid)
{
    LPWSTR t = NULL, r;
    if (!sid || !IsValidSid(sid) || !ConvertSidToStringSidW(sid, &t)) return NULL;
    r = malloc((wcslen(t) + 1) * 2);
    if (r) wcscpy(r, t);
    LocalFree(t);
    return r;
}
static BOOL account_key(PSID sid, BOOL create, REGSAM access, HKEY *key)
{
    WCHAR sub[300] = L"Accounts\\";
    WCHAR *t = sid_text(sid);
    BOOL ok;
    if (!t) return FALSE;
    wcsncat_(sub, t, 250);
    free(t);
    ok = open_policy(sub, create, access, key);
    return ok;
}
/* The rights of an account as a MULTI_SZ block (free() it); an empty block if it has none. */
static WCHAR *account_rights(HKEY key)
{
    DWORD type = 0, size = 0;
    WCHAR *list;
    if (RegQueryValueExW(key, L"Rights", NULL, &type, NULL, &size) != ERROR_SUCCESS || type != REG_MULTI_SZ || size > 65536) size = 0;
    if (!(list = calloc(1, size + 8))) return NULL;
    if (size && RegQueryValueExW(key, L"Rights", NULL, &type, (BYTE *)list, &size) != ERROR_SUCCESS) list[0] = 0;
    return list;
}
static BOOL right_in(const WCHAR *list, LPCWSTR name)
{
    for (; *list; list += wcslen(list) + 1) if (!wcsicmp(list, name)) return TRUE;
    return FALSE;
}
static void store_rights(HKEY key, const WCHAR *list)
{
    DWORD n = 0;
    while (list[n]) n += (DWORD)wcslen(list + n) + 1;
    if (n == 0) RegDeleteValueW(key, L"Rights"); else RegSetValueExW(key, L"Rights", 0, REG_MULTI_SZ, (const BYTE *)list, (n + 1) * 2);
}
static BOOL account_empty(HKEY key)
{
    DWORD values = 0;
    WCHAR *r = account_rights(key);
    DWORD access = 0, size = 4, type;
    BOOL empty;
    (void)values;
    RegQueryValueExW(key, L"SystemAccess", NULL, &type, (BYTE *)&access, &size);
    empty = r && !r[0] && !access;
    free(r);
    return empty;
}

__declspec(dllexport) NTSTATUS WINAPI LsaAddAccountRights(LSA_HANDLE policy, PSID sid, PLSA_UNICODE_STRING rights, ULONG count)
{
    HKEY key;
    WCHAR *old, *merged;
    ULONG i;
    size_t total;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!sid || !IsValidSid(sid) || !rights || !count) return STATUS_INVALID_PARAMETER;
    for (i = 0; i < count; ++i) {
        WCHAR *n = dup_unicode(&rights[i]);
        BOOL ok = n && known_right(n);
        free(n);
        if (!ok) return STATUS_NO_SUCH_PRIVILEGE_;
    }
    if (!account_key(sid, TRUE, KEY_ALL_ACCESS, &key)) return STATUS_ACCESS_DENIED;
    if (!(old = account_rights(key))) { RegCloseKey(key); return STATUS_NO_MEMORY; }
    total = 1;
    { const WCHAR *p; for (p = old; *p; p += wcslen(p) + 1) total += wcslen(p) + 1; }
    for (i = 0; i < count; ++i) total += rights[i].Length / 2 + 1;
    if (!(merged = calloc(total + 2, 2))) { free(old); RegCloseKey(key); return STATUS_NO_MEMORY; }
    {
        WCHAR *at = merged;
        const WCHAR *p;
        for (p = old; *p; p += wcslen(p) + 1) { wcscpy(at, p); at += wcslen(at) + 1; }
        for (i = 0; i < count; ++i) {
            WCHAR *n = dup_unicode(&rights[i]);
            const WCHAR *canonical = n ? canonical_right(n) : NULL;
            if (canonical && !right_in(merged, canonical)) { wcscpy(at, canonical); at += wcslen(at) + 1; }
            free(n);
        }
        *at = 0;
    }
    store_rights(key, merged);
    free(merged); free(old);
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaRemoveAccountRights(LSA_HANDLE policy, PSID sid, BOOLEAN all, PLSA_UNICODE_STRING rights, ULONG count)
{
    HKEY key;
    WCHAR *old, *kept;
    ULONG i;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!sid || !IsValidSid(sid)) return STATUS_INVALID_PARAMETER;
    if (!all && (!rights || !count)) return STATUS_INVALID_PARAMETER;
    if (!account_key(sid, FALSE, KEY_ALL_ACCESS, &key)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    if (all) {
        WCHAR path[300] = L"Accounts", *t = sid_text(sid);
        HKEY accounts;
        RegCloseKey(key);
        if (t && open_policy(path, FALSE, KEY_ALL_ACCESS, &accounts)) { RegDeleteTreeW(accounts, t); RegCloseKey(accounts); }
        free(t);
        return STATUS_SUCCESS;
    }
    for (i = 0; i < count; ++i) {
        WCHAR *n = dup_unicode(&rights[i]);
        BOOL ok = n && known_right(n);
        free(n);
        if (!ok) { RegCloseKey(key); return STATUS_NO_SUCH_PRIVILEGE_; }
    }
    old = account_rights(key);
    if (!old || !(kept = calloc(wcslen(old) + 8 + 2, 2))) { free(old); RegCloseKey(key); return STATUS_NO_MEMORY; }
    {
        const WCHAR *p;
        WCHAR *at = kept;
        for (p = old; *p; p += wcslen(p) + 1) {
            BOOL drop = FALSE;
            size_t len = 0;
            for (i = 0; i < count && !drop; ++i) {
                WCHAR *n = dup_unicode(&rights[i]);
                if (n && !wcsicmp(n, p)) drop = TRUE;
                free(n);
            }
            (void)len;
            if (!drop) { wcscpy(at, p); at += wcslen(at) + 1; }
        }
        *at = 0;
    }
    store_rights(key, kept);
    free(kept); free(old);
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaEnumerateAccountRights(LSA_HANDLE policy, PSID sid, PLSA_UNICODE_STRING *rights, PULONG count)
{
    HKEY key;
    WCHAR *list;
    const WCHAR *p;
    ULONG n = 0;
    size_t chars = 0;
    LSA_UNICODE_STRING *out;
    WCHAR *text;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!sid || !IsValidSid(sid) || !rights || !count) return STATUS_INVALID_PARAMETER;
    *rights = NULL;
    *count = 0;
    if (!account_key(sid, FALSE, KEY_READ, &key)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    list = account_rights(key);
    RegCloseKey(key);
    if (!list) return STATUS_NO_MEMORY;
    for (p = list; *p; p += wcslen(p) + 1) { ++n; chars += wcslen(p) + 1; }
    if (!n) { free(list); return STATUS_OBJECT_NAME_NOT_FOUND_; }
    if (!(out = malloc(n * sizeof(*out) + chars * 2))) { free(list); return STATUS_NO_MEMORY; }
    text = (WCHAR *)(out + n);
    for (n = 0, p = list; *p; p += wcslen(p) + 1, ++n) {
        size_t len = wcslen(p);
        memcpy(text, p, (len + 1) * 2);
        out[n].Buffer = text;
        out[n].Length = (USHORT)(len * 2);
        out[n].MaximumLength = (USHORT)(len * 2 + 2);
        text += len + 1;
    }
    free(list);
    *rights = out;
    *count = n;
    return STATUS_SUCCESS;
}

/* the SIDs of all accounts, or of those with a right */
static NTSTATUS collect_accounts(LPCWSTR right, PSID **sids, ULONG *count)
{
    HKEY accounts;
    DWORD i, n = 0, capacity = 8;
    PSID *list = calloc(capacity, sizeof(PSID));
    *sids = NULL;
    *count = 0;
    if (!list) return STATUS_NO_MEMORY;
    if (!open_policy(L"Accounts", FALSE, KEY_READ, &accounts)) { free(list); return STATUS_SUCCESS; }
    for (i = 0;; ++i) {
        WCHAR name[200];
        DWORD len = 200;
        HKEY key;
        PSID sid = NULL;
        BOOL match;
        if (RegEnumKeyExW(accounts, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
        if (RegOpenKeyExW(accounts, name, 0, KEY_READ, &key) != ERROR_SUCCESS) continue;
        if (right) { WCHAR *r = account_rights(key); match = r && right_in(r, right); free(r); } else match = TRUE;
        RegCloseKey(key);
        if (!match || !ConvertStringSidToSidW(name, &sid)) continue;
        if (n == capacity) { PSID *bigger = realloc(list, capacity * 2 * sizeof(PSID)); if (!bigger) { LocalFree(sid); break; } list = bigger; capacity *= 2; }
        list[n++] = sid;
    }
    RegCloseKey(accounts);
    *sids = list;
    *count = n;
    return STATUS_SUCCESS;
}
/* An array of SIDs as one block: LSA_ENUMERATION_INFORMATION[] (a PSID each), the SIDs after them. */
static NTSTATUS pack_sids(PSID *list, ULONG n, PVOID *buffer)
{
    ULONG i, total = n * sizeof(LSA_ENUMERATION_INFORMATION), at;
    BYTE *block;
    LSA_ENUMERATION_INFORMATION *info;
    for (i = 0; i < n; ++i) total += (GetLengthSid(list[i]) + 7) & ~7u;
    if (!(block = calloc(1, total + 8))) return STATUS_NO_MEMORY;
    info = (LSA_ENUMERATION_INFORMATION *)block;
    at = n * sizeof(LSA_ENUMERATION_INFORMATION);
    for (i = 0; i < n; ++i) {
        DWORD len = GetLengthSid(list[i]);
        memcpy(block + at, list[i], len);
        info[i].Sid = block + at;
        at += (len + 7) & ~7u;
    }
    *buffer = block;
    return STATUS_SUCCESS;
}
static void free_sids(PSID *list, ULONG n) { ULONG i; for (i = 0; i < n; ++i) LocalFree(list[i]); free(list); }
__declspec(dllexport) NTSTATUS WINAPI LsaEnumerateAccounts(LSA_HANDLE policy, PLSA_ENUMERATION_HANDLE context, PVOID *buffer, ULONG max_length, PULONG count)
{
    PSID *list;
    ULONG n, first = context ? (ULONG)*context : 0;
    NTSTATUS status;
    (void)max_length;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!context || !buffer || !count) return STATUS_INVALID_PARAMETER;
    *count = 0;
    if ((status = collect_accounts(NULL, &list, &n))) return status;
    if (first >= n) { free_sids(list, n); return STATUS_NO_MORE_ENTRIES; }
    status = pack_sids(list + first, n - first, buffer);
    if (!status) { *count = n - first; *context = n; }
    free_sids(list, n);
    return status;
}
__declspec(dllexport) NTSTATUS WINAPI LsaEnumerateAccountsWithUserRight(LSA_HANDLE policy, PLSA_UNICODE_STRING right, PVOID *buffer, PULONG count)
{
    PSID *list;
    ULONG n;
    NTSTATUS status;
    WCHAR *name;
    const WCHAR *canonical;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!buffer || !count) return STATUS_INVALID_PARAMETER;
    *count = 0;
    name = dup_unicode(right);
    canonical = name ? canonical_right(name) : NULL;
    free(name);
    if (right && right->Buffer && !canonical) return STATUS_NO_SUCH_PRIVILEGE_;
    if ((status = collect_accounts(canonical, &list, &n))) return status;
    if (!n) { free_sids(list, n); return STATUS_NO_MORE_ENTRIES; }
    status = pack_sids(list, n, buffer);
    if (!status) *count = n;
    free_sids(list, n);
    return status;
}

/* ---- LSA objects (accounts and secrets) opened by handle ------------------------------------------------------------------------------------------------------ */

enum { OBJ_FREE, OBJ_ACCOUNT, OBJ_SECRET };
typedef struct { DWORD magic, kind; WCHAR name[260]; } LsaObject;
#define OBJ_SLOTS 64
#define OBJ_BASE 0x4C530000u
static LsaObject g_objects[OBJ_SLOTS];
static volatile LONG g_object_lock;
static void ol(void) { while (InterlockedCompareExchange(&g_object_lock, 1, 0)) Sleep(0); }
static void ou(void) { InterlockedExchange(&g_object_lock, 0); }
static LSA_HANDLE new_object(DWORD kind, LPCWSTR name)
{
    DWORD i;
    LSA_HANDLE h = NULL;
    ol();
    for (i = 0; i < OBJ_SLOTS; ++i) if (!g_objects[i].kind) { g_objects[i].kind = kind; g_objects[i].magic = OBJ_BASE; wcsncpy(g_objects[i].name, name, 259); g_objects[i].name[259] = 0; h = (LSA_HANDLE)(ULONG_PTR)(OBJ_BASE + i + 1); break; }
    ou();
    return h;
}
static LsaObject *object_of(LSA_HANDLE h, DWORD kind)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v <= OBJ_BASE || v > OBJ_BASE + OBJ_SLOTS) return NULL;
    return g_objects[v - OBJ_BASE - 1].kind == kind ? &g_objects[v - OBJ_BASE - 1] : NULL;
}
/* called by LsaClose (lsa.c): true if it was one of these */
BOOL lsa_close_object(LSA_HANDLE h)
{
    ULONG_PTR v = (ULONG_PTR)h;
    if (v <= OBJ_BASE || v > OBJ_BASE + OBJ_SLOTS) return FALSE;
    ol();
    g_objects[v - OBJ_BASE - 1].kind = OBJ_FREE;
    ou();
    return TRUE;
}

__declspec(dllexport) NTSTATUS WINAPI LsaCreateAccount(LSA_HANDLE policy, PSID sid, ACCESS_MASK access, PLSA_HANDLE out)
{
    HKEY key;
    WCHAR *t;
    (void)access;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!sid || !IsValidSid(sid) || !out) return STATUS_INVALID_PARAMETER;
    if (account_key(sid, FALSE, KEY_READ, &key)) { RegCloseKey(key); return STATUS_OBJECT_NAME_EXISTS_ | 0xC0000000u; }      /* STATUS_OBJECT_NAME_COLLISION */
    if (!account_key(sid, TRUE, KEY_ALL_ACCESS, &key)) return STATUS_ACCESS_DENIED;
    RegCloseKey(key);
    t = sid_text(sid);
    *out = new_object(OBJ_ACCOUNT, t);
    free(t);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
__declspec(dllexport) NTSTATUS WINAPI LsaOpenAccount(LSA_HANDLE policy, PSID sid, ACCESS_MASK access, PLSA_HANDLE out)
{
    HKEY key;
    WCHAR *t;
    (void)access;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!sid || !IsValidSid(sid) || !out) return STATUS_INVALID_PARAMETER;
    if (!account_key(sid, FALSE, KEY_READ, &key)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    RegCloseKey(key);
    t = sid_text(sid);
    *out = new_object(OBJ_ACCOUNT, t);
    free(t);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
static BOOL account_of(LSA_HANDLE h, BOOL write, HKEY *key, LsaObject **o)
{
    WCHAR sub[300] = L"Accounts\\";
    *o = object_of(h, OBJ_ACCOUNT);
    if (!*o) return FALSE;
    wcscat(sub, (*o)->name);
    return open_policy(sub, FALSE, write ? KEY_ALL_ACCESS : KEY_READ, key);
}
__declspec(dllexport) NTSTATUS WINAPI LsaAddPrivilegesToAccount(LSA_HANDLE account, PPRIVILEGE_SET privileges)
{
    HKEY key;
    LsaObject *o;
    WCHAR *old, *merged;
    DWORD i;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (!privileges || !privileges->PrivilegeCount) return STATUS_INVALID_PARAMETER;
    if (!account_of(account, TRUE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    old = account_rights(key);
    merged = old ? calloc(wcslen(old) + 8 + privileges->PrivilegeCount * 40, 2) : NULL;
    if (!merged) { free(old); RegCloseKey(key); return STATUS_NO_MEMORY; }
    {
        const WCHAR *p;
        WCHAR *at = merged;
        for (p = old; *p; p += wcslen(p) + 1) { wcscpy(at, p); at += wcslen(at) + 1; }
        for (i = 0; i < privileges->PrivilegeCount; ++i) {
            const WCHAR *n = get_wellknown_privilege_name(&privileges->Privilege[i].Luid);
            if (!n) { free(merged); free(old); RegCloseKey(key); return STATUS_NO_SUCH_PRIVILEGE_; }
            if (!right_in(merged, n)) { wcscpy(at, n); at += wcslen(at) + 1; }
        }
        *at = 0;
    }
    store_rights(key, merged);
    free(merged); free(old);
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaRemovePrivilegesFromAccount(LSA_HANDLE account, BOOLEAN all, PPRIVILEGE_SET privileges)
{
    HKEY key;
    LsaObject *o;
    WCHAR *old, *kept;
    DWORD i;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (!all && (!privileges || !privileges->PrivilegeCount)) return STATUS_INVALID_PARAMETER;
    if (!account_of(account, TRUE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    old = account_rights(key);
    kept = old ? calloc(wcslen(old) + 8 + 2, 2) : NULL;
    if (!kept) { free(old); RegCloseKey(key); return STATUS_NO_MEMORY; }
    {
        const WCHAR *p;
        WCHAR *at = kept;
        for (p = old; *p; p += wcslen(p) + 1) {
            BOOL drop = FALSE;
            if (is_privilege_name(p)) {
                if (all) drop = TRUE;
                else for (i = 0; i < privileges->PrivilegeCount; ++i) { const WCHAR *n = get_wellknown_privilege_name(&privileges->Privilege[i].Luid); if (n && !wcsicmp(n, p)) drop = TRUE; }
            }
            if (!drop) { wcscpy(at, p); at += wcslen(at) + 1; }
        }
        *at = 0;
    }
    store_rights(key, kept);
    free(kept); free(old);
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaEnumeratePrivilegesOfAccount(LSA_HANDLE account, PPRIVILEGE_SET *out)
{
    HKEY key;
    LsaObject *o;
    WCHAR *rights;
    const WCHAR *p;
    DWORD n = 0;
    PRIVILEGE_SET *set;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (!out) return STATUS_INVALID_PARAMETER;
    if (!account_of(account, FALSE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    rights = account_rights(key);
    RegCloseKey(key);
    if (!rights) return STATUS_NO_MEMORY;
    for (p = rights; *p; p += wcslen(p) + 1) if (is_privilege_name(p)) ++n;
    if (!(set = calloc(1, sizeof(PRIVILEGE_SET) + (n ? n - 1 : 0) * sizeof(LUID_AND_ATTRIBUTES) + 8))) { free(rights); return STATUS_NO_MEMORY; }
    set->PrivilegeCount = n;
    for (n = 0, p = rights; *p; p += wcslen(p) + 1) if (privilege_luid(p, &set->Privilege[n].Luid)) { set->Privilege[n].Attributes = 0; ++n; }
    free(rights);
    *out = set;
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaGetQuotasForAccount(LSA_HANDLE account, PQUOTA_LIMITS limits)
{
    HKEY key;
    LsaObject *o;
    DWORD size = sizeof(*limits), type;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (!limits) return STATUS_INVALID_PARAMETER;
    memset(limits, 0, sizeof(*limits));
    if (!account_of(account, FALSE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    if (RegQueryValueExW(key, L"Quotas", NULL, &type, (BYTE *)limits, &size) != ERROR_SUCCESS || type != REG_BINARY || size != sizeof(*limits)) memset(limits, 0, sizeof(*limits));
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaSetQuotasForAccount(LSA_HANDLE account, PQUOTA_LIMITS limits)
{
    HKEY key;
    LsaObject *o;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (!limits) return STATUS_INVALID_PARAMETER;
    if (!account_of(account, TRUE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    RegSetValueExW(key, L"Quotas", 0, REG_BINARY, (const BYTE *)limits, sizeof(*limits));
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaGetSystemAccessAccount(LSA_HANDLE account, PULONG access)
{
    HKEY key;
    LsaObject *o;
    WCHAR *rights;
    DWORD i, bits = 0;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (!access) return STATUS_INVALID_PARAMETER;
    if (!account_of(account, FALSE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    rights = account_rights(key);
    RegCloseKey(key);
    if (!rights) return STATUS_NO_MEMORY;
    for (i = 0; i < sizeof(g_logon_rights) / sizeof(g_logon_rights[0]); ++i) if (right_in(rights, g_logon_rights[i])) bits |= g_logon_right_bits[i];
    free(rights);
    *access = bits;
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaSetSystemAccessAccount(LSA_HANDLE account, ULONG access)
{
    HKEY key;
    LsaObject *o;
    WCHAR *rights, *rebuilt;
    DWORD i;
    if (!object_of(account, OBJ_ACCOUNT)) return STATUS_INVALID_HANDLE_;
    if (access & ~0xFFFu) return STATUS_INVALID_PARAMETER;
    if (!account_of(account, TRUE, &key, &o)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    rights = account_rights(key);
    rebuilt = rights ? calloc(wcslen(rights) + 8 + 600, 2) : NULL;
    if (!rebuilt) { free(rights); RegCloseKey(key); return STATUS_NO_MEMORY; }
    {
        const WCHAR *p;
        WCHAR *at = rebuilt;
        for (p = rights; *p; p += wcslen(p) + 1) {
            BOOL logon = FALSE;
            for (i = 0; i < sizeof(g_logon_rights) / sizeof(g_logon_rights[0]); ++i) if (!wcsicmp(g_logon_rights[i], p)) logon = TRUE;
            if (!logon) { wcscpy(at, p); at += wcslen(at) + 1; }
        }
        for (i = 0; i < sizeof(g_logon_rights) / sizeof(g_logon_rights[0]); ++i) if (access & g_logon_right_bits[i]) { wcscpy(at, g_logon_rights[i]); at += wcslen(at) + 1; }
        *at = 0;
    }
    store_rights(key, rebuilt);
    free(rebuilt); free(rights);
    RegCloseKey(key);
    return STATUS_SUCCESS;
}

/* ---- secrets ---------------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL secret_key(LPCWSTR name, BOOL create, REGSAM access, HKEY *key)
{
    WCHAR sub[400] = L"Secrets\\";
    wcsncat_(sub, name, 300);
    return open_policy(sub, create, access, key);
}
static NTSTATUS valid_secret_name(const LSA_UNICODE_STRING *s, WCHAR **name)
{
    *name = dup_unicode(s);
    if (!*name || !**name || wcschr(*name, '\\') || wcslen(*name) > 255) { free(*name); *name = NULL; return STATUS_INVALID_PARAMETER; }
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaCreateSecret(LSA_HANDLE policy, PLSA_UNICODE_STRING name, ACCESS_MASK access, PLSA_HANDLE out)
{
    HKEY key;
    WCHAR *n;
    NTSTATUS status;
    (void)access;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!out) return STATUS_INVALID_PARAMETER;
    if ((status = valid_secret_name(name, &n))) return status;
    if (secret_key(n, FALSE, KEY_READ, &key)) { RegCloseKey(key); free(n); return (NTSTATUS)0xC0000035; }      /* STATUS_OBJECT_NAME_COLLISION */
    if (!secret_key(n, TRUE, KEY_ALL_ACCESS, &key)) { free(n); return STATUS_ACCESS_DENIED; }
    RegCloseKey(key);
    *out = new_object(OBJ_SECRET, n);
    free(n);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
__declspec(dllexport) NTSTATUS WINAPI LsaOpenSecret(LSA_HANDLE policy, PLSA_UNICODE_STRING name, ACCESS_MASK access, PLSA_HANDLE out)
{
    HKEY key;
    WCHAR *n;
    NTSTATUS status;
    (void)access;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!out) return STATUS_INVALID_PARAMETER;
    if ((status = valid_secret_name(name, &n))) return status;
    if (!secret_key(n, FALSE, KEY_READ, &key)) { free(n); return STATUS_OBJECT_NAME_NOT_FOUND_; }
    RegCloseKey(key);
    *out = new_object(OBJ_SECRET, n);
    free(n);
    return *out ? STATUS_SUCCESS : STATUS_NO_MEMORY;
}
__declspec(dllexport) NTSTATUS WINAPI LsaSetSecret(LSA_HANDLE secret, PLSA_UNICODE_STRING current, PLSA_UNICODE_STRING old)
{
    LsaObject *o = object_of(secret, OBJ_SECRET);
    HKEY key;
    FILETIME ft;
    if (!o) return STATUS_INVALID_HANDLE_;
    if (!secret_key(o->name, FALSE, KEY_ALL_ACCESS, &key)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    GetSystemTimeAsFileTime(&ft);
    if (current) {
        RegSetValueExW(key, L"CurrVal", 0, REG_BINARY, (const BYTE *)current->Buffer, current->Length);
        RegSetValueExW(key, L"CurrTime", 0, REG_QWORD, (const BYTE *)&ft, 8);
    }
    if (old) {
        RegSetValueExW(key, L"OldVal", 0, REG_BINARY, (const BYTE *)old->Buffer, old->Length);
        RegSetValueExW(key, L"OldTime", 0, REG_QWORD, (const BYTE *)&ft, 8);
    }
    RegCloseKey(key);
    return STATUS_SUCCESS;
}
static NTSTATUS secret_value(HKEY key, LPCWSTR value, LPCWSTR time_value, PLSA_UNICODE_STRING *out, PLARGE_INTEGER when)
{
    DWORD size = 0, type = 0;
    if (out) *out = NULL;
    if (RegQueryValueExW(key, value, NULL, &type, NULL, &size) != ERROR_SUCCESS) return STATUS_SUCCESS;      /* never set */
    if (out) {
        LSA_UNICODE_STRING *s = malloc(sizeof(*s) + size + 8);
        if (!s) return STATUS_NO_MEMORY;
        s->Buffer = (PWSTR)(s + 1);
        if (RegQueryValueExW(key, value, NULL, &type, (BYTE *)s->Buffer, &size) != ERROR_SUCCESS) { free(s); return STATUS_UNSUCCESSFUL; }
        s->Length = (USHORT)size;
        s->MaximumLength = (USHORT)(size + 2);
        *out = s;
    }
    if (when) { DWORD n = 8; when->QuadPart = 0; RegQueryValueExW(key, time_value, NULL, NULL, (BYTE *)when, &n); }
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaQuerySecret(LSA_HANDLE secret, PLSA_UNICODE_STRING *current, PLARGE_INTEGER current_time, PLSA_UNICODE_STRING *old, PLARGE_INTEGER old_time)
{
    LsaObject *o = object_of(secret, OBJ_SECRET);
    HKEY key;
    NTSTATUS status;
    if (!o) return STATUS_INVALID_HANDLE_;
    if (!secret_key(o->name, FALSE, KEY_READ, &key)) return STATUS_OBJECT_NAME_NOT_FOUND_;
    status = secret_value(key, L"CurrVal", L"CurrTime", current, current_time);
    if (!status) status = secret_value(key, L"OldVal", L"OldTime", old, old_time);
    RegCloseKey(key);
    return status;
}
__declspec(dllexport) NTSTATUS WINAPI LsaDelete(LSA_HANDLE object)
{
    LsaObject *o;
    HKEY parent;
    NTSTATUS status = STATUS_INVALID_HANDLE_;
    if ((o = object_of(object, OBJ_SECRET))) {
        if (open_policy(L"Secrets", FALSE, KEY_ALL_ACCESS, &parent)) { RegDeleteTreeW(parent, o->name); RegCloseKey(parent); status = STATUS_SUCCESS; } else status = STATUS_OBJECT_NAME_NOT_FOUND_;
    } else if ((o = object_of(object, OBJ_ACCOUNT))) {
        if (open_policy(L"Accounts", FALSE, KEY_ALL_ACCESS, &parent)) { RegDeleteTreeW(parent, o->name); RegCloseKey(parent); status = STATUS_SUCCESS; } else status = STATUS_OBJECT_NAME_NOT_FOUND_;
    }
    if (!status) lsa_close_object(object);      /* (the handle is closed by deleting the object) */
    return status;
}

/* LsaStorePrivateData / LsaRetrievePrivateData: a named value of the policy; a null data deletes it. */
__declspec(dllexport) NTSTATUS WINAPI LsaStorePrivateData(LSA_HANDLE policy, PLSA_UNICODE_STRING key_name, PLSA_UNICODE_STRING data)
{
    HKEY key;
    WCHAR *name;
    NTSTATUS status = STATUS_SUCCESS;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if ((status = valid_secret_name(key_name, &name))) return status;
    if (!data || !data->Buffer) {
        HKEY parent;
        if (open_policy(L"PrivateData", FALSE, KEY_ALL_ACCESS, &parent)) { LONG r = RegDeleteValueW(parent, name); RegCloseKey(parent); status = r == ERROR_SUCCESS ? STATUS_SUCCESS : STATUS_OBJECT_NAME_NOT_FOUND_; }
        else status = STATUS_OBJECT_NAME_NOT_FOUND_;
    } else if (open_policy(L"PrivateData", TRUE, KEY_ALL_ACCESS, &key)) {
        status = nt_of(RegSetValueExW(key, name, 0, REG_BINARY, (const BYTE *)data->Buffer, data->Length));
        RegCloseKey(key);
    } else status = STATUS_ACCESS_DENIED;
    free(name);
    return status;
}
__declspec(dllexport) NTSTATUS WINAPI LsaRetrievePrivateData(LSA_HANDLE policy, PLSA_UNICODE_STRING key_name, PLSA_UNICODE_STRING *out)
{
    HKEY key;
    WCHAR *name;
    NTSTATUS status;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!out) return STATUS_INVALID_PARAMETER;
    *out = NULL;
    if ((status = valid_secret_name(key_name, &name))) return status;
    if (!open_policy(L"PrivateData", FALSE, KEY_READ, &key)) { free(name); return STATUS_OBJECT_NAME_NOT_FOUND_; }
    status = secret_value(key, name, L"", out, NULL);
    RegCloseKey(key);
    free(name);
    if (!status && !*out) status = STATUS_OBJECT_NAME_NOT_FOUND_;
    return status;
}

/* ---- privileges, security object, misc ------------------------------------------------------------------------------------------------------------------------ */

__declspec(dllexport) NTSTATUS WINAPI LsaEnumeratePrivileges(LSA_HANDLE policy, PLSA_ENUMERATION_HANDLE context, PVOID *buffer, ULONG max_length, PULONG count)
{
    typedef struct { LSA_UNICODE_STRING Name; LUID LocalValue; } PrivilegeDefinition;
    DWORD low, n = 0, first = context ? (DWORD)*context : 0, skip = 0, chars = 0;
    PrivilegeDefinition *defs;
    WCHAR *text;
    (void)max_length;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!context || !buffer || !count) return STATUS_INVALID_PARAMETER;
    *count = 0;
    for (low = 2; low < 64; ++low) { LUID l = {low, 0}; const WCHAR *nm = get_wellknown_privilege_name(&l); if (nm) { if (skip++ >= first) { ++n; chars += (DWORD)wcslen(nm) + 1; } } }
    if (!n) return STATUS_NO_MORE_ENTRIES;
    if (!(defs = malloc(n * sizeof(*defs) + chars * 2))) return STATUS_NO_MEMORY;
    text = (WCHAR *)(defs + n);
    for (low = 2, n = 0, skip = 0; low < 64; ++low) {
        LUID l = {low, 0};
        const WCHAR *nm = get_wellknown_privilege_name(&l);
        size_t len;
        if (!nm || skip++ < first) continue;
        len = wcslen(nm);
        memcpy(text, nm, (len + 1) * 2);
        defs[n].Name.Buffer = text; defs[n].Name.Length = (USHORT)(len * 2); defs[n].Name.MaximumLength = (USHORT)(len * 2 + 2);
        defs[n].LocalValue = l;
        text += len + 1;
        ++n;
    }
    *buffer = defs;
    *count = n;
    *context = first + n;
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaLookupPrivilegeValue(LSA_HANDLE policy, PLSA_UNICODE_STRING name, PLUID luid)
{
    WCHAR *n;
    BOOL found;
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    if (!name || !luid) return STATUS_INVALID_PARAMETER;
    n = dup_unicode(name);
    found = n && privilege_luid(n, luid);
    free(n);
    return found ? STATUS_SUCCESS : STATUS_NO_SUCH_PRIVILEGE_;
}
#define LSA_SECURITY_VALUE L"Security"
__declspec(dllexport) NTSTATUS WINAPI LsaQuerySecurityObject(LSA_HANDLE object, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR *out)
{
    static const WCHAR sddl[] = L"O:BAG:SYD:(A;;0xF0FFF;;;BA)(A;;0xF0FFF;;;SY)(A;;0x20801;;;AU)";      /* administrators and the system: all; others: view */
    PSECURITY_DESCRIPTOR sd = NULL;
    ULONG size = 0;
    (void)info;
    if (!object) return STATUS_INVALID_HANDLE_;
    if (!out) return STATUS_INVALID_PARAMETER;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &sd, &size)) return STATUS_UNSUCCESSFUL;
    {
        void *copy = malloc(size);
        if (copy) memcpy(copy, sd, size);
        LocalFree(sd);
        if (!copy) return STATUS_NO_MEMORY;
        *out = copy;
    }
    return STATUS_SUCCESS;
}
__declspec(dllexport) NTSTATUS WINAPI LsaSetSecurityObject(LSA_HANDLE object, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd)
{
    (void)info;
    if (!object) return STATUS_INVALID_HANDLE_;
    if (!sd || !IsValidSecurityDescriptor(sd)) return STATUS_INVALID_PARAMETER;
    return STATUS_SUCCESS;      /* (the policy objects keep the one descriptor above: a change to it is accepted and not kept) */
}
__declspec(dllexport) NTSTATUS WINAPI LsaClearAuditLog(LSA_HANDLE policy)
{
    if (policy != POLICY_HANDLE) return STATUS_INVALID_HANDLE_;
    return privilege_enabled(8 /* SeSecurityPrivilege */) ? STATUS_SUCCESS : STATUS_PRIVILEGE_NOT_HELD_;
}
NTSTATUS WINAPI LsaOpenPolicy(PLSA_UNICODE_STRING, PLSA_OBJECT_ATTRIBUTES, ACCESS_MASK, PLSA_HANDLE);
__declspec(dllexport) NTSTATUS WINAPI LsaOpenPolicySce(PLSA_UNICODE_STRING system, PLSA_OBJECT_ATTRIBUTES attributes, ACCESS_MASK access, PLSA_HANDLE out) { return LsaOpenPolicy(system, attributes, access, out); }
NTSTATUS WINAPI LsaGetUserName(PUNICODE_STRING *, PUNICODE_STRING *);
__declspec(dllexport) NTSTATUS WINAPI LsaGetRemoteUserName(PLSA_UNICODE_STRING system, PLSA_UNICODE_STRING *user, PLSA_UNICODE_STRING *domain)
{
    if (system && system->Buffer && system->Length) return STATUS_NOT_SUPPORTED_;
    return LsaGetUserName((PUNICODE_STRING *)user, (PUNICODE_STRING *)domain);
}
NTSTATUS NTAPI LsaLookupSids(LSA_HANDLE, ULONG, PSID *, PLSA_REFERENCED_DOMAIN_LIST *, PLSA_TRANSLATED_NAME *);
__declspec(dllexport) NTSTATUS WINAPI LsaLookupSids2(LSA_HANDLE policy, ULONG flags, ULONG count, PSID *sids, PLSA_REFERENCED_DOMAIN_LIST *domains, PLSA_TRANSLATED_NAME *names)
{
    (void)flags;
    return LsaLookupSids(policy, count, sids, domains, names);
}
__declspec(dllexport) BOOLEAN WINAPI LsaValidateProcUniqueLuid(PLUID luid) { return luid != NULL; }
__declspec(dllexport) NTSTATUS WINAPI LsaProfileDeleted(PVOID a) { (void)a; return STATUS_SUCCESS; }
__declspec(dllexport) NTSTATUS WINAPI LsaConfigureAutoLogonCredentials(PVOID a, PVOID b) { (void)a; (void)b; return privilege_enabled(8) ? STATUS_SUCCESS : STATUS_ACCESS_DENIED; }

/* ---- audit policy -------------------------------------------------------------------------------------------------------------------------------------------- */

#define G_CAT(n) {0x69979848u + (n), 0x797A, 0x11D9, {0xBE, 0xD3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}}
#define G_SUB(n) {0x0CCE9210u + (n), 0x69AE, 0x11D9, {0xBE, 0xD3, 0x50, 0x50, 0x54, 0x50, 0x30, 0x30}}
typedef struct { GUID guid; const WCHAR *name; } CategoryEntry;
static const CategoryEntry g_categories[9] = {
    {G_CAT(0), L"System"}, {G_CAT(1), L"Logon/Logoff"}, {G_CAT(2), L"Object Access"}, {G_CAT(3), L"Privilege Use"}, {G_CAT(4), L"Detailed Tracking"},
    {G_CAT(5), L"Policy Change"}, {G_CAT(6), L"Account Management"}, {G_CAT(7), L"DS Access"}, {G_CAT(8), L"Account Logon"}};
typedef struct { DWORD id; DWORD category; const WCHAR *name; } SubEntry;      /* the GUID is 0CCE9210 + id */
static const SubEntry g_subs[] = {
    {0x00, 0, L"Security State Change"}, {0x01, 0, L"Security System Extension"}, {0x02, 0, L"System Integrity"}, {0x03, 0, L"IPsec Driver"}, {0x04, 0, L"Other System Events"},
    {0x05, 1, L"Logon"}, {0x06, 1, L"Logoff"}, {0x07, 1, L"Account Lockout"}, {0x08, 1, L"IPsec Main Mode"}, {0x09, 1, L"IPsec Quick Mode"}, {0x0A, 1, L"IPsec Extended Mode"},
    {0x0B, 1, L"Special Logon"}, {0x0C, 1, L"Other Logon/Logoff Events"}, {0x33, 1, L"Network Policy Server"},
    {0x0D, 2, L"File System"}, {0x0E, 2, L"Registry"}, {0x0F, 2, L"Kernel Object"}, {0x10, 2, L"SAM"}, {0x11, 2, L"Certification Services"}, {0x12, 2, L"Application Generated"},
    {0x13, 2, L"Handle Manipulation"}, {0x14, 2, L"File Share"}, {0x15, 2, L"Filtering Platform Packet Drop"}, {0x16, 2, L"Filtering Platform Connection"},
    {0x17, 2, L"Other Object Access Events"}, {0x34, 2, L"Detailed File Share"}, {0x35, 2, L"Removable Storage"}, {0x36, 2, L"Central Policy Staging"},
    {0x18, 3, L"Sensitive Privilege Use"}, {0x19, 3, L"Non Sensitive Privilege Use"}, {0x1A, 3, L"Other Privilege Use Events"},
    {0x1B, 4, L"Process Creation"}, {0x1C, 4, L"Process Termination"}, {0x1D, 4, L"DPAPI Activity"}, {0x1E, 4, L"RPC Events"},
    {0x1F, 5, L"Audit Policy Change"}, {0x20, 5, L"Authentication Policy Change"}, {0x21, 5, L"Authorization Policy Change"}, {0x22, 5, L"MPSSVC Rule-Level Policy Change"},
    {0x23, 5, L"Filtering Platform Policy Change"}, {0x24, 5, L"Other Policy Change Events"},
    {0x25, 6, L"User Account Management"}, {0x26, 6, L"Computer Account Management"}, {0x27, 6, L"Security Group Management"}, {0x28, 6, L"Distribution Group Management"},
    {0x29, 6, L"Application Group Management"}, {0x2A, 6, L"Other Account Management Events"},
    {0x2B, 7, L"Directory Service Access"}, {0x2C, 7, L"Directory Service Changes"}, {0x2D, 7, L"Directory Service Replication"}, {0x2E, 7, L"Detailed Directory Service Replication"},
    {0x2F, 8, L"Credential Validation"}, {0x30, 8, L"Kerberos Service Ticket Operations"}, {0x31, 8, L"Other Account Logon Events"}, {0x32, 8, L"Kerberos Authentication Service"}};
#define SUB_COUNT (sizeof(g_subs) / sizeof(g_subs[0]))

static BOOL guid_equal(const GUID *a, const GUID *b) { return !memcmp(a, b, sizeof(GUID)); }
static int category_index(const GUID *g) { int i; for (i = 0; i < 9; ++i) if (guid_equal(&g_categories[i].guid, g)) return i; return -1; }
static int sub_index(const GUID *g)
{
    DWORD i;
    for (i = 0; i < SUB_COUNT; ++i) { GUID s = G_SUB(g_subs[i].id); if (guid_equal(&s, g)) return (int)i; }
    return -1;
}
static void sub_guid(DWORD index, GUID *out) { GUID s = G_SUB(g_subs[index].id); *out = s; }
static void audit_error(DWORD e) { SetLastError(e); }
static void guid_text(const GUID *g, WCHAR *out)
{
    static const WCHAR hex[] = L"0123456789ABCDEF";
    const BYTE *b = (const BYTE *)g;
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    DWORD i, at = 0;
    out[at++] = '{';
    for (i = 0; i < 16; ++i) { if (i == 4 || i == 6 || i == 8 || i == 10) out[at++] = '-'; out[at++] = hex[b[order[i]] >> 4]; out[at++] = hex[b[order[i]] & 15]; }
    out[at++] = '}';
    out[at] = 0;
}

__declspec(dllexport) VOID WINAPI AuditFree(PVOID buffer) { free(buffer); }
__declspec(dllexport) BOOLEAN WINAPI AuditEnumerateCategories(GUID **out, PULONG count)
{
    GUID *g;
    DWORD i;
    if (!out || !count) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(g = malloc(9 * sizeof(GUID)))) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0; i < 9; ++i) g[i] = g_categories[i].guid;
    *out = g; *count = 9;
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditEnumerateSubCategories(const GUID *category, BOOLEAN all, GUID **out, PULONG count)
{
    GUID *g;
    DWORD i, n = 0;
    int ci = -1;
    if (!out || !count || (!all && !category)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!all && (ci = category_index(category)) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(g = malloc(SUB_COUNT * sizeof(GUID)))) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0; i < SUB_COUNT; ++i) if (all || (int)g_subs[i].category == ci) sub_guid(i, &g[n++]);
    *out = g; *count = n;
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditLookupCategoryGuidFromCategoryId(POLICY_AUDIT_EVENT_TYPE id, GUID *out)
{
    if (!out || (int)id < 0 || (int)id > 8) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *out = g_categories[id].guid;
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditLookupCategoryIdFromCategoryGuid(const GUID *guid, PPOLICY_AUDIT_EVENT_TYPE out)
{
    int i;
    if (!guid || !out || (i = category_index(guid)) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    *out = (POLICY_AUDIT_EVENT_TYPE)i;
    return TRUE;
}
static BOOLEAN name_out(const WCHAR *name, LPWSTR *out)
{
    LPWSTR r;
    if (!(r = malloc((wcslen(name) + 1) * 2))) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    wcscpy(r, name);
    *out = r;
    return TRUE;
}
static BOOLEAN name_out_a(const WCHAR *name, LPSTR *out)
{
    int n = WideCharToMultiByte(CP_ACP, 0, name, -1, NULL, 0, NULL, NULL);
    char *r = malloc(n + 1);
    if (!r) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    WideCharToMultiByte(CP_ACP, 0, name, -1, r, n, NULL, NULL);
    *out = r;
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditLookupCategoryNameW(const GUID *guid, LPWSTR *out)
{
    int i;
    if (!guid || !out || (i = category_index(guid)) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return name_out(g_categories[i].name, out);
}
__declspec(dllexport) BOOLEAN WINAPI AuditLookupCategoryNameA(const GUID *guid, LPSTR *out)
{
    int i;
    if (!guid || !out || (i = category_index(guid)) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return name_out_a(g_categories[i].name, out);
}
__declspec(dllexport) BOOLEAN WINAPI AuditLookupSubCategoryNameW(const GUID *guid, LPWSTR *out)
{
    int i;
    if (!guid || !out || (i = sub_index(guid)) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return name_out(g_subs[i].name, out);
}
__declspec(dllexport) BOOLEAN WINAPI AuditLookupSubCategoryNameA(const GUID *guid, LPSTR *out)
{
    int i;
    if (!guid || !out || (i = sub_index(guid)) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return name_out_a(g_subs[i].name, out);
}

/* the setting of one subcategory in a key (0 when never set) */
static DWORD stored_setting(LPCWSTR sub, const GUID *g)
{
    HKEY key;
    WCHAR name[48];
    DWORD v = 0, size = 4, type;
    guid_text(g, name);
    if (open_policy(sub, FALSE, KEY_READ, &key)) { if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&v, &size) != ERROR_SUCCESS || type != REG_DWORD) v = 0; RegCloseKey(key); }
    return v;
}
static BOOLEAN query_policy(LPCWSTR sub, const GUID *guids, ULONG count, AUDIT_POLICY_INFORMATION **out)
{
    AUDIT_POLICY_INFORMATION *p;
    ULONG i;
    if (!guids || !count || !out) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (i = 0; i < count; ++i) if (sub_index(&guids[i]) < 0) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(p = calloc(count, sizeof(*p)))) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0; i < count; ++i) {
        p[i].AuditSubCategoryGuid = guids[i];
        p[i].AuditingInformation = stored_setting(sub, &guids[i]);
        p[i].AuditCategoryGuid = g_categories[g_subs[sub_index(&guids[i])].category].guid;
    }
    *out = p;
    return TRUE;
}
static BOOLEAN set_policy(LPCWSTR sub, const AUDIT_POLICY_INFORMATION *policy, ULONG count, DWORD valid_mask)
{
    HKEY key;
    ULONG i;
    if (!policy || !count) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    for (i = 0; i < count; ++i) if (sub_index(&policy[i].AuditSubCategoryGuid) < 0 || (policy[i].AuditingInformation & ~valid_mask)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_policy(sub, TRUE, KEY_ALL_ACCESS, &key)) { audit_error(ERROR_ACCESS_DENIED); return FALSE; }
    for (i = 0; i < count; ++i) {
        WCHAR name[48];
        guid_text(&policy[i].AuditSubCategoryGuid, name);
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&policy[i].AuditingInformation, 4);
    }
    RegCloseKey(key);
    return TRUE;
}
/* POLICY_AUDIT_EVENT_UNCHANGED 0, SUCCESS 1, FAILURE 2, NONE 4 */
__declspec(dllexport) BOOLEAN WINAPI AuditQuerySystemPolicy(const GUID *guids, ULONG count, AUDIT_POLICY_INFORMATION **out) { return query_policy(L"Audit", guids, count, out); }
__declspec(dllexport) BOOLEAN WINAPI AuditSetSystemPolicy(PCAUDIT_POLICY_INFORMATION policy, ULONG count)
{
    if (!privilege_enabled(8 /* SeSecurityPrivilege */)) { audit_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
    return set_policy(L"Audit", policy, count, 7);
}
static BOOL user_sub(PSID sid, WCHAR *out)
{
    WCHAR *t = sid_text(sid);
    if (!t) return FALSE;
    wcscpy(out, L"AuditUsers\\");
    wcsncat_(out, t, 250);
    free(t);
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditQueryPerUserPolicy(const PSID sid, const GUID *guids, ULONG count, AUDIT_POLICY_INFORMATION **out)
{
    WCHAR sub[300];
    if (!sid || !IsValidSid(sid) || !user_sub(sid, sub)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return query_policy(sub, guids, count, out);
}
__declspec(dllexport) BOOLEAN WINAPI AuditSetPerUserPolicy(const PSID sid, PCAUDIT_POLICY_INFORMATION policy, ULONG count)
{
    WCHAR sub[300];
    if (!sid || !IsValidSid(sid) || !user_sub(sid, sub)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!privilege_enabled(8)) { audit_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
    return set_policy(sub, policy, count, VALID_PER_USER_AUDIT_POLICY_FLAG);
}
__declspec(dllexport) BOOLEAN WINAPI AuditEnumeratePerUserPolicy(PPOLICY_AUDIT_SID_ARRAY *out)
{
    HKEY users;
    DWORD i, n = 0, total, at;
    PSID *sids;
    BYTE *block;
    PPOLICY_AUDIT_SID_ARRAY array;
    if (!out) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    sids = calloc(64, sizeof(PSID));
    if (!sids) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (open_policy(L"AuditUsers", FALSE, KEY_READ, &users)) {
        for (i = 0; n < 64; ++i) {
            WCHAR name[200];
            DWORD len = 200;
            PSID sid = NULL;
            if (RegEnumKeyExW(users, i, name, &len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            if (ConvertStringSidToSidW(name, &sid)) sids[n++] = sid;
        }
        RegCloseKey(users);
    }
    if (!n) { free(sids); *out = NULL; audit_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    total = sizeof(*array) + n * sizeof(PSID);
    for (i = 0; i < n; ++i) total += (GetLengthSid(sids[i]) + 7) & ~7u;
    block = calloc(1, total + 8);
    if (!block) { free_sids(sids, n); audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    array = (PPOLICY_AUDIT_SID_ARRAY)block;
    array->UsersCount = n;
    array->UserSidArray = (PSID *)(block + sizeof(*array));
    at = sizeof(*array) + n * sizeof(PSID);
    for (i = 0; i < n; ++i) { DWORD len = GetLengthSid(sids[i]); memcpy(block + at, sids[i], len); array->UserSidArray[i] = block + at; at += (len + 7) & ~7u; }
    free_sids(sids, n);
    *out = array;
    return TRUE;
}
/* The policy that applies to a user: the system policy with the user's own settings on top (INCLUDE adds, EXCLUDE takes away). */
__declspec(dllexport) BOOLEAN WINAPI AuditComputeEffectivePolicyBySid(const PSID sid, const GUID *guids, ULONG count, AUDIT_POLICY_INFORMATION **out)
{
    AUDIT_POLICY_INFORMATION *system = NULL, *user = NULL;
    ULONG i;
    WCHAR sub[300];
    if (!sid || !IsValidSid(sid) || !user_sub(sid, sub)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!query_policy(L"Audit", guids, count, &system)) return FALSE;
    if (!query_policy(sub, guids, count, &user)) { free(system); return FALSE; }
    for (i = 0; i < count; ++i) {
        DWORD s = system[i].AuditingInformation & 3, u = user[i].AuditingInformation;
        if (u & PER_USER_AUDIT_NONE) s = 0;
        if (u & PER_USER_AUDIT_SUCCESS_INCLUDE) s |= 1;
        if (u & PER_USER_AUDIT_SUCCESS_EXCLUDE) s &= ~1u;
        if (u & PER_USER_AUDIT_FAILURE_INCLUDE) s |= 2;
        if (u & PER_USER_AUDIT_FAILURE_EXCLUDE) s &= ~2u;
        system[i].AuditingInformation = s ? s : 4;
    }
    free(user);
    *out = system;
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditComputeEffectivePolicyByToken(HANDLE token, const GUID *guids, ULONG count, AUDIT_POLICY_INFORMATION **out)
{
    DWORD size = 0;
    TOKEN_USER *user;
    BOOLEAN r;
    if (!GetTokenInformation(token, TokenUser, NULL, 0, &size) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return FALSE;
    if (!(user = malloc(size))) { audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (!GetTokenInformation(token, TokenUser, user, size, &size)) { free(user); return FALSE; }
    r = AuditComputeEffectivePolicyBySid(user->User.Sid, guids, count, out);
    free(user);
    return r;
}

/* the security descriptor of the audit policy, and the global SACLs of the object types */
__declspec(dllexport) BOOLEAN WINAPI AuditQuerySecurity(SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR *out)
{
    PSECURITY_DESCRIPTOR sd = NULL, copy;
    ULONG size = 0;
    (void)info;
    if (!out) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:SYD:(A;;0x7F;;;BA)(A;;0x7F;;;SY)", SDDL_REVISION_1, &sd, &size)) return FALSE;
    if (!(copy = malloc(size))) { LocalFree(sd); audit_error(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    memcpy(copy, sd, size);
    LocalFree(sd);
    *out = copy;
    return TRUE;
}
__declspec(dllexport) BOOLEAN WINAPI AuditSetSecurity(SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd)
{
    (void)info;
    if (!sd || !IsValidSecurityDescriptor(sd)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    return TRUE;
}
static BOOLEAN global_sacl_name(PCWSTR type, WCHAR *out)
{
    if (!type || (wcsicmp(type, L"File") && wcsicmp(type, L"Key"))) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    wcscpy(out, L"GlobalSacl\\");
    wcscat(out, type);
    return TRUE;
}
__declspec(dllexport) BOOLEAN NTAPI AuditSetGlobalSaclW(PCWSTR type, PACL acl)
{
    WCHAR sub[64];
    HKEY key;
    if (!global_sacl_name(type, sub)) return FALSE;
    if (!privilege_enabled(8)) { audit_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
    if (acl && !IsValidAcl(acl)) { audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_policy(sub, TRUE, KEY_ALL_ACCESS, &key)) { audit_error(ERROR_ACCESS_DENIED); return FALSE; }
    if (acl) RegSetValueExW(key, L"Sacl", 0, REG_BINARY, (const BYTE *)acl, acl->AclSize); else RegDeleteValueW(key, L"Sacl");
    RegCloseKey(key);
    return TRUE;
}
__declspec(dllexport) BOOLEAN NTAPI AuditSetGlobalSaclA(PCSTR type, PACL acl)
{
    WCHAR *w = strdupAW(type);
    BOOLEAN r = AuditSetGlobalSaclW(w, acl);
    DWORD e = GetLastError();
    free(w);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOLEAN NTAPI AuditQueryGlobalSaclW(PCWSTR type, PACL *acl)
{
    WCHAR sub[64];
    HKEY key;
    DWORD size = 0;
    PACL a;
    if (!global_sacl_name(type, sub) || !acl) { if (!acl) audit_error(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!privilege_enabled(8)) { audit_error(ERROR_PRIVILEGE_NOT_HELD); return FALSE; }
    if (!open_policy(sub, FALSE, KEY_READ, &key)) { audit_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    if (RegQueryValueExW(key, L"Sacl", NULL, NULL, NULL, &size) != ERROR_SUCCESS || size < sizeof(ACL) || !(a = malloc(size))) { RegCloseKey(key); audit_error(ERROR_FILE_NOT_FOUND); return FALSE; }
    RegQueryValueExW(key, L"Sacl", NULL, NULL, (BYTE *)a, &size);
    RegCloseKey(key);
    *acl = a;
    return TRUE;
}
__declspec(dllexport) BOOLEAN NTAPI AuditQueryGlobalSaclA(PCSTR type, PACL *acl)
{
    WCHAR *w = strdupAW(type);
    BOOLEAN r = AuditQueryGlobalSaclW(w, acl);
    DWORD e = GetLastError();
    free(w);
    SetLastError(e);
    return r;
}
