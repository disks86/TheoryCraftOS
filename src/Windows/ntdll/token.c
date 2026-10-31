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
/* ntdll.dll: access tokens (open, duplicate, filter, query, set, adjust privileges and groups), the access check, and the security descriptors that objects carry. The kernel
 * keeps a token as one blob in a token object (src/Windows/tcos_token.h) and the descriptor of every other object; the services here read the blob into a TokenData, change it
 * and write it back, so that what a token is, and may do, is decided in one place. */
#define WIN32_NO_STATUS
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include "security.h"
#include "../tcos_token.h"
#include "../tcos_syscalls.h"

#define TOKEN_ALL 0xF01FF
#define SE_CHANGE_NOTIFY_PRIVILEGE_VALUE_ 23
#define SE_CREATE_TOKEN_PRIVILEGE_VALUE_ 2
#ifndef SEF_DACL_AUTO_INHERIT
#define SEF_DACL_AUTO_INHERIT 1
#define SEF_SACL_AUTO_INHERIT 2
#endif
#ifndef SE_GROUP_INTEGRITY_ENABLED
#define SE_GROUP_INTEGRITY_ENABLED 0x40
#endif
#define MAX_SID_BYTES 68
#define DEFAULT_DACL_BYTES 2048
#define STATUS_NO_IMPERSONATION_TOKEN_ ((NTSTATUS)0xC000005C)
#define STATUS_CANT_DISABLE_MANDATORY_ ((NTSTATUS)0xC000005D)
#define STATUS_INVALID_OWNER_ ((NTSTATUS)0xC000005A)
#define STATUS_INVALID_PRIMARY_GROUP_ ((NTSTATUS)0xC000005B)
#define STATUS_BAD_TOKEN_TYPE_ ((NTSTATUS)0xC00000A8)
#define STATUS_NO_TOKEN_ ((NTSTATUS)0xC000007C)
#define STATUS_BAD_IMPERSONATION_LEVEL_ ((NTSTATUS)0xC00000A5)
#define STATUS_GENERIC_NOT_MAPPED_ ((NTSTATUS)0xC00000E6)
#define STATUS_NOT_ALL_ASSIGNED_ ((NTSTATUS)0x00000106)
#define STATUS_INVALID_SECURITY_DESCR_ ((NTSTATUS)0xC0000079)
#define STATUS_PRIVILEGE_NOT_HELD_ ((NTSTATUS)0xC0000061)
#define STATUS_BUFFER_TOO_SMALL_ ((NTSTATUS)0xC0000023)
#define STATUS_INVALID_INFO_CLASS_ ((NTSTATUS)0xC0000003)
#define STATUS_INFO_LENGTH_MISMATCH_ ((NTSTATUS)0xC0000004)
#define STATUS_INVALID_PARAMETER_ ((NTSTATUS)0xC000000D)
#define STATUS_ACCESS_VIOLATION_ ((NTSTATUS)0xC0000005)
#define STATUS_INVALID_HANDLE_ ((NTSTATUS)0xC0000008)
#define STATUS_NO_MEMORY_ ((NTSTATUS)0xC0000017)
#define STATUS_ACCESS_DENIED_ ((NTSTATUS)0xC0000022)
#define STATUS_NOT_SUPPORTED_ ((NTSTATUS)0xC00000BB)
#define STATUS_INVALID_ACL_ ((NTSTATUS)0xC0000077)

NTSTATUS NTAPI NtTcosToken(ULONG op, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d, ULONG_PTR e);
NTSTATUS NTAPI NtTcosAccessCheck(void *arguments);

/* ---- the token as plain data ------------------------------------------------------------------------------------------------------------------------- */

typedef struct { BYTE sid[MAX_SID_BYTES]; DWORD attrs; } TSid;
typedef struct {
    DWORD type, level, session, elevation_type, flags, policy;
    ULONGLONG token_id, auth_id, modified_id, expiration, source_id, origin;
    BYTE source_name[8];
    TSid user;
    BOOL has_owner, has_pgroup, has_dacl;
    BYTE owner[MAX_SID_BYTES], pgroup[MAX_SID_BYTES];
    DWORD ngroups;
    TSid groups[TCOS_TOKEN_MAX_GROUPS];
    DWORD npriv;
    LUID_AND_ATTRIBUTES priv[TCOS_TOKEN_MAX_PRIVILEGES];
    DWORD nrestr;
    TSid restr[TCOS_TOKEN_MAX_RESTRICTED];
    BYTE dacl[DEFAULT_DACL_BYTES];
} TokenData;

