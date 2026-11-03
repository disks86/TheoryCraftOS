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

/* advapi32.dll: the credential store (CredWrite, CredRead, CredEnumerate ...). A credential is one binary value of the key HKCU\Software\Microsoft\TcosCredentials, named by its type
 * and target (the target is compared without regard to case), holding the CREDENTIAL fields in a plain layout of this OS. The registry is in memory: credentials last until the
 * system is shut down, whatever their persistence says. Credentials are returned in one block that CredFree gives back. */
#include "security.h"
#include <wincred.h>

#ifndef CRED_ENUMERATE_ALL_CREDENTIALS
#define CRED_ENUMERATE_ALL_CREDENTIALS 0x1
#endif
#ifndef CRED_TYPE_DOMAIN_EXTENDED
#define CRED_TYPE_DOMAIN_EXTENDED 6
#endif
#define BinaryBlobCredential ((CRED_MARSHAL_TYPE)3)
typedef struct { ULONG cbBlob; PUCHAR pbBlob; } BINARY_BLOB_CREDENTIAL_INFO;
#define CRED_KEY L"Software\\Microsoft\\TcosCredentials"
#define MAX_TARGET 32767
#define MAX_BLOB (5 * 512)
#define MAX_ATTRIBUTES 64
#define MAX_RECORD (1u << 20)

BOOL TCOS_FillRandom(BYTE *out, DWORD length);

/* ---- records ---------------------------------------------------------------------------------------------------------------------------------------------------- */

/* The stored form: DWORD version, flags, type, persist, filetime (2), blob size, attribute count, then four strings (each a DWORD of characters and the characters: target, comment,
 * alias, user name), the blob, and the attributes (keyword string, flags, value size, value). */
typedef struct { BYTE *data; DWORD size, capacity; BOOL failed; } Buf;
static void put_bytes(Buf *b, const void *p, DWORD n)
{
    if (b->failed) return;
    if (b->size + n > b->capacity) {
        DWORD c = b->capacity ? b->capacity * 2 : 256;
        BYTE *m;
        while (c < b->size + n) c *= 2;
        if (!(m = realloc(b->data, c))) { b->failed = TRUE; return; }
        b->data = m; b->capacity = c;
    }
    if (n) memcpy(b->data + b->size, p, n);
    b->size += n;
}
static void put_dword(Buf *b, DWORD v) { put_bytes(b, &v, 4); }
static void put_string(Buf *b, LPCWSTR s)
{
    DWORD n = s ? (DWORD)wcslen(s) : 0;
    put_dword(b, s ? n + 1 : 0);      /* 0: none; else the characters with the NUL */
    if (s) put_bytes(b, s, (n + 1) * 2);
}
typedef struct { const BYTE *p; DWORD left; BOOL failed; } Rd;
static DWORD rd_dword(Rd *r) { DWORD v = 0; if (r->failed || r->left < 4) { r->failed = TRUE; return 0; } memcpy(&v, r->p, 4); r->p += 4; r->left -= 4; return v; }
static const BYTE *rd_bytes(Rd *r, DWORD n) { const BYTE *p = r->p; if (r->failed || r->left < n) { r->failed = TRUE; return NULL; } r->p += n; r->left -= n; return p; }
static const WCHAR *rd_string(Rd *r, DWORD *chars)
{
    DWORD n = rd_dword(r);
    const WCHAR *s;
    *chars = n;
    if (!n) return NULL;
    if (n > r->left / 2 + 1) { r->failed = TRUE; return NULL; }
    s = (const WCHAR *)rd_bytes(r, n * 2);
    if (s && s[n - 1]) r->failed = TRUE;
    return s;
}

