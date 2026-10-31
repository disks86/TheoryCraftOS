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
/* advapi32.dll: the basic security functions that a real system keeps in kernelbase.dll: SIDs and their well known forms, security descriptor and ACL accessors (over the Rtl
 * functions of ntdll), account names, the privileges, and the security of kernel objects and files. Built with the mingw headers as a unit of its own. */
#include "security.h"

#define EXP __declspec(dllexport)
#define NT_ERROR(status) RtlNtStatusToDosError(status)
static BOOL nt_fail(NTSTATUS status)
{
    SetLastError(RtlNtStatusToDosError(status));
    return FALSE;
}
static BOOL win_fail(DWORD error)
{
    SetLastError(error);
    return FALSE;
}

/* ---- SIDs -------------------------------------------------------------------------------------------------------------------------------------------------- */

EXP BOOL WINAPI IsValidSid(PSID sid) { return RtlValidSid(sid); }
EXP BOOL WINAPI EqualSid(PSID a, PSID b) { SetLastError(ERROR_SUCCESS); return RtlEqualSid(a, b); }
EXP BOOL WINAPI EqualPrefixSid(PSID a, PSID b) { SetLastError(ERROR_SUCCESS); return RtlEqualPrefixSid(a, b); }
EXP DWORD WINAPI GetSidLengthRequired(UCHAR count) { return RtlLengthRequiredSid(count); }
EXP DWORD WINAPI GetLengthSid(PSID sid) { return RtlLengthSid(sid); }
EXP BOOL WINAPI InitializeSid(PSID sid, PSID_IDENTIFIER_AUTHORITY authority, BYTE count)
{
    NTSTATUS status = RtlInitializeSid(sid, authority, count);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI AllocateAndInitializeSid(PSID_IDENTIFIER_AUTHORITY authority, BYTE count, DWORD s0, DWORD s1, DWORD s2, DWORD s3, DWORD s4, DWORD s5, DWORD s6, DWORD s7, PSID *sid)
{
    NTSTATUS status = RtlAllocateAndInitializeSid(authority, count, s0, s1, s2, s3, s4, s5, s6, s7, sid);
    return status ? nt_fail(status) : TRUE;
}
EXP PVOID WINAPI FreeSid(PSID sid)
{
    RtlFreeSid(sid);
    return NULL;
}
EXP BOOL WINAPI CopySid(DWORD length, PSID dest, PSID source)
{
    NTSTATUS status = RtlCopySid(length, dest, source);
    return status ? nt_fail(status) : TRUE;
}
EXP PSID_IDENTIFIER_AUTHORITY WINAPI GetSidIdentifierAuthority(PSID sid) { SetLastError(ERROR_SUCCESS); return RtlIdentifierAuthoritySid(sid); }
EXP PDWORD WINAPI GetSidSubAuthority(PSID sid, DWORD index) { SetLastError(ERROR_SUCCESS); return RtlSubAuthoritySid(sid, index); }
EXP PUCHAR WINAPI GetSidSubAuthorityCount(PSID sid) { SetLastError(ERROR_SUCCESS); return RtlSubAuthorityCountSid(sid); }

/* ---- security descriptors and ACLs --------------------------------------------------------------------------------------------------------------------------- */

EXP BOOL WINAPI InitializeSecurityDescriptor(PSECURITY_DESCRIPTOR sd, DWORD revision)
{
    NTSTATUS status = RtlCreateSecurityDescriptor(sd, revision);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI IsValidSecurityDescriptor(PSECURITY_DESCRIPTOR sd)
{
    if (!sd) return win_fail(ERROR_INVALID_SECURITY_DESCR);
    if (!RtlValidSecurityDescriptor(sd)) return win_fail(ERROR_INVALID_SECURITY_DESCR);
    return TRUE;
}
EXP BOOL WINAPI IsValidRelativeSecurityDescriptor(PSECURITY_DESCRIPTOR sd, ULONG length, SECURITY_INFORMATION required)
{
    if (!RtlValidRelativeSecurityDescriptor(sd, length, required)) return win_fail(ERROR_INVALID_SECURITY_DESCR);
    return TRUE;
}
EXP BOOL WINAPI IsValidAcl(PACL acl)
{
    if (!RtlValidAcl(acl)) return win_fail(ERROR_INVALID_ACL);
    return TRUE;
}
EXP DWORD WINAPI GetSecurityDescriptorLength(PSECURITY_DESCRIPTOR sd) { return RtlLengthSecurityDescriptor(sd); }
EXP BOOL WINAPI GetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd, PSECURITY_DESCRIPTOR_CONTROL control, LPDWORD revision)
{
    NTSTATUS status = RtlGetControlSecurityDescriptor(sd, control, revision);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI SetSecurityDescriptorControl(PSECURITY_DESCRIPTOR sd, SECURITY_DESCRIPTOR_CONTROL mask, SECURITY_DESCRIPTOR_CONTROL bits)
{
    NTSTATUS status = RtlSetControlSecurityDescriptor(sd, mask, bits);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI GetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID *owner, LPBOOL defaulted)
{
    BOOLEAN d;
    NTSTATUS status = RtlGetOwnerSecurityDescriptor(sd, owner, &d);
    if (status) return nt_fail(status);
    *defaulted = d;
    return TRUE;
}
EXP BOOL WINAPI SetSecurityDescriptorOwner(PSECURITY_DESCRIPTOR sd, PSID owner, BOOL defaulted)
{
    NTSTATUS status = RtlSetOwnerSecurityDescriptor(sd, owner, defaulted);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI GetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID *group, LPBOOL defaulted)
{
    BOOLEAN d;
    NTSTATUS status = RtlGetGroupSecurityDescriptor(sd, group, &d);
    if (status) return nt_fail(status);
    *defaulted = d;
    return TRUE;
}
EXP BOOL WINAPI SetSecurityDescriptorGroup(PSECURITY_DESCRIPTOR sd, PSID group, BOOL defaulted)
{
    NTSTATUS status = RtlSetGroupSecurityDescriptor(sd, group, defaulted);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI GetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *dacl, LPBOOL defaulted)
{
    BOOLEAN p, d;
    NTSTATUS status = RtlGetDaclSecurityDescriptor(sd, &p, dacl, &d);
    if (status) return nt_fail(status);
    *present = p;
    *defaulted = d;
    return TRUE;
}
EXP BOOL WINAPI SetSecurityDescriptorDacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL dacl, BOOL defaulted)
{
    NTSTATUS status = RtlSetDaclSecurityDescriptor(sd, present, dacl, defaulted);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI GetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd, LPBOOL present, PACL *sacl, LPBOOL defaulted)
{
    BOOLEAN p, d;
    NTSTATUS status = RtlGetSaclSecurityDescriptor(sd, &p, sacl, &d);
    if (status) return nt_fail(status);
    *present = p;
    *defaulted = d;
    return TRUE;
}
EXP BOOL WINAPI SetSecurityDescriptorSacl(PSECURITY_DESCRIPTOR sd, BOOL present, PACL sacl, BOOL defaulted)
{
    NTSTATUS status = RtlSetSaclSecurityDescriptor(sd, present, sacl, defaulted);
    return status ? nt_fail(status) : TRUE;
}
EXP DWORD WINAPI GetSecurityDescriptorRMControl(PSECURITY_DESCRIPTOR sd, PUCHAR control)
{
    if (!sd || !control) return ERROR_INVALID_PARAMETER;
    if (!(((SECURITY_DESCRIPTOR *)sd)->Control & SE_RM_CONTROL_VALID)) { *control = 0; return ERROR_INVALID_DATA; }
    *control = ((SECURITY_DESCRIPTOR *)sd)->Sbz1;
    return ERROR_SUCCESS;
}
EXP DWORD WINAPI SetSecurityDescriptorRMControl(PSECURITY_DESCRIPTOR sd, PUCHAR control)
{
    SECURITY_DESCRIPTOR *s = sd;
    if (!control) { s->Control &= ~SE_RM_CONTROL_VALID; s->Sbz1 = 0; }
    else { s->Control |= SE_RM_CONTROL_VALID; s->Sbz1 = *control; }
    return ERROR_SUCCESS;
}
EXP BOOL WINAPI MakeSelfRelativeSD(PSECURITY_DESCRIPTOR absolute, PSECURITY_DESCRIPTOR relative, LPDWORD length)
{
    NTSTATUS status = RtlMakeSelfRelativeSD(absolute, relative, length);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI MakeAbsoluteSD(PSECURITY_DESCRIPTOR relative, PSECURITY_DESCRIPTOR absolute, LPDWORD sd_size, PACL dacl, LPDWORD dacl_size, PACL sacl, LPDWORD sacl_size, PSID owner, LPDWORD owner_size,
    PSID group, LPDWORD group_size)
{
    NTSTATUS status = RtlSelfRelativeToAbsoluteSD(relative, absolute, sd_size, dacl, dacl_size, sacl, sacl_size, owner, owner_size, group, group_size);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI MakeAbsoluteSD2(PSECURITY_DESCRIPTOR relative, LPDWORD size)
{
    SECURITY_DESCRIPTOR_RELATIVE *rel = relative;
    SECURITY_DESCRIPTOR *abs;
    BYTE *p;
    if (!size || !relative) return win_fail(ERROR_INVALID_PARAMETER);
    if (!(rel->Control & SE_SELF_RELATIVE)) return win_fail(ERROR_INVALID_SECURITY_DESCR);
    DWORD need = sizeof(SECURITY_DESCRIPTOR) + RtlLengthSecurityDescriptor(rel) - sizeof(SECURITY_DESCRIPTOR_RELATIVE);
    if (*size < need) { *size = need; return win_fail(ERROR_INSUFFICIENT_BUFFER); }
    abs = relative;
    /* moves the parts up and rewrites the header in place */
    BYTE *copy = malloc(*size);
    if (!copy) return win_fail(ERROR_NOT_ENOUGH_MEMORY);
    memcpy(copy, relative, RtlLengthSecurityDescriptor(rel));
    SECURITY_DESCRIPTOR_RELATIVE *src = (SECURITY_DESCRIPTOR_RELATIVE *)copy;
    p = (BYTE *)abs + sizeof(SECURITY_DESCRIPTOR);
    abs->Revision = src->Revision;
    abs->Sbz1 = src->Sbz1;
    abs->Control = src->Control & ~SE_SELF_RELATIVE;
    abs->Owner = abs->Group = NULL;
    abs->Sacl = abs->Dacl = NULL;
    if (src->Owner) { DWORD n = RtlLengthSid(copy + src->Owner); memcpy(p, copy + src->Owner, n); abs->Owner = (PSID)p; p += n; }
    if (src->Group) { DWORD n = RtlLengthSid(copy + src->Group); memcpy(p, copy + src->Group, n); abs->Group = (PSID)p; p += n; }
    if (src->Sacl) { DWORD n = ((ACL *)(copy + src->Sacl))->AclSize; memcpy(p, copy + src->Sacl, n); abs->Sacl = (PACL)p; p += n; }
    if (src->Dacl) { DWORD n = ((ACL *)(copy + src->Dacl))->AclSize; memcpy(p, copy + src->Dacl, n); abs->Dacl = (PACL)p; p += n; }
    free(copy);
    *size = need;
    return TRUE;
}
EXP BOOL WINAPI InitializeAcl(PACL acl, DWORD size, DWORD revision)
{
    NTSTATUS status = RtlCreateAcl(acl, size, revision);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI AddAce(PACL acl, DWORD revision, DWORD index, LPVOID list, DWORD length)
{
    NTSTATUS status = RtlAddAce(acl, revision, index, list, length);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI DeleteAce(PACL acl, DWORD index)
{
    NTSTATUS status = RtlDeleteAce(acl, index);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI GetAce(PACL acl, DWORD index, LPVOID *ace)
{
    NTSTATUS status = RtlGetAce(acl, index, ace);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI FindFirstFreeAce(PACL acl, LPVOID *ace) { return RtlFirstFreeAce(acl, (PACE_HEADER *)ace); }
EXP BOOL WINAPI GetAclInformation(PACL acl, LPVOID info, DWORD length, ACL_INFORMATION_CLASS class_)
{
    NTSTATUS status = RtlQueryInformationAcl(acl, info, length, class_);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI SetAclInformation(PACL acl, LPVOID info, DWORD length, ACL_INFORMATION_CLASS class_)
{
    NTSTATUS status = RtlSetInformationAcl(acl, info, length, class_);
    return status ? nt_fail(status) : TRUE;
}
#define ADD_ACE(name, rtl_args, ...) \
    EXP BOOL WINAPI name(__VA_ARGS__) \
    { NTSTATUS status = rtl_args; return status ? nt_fail(status) : TRUE; }
ADD_ACE(AddAccessAllowedAce, RtlAddAccessAllowedAce(acl, revision, mask, sid), PACL acl, DWORD revision, DWORD mask, PSID sid)
ADD_ACE(AddAccessAllowedAceEx, RtlAddAccessAllowedAceEx(acl, revision, flags, mask, sid), PACL acl, DWORD revision, DWORD flags, DWORD mask, PSID sid)
ADD_ACE(AddAccessDeniedAce, RtlAddAccessDeniedAce(acl, revision, mask, sid), PACL acl, DWORD revision, DWORD mask, PSID sid)
ADD_ACE(AddAccessDeniedAceEx, RtlAddAccessDeniedAceEx(acl, revision, flags, mask, sid), PACL acl, DWORD revision, DWORD flags, DWORD mask, PSID sid)
ADD_ACE(AddAuditAccessAce, RtlAddAuditAccessAce(acl, revision, mask, sid, success, failure), PACL acl, DWORD revision, DWORD mask, PSID sid, BOOL success, BOOL failure)
ADD_ACE(AddAuditAccessAceEx, RtlAddAuditAccessAceEx(acl, revision, flags, mask, sid, success, failure), PACL acl, DWORD revision, DWORD flags, DWORD mask, PSID sid, BOOL success, BOOL failure)
ADD_ACE(AddAccessAllowedObjectAce, RtlAddAccessAllowedObjectAce(acl, revision, flags, mask, type, inherit, sid), PACL acl, DWORD revision, DWORD flags, DWORD mask, GUID *type, GUID *inherit, PSID sid)
ADD_ACE(AddAccessDeniedObjectAce, RtlAddAccessDeniedObjectAce(acl, revision, flags, mask, type, inherit, sid), PACL acl, DWORD revision, DWORD flags, DWORD mask, GUID *type, GUID *inherit, PSID sid)
ADD_ACE(AddAuditAccessObjectAce, RtlAddAuditAccessObjectAce(acl, revision, flags, mask, type, inherit, sid, success, failure), PACL acl, DWORD revision, DWORD flags, DWORD mask, GUID *type, GUID *inherit, PSID sid, BOOL success, BOOL failure)
ADD_ACE(AddMandatoryAce, RtlAddMandatoryAce(acl, revision, flags, policy, SYSTEM_MANDATORY_LABEL_ACE_TYPE, sid), PACL acl, DWORD revision, DWORD flags, DWORD policy, PSID sid)

EXP BOOL WINAPI AreAllAccessesGranted(DWORD granted, DWORD desired) { return RtlAreAllAccessesGranted(granted, desired); }
EXP BOOL WINAPI AreAnyAccessesGranted(DWORD granted, DWORD desired) { return RtlAreAnyAccessesGranted(granted, desired); }
EXP VOID WINAPI MapGenericMask(PDWORD mask, PGENERIC_MAPPING mapping) { RtlMapGenericMask(mask, mapping); }
EXP BOOL WINAPI AllocateLocallyUniqueId(PLUID luid)
{
    NTSTATUS status = NtAllocateLocallyUniqueId(luid);
    return status ? nt_fail(status) : TRUE;
}
EXP VOID WINAPI QuerySecurityAccessMask(SECURITY_INFORMATION info, LPDWORD access)
{
    *access = 0;
    if (info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION | ATTRIBUTE_SECURITY_INFORMATION | SCOPE_SECURITY_INFORMATION | BACKUP_SECURITY_INFORMATION)) *access |= READ_CONTROL;
    if (info & SACL_SECURITY_INFORMATION) *access |= ACCESS_SYSTEM_SECURITY;
}
EXP VOID WINAPI SetSecurityAccessMask(SECURITY_INFORMATION info, LPDWORD access)
{
    *access = 0;
    if (info & (DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION)) *access |= WRITE_DAC;
    if (info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION)) *access |= WRITE_OWNER;
    if (info & (SACL_SECURITY_INFORMATION | PROTECTED_SACL_SECURITY_INFORMATION | UNPROTECTED_SACL_SECURITY_INFORMATION)) *access |= ACCESS_SYSTEM_SECURITY;
}

BOOL ADVAPI_GetComputerSid(PSID sid);

/* ---- well known SIDs ------------------------------------------------------------------------------------------------------------------------------------- */

/* The SIDs by WELL_KNOWN_SID_TYPE: a fixed one as text, or a relative identifier of a domain (the text then names the SID that is made for a domain). Zero means: not known. */
static const struct { const char *text; DWORD domain_rid; } well_known[] = {
    /* 0 */ {"S-1-0-0"}, {"S-1-1-0"}, {"S-1-2-0"}, {"S-1-3-0"}, {"S-1-3-1"}, {"S-1-3-2"}, {"S-1-3-3"}, {"S-1-5"},
    /* 8 */ {"S-1-5-1"}, {"S-1-5-2"}, {"S-1-5-3"}, {"S-1-5-4"}, {"S-1-5-6"}, {"S-1-5-7"}, {"S-1-5-8"}, {"S-1-5-9"},
    /* 16 */ {"S-1-5-10"}, {"S-1-5-11"}, {"S-1-5-12"}, {"S-1-5-13"}, {"S-1-5-14"}, {NULL}, {"S-1-5-18"}, {"S-1-5-19"},
    /* 24 */ {"S-1-5-20"}, {"S-1-5-32"}, {"S-1-5-32-544"}, {"S-1-5-32-545"}, {"S-1-5-32-546"}, {"S-1-5-32-547"}, {"S-1-5-32-548"}, {"S-1-5-32-549"},
    /* 32 */ {"S-1-5-32-550"}, {"S-1-5-32-551"}, {"S-1-5-32-552"}, {"S-1-5-32-554"}, {"S-1-5-32-555"}, {"S-1-5-32-556"},
    /* 38 */ {NULL, 500}, {NULL, 501}, {NULL, 502}, {NULL, 512}, {NULL, 513}, {NULL, 514}, {NULL, 515}, {NULL, 516}, {NULL, 517}, {NULL, 518}, {NULL, 519}, {NULL, 520}, {NULL, 553},
    /* 51 */ {"S-1-5-64-10"}, {"S-1-5-64-21"}, {"S-1-5-64-14"}, {"S-1-5-15"}, {"S-1-5-1000"}, {"S-1-5-32-557"}, {"S-1-5-32-558"}, {"S-1-5-32-559"}, {"S-1-5-32-560"}, {"S-1-5-32-561"},
    /* 61 */ {"S-1-5-32-562"}, {"S-1-5-32-568"}, {"S-1-5-17"}, {"S-1-5-32-569"}, {"S-1-16-0"}, {"S-1-16-4096"}, {"S-1-16-8192"}, {"S-1-16-12288"}, {"S-1-16-16384"}, {"S-1-5-33"},
    /* 71 */ {"S-1-3-4"}, {NULL, 571}, {NULL, 572}, {"S-1-5-22"}, {NULL, 521}, {"S-1-5-32-573"}, {NULL, 498}, {"S-1-5-32-574"}, {"S-1-16-8448"}, {NULL},
    /* 81 */ {"S-1-2-1"}, {"S-1-5-65-1"}, {NULL}, {"S-1-15-2-1"},
};
#define WELL_KNOWN_COUNT ((int)ARRAY_SIZE(well_known))

/* A SID from its text "S-1-5-32-544"; returns the length of the text taken (the number of characters used) or 0. `sid` has room for the largest. */
static const WCHAR *parse_sid_number(const WCHAR *p, ULONGLONG *value, int *hex)
{
    ULONGLONG v = 0;
    int digits = 0;
    *hex = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        *hex = 1;
        for (;; ++p, ++digits) {
            WCHAR c = *p;
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            if (v >> 60) return NULL;
            v = v * 16 + (ULONGLONG)d;
        }
    } else {
        for (; *p >= '0' && *p <= '9'; ++p, ++digits) {
            if (v > (0xFFFFFFFFFFFFFFFFull - 9) / 10) return NULL;
            v = v * 10 + (ULONGLONG)(*p - '0');
        }
    }
    if (!digits) return NULL;
    *value = v;
    return p;
}
static const WCHAR *parse_text_sid(const WCHAR *text, SID *sid)
{
    ULONGLONG v;
    int hex;
    const WCHAR *p = text;
    if ((p[0] != 'S' && p[0] != 's') || p[1] != '-') return NULL;
    p += 2;
    if (!(p = parse_sid_number(p, &v, &hex)) || v != 1 || *p != '-') return NULL;
    sid->Revision = 1;
    ++p;
    if (!(p = parse_sid_number(p, &v, &hex))) return NULL;
    if (hex) {
        if (v >> 48) return NULL;
    } else if (v >> 32) return NULL;
    for (int i = 0; i < 6; ++i) sid->IdentifierAuthority.Value[i] = (BYTE)(v >> (8 * (5 - i)));
    sid->SubAuthorityCount = 0;
    while (*p == '-') {
        if (sid->SubAuthorityCount >= SID_MAX_SUB_AUTHORITIES) return NULL;
        ++p;
        if (!(p = parse_sid_number(p, &v, &hex)) || (v >> 32)) return NULL;
        sid->SubAuthority[sid->SubAuthorityCount++] = (DWORD)v;
    }
    if (sid->SubAuthorityCount == 0 && 0) return NULL;
    return p;
}
static BOOL text_to_sid(const char *text, SID *sid)
{
    WCHAR wide[64];
    int n = 0;
    for (; text[n] && n < 63; ++n) wide[n] = (WCHAR)text[n];
    wide[n] = 0;
    const WCHAR *end = parse_text_sid(wide, sid);
    return end && !*end;
}

EXP BOOL WINAPI CreateWellKnownSid(WELL_KNOWN_SID_TYPE type, PSID domain, PSID sid, DWORD *size)
{
    BYTE buffer[sizeof(SID) + 16 * sizeof(DWORD)];
    SID *made = (SID *)buffer;
    if (!size) return win_fail(ERROR_INVALID_PARAMETER);
    if ((int)type < 0 || (int)type >= WELL_KNOWN_COUNT) return win_fail(ERROR_INVALID_PARAMETER);
    if (well_known[type].text) {
        if (!text_to_sid(well_known[type].text, made)) return win_fail(ERROR_INVALID_PARAMETER);
    } else if (well_known[type].domain_rid) {
        BYTE machine[sizeof(SID) + 16 * sizeof(DWORD)];
        if (!domain) { ADVAPI_GetComputerSid(machine); domain = machine; }      /* (the domain of this machine) */
        if (!RtlValidSid(domain) || ((SID *)domain)->SubAuthorityCount >= SID_MAX_SUB_AUTHORITIES) return win_fail(ERROR_INVALID_SID);
        memcpy(made, domain, RtlLengthSid(domain));
        made->SubAuthority[made->SubAuthorityCount++] = well_known[type].domain_rid;
    } else return win_fail(ERROR_INVALID_PARAMETER);
    DWORD need = RtlLengthSid(made);
    if (*size < need) { *size = need; return win_fail(ERROR_INSUFFICIENT_BUFFER); }
    if (!sid) return win_fail(ERROR_INVALID_PARAMETER);
    memcpy(sid, made, need);
    *size = need;
    return TRUE;
}
EXP BOOL WINAPI IsWellKnownSid(PSID sid, WELL_KNOWN_SID_TYPE type)
{
    BYTE buffer[sizeof(SID) + 16 * sizeof(DWORD)];
    SID *s = sid;
    if (!sid || !RtlValidSid(sid) || (int)type < 0 || (int)type >= WELL_KNOWN_COUNT) return FALSE;
    if (well_known[type].text) return text_to_sid(well_known[type].text, (SID *)buffer) && RtlEqualSid(sid, buffer);
    if (type == WinLogonIdsSid) {      /* S-1-5-5-x-y */
        return s->SubAuthorityCount == 3 && s->SubAuthority[0] == 5 && s->IdentifierAuthority.Value[5] == 5 && !s->IdentifierAuthority.Value[0] && !s->IdentifierAuthority.Value[1] &&
               !s->IdentifierAuthority.Value[2] && !s->IdentifierAuthority.Value[3] && !s->IdentifierAuthority.Value[4];
    }
    if (well_known[type].domain_rid) {
        /* a domain SID of 4 subauthorities (21-a-b-c) and the relative identifier */
        return s->SubAuthorityCount == 5 && s->SubAuthority[0] == 21 && s->SubAuthority[4] == well_known[type].domain_rid &&
               s->IdentifierAuthority.Value[5] == 5 && !s->IdentifierAuthority.Value[0] && !s->IdentifierAuthority.Value[1] && !s->IdentifierAuthority.Value[2] && !s->IdentifierAuthority.Value[3] && !s->IdentifierAuthority.Value[4];
    }
    return FALSE;
}
EXP BOOL WINAPI GetWindowsAccountDomainSid(PSID sid, PSID domain, DWORD *size)
{
    SID *s = sid;
    DWORD need;
    if (!sid || !RtlValidSid(sid)) return win_fail(ERROR_INVALID_SID);
    /* a domain account has the form S-1-5-21-a-b-c-rid */
    if (s->SubAuthorityCount < 5 || s->IdentifierAuthority.Value[5] != 5 || s->IdentifierAuthority.Value[0] || s->IdentifierAuthority.Value[1] || s->IdentifierAuthority.Value[2] ||
        s->IdentifierAuthority.Value[3] || s->IdentifierAuthority.Value[4] || s->SubAuthority[0] != 21) return win_fail(ERROR_NON_ACCOUNT_SID);
    if (!size) return win_fail(ERROR_INVALID_PARAMETER);
    need = RtlLengthRequiredSid(4);
    if (!domain) { *size = need; return win_fail(ERROR_INVALID_PARAMETER); }
    if (*size < need) { *size = need; return win_fail(ERROR_INSUFFICIENT_BUFFER); }
    memcpy(domain, sid, 8 + 4 * sizeof(DWORD));
    ((SID *)domain)->SubAuthorityCount = 4;
    *size = need;
    return TRUE;
}
EXP BOOL WINAPI EqualDomainSid(PSID a, PSID b, BOOL *equal)
{
    SID *x = a, *y = b;
    if (!RtlValidSid(a) || !RtlValidSid(b)) return win_fail(ERROR_INVALID_SID);
    if (!equal) return win_fail(ERROR_INVALID_PARAMETER);
    /* only the SIDs of the builtin domain (S-1-5-32-x) have a domain here; the others are not SIDs of an account of a domain */
    if (x->SubAuthorityCount < 2 || x->IdentifierAuthority.Value[5] != 5 || x->SubAuthority[0] != 32) return win_fail(ERROR_NON_DOMAIN_SID);
    *equal = y->SubAuthorityCount >= 2 && y->IdentifierAuthority.Value[5] == 5 && y->SubAuthority[0] == 32;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}

/* ---- the text of SIDs ------------------------------------------------------------------------------------------------------------------------------------ */

EXP BOOL WINAPI ConvertSidToStringSidW(PSID sid, LPWSTR *text)
{
    UNICODE_STRING string;
    NTSTATUS status;
    if (!RtlValidSid(sid)) return win_fail(ERROR_INVALID_SID);
    if (!text) return win_fail(ERROR_INVALID_PARAMETER);
    status = RtlConvertSidToUnicodeString(&string, sid, TRUE);
    if (status) return nt_fail(status);
    WCHAR *out = LocalAlloc(LMEM_FIXED, string.Length + sizeof(WCHAR));
    if (!out) { RtlFreeUnicodeString(&string); return win_fail(ERROR_NOT_ENOUGH_MEMORY); }
    memcpy(out, string.Buffer, string.Length);
    out[string.Length / sizeof(WCHAR)] = 0;
    RtlFreeUnicodeString(&string);
    *text = out;
    return TRUE;
}

/* The two letter names of SDDL for SIDs: those that are fixed, and those relative to a domain (the domain SID is then needed). */
static const struct { char name[3]; const char *sid; DWORD domain_rid; DWORD root_rid; } sddl_sids[] = {
    {"WD", "S-1-1-0"}, {"CO", "S-1-3-0"}, {"CG", "S-1-3-1"}, {"OW", "S-1-3-4"}, {"NU", "S-1-5-2"}, {"IU", "S-1-5-4"}, {"SU", "S-1-5-6"}, {"AN", "S-1-5-7"}, {"PS", "S-1-5-10"},
    {"AU", "S-1-5-11"}, {"RC", "S-1-5-12"}, {"SY", "S-1-5-18"}, {"LS", "S-1-5-19"}, {"NS", "S-1-5-20"}, {"BA", "S-1-5-32-544"}, {"BU", "S-1-5-32-545"}, {"BG", "S-1-5-32-546"},
    {"PU", "S-1-5-32-547"}, {"AO", "S-1-5-32-548"}, {"SO", "S-1-5-32-549"}, {"PO", "S-1-5-32-550"}, {"BO", "S-1-5-32-551"}, {"RE", "S-1-5-32-552"}, {"RU", "S-1-5-32-554"},
    {"RD", "S-1-5-32-555"}, {"NO", "S-1-5-32-556"}, {"MU", "S-1-5-32-558"}, {"LU", "S-1-5-32-559"}, {"IS", "S-1-5-32-568"}, {"CY", "S-1-5-32-569"}, {"ER", "S-1-5-32-573"},
    {"RA", "S-1-5-32-575"}, {"ES", "S-1-5-32-576"}, {"MS", "S-1-5-32-577"}, {"HA", "S-1-5-32-578"}, {"AA", "S-1-5-32-579"}, {"RM", "S-1-5-32-580"},
    {"ED", "S-1-5-9"}, {"LW", "S-1-16-4096"}, {"ME", "S-1-16-8192"}, {"MP", "S-1-16-8448"}, {"HI", "S-1-16-12288"}, {"SI", "S-1-16-16384"}, {"AC", "S-1-15-2-1"}, {"OW", "S-1-3-4"},
    {"AP", "S-1-5-64-10"}, {"SS", "S-1-18-2"}, {"UD", "S-1-5-84-0-0-0-0-0"},
    {"LG", NULL, 501}, {"LA", NULL, 500}, {"RS", NULL, 553, 1}, {"DA", NULL, 512, 1}, {"DU", NULL, 513, 1}, {"DG", NULL, 514, 1}, {"DC", NULL, 515, 1}, {"DD", NULL, 516, 1}, {"CA", NULL, 517, 1},
    {"SA", NULL, 518, 1}, {"EA", NULL, 519, 1}, {"PA", NULL, 520, 1}, {"RO", NULL, 498, 1}, {"CN", NULL, 522, 1}, {"AK", NULL, 526, 1}, {"EK", NULL, 527, 1}, {"CD", "S-1-5-32-574"},
};

/* The name of a SID in SDDL (two letters), if it has one. */
static const char *sddl_sid_name(PSID sid)
{
    BYTE buffer[sizeof(SID) + 16 * sizeof(DWORD)];
    SID *s = sid;
    for (int i = 0; i < (int)ARRAY_SIZE(sddl_sids); ++i) {
        if (sddl_sids[i].sid) {
            if (text_to_sid(sddl_sids[i].sid, (SID *)buffer) && RtlEqualSid(sid, buffer)) return sddl_sids[i].name;
        } else if (s->SubAuthorityCount == (sddl_sids[i].root_rid ? 5 : 3) && s->SubAuthority[s->SubAuthorityCount - 1] == sddl_sids[i].domain_rid) {
            if (sddl_sids[i].root_rid) {
                if (s->SubAuthority[0] == 21) return sddl_sids[i].name;      /* (a domain's own) */
            } else if (s->IdentifierAuthority.Value[5] == 5 && s->SubAuthority[0] == 32) return sddl_sids[i].name;
        }
    }
    return NULL;
}
/* The SID of a two letter SDDL alias at p (not case sensitive); FALSE if p does not start with one. The aliases of a domain's accounts are made over the SID of this machine. */
static BOOL alias_to_sid(const WCHAR *p, SID *out)
{
    for (int i = 0; i < (int)ARRAY_SIZE(sddl_sids); ++i) {
        if ((p[0] | 0x20) != (WCHAR)(sddl_sids[i].name[0] | 0x20) || (p[1] | 0x20) != (WCHAR)(sddl_sids[i].name[1] | 0x20)) continue;
        if (sddl_sids[i].sid) return text_to_sid(sddl_sids[i].sid, out);
        if (!ADVAPI_GetComputerSid(out) || out->SubAuthorityCount >= SID_MAX_SUB_AUTHORITIES) return FALSE;
        out->SubAuthority[out->SubAuthorityCount++] = sddl_sids[i].domain_rid;
        return TRUE;
    }
    return FALSE;
}
EXP BOOL WINAPI ConvertStringSidToSidW(LPCWSTR text, PSID *sid_out)
{
    BYTE buffer[sizeof(SID) + 16 * sizeof(DWORD)];
    SID *sid = (SID *)buffer;
    const WCHAR *end;
    if (!text || !sid_out) return win_fail(ERROR_INVALID_PARAMETER);
    if ((text[0] == 'S' || text[0] == 's') && text[1] == '-') {
        end = parse_text_sid(text, sid);
        if (!end || *end || sid->SubAuthorityCount == 0) return win_fail(ERROR_INVALID_SID);
    } else {
        /* a two letter alias that stands alone */
        if (!text[0] || !text[1] || text[2] || !alias_to_sid(text, sid)) return win_fail(ERROR_INVALID_SID);
    }
    DWORD length = RtlLengthSid(sid);
    void *out = LocalAlloc(LMEM_FIXED, length);
    if (!out) return win_fail(ERROR_NOT_ENOUGH_MEMORY);
    memcpy(out, sid, length);
    *sid_out = out;
    return TRUE;
}
#include "sddl2.inc"
#include "token.inc"