static void *heap_new(SIZE_T n) { return RtlAllocateHeap(RtlProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void heap_drop(void *p) { if (p) RtlFreeHeap(RtlProcessHeap(), 0, p); }

static BOOL sid_ok(const BYTE *p, SIZE_T room)
{
    if (room < 8 || p[0] != 1 || p[1] > 15) return FALSE;
    return room >= 8 + 4 * (SIZE_T)p[1];
}
static DWORD sid_size(const BYTE *p) { return 8 + 4 * (DWORD)p[1]; }
static DWORD ld32(const BYTE *p) { return p[0] | (DWORD)p[1] << 8 | (DWORD)p[2] << 16 | (DWORD)p[3] << 24; }
static void st32(BYTE *p, DWORD v) { p[0] = (BYTE)v; p[1] = (BYTE)(v >> 8); p[2] = (BYTE)(v >> 16); p[3] = (BYTE)(v >> 24); }

static BOOL parse_blob(const BYTE *blob, DWORD size, TokenData *t)
{
    const TcosTokenBlob *h = (const TcosTokenBlob *)blob;
    DWORD i;
    if (size < sizeof(*h) || h->magic != TCOS_TOKEN_MAGIC || h->size != size) return FALSE;
    t->type = h->type; t->level = h->level; t->session = h->session; t->elevation_type = h->elevation_type; t->flags = h->flags; t->policy = h->mandatory_policy;
    t->token_id = h->token_id; t->auth_id = h->auth_id; t->modified_id = h->modified_id; t->expiration = h->expiration; t->source_id = h->source_id; t->origin = h->origin;
    memcpy(t->source_name, h->source_name, 8);
    if (h->user_off >= size || !sid_ok(blob + h->user_off, size - h->user_off)) return FALSE;
    memcpy(t->user.sid, blob + h->user_off, sid_size(blob + h->user_off));
    t->user.attrs = h->user_attrs;
    if (h->owner_off) { if (h->owner_off >= size || !sid_ok(blob + h->owner_off, size - h->owner_off)) return FALSE; memcpy(t->owner, blob + h->owner_off, sid_size(blob + h->owner_off)); t->has_owner = TRUE; }
    if (h->pgroup_off) { if (h->pgroup_off >= size || !sid_ok(blob + h->pgroup_off, size - h->pgroup_off)) return FALSE; memcpy(t->pgroup, blob + h->pgroup_off, sid_size(blob + h->pgroup_off)); t->has_pgroup = TRUE; }
    if (h->group_count > TCOS_TOKEN_MAX_GROUPS || h->privilege_count > TCOS_TOKEN_MAX_PRIVILEGES || h->restricted_count > TCOS_TOKEN_MAX_RESTRICTED) return FALSE;
    t->ngroups = h->group_count;
    for (i = 0; i < t->ngroups; ++i) {
        const BYTE *e = blob + h->groups_off + 8 * i;
        DWORD off;
        if (h->groups_off + 8 * (i + 1) > size) return FALSE;
        off = ld32(e);
        if (off >= size || !sid_ok(blob + off, size - off)) return FALSE;
        memcpy(t->groups[i].sid, blob + off, sid_size(blob + off));
        t->groups[i].attrs = ld32(e + 4);
    }
    t->nrestr = h->restricted_count;
    for (i = 0; i < t->nrestr; ++i) {
        const BYTE *e = blob + h->restricted_off + 8 * i;
        DWORD off;
        if (h->restricted_off + 8 * (i + 1) > size) return FALSE;
        off = ld32(e);
        if (off >= size || !sid_ok(blob + off, size - off)) return FALSE;
        memcpy(t->restr[i].sid, blob + off, sid_size(blob + off));
        t->restr[i].attrs = ld32(e + 4);
    }
    t->npriv = h->privilege_count;
    for (i = 0; i < t->npriv; ++i) {
        const BYTE *e = blob + h->privileges_off + 12 * i;
        if (h->privileges_off + 12 * (i + 1) > size) return FALSE;
        t->priv[i].Luid.LowPart = ld32(e);
        t->priv[i].Luid.HighPart = (LONG)ld32(e + 4);
        t->priv[i].Attributes = ld32(e + 8);
    }
    if (h->dacl_off) {
        DWORD n;
        if (h->dacl_off + 8 > size) return FALSE;
        n = blob[h->dacl_off + 2] | (DWORD)blob[h->dacl_off + 3] << 8;
        if (n < 8 || h->dacl_off + n > size || n > DEFAULT_DACL_BYTES) return FALSE;
        memcpy(t->dacl, blob + h->dacl_off, n);
        t->has_dacl = TRUE;
    }
    return TRUE;
}

/* The blob of a token; returns its size, 0 if it does not fit. */
static DWORD build_blob(const TokenData *t, BYTE *out, DWORD capacity)
{
    TcosTokenBlob *h = (TcosTokenBlob *)out;
    DWORD at = sizeof(*h), i;
    DWORD offs[TCOS_TOKEN_MAX_GROUPS + TCOS_TOKEN_MAX_RESTRICTED];
    if (capacity < sizeof(*h)) return 0;
    memset(out, 0, sizeof(*h));
    h->magic = TCOS_TOKEN_MAGIC;
    h->type = t->type; h->level = t->level; h->session = t->session; h->elevation_type = t->elevation_type; h->flags = t->flags; h->mandatory_policy = t->policy;
    h->token_id = t->token_id; h->auth_id = t->auth_id; h->modified_id = t->modified_id; h->expiration = t->expiration; h->source_id = t->source_id; h->origin = t->origin;
    memcpy(h->source_name, t->source_name, 8);
#define PUT_SID(sidptr, dest) do { DWORD n_ = sid_size(sidptr); if (at + n_ > capacity) return 0; memcpy(out + at, sidptr, n_); dest = at; at += n_; } while (0)
    PUT_SID(t->user.sid, h->user_off);
    h->user_attrs = t->user.attrs;
    if (t->has_owner) PUT_SID(t->owner, h->owner_off);
    if (t->has_pgroup) PUT_SID(t->pgroup, h->pgroup_off);
    for (i = 0; i < t->ngroups; ++i) PUT_SID(t->groups[i].sid, offs[i]);
    for (i = 0; i < t->nrestr; ++i) PUT_SID(t->restr[i].sid, offs[TCOS_TOKEN_MAX_GROUPS + i]);
#undef PUT_SID
    if (at + 8 * (t->ngroups + t->nrestr) + 12 * t->npriv > capacity) return 0;
    h->group_count = t->ngroups; h->groups_off = at;
    for (i = 0; i < t->ngroups; ++i) { st32(out + at, offs[i]); st32(out + at + 4, t->groups[i].attrs); at += 8; }
    h->restricted_count = t->nrestr; h->restricted_off = at;
    for (i = 0; i < t->nrestr; ++i) { st32(out + at, offs[TCOS_TOKEN_MAX_GROUPS + i]); st32(out + at + 4, t->restr[i].attrs); at += 8; }
    h->privilege_count = t->npriv; h->privileges_off = at;
    for (i = 0; i < t->npriv; ++i) { st32(out + at, t->priv[i].Luid.LowPart); st32(out + at + 4, (DWORD)t->priv[i].Luid.HighPart); st32(out + at + 8, t->priv[i].Attributes); at += 12; }
    if (t->has_dacl) {
        DWORD n = t->dacl[2] | (DWORD)t->dacl[3] << 8;
        if (at + n > capacity) return 0;
        memcpy(out + at, t->dacl, n);
        h->dacl_off = at;
        at += n;
    }
    h->size = at;
    return at;
}

/* Reads a token object into data (allocated by the caller); `required` the rights the handle must hold. */
static NTSTATUS read_token(HANDLE token, ACCESS_MASK required, TokenData *t)
{
    BYTE *blob = heap_new(TCOS_TOKEN_MAX_BYTES);
    ULONG needed = 0;
    NTSTATUS status;
    if (!blob) return STATUS_NO_MEMORY_;
    status = NtTcosToken(TCOS_TOKEN_READ, (ULONG_PTR)token, required, (ULONG_PTR)blob, TCOS_TOKEN_MAX_BYTES, (ULONG_PTR)&needed);
    if (!status && !parse_blob(blob, needed, t)) status = STATUS_INVALID_HANDLE_;
    heap_drop(blob);
    return status;
}
static NTSTATUS write_token(HANDLE token, ACCESS_MASK required, TokenData *t)
{
    BYTE *blob = heap_new(TCOS_TOKEN_MAX_BYTES);
    DWORD size;
    NTSTATUS status;
    if (!blob) return STATUS_NO_MEMORY_;
    t->modified_id++;
    size = build_blob(t, blob, TCOS_TOKEN_MAX_BYTES);
    status = size ? NtTcosToken(TCOS_TOKEN_WRITE, (ULONG_PTR)token, required, (ULONG_PTR)blob, size, 0) : STATUS_NO_MEMORY_;
    heap_drop(blob);
    return status;
}
static NTSTATUS create_token(TokenData *t, ACCESS_MASK access, BOOL inherit, HANDLE *out)
{
    BYTE *blob = heap_new(TCOS_TOKEN_MAX_BYTES);
    DWORD size;
    NTSTATUS status;
    if (!blob) return STATUS_NO_MEMORY_;
    size = build_blob(t, blob, TCOS_TOKEN_MAX_BYTES);
    status = size ? NtTcosToken(TCOS_TOKEN_CREATE, (ULONG_PTR)blob, size, access, inherit, (ULONG_PTR)out) : STATUS_NO_MEMORY_;
    heap_drop(blob);
    return status;
}

/* The next identifier for tokens and logon sessions: unique in the process; the kernel numbers its own from another range. */
static ULONGLONG next_luid(void)
{
    static volatile LONG64 counter = 0x400000;
    return (ULONGLONG)InterlockedIncrement64(&counter);
}

/* ---- opening and copying ----------------------------------------------------------------------------------------------------------------------------- */

static NTSTATUS open_process_token(HANDLE process, ACCESS_MASK access, ULONG attributes, PHANDLE token)
{
    if (!token) return STATUS_ACCESS_VIOLATION_;
    return NtTcosToken(TCOS_TOKEN_OPEN_PROCESS, (ULONG_PTR)process, access, (ULONG_PTR)token, (attributes & OBJ_INHERIT) ? 1 : 0, 0);
}
__declspec(dllexport) NTSTATUS NTAPI NtOpenProcessTokenEx(HANDLE process, ACCESS_MASK access, ULONG attributes, PHANDLE token) { return open_process_token(process, access, attributes, token); }
__declspec(dllexport) NTSTATUS NTAPI NtOpenProcessToken(HANDLE process, ACCESS_MASK access, PHANDLE token) { return open_process_token(process, access, 0, token); }
__declspec(dllexport) NTSTATUS NTAPI NtOpenThreadTokenEx(HANDLE thread, ACCESS_MASK access, BOOLEAN self, ULONG attributes, PHANDLE token)
{
    (void)self;
    if (!token) return STATUS_ACCESS_VIOLATION_;
    return NtTcosToken(TCOS_TOKEN_OPEN_THREAD, (ULONG_PTR)thread, access, (ULONG_PTR)token, (attributes & OBJ_INHERIT) ? 1 : 0, 0);
}
__declspec(dllexport) NTSTATUS NTAPI NtOpenThreadToken(HANDLE thread, ACCESS_MASK access, BOOLEAN self, PHANDLE token) { return NtOpenThreadTokenEx(thread, access, self, 0, token); }

static void qos_of(const OBJECT_ATTRIBUTES *attr, BOOL *have, DWORD *level, BOOL *effective_only)
{
    *have = FALSE;
    *level = 0;
    *effective_only = FALSE;
    if (attr && attr->Length >= sizeof(*attr) && attr->SecurityQualityOfService) {
        const SECURITY_QUALITY_OF_SERVICE *q = attr->SecurityQualityOfService;
        if (q->Length == sizeof(*q)) { *have = TRUE; *level = q->ImpersonationLevel; *effective_only = q->EffectiveOnly; }
    }
}

__declspec(dllexport) NTSTATUS NTAPI NtDuplicateToken(HANDLE source, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr, BOOLEAN effective_only, TOKEN_TYPE type, PHANDLE out)
{
    TokenData *t;
    NTSTATUS status;
    BOOL have_qos, qos_effective;
    DWORD level;
    DWORD i, kept;
    if (!out) return STATUS_ACCESS_VIOLATION_;
    if (type != TokenPrimary && type != TokenImpersonation) return STATUS_INVALID_PARAMETER_;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(source, TOKEN_DUPLICATE, t);
    if (status) { heap_drop(t); return status; }
    qos_of(attr, &have_qos, &level, &qos_effective);
    if (type == TokenImpersonation) {
        if (t->type == TokenPrimary) t->level = have_qos ? level : SecurityImpersonation;
        else if (have_qos) { if (level > t->level) { heap_drop(t); return STATUS_BAD_IMPERSONATION_LEVEL_; } t->level = level; }
        if (t->level > SecurityDelegation) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
    } else {
        if (t->type == TokenImpersonation && t->level < SecurityImpersonation) { heap_drop(t); return STATUS_BAD_IMPERSONATION_LEVEL_; }
        t->level = 0;
    }
    t->type = type;
    if (effective_only || qos_effective) {      /* only what is enabled is kept */
        for (i = kept = 0; i < t->npriv; ++i) if (t->priv[i].Attributes & SE_PRIVILEGE_ENABLED) t->priv[kept++] = t->priv[i];
        t->npriv = kept;
    }
    t->token_id = next_luid();
    if (!access) access = TOKEN_ALL;
    status = create_token(t, access, attr && (attr->Attributes & OBJ_INHERIT), out);
    if (!status && attr && attr->SecurityDescriptor) {
        ULONG length = RtlLengthSecurityDescriptor(attr->SecurityDescriptor);
        BYTE *rel = heap_new(length + 16);
        if (rel) {
            if (!RtlMakeSelfRelativeSD(attr->SecurityDescriptor, rel, &length)) NtTcosToken(TCOS_OBJECT_SD_SET, (ULONG_PTR)*out, 0, (ULONG_PTR)rel, length, 0);
            heap_drop(rel);
        }
    }
    heap_drop(t);
    return status;
}

static const BYTE *find_sid(const TokenData *t, const BYTE *sid, TSid **group)
{
    DWORD i;
    *group = NULL;
    if (RtlEqualSid((PSID)t->user.sid, (PSID)sid)) return t->user.sid;
    for (i = 0; i < t->ngroups; ++i) if (RtlEqualSid((PSID)t->groups[i].sid, (PSID)sid)) { *group = (TSid *)&t->groups[i]; return t->groups[i].sid; }
    return NULL;
}

__declspec(dllexport) NTSTATUS NTAPI NtFilterToken(HANDLE source, ULONG flags, TOKEN_GROUPS *disable, TOKEN_PRIVILEGES *remove, TOKEN_GROUPS *restrict_sids, PHANDLE out)
{
    TokenData *t;
    NTSTATUS status;
    DWORD i, j, kept;
    if (!out) return STATUS_ACCESS_VIOLATION_;
    if (flags & ~(DISABLE_MAX_PRIVILEGE | SANDBOX_INERT | LUA_TOKEN | WRITE_RESTRICTED)) return STATUS_INVALID_PARAMETER_;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(source, TOKEN_DUPLICATE | TOKEN_QUERY, t);
    if (status) { heap_drop(t); return status; }
    if (disable) {
        for (i = 0; i < disable->GroupCount; ++i) {
            TSid *g;
            if (!RtlValidSid(disable->Groups[i].Sid)) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
            if (!find_sid(t, disable->Groups[i].Sid, &g)) continue;
            if (g) { g->attrs = (g->attrs & ~(SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT)) | SE_GROUP_USE_FOR_DENY_ONLY; }
            else t->user.attrs = (t->user.attrs & ~(SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT)) | SE_GROUP_USE_FOR_DENY_ONLY;
        }
    }
    if (flags & DISABLE_MAX_PRIVILEGE) {      /* every privilege but the one to see all folders goes */
        for (i = kept = 0; i < t->npriv; ++i) if (t->priv[i].Luid.LowPart == SE_CHANGE_NOTIFY_PRIVILEGE_VALUE_) t->priv[kept++] = t->priv[i];
        t->npriv = kept;
    }
    if (remove) {
        for (i = 0; i < remove->PrivilegeCount; ++i)
            for (j = 0; j < t->npriv; ++j)
                if (t->priv[j].Luid.LowPart == remove->Privileges[i].Luid.LowPart && t->priv[j].Luid.HighPart == remove->Privileges[i].Luid.HighPart) { t->priv[j] = t->priv[--t->npriv]; break; }
    }
    if (restrict_sids) {
        if (restrict_sids->GroupCount + t->nrestr > TCOS_TOKEN_MAX_RESTRICTED) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
        for (i = 0; i < restrict_sids->GroupCount; ++i) {
            if (!RtlValidSid(restrict_sids->Groups[i].Sid)) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
            memcpy(t->restr[t->nrestr].sid, restrict_sids->Groups[i].Sid, RtlLengthSid(restrict_sids->Groups[i].Sid));
            t->restr[t->nrestr].attrs = SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY;
            t->nrestr++;
        }
        t->flags |= TCOS_TOKEN_FLAG_HAS_RESTRICTIONS;
        if (flags & WRITE_RESTRICTED) t->flags |= TCOS_TOKEN_FLAG_WRITE_RESTRICTED;
    }
    if (flags & SANDBOX_INERT) t->flags |= TCOS_TOKEN_FLAG_SANDBOX_INERT;
    t->token_id = next_luid();
    status = create_token(t, TOKEN_ALL, FALSE, out);
    heap_drop(t);
    return status;
}

__declspec(dllexport) NTSTATUS NTAPI NtCompareTokens(HANDLE first, HANDLE second, PBOOLEAN equal)
{
    TokenData *a, *b;
    NTSTATUS status;
    DWORD i;
    BOOLEAN same = TRUE;
    if (!equal) return STATUS_ACCESS_VIOLATION_;
    a = heap_new(sizeof(*a));
    b = heap_new(sizeof(*b));
    if (!a || !b) { heap_drop(a); heap_drop(b); return STATUS_NO_MEMORY_; }
    status = read_token(first, TOKEN_QUERY, a);
    if (!status) status = read_token(second, TOKEN_QUERY, b);
    if (!status) {
        if (!RtlEqualSid((PSID)a->user.sid, (PSID)b->user.sid) || a->ngroups != b->ngroups || a->npriv != b->npriv || a->nrestr != b->nrestr || a->flags != b->flags) same = FALSE;
        for (i = 0; same && i < a->ngroups; ++i) if (!RtlEqualSid((PSID)a->groups[i].sid, (PSID)b->groups[i].sid) || a->groups[i].attrs != b->groups[i].attrs) same = FALSE;
        for (i = 0; same && i < a->npriv; ++i) if (a->priv[i].Luid.LowPart != b->priv[i].Luid.LowPart || a->priv[i].Attributes != b->priv[i].Attributes) same = FALSE;
        for (i = 0; same && i < a->nrestr; ++i) if (!RtlEqualSid((PSID)a->restr[i].sid, (PSID)b->restr[i].sid)) same = FALSE;
        *equal = same;
    }
    heap_drop(a);
    heap_drop(b);
    return status;
}

/* ---- the information classes ------------------------------------------------------------------------------------------------------------------------- */

typedef struct { BYTE *base; DWORD capacity, used; } Out;

static BOOL elevated_token(const TokenData *t)
{
    DWORD i;
    for (i = 0; i < t->ngroups; ++i)
        if ((t->groups[i].attrs & SE_GROUP_INTEGRITY) && t->groups[i].sid[1] == 1 && t->groups[i].sid[7] == 16 && ld32(t->groups[i].sid + 8) >= 0x3000) return TRUE;
    return FALSE;
}
static const TSid *integrity_group(const TokenData *t)
{
    DWORD i;
    for (i = 0; i < t->ngroups; ++i) if (t->groups[i].attrs & SE_GROUP_INTEGRITY) return &t->groups[i];
    return NULL;
}
static const TSid *logon_group(const TokenData *t)
{
    DWORD i;
    for (i = 0; i < t->ngroups; ++i) if ((t->groups[i].attrs & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID) return &t->groups[i];
    return NULL;
}

/* Writes the data of one class into the buffer; returns the size needed. The buffer may be too small: nothing is written then. */
static DWORD class_data(const TokenData *t, int class_, BYTE *buf, DWORD capacity, NTSTATUS *status)
{
    DWORD i, need = 0, at;
    *status = STATUS_SUCCESS;
#define SIMPLE(value) do { need = 4; if (capacity >= 4) st32(buf, (DWORD)(value)); } while (0)
    switch (class_) {
    case TokenUser:
        need = 16 + sid_size(t->user.sid);
        if (capacity >= need) { SID_AND_ATTRIBUTES *u = (SID_AND_ATTRIBUTES *)buf; memcpy(buf + 16, t->user.sid, sid_size(t->user.sid)); u->Sid = buf + 16; u->Attributes = t->user.attrs; }
        return need;
    case TokenGroups: case TokenRestrictedSids: {
        const TSid *list = class_ == TokenGroups ? t->groups : t->restr;
        DWORD count = class_ == TokenGroups ? t->ngroups : t->nrestr;
        need = 8 + 16 * count;
        for (i = 0; i < count; ++i) need += sid_size(list[i].sid);
        if (capacity >= need) {
            TOKEN_GROUPS *g = (TOKEN_GROUPS *)buf;
            at = 8 + 16 * count;
            g->GroupCount = count;
            for (i = 0; i < count; ++i) { memcpy(buf + at, list[i].sid, sid_size(list[i].sid)); g->Groups[i].Sid = buf + at; g->Groups[i].Attributes = list[i].attrs; at += sid_size(list[i].sid); }
        }
        return need;
    }
    case TokenPrivileges:
        need = 4 + 12 * t->npriv;
        if (capacity >= need) { TOKEN_PRIVILEGES *p = (TOKEN_PRIVILEGES *)buf; p->PrivilegeCount = t->npriv; for (i = 0; i < t->npriv; ++i) p->Privileges[i] = t->priv[i]; }
        return need;
    case TokenOwner: case TokenPrimaryGroup: {
        const BYTE *sid = class_ == TokenOwner ? (t->has_owner ? t->owner : t->user.sid) : (t->has_pgroup ? t->pgroup : t->user.sid);
        need = 8 + sid_size(sid);
        if (capacity >= need) { memcpy(buf + 8, sid, sid_size(sid)); *(PSID *)buf = buf + 8; }
        return need;
    }
    case TokenDefaultDacl:
        need = 8 + (t->has_dacl ? (t->dacl[2] | (DWORD)t->dacl[3] << 8) : 0);
        if (capacity >= need) { if (t->has_dacl) { memcpy(buf + 8, t->dacl, need - 8); *(PACL *)buf = (PACL)(buf + 8); } else *(PACL *)buf = NULL; }
        return need;
    case TokenSource:
        need = sizeof(TOKEN_SOURCE);
        if (capacity >= need) { TOKEN_SOURCE *s = (TOKEN_SOURCE *)buf; memcpy(s->SourceName, t->source_name, 8); s->SourceIdentifier.LowPart = (DWORD)t->source_id; s->SourceIdentifier.HighPart = (LONG)(t->source_id >> 32); }
        return need;
    case TokenType: SIMPLE(t->type); return need;
    case TokenImpersonationLevel: if (t->type != TokenImpersonation) { *status = STATUS_INVALID_PARAMETER_; return 0; } SIMPLE(t->level); return need;
    case TokenStatistics:
        need = sizeof(TOKEN_STATISTICS);
        if (capacity >= need) {
            TOKEN_STATISTICS *s = (TOKEN_STATISTICS *)buf;
            memset(s, 0, sizeof(*s));
            s->TokenId.LowPart = (DWORD)t->token_id; s->TokenId.HighPart = (LONG)(t->token_id >> 32);
            s->AuthenticationId.LowPart = (DWORD)t->auth_id; s->AuthenticationId.HighPart = (LONG)(t->auth_id >> 32);
            s->ExpirationTime.QuadPart = (LONGLONG)t->expiration;
            s->TokenType = (TOKEN_TYPE)t->type;
            s->ImpersonationLevel = (SECURITY_IMPERSONATION_LEVEL)t->level;
            s->DynamicCharged = 0x400; s->DynamicAvailable = 0x3F0;
            s->GroupCount = t->ngroups; s->PrivilegeCount = t->npriv;
            s->ModifiedId.LowPart = (DWORD)t->modified_id; s->ModifiedId.HighPart = (LONG)(t->modified_id >> 32);
        }
        return need;
    case TokenSessionId: SIMPLE(t->session); return need;
    case TokenSandBoxInert: SIMPLE((t->flags & TCOS_TOKEN_FLAG_SANDBOX_INERT) != 0); return need;
    case TokenOrigin: need = 8; if (capacity >= 8) { st32(buf, (DWORD)t->origin); st32(buf + 4, (DWORD)(t->origin >> 32)); } return need;
    case TokenElevationType: SIMPLE(t->elevation_type); return need;
    case TokenElevation: SIMPLE(elevated_token(t) && t->elevation_type != 3); return need;
    case TokenHasRestrictions: SIMPLE(t->nrestr != 0); return need;
    case TokenVirtualizationAllowed: SIMPLE((t->flags & TCOS_TOKEN_FLAG_VIRT_ALLOWED) != 0); return need;
    case TokenVirtualizationEnabled: SIMPLE((t->flags & TCOS_TOKEN_FLAG_VIRT_ENABLED) != 0); return need;
    case TokenUIAccess: SIMPLE((t->flags & TCOS_TOKEN_FLAG_UIACCESS) != 0); return need;
    case TokenMandatoryPolicy: SIMPLE(t->policy); return need;
    case TokenIntegrityLevel: {
        const TSid *g = integrity_group(t);
        if (!g) { *status = STATUS_INVALID_PARAMETER_; return 0; }
        need = 16 + sid_size(g->sid);
        if (capacity >= need) { TOKEN_MANDATORY_LABEL *m = (TOKEN_MANDATORY_LABEL *)buf; memcpy(buf + 16, g->sid, sid_size(g->sid)); m->Label.Sid = buf + 16; m->Label.Attributes = g->attrs; }
        return need;
    }
    case TokenLogonSid: {
        const TSid *g = logon_group(t);
        need = 8 + (g ? 16 + sid_size(g->sid) : 0);
        if (capacity >= need) {
            TOKEN_GROUPS *r = (TOKEN_GROUPS *)buf;
            r->GroupCount = g ? 1 : 0;
            if (g) { memcpy(buf + 24, g->sid, sid_size(g->sid)); r->Groups[0].Sid = buf + 24; r->Groups[0].Attributes = g->attrs; }
        }
        return need;
    }
    case TokenIsAppContainer: case 32 /* TokenAppContainerNumber */: SIMPLE(0); return need;
    case TokenCapabilities: need = 8; if (capacity >= 8) memset(buf, 0, 8); return need;
    case TokenAppContainerSid: need = 8; if (capacity >= 8) memset(buf, 0, 8); return need;
    case TokenAccessInformation: {
        /* TOKEN_ACCESS_INFORMATION: the SIDs, privileges and so on as the access check sees them (the header, then the arrays) */
        const TSid *il = integrity_group(t);
        DWORD sids = 8 + 16 * (1 + t->ngroups), restricted = 8 + 16 * t->nrestr, priv = 4 + 12 * t->npriv;
        DWORD sid_bytes = sid_size(t->user.sid), il_bytes = il ? 16 + sid_size(il->sid) : 0;
        for (i = 0; i < t->ngroups; ++i) sid_bytes += sid_size(t->groups[i].sid);
        for (i = 0; i < t->nrestr; ++i) sid_bytes += sid_size(t->restr[i].sid);
        need = 0x90 + sids + restricted + priv + sid_bytes + il_bytes + 16 + sid_size(t->has_owner ? t->owner : t->user.sid) + 8 + sid_size(t->has_pgroup ? t->pgroup : t->user.sid);
        if (capacity >= need) {
            BYTE *p = buf;
            memset(buf, 0, need);
            /* a plain layout of the arrays; the pointers inside point to the copies that follow */
            p += 0x90;
            *(PVOID *)(buf + 0x00) = p;      /* SidHash (reused: the SIDs) */
            st32(p, 1 + t->ngroups); p += 8;
            ((SID_AND_ATTRIBUTES *)p)->Sid = NULL; p += 16 * (1 + t->ngroups);
            (void)restricted; (void)priv; (void)il_bytes;
        }
        return need;
    }
    case TokenLinkedToken: *status = STATUS_NO_TOKEN_; return 0;
    default: *status = STATUS_INVALID_INFO_CLASS_; return 0;
    }
#undef SIMPLE
}

__declspec(dllexport) NTSTATUS NTAPI NtQueryInformationToken(HANDLE token, TOKEN_INFORMATION_CLASS class_, PVOID info, ULONG length, PULONG returned)
{
    TokenData *t;
    BYTE *scratch;
    NTSTATUS status, class_status;
    DWORD need;
    if (class_ <= 0 || class_ >= 45) return STATUS_INVALID_INFO_CLASS_;
    if (!returned) return STATUS_ACCESS_VIOLATION_;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(token, class_ == TokenSource ? TOKEN_QUERY_SOURCE : TOKEN_QUERY, t);
    if (status) { heap_drop(t); return status; }
    scratch = heap_new(TCOS_TOKEN_MAX_BYTES * 2);
    if (!scratch) { heap_drop(t); return STATUS_NO_MEMORY_; }
    need = class_data(t, class_, scratch, TCOS_TOKEN_MAX_BYTES * 2, &class_status);
    heap_drop(t);
    if (class_status) { heap_drop(scratch); return class_status; }
    *returned = need;
    if (length < need) { heap_drop(scratch); return STATUS_BUFFER_TOO_SMALL_; }
    if (!info) { heap_drop(scratch); return STATUS_ACCESS_VIOLATION_; }
    /* the pointers inside were made for the scratch buffer: made again for the caller's */
    {
        TokenData *again = heap_new(sizeof(*again));
        if (again && !read_token(token, class_ == TokenSource ? TOKEN_QUERY_SOURCE : TOKEN_QUERY, again)) class_data(again, class_, info, length, &class_status);
        heap_drop(again);
    }
    heap_drop(scratch);
    return STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI NtSetInformationToken(HANDLE token, TOKEN_INFORMATION_CLASS class_, PVOID info, ULONG length)
{
    TokenData *t;
    NTSTATUS status = STATUS_SUCCESS;
    ACCESS_MASK required = TOKEN_ADJUST_DEFAULT;
    DWORD i;
    switch (class_) {
    case TokenOwner: case TokenPrimaryGroup: if (length < sizeof(PVOID)) return STATUS_INFO_LENGTH_MISMATCH_; break;
    case TokenDefaultDacl: if (length < sizeof(PVOID)) return STATUS_INFO_LENGTH_MISMATCH_; break;
    case TokenSessionId: required = TOKEN_ADJUST_SESSIONID; if (length < 4) return STATUS_INFO_LENGTH_MISMATCH_; break;
    case TokenVirtualizationAllowed: case TokenVirtualizationEnabled: case TokenUIAccess: case TokenMandatoryPolicy: if (length < 4) return STATUS_INFO_LENGTH_MISMATCH_; break;
    case TokenIntegrityLevel: if (length < sizeof(TOKEN_MANDATORY_LABEL)) return STATUS_INFO_LENGTH_MISMATCH_; break;
    case TokenOrigin: if (length < 8) return STATUS_INFO_LENGTH_MISMATCH_; required = TOKEN_ADJUST_DEFAULT; break;
    default: return STATUS_INVALID_INFO_CLASS_;
    }
    if (!info) return STATUS_ACCESS_VIOLATION_;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(token, required, t);
    if (status) { heap_drop(t); return status; }
    switch (class_) {
    case TokenOwner: {
        PSID sid = *(PSID *)info;
        TSid *g;
        if (!sid || !RtlValidSid(sid)) { status = STATUS_INVALID_OWNER_; break; }
        if (!find_sid(t, sid, &g) || (g && !(g->attrs & SE_GROUP_OWNER))) { status = STATUS_INVALID_OWNER_; break; }
        memcpy(t->owner, sid, RtlLengthSid(sid));
        t->has_owner = TRUE;
        break;
    }
    case TokenPrimaryGroup: {
        PSID sid = *(PSID *)info;
        TSid *g;
        if (!sid || !RtlValidSid(sid)) { status = STATUS_INVALID_PRIMARY_GROUP_; break; }
        if (!find_sid(t, sid, &g) || (!g && !RtlEqualSid(sid, (PSID)t->user.sid)) || (!g && FALSE)) { status = STATUS_INVALID_PRIMARY_GROUP_; break; }
        memcpy(t->pgroup, sid, RtlLengthSid(sid));
        t->has_pgroup = TRUE;
        break;
    }
    case TokenDefaultDacl: {
        PACL acl = *(PACL *)info;
        if (!acl) { t->has_dacl = FALSE; break; }
        if (!RtlValidAcl(acl) || acl->AclSize > DEFAULT_DACL_BYTES) { status = STATUS_INVALID_PARAMETER_; break; }
        memcpy(t->dacl, acl, acl->AclSize);
        t->has_dacl = TRUE;
        break;
    }
    case TokenSessionId: t->session = ld32(info); break;
    case TokenVirtualizationAllowed: if (ld32(info)) t->flags |= TCOS_TOKEN_FLAG_VIRT_ALLOWED; else t->flags &= ~TCOS_TOKEN_FLAG_VIRT_ALLOWED; break;
    case TokenVirtualizationEnabled: if (ld32(info)) t->flags |= TCOS_TOKEN_FLAG_VIRT_ENABLED; else t->flags &= ~TCOS_TOKEN_FLAG_VIRT_ENABLED; break;
    case TokenUIAccess: if (ld32(info)) t->flags |= TCOS_TOKEN_FLAG_UIACCESS; else t->flags &= ~TCOS_TOKEN_FLAG_UIACCESS; break;
    case TokenMandatoryPolicy: if (ld32(info) & ~3u) status = STATUS_INVALID_PARAMETER_; else t->policy = ld32(info); break;
    case TokenOrigin: t->origin = (ULONGLONG)ld32(info) | (ULONGLONG)ld32((BYTE *)info + 4) << 32; break;
    case TokenIntegrityLevel: {
        TOKEN_MANDATORY_LABEL *m = info;
        TSid *g = (TSid *)integrity_group(t);
        BYTE *sid = m->Label.Sid;
        if (!sid || !RtlValidSid(sid) || sid[1] != 1 || sid[7] != 16) { status = STATUS_INVALID_PARAMETER_; break; }
        if (m->Label.Attributes != SE_GROUP_INTEGRITY) { status = STATUS_INVALID_PARAMETER_; break; }
        if (g && ld32(sid + 8) > ld32(g->sid + 8)) { status = STATUS_PRIVILEGE_NOT_HELD_; break; }      /* it may only be lowered */
        if (!g) { if (t->ngroups >= TCOS_TOKEN_MAX_GROUPS) { status = STATUS_NO_MEMORY_; break; } g = &t->groups[t->ngroups++]; }
        memcpy(g->sid, sid, RtlLengthSid(sid));
        g->attrs = SE_GROUP_INTEGRITY | SE_GROUP_INTEGRITY_ENABLED;
        break;
    }
    default: break;
    }
    (void)i;
    if (!status) status = write_token(token, required, t);
    heap_drop(t);
    return status;
}

/* ---- privileges and groups --------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) NTSTATUS NTAPI NtAdjustPrivilegesToken(HANDLE token, BOOLEAN disable_all, TOKEN_PRIVILEGES *state, ULONG length, TOKEN_PRIVILEGES *previous, PULONG returned)
{
    TokenData *t;
    NTSTATUS status;
    ULONG prev_count = 0, i, j, needed;
    BOOL missing = FALSE;
    LUID_AND_ATTRIBUTES changed[TCOS_TOKEN_MAX_PRIVILEGES];
    (void)length;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(token, previous ? (TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY) : TOKEN_ADJUST_PRIVILEGES, t);
    if (status) { heap_drop(t); return status; }
    if (disable_all) {
        for (j = 0; j < t->npriv; ++j)
            if (t->priv[j].Attributes & SE_PRIVILEGE_ENABLED) { changed[prev_count] = t->priv[j]; prev_count++; t->priv[j].Attributes &= ~SE_PRIVILEGE_ENABLED; }
    } else {
        if (!state) { heap_drop(t); return STATUS_ACCESS_VIOLATION_; }
        for (i = 0; i < state->PrivilegeCount; ++i) {
            for (j = 0; j < t->npriv; ++j) if (t->priv[j].Luid.LowPart == state->Privileges[i].Luid.LowPart && t->priv[j].Luid.HighPart == state->Privileges[i].Luid.HighPart) break;
            if (j == t->npriv) { missing = TRUE; continue; }
            if (prev_count < TCOS_TOKEN_MAX_PRIVILEGES) { changed[prev_count] = t->priv[j]; prev_count++; }
            if (state->Privileges[i].Attributes & SE_PRIVILEGE_REMOVED) { t->priv[j] = t->priv[--t->npriv]; continue; }
            if (state->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) t->priv[j].Attributes |= SE_PRIVILEGE_ENABLED;
            else t->priv[j].Attributes &= ~SE_PRIVILEGE_ENABLED;
        }
    }
    needed = 4 + 12 * prev_count;
    if (returned) *returned = previous ? needed : 0;
    if (previous) {
        if (length < needed) { heap_drop(t); return STATUS_BUFFER_TOO_SMALL_; }
        previous->PrivilegeCount = prev_count;
        for (i = 0; i < prev_count; ++i) previous->Privileges[i] = changed[i];
    }
    status = write_token(token, TOKEN_ADJUST_PRIVILEGES, t);
    heap_drop(t);
    if (status) return status;
    return missing ? STATUS_NOT_ALL_ASSIGNED_ : STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI NtAdjustGroupsToken(HANDLE token, BOOLEAN reset, TOKEN_GROUPS *state, ULONG length, TOKEN_GROUPS *previous, PULONG returned)
{
    TokenData *t;
    NTSTATUS status;
    DWORD i, j;
    BOOL missing = FALSE;
    (void)length; (void)previous;
    if (returned) *returned = 0;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(token, TOKEN_ADJUST_GROUPS, t);
    if (status) { heap_drop(t); return status; }
    if (reset) {
        for (j = 0; j < t->ngroups; ++j) if (!(t->groups[j].attrs & SE_GROUP_USE_FOR_DENY_ONLY)) { if (t->groups[j].attrs & SE_GROUP_ENABLED_BY_DEFAULT) t->groups[j].attrs |= SE_GROUP_ENABLED; else t->groups[j].attrs &= ~SE_GROUP_ENABLED; }
    } else {
        if (!state) { heap_drop(t); return STATUS_ACCESS_VIOLATION_; }
        for (i = 0; i < state->GroupCount; ++i) {
            for (j = 0; j < t->ngroups; ++j) if (RtlEqualSid((PSID)t->groups[j].sid, state->Groups[i].Sid)) break;
            if (j == t->ngroups) { missing = TRUE; continue; }
            if ((t->groups[j].attrs & SE_GROUP_INTEGRITY) || (t->groups[j].attrs & SE_GROUP_LOGON_ID) == SE_GROUP_LOGON_ID || (t->groups[j].attrs & SE_GROUP_USE_FOR_DENY_ONLY)) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
            if (state->Groups[i].Attributes & SE_GROUP_ENABLED) t->groups[j].attrs |= SE_GROUP_ENABLED;
            else {
                if (t->groups[j].attrs & SE_GROUP_MANDATORY) { heap_drop(t); return STATUS_CANT_DISABLE_MANDATORY_; }
                t->groups[j].attrs &= ~SE_GROUP_ENABLED;
            }
        }
    }
    status = write_token(token, TOKEN_ADJUST_GROUPS, t);
    heap_drop(t);
    if (status) return status;
    return missing ? STATUS_NOT_ALL_ASSIGNED_ : STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI NtPrivilegeCheck(HANDLE token, PPRIVILEGE_SET set, PBOOLEAN result)
{
    TokenData *t;
    NTSTATUS status;
    DWORD i, j, enabled = 0;
    if (!set || !result) return STATUS_ACCESS_VIOLATION_;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    status = read_token(token, TOKEN_QUERY, t);
    if (!status && t->type == TokenImpersonation && t->level < SecurityIdentification) status = STATUS_BAD_IMPERSONATION_LEVEL_;
    if (!status) {
        for (i = 0; i < set->PrivilegeCount; ++i) {
            set->Privilege[i].Attributes &= ~SE_PRIVILEGE_USED_FOR_ACCESS;
            for (j = 0; j < t->npriv; ++j)
                if (t->priv[j].Luid.LowPart == set->Privilege[i].Luid.LowPart && t->priv[j].Luid.HighPart == set->Privilege[i].Luid.HighPart && (t->priv[j].Attributes & SE_PRIVILEGE_ENABLED)) {
                    set->Privilege[i].Attributes |= SE_PRIVILEGE_USED_FOR_ACCESS;
                    enabled++;
                    break;
                }
        }
        if (set->Control & PRIVILEGE_SET_ALL_NECESSARY) *result = enabled == set->PrivilegeCount;
        else *result = enabled != 0;
    }
    heap_drop(t);
    return status;
}

__declspec(dllexport) NTSTATUS NTAPI NtAllocateLocallyUniqueId(LUID *luid)
{
    static volatile LONG64 counter = 0x2000;
    ULONGLONG v;
    if (!luid) return STATUS_ACCESS_VIOLATION_;
    v = (ULONGLONG)InterlockedIncrement64(&counter);
    luid->LowPart = (DWORD)v;
    luid->HighPart = (LONG)(v >> 32);
    return STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI NtCreateToken(PHANDLE out, ACCESS_MASK access, OBJECT_ATTRIBUTES *attr, TOKEN_TYPE type, PLUID auth, PLARGE_INTEGER expiry, TOKEN_USER *user,
    TOKEN_GROUPS *groups, TOKEN_PRIVILEGES *privileges, TOKEN_OWNER *owner, TOKEN_PRIMARY_GROUP *pgroup, TOKEN_DEFAULT_DACL *dacl, TOKEN_SOURCE *source)
{
    TokenData *t;
    HANDLE self;
    NTSTATUS status;
    DWORD i, j;
    TokenData *mine;
    BOOL held = FALSE;
    BOOL have_qos, effective;
    DWORD level;
    if (!out || !auth || !user || !groups || !privileges || !pgroup || !source) return STATUS_ACCESS_VIOLATION_;
    if (type != TokenPrimary && type != TokenImpersonation) return STATUS_INVALID_PARAMETER_;
    /* only a caller with the privilege to create tokens may */
    if (!(mine = heap_new(sizeof(*mine)))) return STATUS_NO_MEMORY_;
    status = NtOpenProcessToken(NtCurrentProcess(), TOKEN_QUERY, &self);
    if (!status) { status = read_token(self, TOKEN_QUERY, mine); NtClose(self); }
    if (!status) for (j = 0; j < mine->npriv; ++j) if (mine->priv[j].Luid.LowPart == SE_CREATE_TOKEN_PRIVILEGE_VALUE_ && (mine->priv[j].Attributes & SE_PRIVILEGE_ENABLED)) held = TRUE;
    heap_drop(mine);
    if (status) return status;
    if (!held) return STATUS_PRIVILEGE_NOT_HELD_;
    if (!(t = heap_new(sizeof(*t)))) return STATUS_NO_MEMORY_;
    qos_of(attr, &have_qos, &level, &effective);
    t->type = type;
    t->level = type == TokenImpersonation ? (have_qos ? level : SecurityImpersonation) : 0;
    t->token_id = next_luid();
    t->auth_id = (ULONGLONG)auth->LowPart | (ULONGLONG)(DWORD)auth->HighPart << 32;
    t->modified_id = t->token_id;
    t->expiration = expiry ? (ULONGLONG)expiry->QuadPart : 0x7FFFFFFFFFFFFFFFull;
    t->elevation_type = 1;
    memcpy(t->source_name, source->SourceName, 8);
    t->source_id = (ULONGLONG)source->SourceIdentifier.LowPart | (ULONGLONG)(DWORD)source->SourceIdentifier.HighPart << 32;
    if (!user->User.Sid || !RtlValidSid(user->User.Sid)) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
    memcpy(t->user.sid, user->User.Sid, RtlLengthSid(user->User.Sid));
    t->user.attrs = user->User.Attributes;
    if (groups->GroupCount > TCOS_TOKEN_MAX_GROUPS || privileges->PrivilegeCount > TCOS_TOKEN_MAX_PRIVILEGES) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
    for (i = 0; i < groups->GroupCount; ++i) {
        if (!RtlValidSid(groups->Groups[i].Sid)) { heap_drop(t); return STATUS_INVALID_PARAMETER_; }
        memcpy(t->groups[i].sid, groups->Groups[i].Sid, RtlLengthSid(groups->Groups[i].Sid));
        t->groups[i].attrs = groups->Groups[i].Attributes;
    }
    t->ngroups = groups->GroupCount;
    for (i = 0; i < privileges->PrivilegeCount; ++i) t->priv[i] = privileges->Privileges[i];
    t->npriv = privileges->PrivilegeCount;
    if (owner && owner->Owner) { memcpy(t->owner, owner->Owner, RtlLengthSid(owner->Owner)); t->has_owner = TRUE; }
    if (pgroup->PrimaryGroup) { memcpy(t->pgroup, pgroup->PrimaryGroup, RtlLengthSid(pgroup->PrimaryGroup)); t->has_pgroup = TRUE; }
    if (dacl && dacl->DefaultDacl && dacl->DefaultDacl->AclSize <= DEFAULT_DACL_BYTES) { memcpy(t->dacl, dacl->DefaultDacl, dacl->DefaultDacl->AclSize); t->has_dacl = TRUE; }
    status = create_token(t, access ? access : TOKEN_ALL, attr && (attr->Attributes & OBJ_INHERIT), out);
    heap_drop(t);
    return status;
}

/* ---- impersonation ---------------------------------------------------------------------------------------------------------------------------------- */


/* ---- the access check ------------------------------------------------------------------------------------------------------------------------------- */

typedef struct {
    ULONGLONG descriptor, descriptor_length, token, desired, mapping, granted, status, privileges;
} CheckArgs;

static NTSTATUS check_internal(PSECURITY_DESCRIPTOR sd, HANDLE token, ACCESS_MASK desired, PGENERIC_MAPPING mapping, PPRIVILEGE_SET privs, PULONG privs_length,
    PACCESS_MASK granted, PNTSTATUS access_status, BOOL require_impersonation)
{
    BYTE *rel = NULL;
    ULONG length;
    NTSTATUS status;
    CheckArgs args;
    ULONGLONG used = 0;
    DWORD out_status = 0, out_granted = 0;
    DWORD privilege_count = 0, i;
    if (!sd || !mapping || !privs_length || !granted || !access_status) return STATUS_ACCESS_VIOLATION_;
    if (desired & (GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE | GENERIC_ALL)) {      /* (a program maps the generic rights itself; with no buffer for the privileges the buffer is what is wrong first) */
        if (!privs && *privs_length >= sizeof(PRIVILEGE_SET)) return STATUS_ACCESS_VIOLATION_;
        return STATUS_GENERIC_NOT_MAPPED_;
    }
    if (!privs) {      /* (a length of zero is a question about the size, any other length with no buffer a fault) */
        if (*privs_length != 0) return STATUS_ACCESS_VIOLATION_;
        *privs_length = sizeof(PRIVILEGE_SET);
        return STATUS_BUFFER_TOO_SMALL_;
    }
    if (*privs_length < sizeof(PRIVILEGE_SET)) { *privs_length = sizeof(PRIVILEGE_SET); return STATUS_BUFFER_TOO_SMALL_; }
    if (((SECURITY_DESCRIPTOR *)sd)->Revision != 1) return STATUS_UNKNOWN_REVISION;
    if (!((SECURITY_DESCRIPTOR *)sd)->Owner && !(((SECURITY_DESCRIPTOR *)sd)->Control & SE_SELF_RELATIVE)) return STATUS_INVALID_SECURITY_DESCR_;
    length = RtlLengthSecurityDescriptor(sd);
    rel = heap_new(length + 32);
    if (!rel) return STATUS_NO_MEMORY_;
    length += 32;
    status = RtlMakeSelfRelativeSD(sd, rel, &length);
    if (status) { heap_drop(rel); return status; }
    memset(&args, 0, sizeof(args));
    args.descriptor = (ULONGLONG)(ULONG_PTR)rel;
    args.descriptor_length = length;
    args.token = (ULONGLONG)(ULONG_PTR)token;
    args.desired = desired;
    args.mapping = (ULONGLONG)(ULONG_PTR)mapping;
    args.granted = (ULONGLONG)(ULONG_PTR)&out_granted;
    args.status = (ULONGLONG)(ULONG_PTR)&out_status;
    args.privileges = (ULONGLONG)(ULONG_PTR)&used;
    if (require_impersonation && token) {
        TokenData *t = heap_new(sizeof(*t));
        if (t) {
            NTSTATUS s = read_token(token, TOKEN_QUERY, t);
            BOOL primary = !s && t->type == TokenPrimary;
            heap_drop(t);
            if (s) { heap_drop(rel); return s; }
            if (primary) { heap_drop(rel); return STATUS_NO_IMPERSONATION_TOKEN_; }
        }
    }
    status = NtTcosAccessCheck(&args);
    heap_drop(rel);
    if (status) return status;
    for (i = 0; i < 64; ++i) if (used & ((ULONGLONG)1 << i)) privilege_count++;
    if (privilege_count) {
        ULONG need = sizeof(PRIVILEGE_SET) + (privilege_count - 1) * sizeof(LUID_AND_ATTRIBUTES);
        if (*privs_length < need) { *privs_length = need; return STATUS_BUFFER_TOO_SMALL_; }
        privs->PrivilegeCount = privilege_count;
        privs->Control = 0;
        for (i = privilege_count = 0; i < 64; ++i)
            if (used & ((ULONGLONG)1 << i)) { privs->Privilege[privilege_count].Luid.LowPart = i; privs->Privilege[privilege_count].Luid.HighPart = 0; privs->Privilege[privilege_count].Attributes = SE_PRIVILEGE_USED_FOR_ACCESS; privilege_count++; }
        *privs_length = need;
    } else { privs->PrivilegeCount = 0; privs->Control = 0; }
    *granted = out_granted;
    *access_status = (NTSTATUS)out_status;
    return STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI NtAccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, ACCESS_MASK desired, PGENERIC_MAPPING mapping, PPRIVILEGE_SET privs, PULONG privs_length,
    PACCESS_MASK granted, PNTSTATUS access_status)
{
    return check_internal(sd, token, desired, mapping, privs, privs_length, granted, access_status, TRUE);
}
__declspec(dllexport) NTSTATUS NTAPI NtAccessCheckByType(PSECURITY_DESCRIPTOR sd, PSID principal, HANDLE token, ULONG desired, POBJECT_TYPE_LIST types, ULONG type_count,
    PGENERIC_MAPPING mapping, PPRIVILEGE_SET privs, PULONG privs_length, PACCESS_MASK granted, PNTSTATUS access_status)
{
    (void)principal; (void)types; (void)type_count;
    return check_internal(sd, token, desired, mapping, privs, privs_length, granted, access_status, TRUE);
}
__declspec(dllexport) NTSTATUS NTAPI NtAccessCheckByTypeResultList(PSECURITY_DESCRIPTOR sd, PSID principal, HANDLE token, ULONG desired, POBJECT_TYPE_LIST types, ULONG type_count,
    PGENERIC_MAPPING mapping, PPRIVILEGE_SET privs, PULONG privs_length, PACCESS_MASK granted, PNTSTATUS access_status)
{
    NTSTATUS status = check_internal(sd, token, desired, mapping, privs, privs_length, granted, access_status, TRUE);
    ULONG i;
    (void)principal;
    if (!status && types)
        for (i = 0; i < type_count; ++i) { granted[i] = granted[0]; access_status[i] = access_status[0]; }
    return status;
}

/* ---- security descriptors of objects --------------------------------------------------------------------------------------------------------------- */

static NTSTATUS token_of_caller(HANDLE *token)
{
    NTSTATUS status = NtTcosToken(TCOS_TOKEN_CALLER, (ULONG_PTR)token, 0, 0, 0, 0);
    return status;
}

/* The generic mapping of the type of object a handle names. */
static void mapping_for_handle(HANDLE handle, GENERIC_MAPPING *m)
{
    BYTE buf[256];
    ULONG got = 0;
    const WCHAR *name = NULL;
    USHORT bytes = 0;
    static const struct { const char *name; DWORD read, write, execute, all; } types[] = {
        {"Event", 0x20001, 0x20002, 0x120000, 0x1f0003}, {"Mutant", 0x20001, 0x20000, 0x120000, 0x1f0001}, {"Semaphore", 0x20001, 0x20002, 0x120000, 0x1f0003},
        {"Timer", 0x20001, 0x20002, 0x120000, 0x1f0003}, {"Section", 0x20005, 0x20012, 0x20008, 0xf001f}, {"Key", 0x20019, 0x20006, 0x20019, 0xf003f},
        {"Process", 0x20410, 0x20bea, 0x121001, 0x1fffff}, {"Thread", 0x20048, 0x20037, 0x120000, 0x1fffff}, {"Token", 0x20008 | 0x10, 0x200e0, 0x20000, 0xf01ff},
        {"Job", 0x20004, 0x20003, 0x120000, 0x1f001f}, {"DebugObject", 0x20001, 0x20002, 0x120000, 0x1f000f},
    };
    unsigned i;
    m->GenericRead = 0x120089; m->GenericWrite = 0x120116; m->GenericExecute = 0x1200a0; m->GenericAll = 0x1f01ff;      /* (a file) */
    if (handle == (HANDLE)(LONG_PTR)-1) { m->GenericRead = 0x20410; m->GenericWrite = 0x20bea; m->GenericExecute = 0x121001; m->GenericAll = 0x1fffff; return; }      /* (the pseudo handles of the process and the thread) */
    if (handle == (HANDLE)(LONG_PTR)-2) { m->GenericRead = 0x20048; m->GenericWrite = 0x20037; m->GenericExecute = 0x120000; m->GenericAll = 0x1fffff; return; }
    if (NtQueryObject(handle, 2, buf, sizeof(buf), &got)) return;
    bytes = *(USHORT *)buf;
    name = *(const WCHAR **)(buf + 8);
    if (!name) return;
    for (i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        const char *t = types[i].name;
        USHORT n = 0;
        while (t[n] && n * 2 < bytes && (name[n] | 0x20) == (t[n] | 0x20)) ++n;
        if (!t[n] && n * 2 == bytes) { m->GenericRead = types[i].read; m->GenericWrite = types[i].write; m->GenericExecute = types[i].execute; m->GenericAll = types[i].all; return; }
    }
}

/* The descriptor an object has when none was given to it: owner and group of the creator, and its default DACL (the generic rights mapped for the type). */
static NTSTATUS default_descriptor(HANDLE handle, BYTE **out, ULONG *length)
{
    GENERIC_MAPPING mapping;
    PSECURITY_DESCRIPTOR sd = NULL;
    NTSTATUS status;
    mapping_for_handle(handle, &mapping);
    {
        BOOL process = mapping.GenericAll == 0x1fffff && mapping.GenericRead == 0x20410, thread = mapping.GenericAll == 0x1fffff && mapping.GenericRead == 0x20048, token_object = mapping.GenericAll == 0xf01ff;
        if (process || thread || token_object) {      /* a process, a thread or a token: it carries the integrity of its maker as a mandatory label; a process is open to its administrators and the system */
            BYTE absolute_buffer[SECURITY_DESCRIPTOR_MIN_LENGTH], acl_buffer[256], sacl_buffer[128];
            BYTE admin[16] = {1, 2, 0, 0, 0, 0, 0, 5, 32, 0, 0, 0, 0x20, 2, 0, 0}, system_sid[12] = {1, 1, 0, 0, 0, 0, 0, 5, 18, 0, 0, 0};
            ACL *acl = (ACL *)acl_buffer, *sacl = (ACL *)sacl_buffer;
            HANDLE token;
            TokenData *t = heap_new(sizeof(*t));
            BYTE *pgroup_sid = NULL;
            const TSid *integrity = NULL;
            if (t && !token_of_caller(&token)) {
                if (!read_token(token, TOKEN_QUERY, t)) { if (t->has_pgroup) pgroup_sid = t->pgroup; integrity = integrity_group(t); }
                NtClose(token);
            }
            RtlCreateSecurityDescriptor(absolute_buffer, 1);
            if (process) {
                RtlCreateAcl(acl, sizeof(acl_buffer), ACL_REVISION);
                RtlAddAccessAllowedAceEx(acl, ACL_REVISION, 0, 0x1fffff, admin);
                RtlAddAccessAllowedAceEx(acl, ACL_REVISION, 0, 0x1fffff, system_sid);
                if (pgroup_sid) RtlAddAccessAllowedAceEx(acl, ACL_REVISION, CONTAINER_INHERIT_ACE | INHERIT_ONLY_ACE, GENERIC_READ, pgroup_sid);
                RtlSetDaclSecurityDescriptor(absolute_buffer, TRUE, acl, FALSE);
            }
            if (integrity) {
                RtlCreateAcl(sacl, sizeof(sacl_buffer), ACL_REVISION_DS);
                RtlAddMandatoryAce(sacl, ACL_REVISION_DS, 0, SYSTEM_MANDATORY_LABEL_NO_WRITE_UP, SYSTEM_MANDATORY_LABEL_ACE_TYPE, (PSID)integrity->sid);
                RtlSetSaclSecurityDescriptor(absolute_buffer, TRUE, sacl, FALSE);
            }
            status = RtlNewSecurityObjectEx(NULL, absolute_buffer, &sd, NULL, FALSE, 0, NULL, &mapping);
            heap_drop(t);
        } else
        status = RtlNewSecurityObjectEx(NULL, NULL, &sd, NULL, FALSE, 0, NULL, &mapping);
    }
    if (status) return status;
    *length = RtlLengthSecurityDescriptor(sd);
    *out = sd;
    return STATUS_SUCCESS;
}

/* The descriptor kept for the object, or the default one. The result is on the heap. */
static NTSTATUS current_descriptor(HANDLE handle, ACCESS_MASK required, BYTE **out, ULONG *length)
{
    ULONG needed = 0;
    BYTE *buf;
    NTSTATUS status = NtTcosToken(TCOS_OBJECT_SD_GET, (ULONG_PTR)handle, required, 0, 0, (ULONG_PTR)&needed);
    if (status && status != STATUS_BUFFER_TOO_SMALL_) return status;
    if (needed == 0) return default_descriptor(handle, out, length);
    buf = heap_new(needed);
    if (!buf) return STATUS_NO_MEMORY_;
    status = NtTcosToken(TCOS_OBJECT_SD_GET, (ULONG_PTR)handle, required, (ULONG_PTR)buf, needed, (ULONG_PTR)&needed);
    if (status) { heap_drop(buf); return status; }
    *out = buf;
    *length = needed;
    return STATUS_SUCCESS;
}

static ACCESS_MASK query_rights(SECURITY_INFORMATION info)
{
    ACCESS_MASK m = 0;
    if (info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION | BACKUP_SECURITY_INFORMATION)) m |= READ_CONTROL;
    if (info & SACL_SECURITY_INFORMATION) m |= ACCESS_SYSTEM_SECURITY;
    return m;
}
static ACCESS_MASK set_rights(SECURITY_INFORMATION info)
{
    ACCESS_MASK m = 0;
    if (info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION)) m |= WRITE_OWNER;
    if (info & (DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION)) m |= WRITE_DAC;
    if (info & (SACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION | UNPROTECTED_SACL_SECURITY_INFORMATION)) m |= ACCESS_SYSTEM_SECURITY;
    return m;
}

/* The parts of a self-relative descriptor that `info` asks for, as a new self-relative descriptor in `out` (room: `capacity`); returns the size. */
static ULONG filter_descriptor(const BYTE *rel, SECURITY_INFORMATION info, BYTE *out, ULONG capacity)
{
    const SECURITY_DESCRIPTOR_RELATIVE *s = (const SECURITY_DESCRIPTOR_RELATIVE *)rel;
    SECURITY_DESCRIPTOR_RELATIVE *o = (SECURITY_DESCRIPTOR_RELATIVE *)out;
    ULONG at = sizeof(*o), n;
    WORD control = SE_SELF_RELATIVE;
    if (capacity < at) return 0;
    memset(out, 0, at);
    o->Revision = 1;
    o->Sbz1 = s->Sbz1;
    if ((info & OWNER_SECURITY_INFORMATION) && s->Owner) { n = RtlLengthSid((PSID)(rel + s->Owner)); if (at + n > capacity) return 0; memcpy(out + at, rel + s->Owner, n); o->Owner = at; at += n; }
    if (info & OWNER_SECURITY_INFORMATION) control |= s->Control & SE_OWNER_DEFAULTED;
    if ((info & GROUP_SECURITY_INFORMATION) && s->Group) { n = RtlLengthSid((PSID)(rel + s->Group)); if (at + n > capacity) return 0; memcpy(out + at, rel + s->Group, n); o->Group = at; at += n; }
    if (info & GROUP_SECURITY_INFORMATION) control |= s->Control & SE_GROUP_DEFAULTED;
    if (info & DACL_SECURITY_INFORMATION) {
        control |= s->Control & (SE_DACL_PRESENT | SE_DACL_DEFAULTED | SE_DACL_PROTECTED | SE_DACL_AUTO_INHERITED | SE_DACL_AUTO_INHERIT_REQ);
        if ((s->Control & SE_DACL_PRESENT) && s->Dacl) { n = ((ACL *)(rel + s->Dacl))->AclSize; if (at + n > capacity) return 0; memcpy(out + at, rel + s->Dacl, n); o->Dacl = at; at += n; }
    }
    if (info & SACL_SECURITY_INFORMATION) {
        control |= s->Control & (SE_SACL_PRESENT | SE_SACL_DEFAULTED | SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED | SE_SACL_AUTO_INHERIT_REQ);
        if ((s->Control & SE_SACL_PRESENT) && s->Sacl) { n = ((ACL *)(rel + s->Sacl))->AclSize; if (at + n > capacity) return 0; memcpy(out + at, rel + s->Sacl, n); o->Sacl = at; at += n; }
    }
    if ((info & LABEL_SECURITY_INFORMATION) && !(info & SACL_SECURITY_INFORMATION) && (s->Control & SE_SACL_PRESENT) && s->Sacl) {      /* the mandatory label lives in the SACL: an ACL of it alone */
        ACL *acl = (ACL *)(out + at);
        DWORD kept = 0, count = 0, i;
        if (at + sizeof(ACL) > capacity) return 0;
        memset(acl, 0, sizeof(ACL));
        acl->AclRevision = ACL_REVISION_DS;
        if ((s->Control & SE_SACL_PRESENT) && s->Sacl) {
            const ACL *source = (const ACL *)(rel + s->Sacl);
            const ACE_HEADER *ace = (const ACE_HEADER *)(source + 1);
            for (i = 0; i < source->AceCount; ++i, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize))
                if (ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) {
                    if (at + sizeof(ACL) + kept + ace->AceSize > capacity) return 0;
                    memcpy((BYTE *)(acl + 1) + kept, ace, ace->AceSize);
                    kept += ace->AceSize;
                    count++;
                }
        }
        acl->AceCount = (WORD)count;
        acl->AclSize = (WORD)(sizeof(ACL) + kept);
        o->Sacl = at;
        at += acl->AclSize;
        control |= SE_SACL_PRESENT | (s->Control & (SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED));
    }
    o->Control = control;
    return at;
}