static void serialize(const CREDENTIALW *c, Buf *b)
{
    DWORD i;
    put_dword(b, 1);
    put_dword(b, c->Flags); put_dword(b, c->Type); put_dword(b, c->Persist);
    put_dword(b, c->LastWritten.dwLowDateTime); put_dword(b, c->LastWritten.dwHighDateTime);
    put_dword(b, c->CredentialBlobSize); put_dword(b, c->AttributeCount);
    put_string(b, c->TargetName); put_string(b, c->Comment); put_string(b, c->TargetAlias); put_string(b, c->UserName);
    put_bytes(b, c->CredentialBlob, c->CredentialBlobSize);
    for (i = 0; i < c->AttributeCount; ++i) {
        put_string(b, c->Attributes[i].Keyword);
        put_dword(b, c->Attributes[i].Flags); put_dword(b, c->Attributes[i].ValueSize);
        put_bytes(b, c->Attributes[i].Value, c->Attributes[i].ValueSize);
    }
}
/* Makes the credential out of a stored record: one block (CREDENTIALW, then the strings, the blob, the attributes). */
static CREDENTIALW *deserialize(const BYTE *data, DWORD size, DWORD *block_size)
{
    Rd r = {data, size, FALSE};
    DWORD version = rd_dword(&r), flags = rd_dword(&r), type = rd_dword(&r), persist = rd_dword(&r), lo = rd_dword(&r), hi = rd_dword(&r), blob_size = rd_dword(&r), attrs = rd_dword(&r), i;
    DWORD chars[4];
    const WCHAR *strings[4];
    const BYTE *blob;
    CREDENTIALW *c;
    BYTE *at;
    DWORD total, strings_bytes = 0, attr_bytes = 0;
    struct { const WCHAR *keyword; DWORD kchars, flags, size; const BYTE *value; } *av = NULL;
    if (r.failed || version != 1 || attrs > MAX_ATTRIBUTES || blob_size > MAX_BLOB) return NULL;
    for (i = 0; i < 4; ++i) { strings[i] = rd_string(&r, &chars[i]); strings_bytes += chars[i] * 2; }
    blob = rd_bytes(&r, blob_size);
    if (attrs && !(av = calloc(attrs, sizeof(*av)))) return NULL;
    for (i = 0; i < attrs; ++i) {
        av[i].keyword = rd_string(&r, &av[i].kchars);
        av[i].flags = rd_dword(&r);
        av[i].size = rd_dword(&r);
        av[i].value = av[i].size ? rd_bytes(&r, av[i].size) : NULL;
        strings_bytes += av[i].kchars * 2;
        attr_bytes += (av[i].size + 7) & ~7u;
    }
    if (r.failed) { free(av); return NULL; }
    total = sizeof(CREDENTIALW) + strings_bytes + 8 + ((blob_size + 7) & ~7u) + attrs * sizeof(CREDENTIAL_ATTRIBUTEW) + attr_bytes;
    if (!(c = calloc(1, total))) { free(av); return NULL; }
    c->Flags = flags; c->Type = type; c->Persist = persist;
    c->LastWritten.dwLowDateTime = lo; c->LastWritten.dwHighDateTime = hi;
    c->CredentialBlobSize = blob_size; c->AttributeCount = attrs;
    at = (BYTE *)(c + 1);
    {
        LPWSTR *targets[4] = {&c->TargetName, &c->Comment, &c->TargetAlias, &c->UserName};
        for (i = 0; i < 4; ++i) if (strings[i]) { *targets[i] = (LPWSTR)at; memcpy(at, strings[i], chars[i] * 2); at += chars[i] * 2; }
    }
    at = (BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
    if (blob_size) { c->CredentialBlob = at; memcpy(at, blob, blob_size); at += (blob_size + 7) & ~7u; }
    if (attrs) {
        c->Attributes = (PCREDENTIAL_ATTRIBUTEW)at;
        at += attrs * sizeof(CREDENTIAL_ATTRIBUTEW);
        for (i = 0; i < attrs; ++i) {
            if (av[i].keyword) { c->Attributes[i].Keyword = (LPWSTR)at; memcpy(at, av[i].keyword, av[i].kchars * 2); at += av[i].kchars * 2; }
        }
        at = (BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
        for (i = 0; i < attrs; ++i) {
            c->Attributes[i].Flags = av[i].flags; c->Attributes[i].ValueSize = av[i].size;
            if (av[i].size) { c->Attributes[i].Value = at; memcpy(at, av[i].value, av[i].size); at += (av[i].size + 7) & ~7u; }
        }
    }
    free(av);
    *block_size = total;
    return c;
}

/* ---- the store ------------------------------------------------------------------------------------------------------------------------------------------------------ */

static void value_name(DWORD type, LPCWSTR target, WCHAR *out)
{
    static const WCHAR hex[] = L"0123456789abcdef";
    DWORD at = 0;
    out[at++] = hex[type & 15];
    out[at++] = '_';
    for (; *target; ++target) {      /* lower-cased, as four hex digits each (names may hold any character) */
        WCHAR c = *target;
        if (c >= 'A' && c <= 'Z') c = (WCHAR)(c + 32);
        out[at++] = hex[(c >> 12) & 15]; out[at++] = hex[(c >> 8) & 15]; out[at++] = hex[(c >> 4) & 15]; out[at++] = hex[c & 15];
    }
    out[at] = 0;
}
static WCHAR *value_name_alloc(DWORD type, LPCWSTR target)
{
    WCHAR *n = malloc((wcslen(target) * 4 + 4) * 2);
    if (n) value_name(type, target, n);
    return n;
}
static BOOL open_store(BOOL create, HKEY *key)
{
    LONG r = create ? RegCreateKeyExW(HKEY_CURRENT_USER, CRED_KEY, 0, NULL, 0, KEY_ALL_ACCESS, NULL, key, NULL) : RegOpenKeyExW(HKEY_CURRENT_USER, CRED_KEY, 0, KEY_ALL_ACCESS, key);
    if (r == ERROR_SUCCESS) return TRUE;
    SetLastError(create ? r : ERROR_NOT_FOUND);
    return FALSE;
}
static BOOL wildcard_match(LPCWSTR pattern, LPCWSTR text)      /* '*' at the end only, like the filter of CredEnumerate; case does not count */
{
    size_t n = wcslen(pattern);
    if (n && pattern[n - 1] == '*') return !wcsnicmp(pattern, text, n - 1);
    return !wcsicmp(pattern, text);
}
static BOOL valid_type(DWORD type) { return type >= CRED_TYPE_GENERIC && type < CRED_TYPE_MAXIMUM; }

__declspec(dllexport) BOOL WINAPI CredWriteW(PCREDENTIALW c, DWORD flags)
{
    Buf b = {0};
    HKEY store;
    WCHAR *name;
    DWORD i, previous_size = 0, type;
    LONG r;
    CREDENTIALW copy;
    BYTE *old_blob = NULL;
    if (!c || (flags & ~CRED_PRESERVE_CREDENTIAL_BLOB)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!valid_type(c->Type) || c->Type == CRED_TYPE_DOMAIN_EXTENDED) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->Flags & ~(CRED_FLAGS_PROMPT_NOW | CRED_FLAGS_USERNAME_TARGET)) { SetLastError(ERROR_INVALID_FLAGS); return FALSE; }
    if (!c->TargetName || !*c->TargetName || wcslen(c->TargetName) > MAX_TARGET) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->Persist < CRED_PERSIST_SESSION || c->Persist > CRED_PERSIST_ENTERPRISE) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->CredentialBlobSize > MAX_BLOB || (c->CredentialBlobSize && !c->CredentialBlob)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->AttributeCount > MAX_ATTRIBUTES || (c->AttributeCount && !c->Attributes)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->Comment && wcslen(c->Comment) > 256) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->UserName && wcslen(c->UserName) > 513) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if ((c->Type == CRED_TYPE_DOMAIN_PASSWORD || c->Type == CRED_TYPE_DOMAIN_CERTIFICATE || c->Type == CRED_TYPE_DOMAIN_VISIBLE_PASSWORD) && (!c->UserName || !*c->UserName)) {
        SetLastError(ERROR_BAD_USERNAME);
        return FALSE;
    }
    if (c->Type == CRED_TYPE_DOMAIN_CERTIFICATE && c->CredentialBlobSize != 0 && c->CredentialBlobSize != 4) {
        /* (a certificate credential holds the 20-byte hash in a marshaled user name; the blob is its pin) */
    }
    for (i = 0; i < c->AttributeCount; ++i)
        if (!c->Attributes[i].Keyword || c->Attributes[i].ValueSize > 256 || (c->Attributes[i].ValueSize && !c->Attributes[i].Value)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(name = value_name_alloc(c->Type, c->TargetName))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (!open_store(TRUE, &store)) { free(name); return FALSE; }
    copy = *c;
    GetSystemTimeAsFileTime(&copy.LastWritten);
    type = 0;
    if ((flags & CRED_PRESERVE_CREDENTIAL_BLOB) && RegQueryValueExW(store, name, NULL, &type, NULL, &previous_size) == ERROR_SUCCESS && previous_size && previous_size <= MAX_RECORD) {
        BYTE *old = malloc(previous_size);
        if (old && RegQueryValueExW(store, name, NULL, &type, old, &previous_size) == ERROR_SUCCESS) {
            DWORD block;
            CREDENTIALW *prev = deserialize(old, previous_size, &block);
            if (prev && prev->CredentialBlobSize) { old_blob = malloc(prev->CredentialBlobSize); if (old_blob) { memcpy(old_blob, prev->CredentialBlob, prev->CredentialBlobSize); copy.CredentialBlob = old_blob; copy.CredentialBlobSize = prev->CredentialBlobSize; } }
            free(prev);
        }
        free(old);
    }
    serialize(&copy, &b);
    free(old_blob);
    if (b.failed) { free(b.data); RegCloseKey(store); free(name); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    r = RegSetValueExW(store, name, 0, REG_BINARY, b.data, b.size);
    free(b.data);
    RegCloseKey(store);
    free(name);
    if (r != ERROR_SUCCESS) { SetLastError(r); return FALSE; }
    return TRUE;
}

static CREDENTIALW *read_one(HKEY store, DWORD type, LPCWSTR target)
{
    WCHAR *name = value_name_alloc(type, target);
    DWORD size = 0, vt = 0, block;
    BYTE *data = NULL;
    CREDENTIALW *c = NULL;
    if (!name) return NULL;
    if (RegQueryValueExW(store, name, NULL, &vt, NULL, &size) == ERROR_SUCCESS && size <= MAX_RECORD && (data = malloc(size + 8)) && RegQueryValueExW(store, name, NULL, &vt, data, &size) == ERROR_SUCCESS)
        c = deserialize(data, size, &block);
    free(data);
    free(name);
    return c;
}
__declspec(dllexport) BOOL WINAPI CredReadW(LPCWSTR target, DWORD type, DWORD flags, PCREDENTIALW *out)
{
    HKEY store;
    CREDENTIALW *c;
    if (!target || !out || flags) { SetLastError(flags ? ERROR_INVALID_FLAGS : ERROR_INVALID_PARAMETER); return FALSE; }
    if (!valid_type(type)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_store(FALSE, &store)) return FALSE;
    c = read_one(store, type, target);
    RegCloseKey(store);
    if (!c) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    *out = c;
    return TRUE;
}
__declspec(dllexport) VOID WINAPI CredFree(PVOID buffer) { free(buffer); }

__declspec(dllexport) BOOL WINAPI CredDeleteW(LPCWSTR target, DWORD type, DWORD flags)
{
    HKEY store;
    WCHAR *name;
    LONG r;
    if (!target || flags) { SetLastError(flags ? ERROR_INVALID_FLAGS : ERROR_INVALID_PARAMETER); return FALSE; }
    if (!valid_type(type)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_store(FALSE, &store)) return FALSE;
    if (!(name = value_name_alloc(type, target))) { RegCloseKey(store); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    r = RegDeleteValueW(store, name);
    RegCloseKey(store);
    free(name);
    if (r != ERROR_SUCCESS) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredRenameW(LPCWSTR old_target, LPCWSTR new_target, DWORD type, DWORD flags)
{
    HKEY store;
    CREDENTIALW *c;
    WCHAR *old_name, *new_name;
    Buf b = {0};
    BOOL ok = FALSE;
    if (!old_target || !new_target || flags || !*new_target) { SetLastError(flags ? ERROR_INVALID_FLAGS : ERROR_INVALID_PARAMETER); return FALSE; }
    if (type != CRED_TYPE_GENERIC && type != CRED_TYPE_DOMAIN_PASSWORD) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_store(FALSE, &store)) return FALSE;
    c = read_one(store, type, old_target);
    if (!c) { RegCloseKey(store); SetLastError(ERROR_NOT_FOUND); return FALSE; }
    old_name = value_name_alloc(type, old_target);
    new_name = value_name_alloc(type, new_target);
    if (old_name && new_name) {
        WCHAR *probe = new_name;
        DWORD size = 0;
        if (RegQueryValueExW(store, probe, NULL, NULL, NULL, &size) == ERROR_SUCCESS) SetLastError(ERROR_ALREADY_EXISTS);
        else {
            c->TargetName = (LPWSTR)new_target;
            serialize(c, &b);
            if (!b.failed && RegSetValueExW(store, new_name, 0, REG_BINARY, b.data, b.size) == ERROR_SUCCESS) { RegDeleteValueW(store, old_name); ok = TRUE; }
            free(b.data);
        }
    } else SetLastError(ERROR_NOT_ENOUGH_MEMORY);
    free(old_name); free(new_name); free(c);
    RegCloseKey(store);
    return ok;
}

/* Several credentials as one block: the array of pointers, then each credential. */
static BOOL pack(CREDENTIALW **list, DWORD count, DWORD *out_count, PCREDENTIALW **out)
{
    DWORD i, total = count * sizeof(void *), at;
    BYTE *block;
    PCREDENTIALW *array;
    for (i = 0; i < count; ++i) {
        DWORD one = 0;
        Buf b = {0};
        serialize(list[i], &b);
        if (!b.failed) { CREDENTIALW *c = deserialize(b.data, b.size, &one); free(c); }
        free(b.data);
        total += (one + 7) & ~7u;
    }
    if (!(block = calloc(1, total + 8))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    array = (PCREDENTIALW *)block;
    at = count * sizeof(void *);
    for (i = 0; i < count; ++i) {
        DWORD one = 0;
        Buf b = {0};
        CREDENTIALW *c = NULL;
        serialize(list[i], &b);
        if (!b.failed) c = deserialize(b.data, b.size, &one);
        free(b.data);
        if (!c) continue;
        /* the block was made for a standalone credential: pointers are rebased into this one */
        {
            BYTE *dest = block + at;
            LONG_PTR delta = (LONG_PTR)dest - (LONG_PTR)c;
            DWORD j;
            memcpy(dest, c, one);
            #define REBASE(field) do { if (((CREDENTIALW *)dest)->field) ((CREDENTIALW *)dest)->field = (void *)((LONG_PTR)((CREDENTIALW *)dest)->field + delta); } while (0)
            REBASE(TargetName); REBASE(Comment); REBASE(TargetAlias); REBASE(UserName); REBASE(CredentialBlob); REBASE(Attributes);
            for (j = 0; j < c->AttributeCount; ++j) {
                CREDENTIAL_ATTRIBUTEW *a = &((CREDENTIALW *)dest)->Attributes[j];
                if (a->Keyword) a->Keyword = (void *)((LONG_PTR)a->Keyword + delta);
                if (a->Value) a->Value = (void *)((LONG_PTR)a->Value + delta);
            }
            array[i] = (PCREDENTIALW)dest;
        }
        free(c);
        at += (one + 7) & ~7u;
    }
    *out_count = count;
    *out = array;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredEnumerateW(LPCWSTR filter, DWORD flags, DWORD *count, PCREDENTIALW **out)
{
    HKEY store;
    DWORD i, n = 0, capacity = 16;
    CREDENTIALW **list;
    BOOL ok;
    if (!count || !out || (flags & ~CRED_ENUMERATE_ALL_CREDENTIALS)) { SetLastError(ERROR_INVALID_FLAGS); return FALSE; }
    if ((flags & CRED_ENUMERATE_ALL_CREDENTIALS) && filter) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!open_store(FALSE, &store)) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    if (!(list = calloc(capacity, sizeof(*list)))) { RegCloseKey(store); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0;; ++i) {
        WCHAR name[40 + 1];
        DWORD name_size = 40, size = 0, type;
        BYTE *data;
        CREDENTIALW *c;
        DWORD block;
        WCHAR *long_name = NULL;
        DWORD info_names, info_values, max_name, max_data;
        (void)name; (void)name_size; (void)type;
        if (RegQueryInfoKeyW(store, NULL, NULL, NULL, NULL, NULL, NULL, &info_values, &max_name, &max_data, NULL, NULL) != ERROR_SUCCESS || i >= info_values) break;
        (void)info_names;
        long_name = malloc((max_name + 2) * 2);
        data = malloc(max_data + 8);
        if (!long_name || !data) { free(long_name); free(data); break; }
        name_size = max_name + 1; size = max_data;
        if (RegEnumValueW(store, i, long_name, &name_size, NULL, NULL, data, &size) == ERROR_SUCCESS && (c = deserialize(data, size, &block))) {
            if ((!filter || wildcard_match(filter, c->TargetName)) && (n < MAX_RECORD)) {
                if (n == capacity) { CREDENTIALW **bigger = realloc(list, capacity * 2 * sizeof(*list)); if (bigger) { list = bigger; capacity *= 2; } }
                if (n < capacity) list[n++] = c; else free(c);
            } else free(c);
        }
        free(long_name); free(data);
    }
    RegCloseKey(store);
    if (n == 0) { free(list); SetLastError(ERROR_NOT_FOUND); return FALSE; }
    ok = pack(list, n, count, out);
    for (i = 0; i < n; ++i) free(list[i]);
    free(list);
    return ok;
}
/* the credential for a target: its own, or the longest wildcard ("*.example.com") that fits it */
__declspec(dllexport) BOOL WINAPI CredFindBestCredentialW(LPCWSTR target, DWORD type, DWORD flags, PCREDENTIALW *out)
{
    CREDENTIALW **list = NULL, *best = NULL;
    PCREDENTIALW *all = NULL;
    DWORD count = 0, i;
    if (!target || !out || flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!valid_type(type)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (CredReadW(target, type, 0, out)) return TRUE;
    if (!CredEnumerateW(NULL, CRED_ENUMERATE_ALL_CREDENTIALS, &count, &all)) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    (void)list;
    for (i = 0; i < count; ++i) {
        size_t n = wcslen(all[i]->TargetName);
        if (all[i]->Type != type || n < 2 || all[i]->TargetName[0] != '*') continue;      /* a leading wildcard: *.example.com */
        {
            size_t tlen = wcslen(target);
            if (tlen >= n - 1 && !wcsicmp(target + tlen - (n - 1), all[i]->TargetName + 1) && (!best || n > wcslen(best->TargetName))) best = all[i];
        }
    }
    if (!best) { CredFree(all); SetLastError(ERROR_NOT_FOUND); return FALSE; }
    {
        BOOL ok = CredReadW(best->TargetName, type, 0, out);
        CredFree(all);
        return ok;
    }
}

/* ---- target information ---------------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) BOOL WINAPI CredGetTargetInfoW(LPCWSTR server, DWORD flags, PCREDENTIAL_TARGET_INFORMATIONW *out)
{
    CREDENTIAL_TARGET_INFORMATIONW *t;
    DWORD n;
    BYTE *at;
    if (!server || !out || (flags & ~(CRED_ALLOW_NAME_RESOLUTION))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    n = (DWORD)(wcslen(server) + 1) * 2;
    if (!(t = calloc(1, sizeof(*t) + n * 2 + 8))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    at = (BYTE *)(t + 1);
    memcpy(at, server, n);
    t->TargetName = (LPWSTR)at;
    t->NetbiosServerName = (LPWSTR)at;
    t->DnsServerName = (LPWSTR)at;
    t->Flags = 0;
    *out = t;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredReadDomainCredentialsW(PCREDENTIAL_TARGET_INFORMATIONW info, DWORD flags, DWORD *count, PCREDENTIALW **out)
{
    PCREDENTIALW *all = NULL, *picked;
    DWORD total = 0, i, n = 0;
    BOOL ok;
    if (!info || !info->TargetName || !count || !out || (flags & ~CRED_CACHE_TARGET_INFORMATION)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!CredEnumerateW(NULL, CRED_ENUMERATE_ALL_CREDENTIALS, &total, &all)) { SetLastError(ERROR_NOT_FOUND); return FALSE; }
    if (!(picked = calloc(total, sizeof(*picked)))) { CredFree(all); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0; i < total; ++i) {
        DWORD t = all[i]->Type;
        BOOL domain = t == CRED_TYPE_DOMAIN_PASSWORD || t == CRED_TYPE_DOMAIN_CERTIFICATE || t == CRED_TYPE_DOMAIN_VISIBLE_PASSWORD;
        if (!domain) continue;
        if (!wcsicmp(all[i]->TargetName, info->TargetName) || (info->DnsServerName && !wcsicmp(all[i]->TargetName, info->DnsServerName)) ||
            (info->NetbiosServerName && !wcsicmp(all[i]->TargetName, info->NetbiosServerName)))
            picked[n++] = all[i];
    }
    if (!n) { free(picked); CredFree(all); SetLastError(ERROR_NOT_FOUND); return FALSE; }
    ok = pack((CREDENTIALW **)picked, n, count, out);
    free(picked);
    CredFree(all);
    return ok;
}
__declspec(dllexport) BOOL WINAPI CredWriteDomainCredentialsW(PCREDENTIAL_TARGET_INFORMATIONW info, PCREDENTIALW c, DWORD flags)
{
    CREDENTIALW copy;
    if (!info || !c || !info->TargetName || (flags & ~CRED_PRESERVE_CREDENTIAL_BLOB)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (c->Type != CRED_TYPE_DOMAIN_PASSWORD && c->Type != CRED_TYPE_DOMAIN_CERTIFICATE && c->Type != CRED_TYPE_DOMAIN_VISIBLE_PASSWORD) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    copy = *c;
    copy.TargetName = info->TargetName;
    return CredWriteW(&copy, flags);
}
__declspec(dllexport) BOOL WINAPI CredGetSessionTypes(DWORD count, LPDWORD persist)
{
    DWORD i;
    if (!persist || count != CRED_TYPE_MAXIMUM) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    for (i = 0; i < count; ++i) persist[i] = i == 0 ? CRED_PERSIST_NONE : CRED_PERSIST_ENTERPRISE;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredReadByTokenHandle(HANDLE token, LPCWSTR target, DWORD type, DWORD flags, PCREDENTIALW *out)
{
    (void)token;
    return CredReadW(target, type, flags, out);
}
__declspec(dllexport) BOOL WINAPI CredProfileLoaded(void) { return TRUE; }
__declspec(dllexport) BOOL WINAPI CredProfileLoadedEx(HANDLE token) { (void)token; return TRUE; }
__declspec(dllexport) BOOL WINAPI CredProfileUnloaded(void) { return TRUE; }
__declspec(dllexport) BOOL WINAPI CredBackupCredentials(HANDLE token, LPCWSTR file, PVOID key, DWORD key_size, DWORD flags)
{
    PCREDENTIALW *all;
    DWORD count, i;
    Buf b = {0};
    HANDLE f;
    DWORD done;
    BOOL ok;
    (void)token; (void)key; (void)key_size;
    if (!file || flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!CredEnumerateW(NULL, CRED_ENUMERATE_ALL_CREDENTIALS, &count, &all)) { count = 0; all = NULL; }
    put_dword(&b, 0x42435254u); put_dword(&b, count);
    for (i = 0; i < count; ++i) {
        Buf one = {0};
        serialize(all[i], &one);
        put_dword(&b, one.size);
        put_bytes(&b, one.data, one.size);
        free(one.data);
    }
    CredFree(all);
    if (b.failed) { free(b.data); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    f = CreateFileW(file, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) { free(b.data); return FALSE; }
    ok = WriteFile(f, b.data, b.size, &done, NULL) && done == b.size;
    CloseHandle(f);
    free(b.data);
    return ok;
}
__declspec(dllexport) BOOL WINAPI CredRestoreCredentials(LPCWSTR file, DWORD flags)
{
    HANDLE f;
    DWORD size, got = 0, count, i;
    BYTE *data;
    Rd r;
    if (!file || flags) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    size = GetFileSize(f, NULL);
    if (size == INVALID_FILE_SIZE || size > (16u << 20) || !(data = malloc(size + 8))) { CloseHandle(f); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    if (!ReadFile(f, data, size, &got, NULL) || got != size) { CloseHandle(f); free(data); return FALSE; }
    CloseHandle(f);
    r.p = data; r.left = size; r.failed = FALSE;
    if (rd_dword(&r) != 0x42435254u) { free(data); SetLastError(ERROR_INVALID_DATA); return FALSE; }
    count = rd_dword(&r);
    for (i = 0; i < count && !r.failed; ++i) {
        DWORD n = rd_dword(&r), block;
        const BYTE *rec = rd_bytes(&r, n);
        CREDENTIALW *c = rec ? deserialize(rec, n, &block) : NULL;
        if (!c) { r.failed = TRUE; break; }
        CredWriteW(c, 0);
        free(c);
    }
    free(data);
    if (r.failed) { SetLastError(ERROR_INVALID_DATA); return FALSE; }
    return TRUE;
}

/* ---- protection and marshaling --------------------------------------------------------------------------------------------------------------------------------- */

/* A protected credential is a string "@@" + hex of (a random key byte stream applied to the data); the key is of this boot (SystemFunction040), so it can be undone here only. */
NTSTATUS WINAPI SystemFunction040(PVOID memory, ULONG size, ULONG option);
NTSTATUS WINAPI SystemFunction041(PVOID memory, ULONG size, ULONG option);
static const WCHAR hexdigits[] = L"0123456789ABCDEF";
__declspec(dllexport) BOOL WINAPI CredProtectW(BOOL as_self, LPWSTR cleartext, DWORD length, LPWSTR out, DWORD *out_length, CRED_PROTECTION_TYPE *type)
{
    DWORD bytes, padded, need, i;
    BYTE *work;
    (void)as_self;
    if (!cleartext || !length || !out_length) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    bytes = length * 2;
    padded = (bytes + 4 + 7) & ~7u;
    need = 2 + padded * 2 + 1;
    if (!out || *out_length < need) { *out_length = need; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    if (!(work = calloc(1, padded))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    memcpy(work, &length, 4);
    memcpy(work + 4, cleartext, bytes);
    SystemFunction040(work, padded, 0);
    out[0] = '@'; out[1] = '@';
    for (i = 0; i < padded; ++i) { out[2 + 2 * i] = hexdigits[work[i] >> 4]; out[3 + 2 * i] = hexdigits[work[i] & 15]; }
    out[2 + padded * 2] = 0;
    free(work);
    *out_length = need;
    if (type) *type = CredUserProtection;
    return TRUE;
}
static BOOL parse_protected(LPCWSTR text, DWORD length, BYTE **out, DWORD *bytes)
{
    DWORD n = length ? length : (DWORD)wcslen(text), i, padded;
    BYTE *work;
    if (n < 2 || text[0] != '@' || text[1] != '@' || (n - 2) % 2 || (n - 2) / 2 < 8 || ((n - 2) / 2) % 8) return FALSE;
    padded = (n - 2) / 2;
    if (!(work = malloc(padded))) return FALSE;
    for (i = 0; i < padded; ++i) {
        WCHAR a = text[2 + 2 * i], b = text[3 + 2 * i];
        int x = a >= '0' && a <= '9' ? a - '0' : (a >= 'A' && a <= 'F' ? a - 'A' + 10 : (a >= 'a' && a <= 'f' ? a - 'a' + 10 : -1));
        int y = b >= '0' && b <= '9' ? b - '0' : (b >= 'A' && b <= 'F' ? b - 'A' + 10 : (b >= 'a' && b <= 'f' ? b - 'a' + 10 : -1));
        if (x < 0 || y < 0) { free(work); return FALSE; }
        work[i] = (BYTE)(x << 4 | y);
    }
    *out = work; *bytes = padded;
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredIsProtectedW(LPWSTR text, CRED_PROTECTION_TYPE *type)
{
    BYTE *work;
    DWORD bytes;
    if (!text || !type) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    *type = CredUnprotected;
    if (parse_protected(text, 0, &work, &bytes)) { *type = CredUserProtection; free(work); }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredUnprotectW(BOOL as_self, LPWSTR protected_text, DWORD length, LPWSTR out, DWORD *out_length)
{
    BYTE *work;
    DWORD bytes, chars;
    (void)as_self;
    if (!protected_text || !out_length) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!parse_protected(protected_text, length, &work, &bytes)) {      /* not protected: it is its own cleartext */
        DWORD n = length ? length : (DWORD)wcslen(protected_text);
        if (!out || *out_length < n + 1) { *out_length = n + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
        memcpy(out, protected_text, n * 2); out[n] = 0; *out_length = n;
        return TRUE;
    }
    SystemFunction041(work, bytes, 0);
    memcpy(&chars, work, 4);
    if (chars > (bytes - 4) / 2) { free(work); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!out || *out_length < chars + 1) { *out_length = chars + 1; free(work); SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    memcpy(out, work + 4, chars * 2);
    out[chars] = 0;
    *out_length = chars;
    free(work);
    return TRUE;
}

/* Marshaled credentials ("@@" and letters): a certificate hash (CertCredential), a user name that stands for a target (UsernameTargetCredential), or a blob (BinaryBlobCredential). The
 * letters are base 64 of this OS's own layout: a kind byte, then the data. */
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-.";
__declspec(dllexport) BOOL WINAPI CredMarshalCredentialW(CRED_MARSHAL_TYPE type, PVOID credential, LPWSTR *out)
{
    BYTE raw[1024 + 8];
    DWORD n = 0, i, chars;
    WCHAR *text;
    if (!credential || !out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    raw[n++] = (BYTE)type;
    switch (type) {
    case CertCredential: {
        CERT_CREDENTIAL_INFO *c = credential;
        if (c->cbSize < sizeof(*c)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        memcpy(raw + n, c->rgbHashOfCert, CERT_HASH_LENGTH); n += CERT_HASH_LENGTH;
        break;
    }
    case UsernameTargetCredential: {
        USERNAME_TARGET_CREDENTIAL_INFO *c = credential;
        DWORD len = c->UserName ? (DWORD)wcslen(c->UserName) : 0;
        if (!c->UserName || !len || len * 2 > 800) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        memcpy(raw + n, c->UserName, len * 2); n += len * 2;
        break;
    }
    case BinaryBlobCredential: {
        BINARY_BLOB_CREDENTIAL_INFO *c = credential;
        if (c->cbBlob > 800 || (c->cbBlob && !c->pbBlob)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        memcpy(raw + n, c->pbBlob, c->cbBlob); n += c->cbBlob;
        break;
    }
    default: SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    chars = 2 + (n + 2) / 3 * 4 + 1;
    if (!(text = malloc(chars * 2))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    text[0] = '@'; text[1] = '@';
    {
        DWORD at = 2;
        for (i = 0; i < n; i += 3) {
            DWORD v = raw[i] << 16 | (i + 1 < n ? raw[i + 1] << 8 : 0) | (i + 2 < n ? raw[i + 2] : 0);
            text[at++] = b64[(v >> 18) & 63]; text[at++] = b64[(v >> 12) & 63]; text[at++] = b64[(v >> 6) & 63]; text[at++] = b64[v & 63];
        }
        text[at] = 0;
    }
    *out = text;
    return TRUE;
}
static int b64_value(WCHAR c)
{
    const char *p;
    if (c > 127) return -1;
    p = strchr(b64, (int)c);
    return c && p ? (int)(p - b64) : -1;
}
static BOOL decode_marshaled(LPCWSTR text, BYTE *raw, DWORD *n)
{
    DWORD len = text ? (DWORD)wcslen(text) : 0, at = 0, i;
    if (len < 6 || text[0] != '@' || text[1] != '@' || (len - 2) % 4) return FALSE;
    for (i = 2; i < len; i += 4) {
        int a = b64_value(text[i]), b = b64_value(text[i + 1]), c = b64_value(text[i + 2]), d = b64_value(text[i + 3]);
        DWORD v;
        if (a < 0 || b < 0 || c < 0 || d < 0 || at + 3 > 1024) return FALSE;
        v = (DWORD)a << 18 | (DWORD)b << 12 | (DWORD)c << 6 | (DWORD)d;
        raw[at++] = (BYTE)(v >> 16); raw[at++] = (BYTE)(v >> 8); raw[at++] = (BYTE)v;
    }
    *n = at;
    return at > 0;
}
__declspec(dllexport) BOOL WINAPI CredIsMarshaledCredentialW(LPCWSTR text)
{
    BYTE raw[1032];
    DWORD n;
    if (!text || !decode_marshaled(text, raw, &n)) return FALSE;
    return raw[0] >= CertCredential && raw[0] <= BinaryBlobCredential;
}
__declspec(dllexport) BOOL WINAPI CredUnmarshalCredentialW(LPCWSTR text, PCRED_MARSHAL_TYPE type, PVOID *out)
{
    BYTE raw[1032];
    DWORD n;
    if (!text || !type || !out || !decode_marshaled(text, raw, &n)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    switch (raw[0]) {
    case CertCredential: {
        CERT_CREDENTIAL_INFO *c;
        if (n < 1 + CERT_HASH_LENGTH || !(c = calloc(1, sizeof(*c)))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        c->cbSize = sizeof(*c);
        memcpy(c->rgbHashOfCert, raw + 1, CERT_HASH_LENGTH);
        *out = c; *type = CertCredential;
        return TRUE;
    }
    case UsernameTargetCredential: {
        DWORD chars = (n - 1) / 2;
        USERNAME_TARGET_CREDENTIAL_INFO *c;
        while (chars && !((WCHAR *)(raw + 1))[chars - 1]) --chars;      /* (the padding of the last group) */
        if (!chars || !(c = calloc(1, sizeof(*c) + (chars + 1) * 2))) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        c->UserName = (LPWSTR)(c + 1);
        memcpy(c->UserName, raw + 1, chars * 2);
        *out = c; *type = UsernameTargetCredential;
        return TRUE;
    }
    case BinaryBlobCredential: {
        BINARY_BLOB_CREDENTIAL_INFO *c;
        if (!(c = calloc(1, sizeof(*c) + n))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        c->cbBlob = n - 1;
        c->pbBlob = (PUCHAR)(c + 1);
        memcpy(c->pbBlob, raw + 1, n - 1);
        *out = c; *type = BinaryBlobCredential;
        return TRUE;
    }
    default: SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
}
__declspec(dllexport) DWORD WINAPI CredEncryptAndMarshalBinaryBlob(PVOID blob, DWORD size, LPWSTR *out)
{
    BINARY_BLOB_CREDENTIAL_INFO info;
    info.cbBlob = size;
    info.pbBlob = blob;
    return CredMarshalCredentialW(BinaryBlobCredential, &info, out) ? ERROR_SUCCESS : GetLastError();
}

/* ---- the ANSI forms ---------------------------------------------------------------------------------------------------------------------------------------------- */

#define A2W(var, text) WCHAR *var = (text) ? strdupAW(text) : NULL
static char *to_ansi(LPCWSTR w)
{
    int n;
    char *a;
    if (!w) return NULL;
    n = WideCharToMultiByte(CP_ACP, 0, w, -1, NULL, 0, NULL, NULL);
    if ((a = malloc(n + 1))) WideCharToMultiByte(CP_ACP, 0, w, -1, a, n, NULL, NULL);
    return a;
}
/* A CREDENTIALW as a standalone block -> the same in ANSI, in one block. */
static CREDENTIALA *credential_to_ansi(const CREDENTIALW *w)
{
    char *t = to_ansi(w->TargetName), *cm = to_ansi(w->Comment), *al = to_ansi(w->TargetAlias), *un = to_ansi(w->UserName);
    DWORD i, total = sizeof(CREDENTIALA) + (t ? (DWORD)strlen(t) + 1 : 0) + (cm ? (DWORD)strlen(cm) + 1 : 0) + (al ? (DWORD)strlen(al) + 1 : 0) + (un ? (DWORD)strlen(un) + 1 : 0) + w->CredentialBlobSize + 8 +
                     w->AttributeCount * (sizeof(CREDENTIAL_ATTRIBUTEA) + 8);
    char **keys = calloc(w->AttributeCount + 1, sizeof(char *));
    CREDENTIALA *a;
    BYTE *at;
    for (i = 0; i < w->AttributeCount; ++i) { keys[i] = to_ansi(w->Attributes[i].Keyword); total += (keys[i] ? (DWORD)strlen(keys[i]) + 1 : 0) + w->Attributes[i].ValueSize + 8; }
    if (!(a = calloc(1, total + 16))) { free(t); free(cm); free(al); free(un); free(keys); return NULL; }
    a->Flags = w->Flags; a->Type = w->Type; a->LastWritten = w->LastWritten; a->CredentialBlobSize = w->CredentialBlobSize; a->Persist = w->Persist; a->AttributeCount = w->AttributeCount;
    at = (BYTE *)(a + 1);
#define PUT(field, s) do { if (s) { a->field = (char *)at; memcpy(at, s, strlen(s) + 1); at += strlen(s) + 1; } } while (0)
    PUT(TargetName, t); PUT(Comment, cm); PUT(TargetAlias, al); PUT(UserName, un);
    at = (BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
    if (w->CredentialBlobSize) { a->CredentialBlob = at; memcpy(at, w->CredentialBlob, w->CredentialBlobSize); at += (w->CredentialBlobSize + 7) & ~7u; }
    if (w->AttributeCount) {
        a->Attributes = (PCREDENTIAL_ATTRIBUTEA)at;
        at += w->AttributeCount * sizeof(CREDENTIAL_ATTRIBUTEA);
        for (i = 0; i < w->AttributeCount; ++i) {
            a->Attributes[i].Flags = w->Attributes[i].Flags; a->Attributes[i].ValueSize = w->Attributes[i].ValueSize;
            if (keys[i]) { a->Attributes[i].Keyword = (char *)at; memcpy(at, keys[i], strlen(keys[i]) + 1); at += strlen(keys[i]) + 1; }
            at = (BYTE *)(((ULONG_PTR)at + 7) & ~(ULONG_PTR)7);
            if (w->Attributes[i].ValueSize) { a->Attributes[i].Value = at; memcpy(at, w->Attributes[i].Value, w->Attributes[i].ValueSize); at += (w->Attributes[i].ValueSize + 7) & ~7u; }
        }
    }
    for (i = 0; i < w->AttributeCount; ++i) free(keys[i]);
    free(keys); free(t); free(cm); free(al); free(un);
    return a;
}
static CREDENTIALW *credential_from_ansi(const CREDENTIALA *a, WCHAR **parts, BYTE **alloc_attr)
{
    CREDENTIALW *w = calloc(1, sizeof(*w));
    DWORD i;
    (void)alloc_attr;
    if (!w) return NULL;
    w->Flags = a->Flags; w->Type = a->Type; w->LastWritten = a->LastWritten; w->CredentialBlobSize = a->CredentialBlobSize; w->CredentialBlob = a->CredentialBlob; w->Persist = a->Persist;
    parts[0] = a->TargetName ? strdupAW(a->TargetName) : NULL; w->TargetName = parts[0];
    parts[1] = a->Comment ? strdupAW(a->Comment) : NULL; w->Comment = parts[1];
    parts[2] = a->TargetAlias ? strdupAW(a->TargetAlias) : NULL; w->TargetAlias = parts[2];
    parts[3] = a->UserName ? strdupAW(a->UserName) : NULL; w->UserName = parts[3];
    w->AttributeCount = a->AttributeCount > MAX_ATTRIBUTES ? 0 : a->AttributeCount;
    if (w->AttributeCount && a->Attributes) {
        w->Attributes = calloc(w->AttributeCount, sizeof(CREDENTIAL_ATTRIBUTEW));
        if (w->Attributes) for (i = 0; i < w->AttributeCount; ++i) {
            w->Attributes[i].Keyword = a->Attributes[i].Keyword ? strdupAW(a->Attributes[i].Keyword) : NULL;
            w->Attributes[i].Flags = a->Attributes[i].Flags; w->Attributes[i].ValueSize = a->Attributes[i].ValueSize; w->Attributes[i].Value = a->Attributes[i].Value;
        }
    }
    return w;
}
static void free_credential_w(CREDENTIALW *w, WCHAR **parts)
{
    DWORD i;
    for (i = 0; i < 4; ++i) free(parts[i]);
    if (w->Attributes) for (i = 0; i < w->AttributeCount; ++i) free(w->Attributes[i].Keyword);
    free(w->Attributes);
    free(w);
}
__declspec(dllexport) BOOL WINAPI CredWriteA(PCREDENTIALA c, DWORD flags)
{
    WCHAR *parts[4] = {0};
    CREDENTIALW *w;
    BOOL r;
    DWORD e;
    if (!c) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(w = credential_from_ansi(c, parts, NULL))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    r = CredWriteW(w, flags);
    e = GetLastError();
    free_credential_w(w, parts);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredReadA(LPCSTR target, DWORD type, DWORD flags, PCREDENTIALA *out)
{
    WCHAR *t = target ? strdupAW(target) : NULL;
    PCREDENTIALW w;
    BOOL r;
    DWORD e;
    if (!out) { free(t); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    r = CredReadW(t, type, flags, &w);
    e = GetLastError();
    free(t);
    if (r) { *out = credential_to_ansi(w); CredFree(w); if (!*out) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; } return TRUE; }
    SetLastError(e);
    return FALSE;
}
__declspec(dllexport) BOOL WINAPI CredDeleteA(LPCSTR target, DWORD type, DWORD flags)
{
    A2W(t, target);
    BOOL r = CredDeleteW(t, type, flags);
    DWORD e = GetLastError();
    free(t);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredRenameA(LPCSTR old_target, LPCSTR new_target, DWORD type, DWORD flags)
{
    A2W(o, old_target); A2W(n, new_target);
    BOOL r = CredRenameW(o, n, type, flags);
    DWORD e = GetLastError();
    free(o); free(n);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredEnumerateA(LPCSTR filter, DWORD flags, DWORD *count, PCREDENTIALA **out)
{
    A2W(f, filter);
    PCREDENTIALW *w;
    BOOL r;
    DWORD e, i;
    r = CredEnumerateW(f, flags, count, &w);
    e = GetLastError();
    free(f);
    if (!r) { SetLastError(e); return FALSE; }
    {
        PCREDENTIALA *array = calloc(*count + 1, sizeof(*array));
        if (!array) { CredFree(w); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        for (i = 0; i < *count; ++i) array[i] = credential_to_ansi(w[i]);
        CredFree(w);
        *out = array;
    }
    return TRUE;
}
/* (an ANSI enumeration is a pointer array whose credentials are separate blocks: CredFree on the array leaks them in this form; CredFreeA-like cleanup is the program's) */
__declspec(dllexport) BOOL WINAPI CredFindBestCredentialA(LPCSTR target, DWORD type, DWORD flags, PCREDENTIALA *out)
{
    A2W(t, target);
    PCREDENTIALW w;
    BOOL r;
    DWORD e;
    if (!out) { free(t); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    r = CredFindBestCredentialW(t, type, flags, &w);
    e = GetLastError();
    free(t);
    if (!r) { SetLastError(e); return FALSE; }
    *out = credential_to_ansi(w);
    CredFree(w);
    return *out != NULL;
}
__declspec(dllexport) BOOL WINAPI CredReadDomainCredentialsA(PCREDENTIAL_TARGET_INFORMATIONA info, DWORD flags, DWORD *count, PCREDENTIALA **out)
{
    CREDENTIAL_TARGET_INFORMATIONW w;
    PCREDENTIALW *list;
    BOOL r;
    DWORD e, i;
    if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(&w, 0, sizeof(w));
    w.TargetName = strdupAW(info->TargetName);
    w.DnsServerName = strdupAW(info->DnsServerName);
    w.NetbiosServerName = strdupAW(info->NetbiosServerName);
    r = CredReadDomainCredentialsW(&w, flags, count, &list);
    e = GetLastError();
    free(w.TargetName); free(w.DnsServerName); free(w.NetbiosServerName);
    if (!r) { SetLastError(e); return FALSE; }
    {
        PCREDENTIALA *array = calloc(*count + 1, sizeof(*array));
        if (!array) { CredFree(list); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
        for (i = 0; i < *count; ++i) array[i] = credential_to_ansi(list[i]);
        CredFree(list);
        *out = array;
    }
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI CredWriteDomainCredentialsA(PCREDENTIAL_TARGET_INFORMATIONA info, PCREDENTIALA c, DWORD flags)
{
    CREDENTIAL_TARGET_INFORMATIONW w;
    WCHAR *parts[4] = {0};
    CREDENTIALW *cw;
    BOOL r;
    DWORD e;
    if (!info || !c) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    memset(&w, 0, sizeof(w));
    w.TargetName = strdupAW(info->TargetName);
    cw = credential_from_ansi(c, parts, NULL);
    r = cw ? CredWriteDomainCredentialsW(&w, cw, flags) : FALSE;
    e = cw ? GetLastError() : ERROR_NOT_ENOUGH_MEMORY;
    free(w.TargetName);
    if (cw) free_credential_w(cw, parts);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredGetTargetInfoA(LPCSTR server, DWORD flags, PCREDENTIAL_TARGET_INFORMATIONA *out)
{
    A2W(s, server);
    PCREDENTIAL_TARGET_INFORMATIONW w;
    BOOL r;
    DWORD e;
    if (!out) { free(s); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    r = CredGetTargetInfoW(s, flags, &w);
    e = GetLastError();
    free(s);
    if (!r) { SetLastError(e); return FALSE; }
    {
        CREDENTIAL_TARGET_INFORMATIONA *a;
        char *name = to_ansi(w->TargetName);
        DWORD n = name ? (DWORD)strlen(name) + 1 : 1;
        a = calloc(1, sizeof(*a) + n + 8);
        if (a) { memcpy((BYTE *)(a + 1), name ? name : "", n); a->TargetName = a->NetbiosServerName = a->DnsServerName = (char *)(a + 1); }
        free(name);
        CredFree(w);
        *out = a;
        return a != NULL;
    }
}
__declspec(dllexport) BOOL WINAPI CredProtectA(BOOL as_self, LPSTR cleartext, DWORD length, LPSTR out, DWORD *out_length, CRED_PROTECTION_TYPE *type)
{
    WCHAR *w = malloc((length + 1) * 2 * 2), *wide_out;
    DWORD wl, need;
    BOOL r;
    if (!cleartext || !out_length) { free(w); SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!w) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    wl = (DWORD)MultiByteToWideChar(CP_ACP, 0, cleartext, (int)length, w, (int)(length * 2));
    need = 2 + (((wl * 2 + 4 + 7) & ~7u) * 2) + 1;
    wide_out = malloc(need * 2);
    if (!wide_out) { free(w); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    {
        DWORD cap = need;
        r = CredProtectW(as_self, w, wl, wide_out, &cap, type);
        if (r) {
            if (!out || *out_length < cap) { *out_length = cap; SetLastError(ERROR_INSUFFICIENT_BUFFER); r = FALSE; }
            else { DWORD i; for (i = 0; i < cap; ++i) out[i] = (char)wide_out[i]; *out_length = cap; }
        } else *out_length = cap;
    }
    free(w); free(wide_out);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredUnprotectA(BOOL as_self, LPSTR protected_text, DWORD length, LPSTR out, DWORD *out_length)
{
    WCHAR *w, *wide_out;
    DWORD n, cap, i;
    BOOL r;
    if (!protected_text || !out_length) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    n = length ? length : (DWORD)strlen(protected_text);
    w = malloc((n + 1) * 2);
    wide_out = malloc((n + 2) * 2);
    if (!w || !wide_out) { free(w); free(wide_out); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    for (i = 0; i < n; ++i) w[i] = (BYTE)protected_text[i];
    w[n] = 0;
    cap = n + 1;
    r = CredUnprotectW(as_self, w, n, wide_out, &cap);
    if (r) {
        if (!out || *out_length < cap + 1) { *out_length = cap + 1; SetLastError(ERROR_INSUFFICIENT_BUFFER); r = FALSE; }
        else { int bytes = WideCharToMultiByte(CP_ACP, 0, wide_out, (int)cap, out, (int)*out_length, NULL, NULL); out[bytes] = 0; *out_length = (DWORD)bytes; }
    } else *out_length = cap;
    free(w); free(wide_out);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredIsProtectedA(LPSTR text, CRED_PROTECTION_TYPE *type)
{
    A2W(w, text);
    BOOL r = CredIsProtectedW(w, type);
    DWORD e = GetLastError();
    free(w);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredMarshalCredentialA(CRED_MARSHAL_TYPE type, PVOID credential, LPSTR *out)
{
    LPWSTR w = NULL;
    if (!out) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (type == UsernameTargetCredential && credential) {      /* the user name is ANSI in this form */
        USERNAME_TARGET_CREDENTIAL_INFO info;
        WCHAR *name = strdupAW((const char *)((USERNAME_TARGET_CREDENTIAL_INFO *)credential)->UserName);
        BOOL r;
        info.UserName = name;
        r = CredMarshalCredentialW(type, &info, &w);
        free(name);
        if (!r) return FALSE;
    } else if (!CredMarshalCredentialW(type, credential, &w)) return FALSE;
    *out = to_ansi(w);
    free(w);
    return *out != NULL;
}
__declspec(dllexport) BOOL WINAPI CredUnmarshalCredentialA(LPCSTR text, PCRED_MARSHAL_TYPE type, PVOID *out)
{
    A2W(w, text);
    BOOL r = CredUnmarshalCredentialW(w, type, out);
    DWORD e = GetLastError();
    free(w);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI CredIsMarshaledCredentialA(LPCSTR text)
{
    A2W(w, text);
    BOOL r = CredIsMarshaledCredentialW(w);
    free(w);
    return r;
}

/* The Credp* functions are the private interface between advapi32 and the credential service. */
#define CREDP(name) __declspec(dllexport) DWORD WINAPI name(void) { return ERROR_CALL_NOT_IMPLEMENTED; }
CREDP(CredpConvertCredential)
CREDP(CredpConvertOneCredentialSize)
CREDP(CredpConvertTargetInfo)
CREDP(CredpDecodeCredential)
CREDP(CredpEncodeCredential)
CREDP(CredpEncodeSecret)