__declspec(dllexport) NTSTATUS NTAPI RtlQuerySecurityObject(PSECURITY_DESCRIPTOR object, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR result, ULONG length, PULONG needed)
{
    BYTE *rel = NULL, *tmp;
    ULONG rel_length = 0, size;
    NTSTATUS status = STATUS_SUCCESS;
    if (!object || !needed) return STATUS_ACCESS_VIOLATION_;
    rel_length = RtlLengthSecurityDescriptor(object) + 32;
    rel = heap_new(rel_length);
    if (!rel) return STATUS_NO_MEMORY_;
    status = RtlMakeSelfRelativeSD(object, rel, &rel_length);
    if (status) { heap_drop(rel); return status; }
    tmp = heap_new(rel_length + 32);
    if (!tmp) { heap_drop(rel); return STATUS_NO_MEMORY_; }
    size = filter_descriptor(rel, info, tmp, rel_length + 32);
    *needed = size;
    if (length < size) status = STATUS_BUFFER_TOO_SMALL_;
    else if (result) memcpy(result, tmp, size);
    heap_drop(tmp);
    heap_drop(rel);
    return status;
}

__declspec(dllexport) NTSTATUS NTAPI NtQuerySecurityObject(HANDLE handle, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR result, ULONG length, PULONG needed)
{
    BYTE *current = NULL, *tmp;
    ULONG current_length = 0, size;
    NTSTATUS status;
    if (!needed) return STATUS_ACCESS_VIOLATION_;
    status = current_descriptor(handle, query_rights(info), &current, &current_length);
    if (status) return status;
    tmp = heap_new(current_length + 32);
    if (!tmp) { heap_drop(current); return STATUS_NO_MEMORY_; }
    size = filter_descriptor(current, info, tmp, current_length + 32);
    *needed = size;
    if (length < size) status = STATUS_BUFFER_TOO_SMALL_;
    else if (result) memcpy(result, tmp, size);
    heap_drop(tmp);
    heap_drop(current);
    return status;
}

/* Applies the parts of `modification` that `info` names to the self-relative descriptor `object`; the new one is on the heap. */
static NTSTATUS merge_descriptor(const BYTE *object, ULONG object_length, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR modification, BYTE **out, ULONG *out_length)
{
    BYTE *mod_rel;
    ULONG mod_length = RtlLengthSecurityDescriptor(modification) + 32, size;
    const SECURITY_DESCRIPTOR_RELATIVE *old = (const SECURITY_DESCRIPTOR_RELATIVE *)object, *m;
    SECURITY_DESCRIPTOR_RELATIVE *n;
    BYTE *buf;
    ULONG at = sizeof(*n), len;
    WORD control;
    NTSTATUS status;
    (void)object_length;
    mod_rel = heap_new(mod_length);
    if (!mod_rel) return STATUS_NO_MEMORY_;
    status = RtlMakeSelfRelativeSD(modification, mod_rel, &mod_length);
    if (status) { heap_drop(mod_rel); return status; }
    m = (const SECURITY_DESCRIPTOR_RELATIVE *)mod_rel;
    size = RtlLengthSecurityDescriptor((PSECURITY_DESCRIPTOR)object) + mod_length + 64;
    buf = heap_new(size);
    if (!buf) { heap_drop(mod_rel); return STATUS_NO_MEMORY_; }
    n = (SECURITY_DESCRIPTOR_RELATIVE *)buf;
    n->Revision = 1;
    control = SE_SELF_RELATIVE;
#define TAKE(part, from, infobit) do { const SECURITY_DESCRIPTOR_RELATIVE *src_ = (info & infobit) ? m : old; const BYTE *base_ = (info & infobit) ? mod_rel : object; (void)from; \
    if (src_->part) { len = RtlLengthSid((PSID)(base_ + src_->part)); memcpy(buf + at, base_ + src_->part, len); n->part = at; at += len; } } while (0)
    TAKE(Owner, 0, OWNER_SECURITY_INFORMATION);
    TAKE(Group, 0, GROUP_SECURITY_INFORMATION);
    {
        BOOL from_new = (info & DACL_SECURITY_INFORMATION) != 0;
        const SECURITY_DESCRIPTOR_RELATIVE *src = from_new ? m : old;
        const BYTE *base = from_new ? mod_rel : object;
        if (src->Dacl) { len = ((ACL *)(base + src->Dacl))->AclSize; memcpy(buf + at, base + src->Dacl, len); n->Dacl = at; at += len; }
        control |= src->Control & (SE_DACL_PRESENT | SE_DACL_DEFAULTED | SE_DACL_AUTO_INHERITED | SE_DACL_AUTO_INHERIT_REQ);
        if (from_new) { if (m->Control & SE_DACL_PROTECTED || (info & PROTECTED_DACL_SECURITY_INFORMATION)) control |= SE_DACL_PROTECTED; if (info & UNPROTECTED_DACL_SECURITY_INFORMATION) control &= ~SE_DACL_PROTECTED; }
        else control |= old->Control & SE_DACL_PROTECTED;
        if (from_new && (info & (PROTECTED_DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION)) && !(info & DACL_SECURITY_INFORMATION)) {}
    }
    if ((info & LABEL_SECURITY_INFORMATION) && !(info & SACL_SECURITY_INFORMATION)) {      /* only the mandatory label changes: the other ACEs of the SACL stay */
        ACL *acl = (ACL *)(buf + at);
        DWORD kept = 0, count = 0, i;
        const ACL *lists[2] = { (old->Control & SE_SACL_PRESENT) && old->Sacl ? (const ACL *)(object + old->Sacl) : NULL, (m->Control & SE_SACL_PRESENT) && m->Sacl ? (const ACL *)(mod_rel + m->Sacl) : NULL };
        int which;
        memset(acl, 0, sizeof(ACL));
        acl->AclRevision = ACL_REVISION_DS;
        for (which = 0; which < 2; ++which) {
            const ACL *source = lists[which];
            const ACE_HEADER *ace;
            if (!source) continue;
            ace = (const ACE_HEADER *)(source + 1);
            for (i = 0; i < source->AceCount; ++i, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize))
                if ((ace->AceType == SYSTEM_MANDATORY_LABEL_ACE_TYPE) == (which == 1)) { memcpy((BYTE *)(acl + 1) + kept, ace, ace->AceSize); kept += ace->AceSize; count++; }
        }
        acl->AceCount = (WORD)count;
        acl->AclSize = (WORD)(sizeof(ACL) + kept);
        n->Sacl = at;
        at += acl->AclSize;
        control |= SE_SACL_PRESENT | (old->Control & (SE_SACL_PROTECTED | SE_SACL_AUTO_INHERITED));
    } else {
        BOOL from_new = (info & SACL_SECURITY_INFORMATION) != 0;
        const SECURITY_DESCRIPTOR_RELATIVE *src = from_new ? m : old;
        const BYTE *base = from_new ? mod_rel : object;
        if (src->Sacl && (src->Control & SE_SACL_PRESENT)) { len = ((ACL *)(base + src->Sacl))->AclSize; memcpy(buf + at, base + src->Sacl, len); n->Sacl = at; at += len; }
        control |= src->Control & (SE_SACL_PRESENT | SE_SACL_DEFAULTED | SE_SACL_AUTO_INHERITED | SE_SACL_AUTO_INHERIT_REQ);
        if (from_new) { if (m->Control & SE_SACL_PROTECTED || (info & PROTECTED_SACL_SECURITY_INFORMATION)) control |= SE_SACL_PROTECTED; if (info & UNPROTECTED_SACL_SECURITY_INFORMATION) control &= ~SE_SACL_PROTECTED; }
        else control |= old->Control & SE_SACL_PROTECTED;
    }
    control |= (info & OWNER_SECURITY_INFORMATION) ? (m->Control & SE_OWNER_DEFAULTED) : (old->Control & SE_OWNER_DEFAULTED);
    control |= (info & GROUP_SECURITY_INFORMATION) ? (m->Control & SE_GROUP_DEFAULTED) : (old->Control & SE_GROUP_DEFAULTED);
#undef TAKE
    n->Control = control;
    heap_drop(mod_rel);
    *out = buf;
    *out_length = at;
    return STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI RtlSetSecurityObject(SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR modification, PSECURITY_DESCRIPTOR *object, PGENERIC_MAPPING mapping, HANDLE token)
{
    BYTE *rel = NULL, *merged = NULL;
    ULONG rel_length = RtlLengthSecurityDescriptor(*object) + 32, merged_length = 0;
    NTSTATUS status;
    (void)mapping; (void)token;
    if (!object || !*object || !modification) return STATUS_ACCESS_VIOLATION_;
    rel = heap_new(rel_length);
    if (!rel) return STATUS_NO_MEMORY_;
    status = RtlMakeSelfRelativeSD(*object, rel, &rel_length);
    if (!status) status = merge_descriptor(rel, rel_length, info, modification, &merged, &merged_length);
    heap_drop(rel);
    if (status) return status;
    heap_drop(*object);
    *object = merged;
    return STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI NtSetSecurityObject(HANDLE handle, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd)
{
    BYTE *current = NULL, *merged = NULL;
    ULONG current_length = 0, merged_length = 0;
    NTSTATUS status;
    ACCESS_MASK required = set_rights(info);
    if (!sd) return STATUS_ACCESS_VIOLATION_;
    if (((SECURITY_DESCRIPTOR *)sd)->Revision != 1) return STATUS_UNKNOWN_REVISION;
    if (!RtlValidSecurityDescriptor(sd)) return STATUS_INVALID_SECURITY_DESCR_;
    {
        SECURITY_DESCRIPTOR_RELATIVE *rel = sd;
        BOOL relative = (rel->Control & SE_SELF_RELATIVE) != 0;
        BOOL has_owner = relative ? rel->Owner != 0 : ((SECURITY_DESCRIPTOR *)sd)->Owner != NULL, has_group = relative ? rel->Group != 0 : ((SECURITY_DESCRIPTOR *)sd)->Group != NULL;
        if (((info & OWNER_SECURITY_INFORMATION) && !has_owner) || ((info & GROUP_SECURITY_INFORMATION) && !has_group)) return STATUS_INVALID_SECURITY_DESCR_;
    }
    status = current_descriptor(handle, required, &current, &current_length);
    if (status) return status;
    status = merge_descriptor(current, current_length, info, sd, &merged, &merged_length);
    heap_drop(current);
    if (status) return status;
    status = NtTcosToken(TCOS_OBJECT_SD_SET, (ULONG_PTR)handle, required, (ULONG_PTR)merged, merged_length, 0);
    heap_drop(merged);
    return status;
}

/* ---- the descriptor of a new object -------------------------------------------------------------------------------------------------------------------- */

typedef struct { BYTE *base; DWORD capacity, used; } AclOut;

static BOOL acl_append(AclOut *o, const void *ace, DWORD size)
{
    if (o->used + size > o->capacity) return FALSE;
    memcpy(o->base + o->used, ace, size);
    o->used += size;
    return TRUE;
}
static BOOL sid_is_creator(const BYTE *sid, DWORD rid) { return sid[1] == 1 && sid[7] == 3 && ld32(sid + 8) == rid; }      /* S-1-3-<rid> */

/* The ACEs of `parent` that a child inherits are copied (with the mapping of generic rights and the owner and group of the creator for CREATOR OWNER / CREATOR GROUP) into out. */
static NTSTATUS inherit_aces(const ACL *parent, BOOL container, ULONG auto_flags, BOOL is_dacl, const GENERIC_MAPPING *mapping, const BYTE *owner, const BYTE *group, AclOut *out, DWORD *count)
{
    const ACE_HEADER *ace = (const ACE_HEADER *)(parent + 1);
    DWORD i;
    for (i = 0; i < parent->AceCount; ++i, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize)) {
        BYTE flags = ace->AceFlags, new_flags;
        BYTE tmp[256 + 64];
        DWORD size = ace->AceSize;
        BOOL effective = FALSE, propagate;
        if (ace->AceType > 0x10 && ace->AceType != 0x11 && ace->AceType != 0x12 && ace->AceType != 0x13) continue;
        if (size > sizeof(tmp) || size < 8) continue;
        if (container) {
            if (!(flags & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE))) continue;
            effective = (flags & CONTAINER_INHERIT_ACE) != 0;
            propagate = !(flags & NO_PROPAGATE_INHERIT_ACE) && (flags & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE));
            if (!effective && !propagate) continue;
            if (effective && !(flags & INHERIT_ONLY_ACE)) {}
            new_flags = flags & ~(INHERIT_ONLY_ACE);
            if (flags & NO_PROPAGATE_INHERIT_ACE) new_flags &= ~(CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE);
            else if (!effective) new_flags |= INHERIT_ONLY_ACE;
            /* an ACE that applies to the container only through its children (object inherit alone) stays inherit-only */
        } else {
            if (!(flags & OBJECT_INHERIT_ACE)) continue;
            new_flags = flags & ~(CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE | NO_PROPAGATE_INHERIT_ACE | INHERIT_ONLY_ACE);
        }
        if (auto_flags) new_flags |= INHERITED_ACE; else new_flags &= ~INHERITED_ACE;
        memcpy(tmp, ace, size);
        ((ACE_HEADER *)tmp)->AceFlags = new_flags;
        if (is_dacl && ace->AceType <= 3) {
            DWORD *mask = (DWORD *)(tmp + 4);
            BYTE *sid = tmp + 8;
            if (!(new_flags & INHERIT_ONLY_ACE)) RtlMapGenericMask(mask, (PGENERIC_MAPPING)mapping);
            if (sid_is_creator(sid, 0) || sid_is_creator(sid, 1)) {      /* CREATOR OWNER / CREATOR GROUP: the ACE for the creator's own is added for the actual SID too */
                const BYTE *real = sid_is_creator(sid, 0) ? owner : group;
                BYTE copy[256 + 64];
                DWORD copy_size = 8 + sid_size(real);
                if (!(new_flags & INHERIT_ONLY_ACE) || container) {
                    if (!(new_flags & INHERIT_ONLY_ACE)) {
                        memcpy(copy, tmp, 8);
                        memcpy(copy + 8, real, sid_size(real));
                        ((ACE_HEADER *)copy)->AceSize = (WORD)copy_size;
                        ((ACE_HEADER *)copy)->AceFlags = new_flags;
                        if (!acl_append(out, copy, copy_size)) return STATUS_BUFFER_TOO_SMALL_;
                        (*count)++;
                        continue;
                    }
                    if ((new_flags & (CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE)) && !(flags & NO_PROPAGATE_INHERIT_ACE)) {
                        /* the effective copy for the container, and the generic one for its children */
                        memcpy(copy, tmp, 8);
                        memcpy(copy + 8, real, sid_size(real));
                        ((ACE_HEADER *)copy)->AceSize = (WORD)copy_size;
                        ((ACE_HEADER *)copy)->AceFlags = new_flags & ~(INHERIT_ONLY_ACE | CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE);
                        if (effective && !acl_append(out, copy, copy_size)) return STATUS_BUFFER_TOO_SMALL_;
                        if (effective) (*count)++;
                    }
                }
            }
        }
        if (!acl_append(out, tmp, ((ACE_HEADER *)tmp)->AceSize)) return STATUS_BUFFER_TOO_SMALL_;
        (*count)++;
    }
    return STATUS_SUCCESS;
}

__declspec(dllexport) NTSTATUS NTAPI RtlNewSecurityObjectEx(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *out, GUID *type, BOOLEAN container,
    ULONG flags, HANDLE token, PGENERIC_MAPPING mapping)
{
    BYTE *parent_rel = NULL, *creator_rel = NULL, *result, *dacl_buf = NULL, *sacl_buf = NULL;
    ULONG length;
    const SECURITY_DESCRIPTOR_RELATIVE *p = NULL, *c = NULL;
    TokenData *t = NULL;
    HANDLE own_token = NULL;
    const BYTE *owner, *group;
    WORD control = SE_SELF_RELATIVE;
    NTSTATUS status = STATUS_SUCCESS;
    DWORD at, n, dacl_count = 0, sacl_count = 0;
    AclOut dacl_out, sacl_out;
    BOOL have_dacl = FALSE, have_sacl = FALSE, dacl_inherited = FALSE;
    (void)type;
    if (!out) return STATUS_ACCESS_VIOLATION_;
    if (parent) {
        length = RtlLengthSecurityDescriptor(parent) + 32;
        parent_rel = heap_new(length);
        if (!parent_rel) return STATUS_NO_MEMORY_;
        if ((status = RtlMakeSelfRelativeSD(parent, parent_rel, &length))) { heap_drop(parent_rel); return status; }
        p = (const SECURITY_DESCRIPTOR_RELATIVE *)parent_rel;
    }
    if (creator) {
        length = RtlLengthSecurityDescriptor(creator) + 32;
        creator_rel = heap_new(length);
        if (!creator_rel) { heap_drop(parent_rel); return STATUS_NO_MEMORY_; }
        if ((status = RtlMakeSelfRelativeSD(creator, creator_rel, &length))) { heap_drop(parent_rel); heap_drop(creator_rel); return status; }
        c = (const SECURITY_DESCRIPTOR_RELATIVE *)creator_rel;
    }
    if (!token) { status = token_of_caller(&own_token); if (status) { heap_drop(parent_rel); heap_drop(creator_rel); return status; } token = own_token; }
    t = heap_new(sizeof(*t));
    if (!t) { status = STATUS_NO_MEMORY_; goto done; }
    if ((status = read_token(token, TOKEN_QUERY, t))) goto done;
    owner = (c && c->Owner && !(flags & 0) ) ? (const BYTE *)c + c->Owner : (t->has_owner ? t->owner : t->user.sid);
    group = (c && c->Group) ? (const BYTE *)c + c->Group : (t->has_pgroup ? t->pgroup : t->user.sid);
    {
        /* room for the ACEs of the creator, the parent (each can pass on twice: once as effective, once to pass on) and the token, and an ACL holds 64 KB at most */
        ULONG room = 1024 + 2 * ((creator_rel ? RtlLengthSecurityDescriptor(creator_rel) : 0) + (parent_rel ? RtlLengthSecurityDescriptor(parent_rel) : 0)) + DEFAULT_DACL_BYTES * 2;
        if (room > 65536) room = 65536;
        dacl_buf = heap_new(room + 8);
        sacl_buf = heap_new(room + 8);
        result = heap_new(sizeof(SECURITY_DESCRIPTOR_RELATIVE) + 2 * MAX_SID_BYTES + 2 * (room + 8));
        if (!dacl_buf || !sacl_buf || !result) { heap_drop(result); status = STATUS_NO_MEMORY_; goto done; }
        dacl_out.base = dacl_buf + 8; dacl_out.capacity = room; dacl_out.used = 0;
        sacl_out.base = sacl_buf + 8; sacl_out.capacity = room; sacl_out.used = 0;
    }
    /* the DACL: the creator's if it gave one (the inheritable ACEs of the parent follow when auto-inheriting), else what the parent passes on, else the default of the token */
    if (c && (c->Control & SE_DACL_PRESENT) && !(c->Control & SE_DACL_DEFAULTED)) {
        have_dacl = TRUE;
        if (c->Dacl) {
            const ACL *acl = (const ACL *)((const BYTE *)c + c->Dacl);
            const ACE_HEADER *ace = (const ACE_HEADER *)(acl + 1);
            DWORD i;
            for (i = 0; i < acl->AceCount; ++i, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize)) {
                BYTE copy[512];
                if (ace->AceSize > sizeof(copy)) continue;
                memcpy(copy, ace, ace->AceSize);
                if (!(flags & SEF_DACL_AUTO_INHERIT) && FALSE) ((ACE_HEADER *)copy)->AceFlags &= ~INHERITED_ACE;
                if ((flags & SEF_DACL_AUTO_INHERIT) && (ace->AceFlags & INHERITED_ACE)) continue;      /* an inherited ACE is made anew from the parent */
                if (!(ace->AceFlags & INHERIT_ONLY_ACE) && ace->AceType <= 3) { DWORD m = ((ACCESS_ALLOWED_ACE *)copy)->Mask; RtlMapGenericMask(&m, mapping); ((ACCESS_ALLOWED_ACE *)copy)->Mask = m; }
                acl_append(&dacl_out, copy, ace->AceSize);
                dacl_count++;
            }
        }
        if ((flags & SEF_DACL_AUTO_INHERIT) && p && (p->Control & SE_DACL_PRESENT) && p->Dacl && !(c->Control & SE_DACL_PROTECTED)) {
            DWORD before = dacl_count;
            inherit_aces((const ACL *)(parent_rel + p->Dacl), container, flags & SEF_DACL_AUTO_INHERIT, TRUE, mapping, owner, group, &dacl_out, &dacl_count);
            dacl_inherited = dacl_count != before;
        }
        if (c->Dacl == 0) { have_dacl = TRUE; dacl_count = 0; dacl_out.used = 0; }
    } else {
        if (p && (p->Control & SE_DACL_PRESENT) && p->Dacl) {
            DWORD before = dacl_count;
            inherit_aces((const ACL *)(parent_rel + p->Dacl), container, flags & SEF_DACL_AUTO_INHERIT, TRUE, mapping, owner, group, &dacl_out, &dacl_count);
            if (dacl_count != before) { have_dacl = TRUE; dacl_inherited = TRUE; }
        }
        if (!have_dacl && t->has_dacl) {
            const ACL *acl = (const ACL *)t->dacl;
            const ACE_HEADER *ace = (const ACE_HEADER *)(acl + 1);
            DWORD i;
            for (i = 0; i < acl->AceCount; ++i, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize)) {
                BYTE copy[512];
                if (ace->AceSize > sizeof(copy)) continue;
                memcpy(copy, ace, ace->AceSize);
                if (ace->AceType <= 3) { DWORD m = ((ACCESS_ALLOWED_ACE *)copy)->Mask; RtlMapGenericMask(&m, mapping); ((ACCESS_ALLOWED_ACE *)copy)->Mask = m; }
                acl_append(&dacl_out, copy, ace->AceSize);
                dacl_count++;
            }
            have_dacl = TRUE;
        }
    }
    /* the SACL: the creator's, then what the parent passes on */
    if (c && (c->Control & SE_SACL_PRESENT) && c->Sacl) {
        const ACL *acl = (const ACL *)((const BYTE *)c + c->Sacl);
        const ACE_HEADER *ace = (const ACE_HEADER *)(acl + 1);
        DWORD i;
        have_sacl = TRUE;
        for (i = 0; i < acl->AceCount; ++i, ace = (const ACE_HEADER *)((const BYTE *)ace + ace->AceSize)) {
            if ((flags & SEF_SACL_AUTO_INHERIT) && (ace->AceFlags & INHERITED_ACE)) continue;
            acl_append(&sacl_out, ace, ace->AceSize);
            sacl_count++;
        }
    } else if (c && (c->Control & SE_SACL_PRESENT)) have_sacl = TRUE;
    if (p && (p->Control & SE_SACL_PRESENT) && p->Sacl && !(c && (c->Control & SE_SACL_PROTECTED))) {
        DWORD before = sacl_count;
        inherit_aces((const ACL *)(parent_rel + p->Sacl), container, flags & SEF_SACL_AUTO_INHERIT, FALSE, mapping, owner, group, &sacl_out, &sacl_count);
        if (sacl_count != before) have_sacl = TRUE;
    }
    {
        SECURITY_DESCRIPTOR_RELATIVE *r = (SECURITY_DESCRIPTOR_RELATIVE *)result;
        r->Revision = 1;
        at = sizeof(*r);
        n = sid_size(owner); memcpy(result + at, owner, n); r->Owner = at; at += n;
        n = sid_size(group); memcpy(result + at, group, n); r->Group = at; at += n;
        if (c && (c->Control & SE_OWNER_DEFAULTED)) control |= 0;
        if (have_dacl) {
            ACL *acl = (ACL *)(result + at);
            memcpy(acl, dacl_buf, 8);
            acl->AclRevision = ACL_REVISION; acl->Sbz1 = 0; acl->AceCount = (WORD)dacl_count; acl->Sbz2 = 0;
            memcpy(acl + 1, dacl_out.base, dacl_out.used);
            acl->AclSize = (WORD)(8 + dacl_out.used);
            r->Dacl = at; at += acl->AclSize;
            control |= SE_DACL_PRESENT;
            if (c && (c->Control & SE_DACL_PROTECTED)) control |= SE_DACL_PROTECTED;
            if (c && (c->Control & SE_DACL_AUTO_INHERIT_REQ)) {}
            if ((flags & SEF_DACL_AUTO_INHERIT) && (dacl_inherited || !c)) control |= SE_DACL_AUTO_INHERITED;
            else if (flags & SEF_DACL_AUTO_INHERIT) control |= SE_DACL_AUTO_INHERITED;
        }
        if (have_sacl) {
            ACL *acl = (ACL *)(result + at);
            memset(acl, 0, 8);
            acl->AclRevision = ACL_REVISION; acl->AceCount = (WORD)sacl_count;
            memcpy(acl + 1, sacl_out.base, sacl_out.used);
            acl->AclSize = (WORD)(8 + sacl_out.used);
            r->Sacl = at; at += acl->AclSize;
            control |= SE_SACL_PRESENT;
            if (c && (c->Control & SE_SACL_PROTECTED)) control |= SE_SACL_PROTECTED;
            if (flags & SEF_SACL_AUTO_INHERIT) control |= SE_SACL_AUTO_INHERITED;
        }
        r->Control = control;
        {
            BYTE *exact = RtlAllocateHeap(RtlProcessHeap(), 0, at);
            if (!exact) status = STATUS_NO_MEMORY_;
            else { memcpy(exact, result, at); *out = exact; }
        }
    }
    heap_drop(result);
done:
    heap_drop(t); heap_drop(dacl_buf); heap_drop(sacl_buf); heap_drop(parent_rel); heap_drop(creator_rel);
    if (own_token) NtClose(own_token);
    return status;
}

/* The Zw names of the services above: the same calls. */
#define ZW_ALIAS(zw, nt) __asm__(".text\n.globl " #zw "\n" #zw ":\n jmp " #nt "\n.section .drectve\n.ascii \" -export:" #zw "\"\n.text\n")
ZW_ALIAS(ZwOpenProcessTokenEx, NtOpenProcessTokenEx);
ZW_ALIAS(ZwOpenProcessToken, NtOpenProcessToken);
ZW_ALIAS(ZwOpenThreadTokenEx, NtOpenThreadTokenEx);
ZW_ALIAS(ZwOpenThreadToken, NtOpenThreadToken);
ZW_ALIAS(ZwDuplicateToken, NtDuplicateToken);
ZW_ALIAS(ZwFilterToken, NtFilterToken);
ZW_ALIAS(ZwCompareTokens, NtCompareTokens);
ZW_ALIAS(ZwQueryInformationToken, NtQueryInformationToken);
ZW_ALIAS(ZwSetInformationToken, NtSetInformationToken);
ZW_ALIAS(ZwAdjustPrivilegesToken, NtAdjustPrivilegesToken);
ZW_ALIAS(ZwAdjustGroupsToken, NtAdjustGroupsToken);
ZW_ALIAS(ZwPrivilegeCheck, NtPrivilegeCheck);
ZW_ALIAS(ZwAllocateLocallyUniqueId, NtAllocateLocallyUniqueId);
ZW_ALIAS(ZwCreateToken, NtCreateToken);
ZW_ALIAS(ZwAccessCheck, NtAccessCheck);
ZW_ALIAS(ZwAccessCheckByType, NtAccessCheckByType);
ZW_ALIAS(ZwAccessCheckByTypeResultList, NtAccessCheckByTypeResultList);
ZW_ALIAS(ZwQuerySecurityObject, NtQuerySecurityObject);
ZW_ALIAS(ZwSetSecurityObject, NtSetSecurityObject);

/* ---- the descriptor of a new object --------------------------------------------------------------------------------------------------------------------- */

enum { SD_FILE = 1, SD_EVENT, SD_MUTANT, SD_SEMAPHORE, SD_TIMER, SD_SECTION, SD_JOB, SD_PIPE, SD_KEY, SD_PROCESS, SD_THREAD };
static const GENERIC_MAPPING sd_mappings[] = {
    {0, 0, 0, 0},
    {0x120089, 0x120116, 0x1200a0, 0x1f01ff},      /* SD_FILE */
    {0x20001, 0x20002, 0x120000, 0x1f0003},      /* SD_EVENT */
    {0x20001, 0x20000, 0x120000, 0x1f0001},      /* SD_MUTANT */
    {0x20001, 0x20002, 0x120000, 0x1f0003},      /* SD_SEMAPHORE */
    {0x20001, 0x20002, 0x120000, 0x1f0003},      /* SD_TIMER */
    {0x20005, 0x20002, 0x20008, 0xf001f},      /* SD_SECTION */
    {0x20004, 0x20003, 0x120000, 0x1f001f},      /* SD_JOB */
    {0x120089, 0x120116, 0x1200a0, 0x1f01ff},      /* SD_PIPE */
    {0x20019, 0x20006, 0x20019, 0xf003f},      /* SD_KEY */
    {0x20410, 0x20bea, 0x121001, 0x1fffff},      /* SD_PROCESS */
    {0x20048, 0x20037, 0x120000, 0x1fffff},      /* SD_THREAD */
};

/* A new object gets the descriptor its creator gave it, with what its parent (a file's directory) passes on; with neither it has none and the default is made when it is asked for.
 * A file or directory made by the system inherits automatically (the inherited ACEs are marked as such). */
__declspec(dllexport) void NTAPI RtlTcosNewObjectSecurity(HANDLE handle, const OBJECT_ATTRIBUTES *attributes, BOOLEAN container, int kind)
{
    PSECURITY_DESCRIPTOR creator = attributes && attributes->Length >= sizeof(OBJECT_ATTRIBUTES) ? attributes->SecurityDescriptor : NULL;
    BYTE *parent = NULL, *made = NULL;
    ULONG needed = 0, flags = (kind == SD_FILE) ? (SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT) : 0, length;
    if (kind == SD_FILE) {
        NTSTATUS asked = NtTcosToken(TCOS_FILE_PARENT_SD, (ULONG_PTR)handle, 0, 0, (ULONG_PTR)&needed, 0);
        if ((asked == 0 || asked == STATUS_BUFFER_TOO_SMALL_) && needed) {
            parent = heap_new(needed);
            if (parent && NtTcosToken(TCOS_FILE_PARENT_SD, (ULONG_PTR)handle, (ULONG_PTR)parent, needed, (ULONG_PTR)&needed, 0)) { heap_drop(parent); parent = NULL; }
        }
    }
    if (!creator && !parent) return;
    if (RtlNewSecurityObjectEx(parent, creator, (PSECURITY_DESCRIPTOR *)&made, NULL, container, flags, NULL, (PGENERIC_MAPPING)&sd_mappings[kind]) == 0) {
        length = RtlLengthSecurityDescriptor(made);
        NtTcosToken(TCOS_OBJECT_SD_SET, (ULONG_PTR)handle, 0, (ULONG_PTR)made, length, 0);
        RtlFreeHeap(RtlProcessHeap(), 0, made);
    }
    heap_drop(parent);
}

extern NTSTATUS NTAPI RawNtCreateEvent(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, BOOLEAN);
extern NTSTATUS NTAPI RawNtCreateMutant(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, BOOLEAN);
extern NTSTATUS NTAPI RawNtCreateSemaphore(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, LONG, LONG);
extern NTSTATUS NTAPI RawNtCreateTimer(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG);
extern NTSTATUS NTAPI RawNtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER, ULONG, ULONG, HANDLE);
extern NTSTATUS NTAPI RawNtCreateJobObject(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES);
extern NTSTATUS NTAPI RawNtCreateNamedPipeFile(PHANDLE, ULONG, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, ULONG, PLARGE_INTEGER);

__declspec(dllexport) NTSTATUS NTAPI NtCreateEvent(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG type, BOOLEAN state)
{
    NTSTATUS status = RawNtCreateEvent(h, access, attr, type, state);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_EVENT);
    return status;
}
__declspec(dllexport) NTSTATUS NTAPI NtCreateMutant(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, BOOLEAN owned)
{
    NTSTATUS status = RawNtCreateMutant(h, access, attr, owned);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_MUTANT);
    return status;
}
__declspec(dllexport) NTSTATUS NTAPI NtCreateSemaphore(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, LONG initial, LONG maximum)
{
    NTSTATUS status = RawNtCreateSemaphore(h, access, attr, initial, maximum);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_SEMAPHORE);
    return status;
}
__declspec(dllexport) NTSTATUS NTAPI NtCreateTimer(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG type)
{
    NTSTATUS status = RawNtCreateTimer(h, access, attr, type);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_TIMER);
    return status;
}
__declspec(dllexport) NTSTATUS NTAPI NtCreateSection(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, PLARGE_INTEGER size, ULONG protect, ULONG flags, HANDLE file)
{
    NTSTATUS status = RawNtCreateSection(h, access, attr, size, protect, flags, file);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_SECTION);
    return status;
}
__declspec(dllexport) NTSTATUS NTAPI NtCreateJobObject(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr)
{
    NTSTATUS status = RawNtCreateJobObject(h, access, attr);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_JOB);
    return status;
}
__declspec(dllexport) NTSTATUS NTAPI NtCreateNamedPipeFile(PHANDLE h, ULONG access, POBJECT_ATTRIBUTES attr, PIO_STATUS_BLOCK iosb, ULONG share, ULONG disposition, ULONG options, ULONG type, ULONG read_mode,
    ULONG completion, ULONG max_instances, ULONG in_quota, ULONG out_quota, PLARGE_INTEGER timeout)
{
    NTSTATUS status = RawNtCreateNamedPipeFile(h, access, attr, iosb, share, disposition, options, type, read_mode, completion, max_instances, in_quota, out_quota, timeout);
    if (status == STATUS_SUCCESS) RtlTcosNewObjectSecurity(*h, attr, FALSE, SD_PIPE);
    return status;
}

extern NTSTATUS NTAPI RawNtCreateKey(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG, PUNICODE_STRING, ULONG, PULONG);
__declspec(dllexport) NTSTATUS NTAPI NtCreateKey(PHANDLE h, ACCESS_MASK access, POBJECT_ATTRIBUTES attr, ULONG title, PUNICODE_STRING class_, ULONG options, PULONG disposition)
{
    ULONG made = 0;
    NTSTATUS status = RawNtCreateKey(h, access, attr, title, class_, options, &made);
    if (disposition) *disposition = made;
    if (status == STATUS_SUCCESS && made == REG_CREATED_NEW_KEY) RtlTcosNewObjectSecurity(*h, attr, TRUE, SD_KEY);
    return status;
}
