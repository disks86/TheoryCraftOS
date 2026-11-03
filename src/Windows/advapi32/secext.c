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

/* advapi32.dll: the rest of the security API - the audit alarm functions (over the Nt*AuditAlarm services of ntdll.dll), trustees, the string forms of security descriptors with a
 * domain, the old access-control-list (ACTRL) interface, logging on, anonymous impersonation and the shutdown functions (over NtShutdownSystem). */
#include "security.h"

NTSTATUS NTAPI NtAccessCheckAndAuditAlarm(PUNICODE_STRING, PVOID, PUNICODE_STRING, PUNICODE_STRING, PSECURITY_DESCRIPTOR, ACCESS_MASK, PGENERIC_MAPPING, BOOLEAN, PACCESS_MASK, PNTSTATUS, PBOOLEAN);
NTSTATUS NTAPI NtAccessCheckByTypeAndAuditAlarm(PUNICODE_STRING, PVOID, PUNICODE_STRING, PUNICODE_STRING, PSECURITY_DESCRIPTOR, PSID, ACCESS_MASK, ULONG, ULONG, POBJECT_TYPE_LIST, ULONG,
                                                PGENERIC_MAPPING, BOOLEAN, PACCESS_MASK, PNTSTATUS, PBOOLEAN);
NTSTATUS NTAPI NtAccessCheckByTypeResultListAndAuditAlarm(PUNICODE_STRING, PVOID, PUNICODE_STRING, PUNICODE_STRING, PSECURITY_DESCRIPTOR, PSID, ACCESS_MASK, ULONG, ULONG, POBJECT_TYPE_LIST,
                                                          ULONG, PGENERIC_MAPPING, BOOLEAN, PACCESS_MASK, PNTSTATUS, PBOOLEAN);
NTSTATUS NTAPI NtAccessCheckByTypeResultListAndAuditAlarmByHandle(PUNICODE_STRING, PVOID, HANDLE, PUNICODE_STRING, PUNICODE_STRING, PSECURITY_DESCRIPTOR, PSID, ACCESS_MASK, ULONG, ULONG,
                                                                  POBJECT_TYPE_LIST, ULONG, PGENERIC_MAPPING, BOOLEAN, PACCESS_MASK, PNTSTATUS, PBOOLEAN);
NTSTATUS NTAPI NtOpenObjectAuditAlarm(PUNICODE_STRING, PVOID, PUNICODE_STRING, PUNICODE_STRING, PSECURITY_DESCRIPTOR, HANDLE, ACCESS_MASK, ACCESS_MASK, PPRIVILEGE_SET, BOOLEAN, BOOLEAN, PBOOLEAN);
NTSTATUS NTAPI NtCloseObjectAuditAlarm(PUNICODE_STRING, PVOID, BOOLEAN);
NTSTATUS NTAPI NtDeleteObjectAuditAlarm(PUNICODE_STRING, PVOID, BOOLEAN);
NTSTATUS NTAPI NtPrivilegeObjectAuditAlarm(PUNICODE_STRING, PVOID, HANDLE, ACCESS_MASK, PPRIVILEGE_SET, BOOLEAN);
NTSTATUS NTAPI NtPrivilegedServiceAuditAlarm(PUNICODE_STRING, PUNICODE_STRING, HANDLE, PPRIVILEGE_SET, BOOLEAN);
NTSTATUS NTAPI NtImpersonateAnonymousToken(HANDLE);
NTSTATUS NTAPI NtShutdownSystem(ULONG);

static BOOL nt_result(NTSTATUS status)
{
    if (NT_SUCCESS(status)) return TRUE;
    SetLastError(RtlNtStatusToDosError(status));
    return FALSE;
}
static void ustr(UNICODE_STRING *u, LPCWSTR text) { if (text) RtlInitUnicodeString(u, text); else memset(u, 0, sizeof(*u)); }

/* ---- audit alarms --------------------------------------------------------------------------------------------------------------------------------------------- */

static BOOL access_alarm(LPCWSTR subsystem, LPVOID handle_id, HANDLE client, LPCWSTR type_name, LPCWSTR object_name, PSECURITY_DESCRIPTOR sd, PSID principal, DWORD desired, DWORD audit_type,
                         DWORD flags, POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping, BOOL creation, LPDWORD granted, LPBOOL access_status, LPBOOL on_close, int variant)
{
    UNICODE_STRING sub, type, name;
    NTSTATUS status, *statuses = NULL;
    DWORD count = variant >= 2 ? (type_count ? type_count : 1) : 1, i;
    BOOLEAN close = FALSE;
    if (!granted || !access_status || !on_close) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!(statuses = calloc(count, sizeof(*statuses)))) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    ustr(&sub, subsystem); ustr(&type, type_name); ustr(&name, object_name);
    switch (variant) {
    case 0: status = NtAccessCheckAndAuditAlarm(&sub, handle_id, &type, &name, sd, desired, mapping, creation, granted, statuses, &close); break;
    case 1: status = NtAccessCheckByTypeAndAuditAlarm(&sub, handle_id, &type, &name, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, statuses, &close); break;
    case 2: status = NtAccessCheckByTypeResultListAndAuditAlarm(&sub, handle_id, &type, &name, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, statuses, &close); break;
    default: status = NtAccessCheckByTypeResultListAndAuditAlarmByHandle(&sub, handle_id, client, &type, &name, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, statuses, &close); break;
    }
    if (NT_SUCCESS(status)) {
        for (i = 0; i < count; ++i) access_status[i] = NT_SUCCESS(statuses[i]);
        *on_close = close;
        if (!NT_SUCCESS(statuses[0])) SetLastError(RtlNtStatusToDosError(statuses[0]));
    } else SetLastError(RtlNtStatusToDosError(status));
    free(statuses);
    return NT_SUCCESS(status);
}
__declspec(dllexport) BOOL WINAPI AccessCheckAndAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, LPWSTR type_name, LPWSTR object_name, PSECURITY_DESCRIPTOR sd, DWORD desired, PGENERIC_MAPPING mapping,
                                                           BOOL creation, LPDWORD granted, LPBOOL access_status, LPBOOL on_close)
{
    return access_alarm(subsystem, handle_id, NULL, type_name, object_name, sd, NULL, desired, 0, 0, NULL, 0, mapping, creation, granted, access_status, on_close, 0);
}
__declspec(dllexport) BOOL WINAPI AccessCheckByTypeAndAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, LPCWSTR type_name, LPCWSTR object_name, PSECURITY_DESCRIPTOR sd, PSID principal, DWORD desired,
                                                                 AUDIT_EVENT_TYPE audit_type, DWORD flags, POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping, BOOL creation,
                                                                 LPDWORD granted, LPBOOL access_status, LPBOOL on_close)
{
    return access_alarm(subsystem, handle_id, NULL, type_name, object_name, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, access_status, on_close, 1);
}
__declspec(dllexport) BOOL WINAPI AccessCheckByTypeResultListAndAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, LPCWSTR type_name, LPCWSTR object_name, PSECURITY_DESCRIPTOR sd, PSID principal,
                                                                           DWORD desired, AUDIT_EVENT_TYPE audit_type, DWORD flags, POBJECT_TYPE_LIST types, DWORD type_count,
                                                                           PGENERIC_MAPPING mapping, BOOL creation, LPDWORD granted, LPDWORD access_status, LPBOOL on_close)
{
    return access_alarm(subsystem, handle_id, NULL, type_name, object_name, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, (LPBOOL)access_status, on_close, 2);
}
__declspec(dllexport) BOOL WINAPI AccessCheckByTypeResultListAndAuditAlarmByHandleW(LPCWSTR subsystem, LPVOID handle_id, HANDLE client, LPCWSTR type_name, LPCWSTR object_name,
                                                                                   PSECURITY_DESCRIPTOR sd, PSID principal, DWORD desired, AUDIT_EVENT_TYPE audit_type, DWORD flags,
                                                                                   POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping, BOOL creation, LPDWORD granted,
                                                                                   LPDWORD access_status, LPBOOL on_close)
{
    return access_alarm(subsystem, handle_id, client, type_name, object_name, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, (LPBOOL)access_status, on_close, 3);
}
/* The ANSI forms: the strings converted, then the wide form. */
#define A2W(var, text) WCHAR *var = (text) ? strdupAW(text) : NULL
__declspec(dllexport) BOOL WINAPI AccessCheckAndAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, LPSTR type_name, LPSTR object_name, PSECURITY_DESCRIPTOR sd, DWORD desired, PGENERIC_MAPPING mapping,
                                                           BOOL creation, LPDWORD granted, LPBOOL access_status, LPBOOL on_close)
{
    A2W(s, subsystem); A2W(t, type_name); A2W(n, object_name);
    BOOL r = AccessCheckAndAuditAlarmW(s, handle_id, t, n, sd, desired, mapping, creation, granted, access_status, on_close);
    DWORD e = GetLastError();
    free(s); free(t); free(n);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI AccessCheckByTypeAndAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, LPCSTR type_name, LPCSTR object_name, PSECURITY_DESCRIPTOR sd, PSID principal, DWORD desired,
                                                                 AUDIT_EVENT_TYPE audit_type, DWORD flags, POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping, BOOL creation,
                                                                 LPDWORD granted, LPBOOL access_status, LPBOOL on_close)
{
    A2W(s, subsystem); A2W(t, type_name); A2W(n, object_name);
    BOOL r = AccessCheckByTypeAndAuditAlarmW(s, handle_id, t, n, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, access_status, on_close);
    DWORD e = GetLastError();
    free(s); free(t); free(n);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI AccessCheckByTypeResultListAndAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, LPCSTR type_name, LPCSTR object_name, PSECURITY_DESCRIPTOR sd, PSID principal,
                                                                           DWORD desired, AUDIT_EVENT_TYPE audit_type, DWORD flags, POBJECT_TYPE_LIST types, DWORD type_count,
                                                                           PGENERIC_MAPPING mapping, BOOL creation, LPDWORD granted, LPDWORD access_status, LPBOOL on_close)
{
    A2W(s, subsystem); A2W(t, type_name); A2W(n, object_name);
    BOOL r = AccessCheckByTypeResultListAndAuditAlarmW(s, handle_id, t, n, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, access_status, on_close);
    DWORD e = GetLastError();
    free(s); free(t); free(n);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI AccessCheckByTypeResultListAndAuditAlarmByHandleA(LPCSTR subsystem, LPVOID handle_id, HANDLE client, LPCSTR type_name, LPCSTR object_name,
                                                                                   PSECURITY_DESCRIPTOR sd, PSID principal, DWORD desired, AUDIT_EVENT_TYPE audit_type, DWORD flags,
                                                                                   POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping, BOOL creation, LPDWORD granted,
                                                                                   LPDWORD access_status, LPBOOL on_close)
{
    A2W(s, subsystem); A2W(t, type_name); A2W(n, object_name);
    BOOL r = AccessCheckByTypeResultListAndAuditAlarmByHandleW(s, handle_id, client, t, n, sd, principal, desired, audit_type, flags, types, type_count, mapping, creation, granted, access_status, on_close);
    DWORD e = GetLastError();
    free(s); free(t); free(n);
    SetLastError(e);
    return r;
}

__declspec(dllexport) BOOL WINAPI ObjectOpenAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, LPWSTR type_name, LPWSTR object_name, PSECURITY_DESCRIPTOR sd, HANDLE client, DWORD desired, DWORD granted,
                                                       PPRIVILEGE_SET privileges, BOOL creation, BOOL access_granted, LPBOOL on_close)
{
    UNICODE_STRING sub, type, name;
    BOOLEAN close = FALSE;
    NTSTATUS status;
    ustr(&sub, subsystem); ustr(&type, type_name); ustr(&name, object_name);
    status = NtOpenObjectAuditAlarm(&sub, handle_id, &type, &name, sd, client, desired, granted, privileges, creation, access_granted, &close);
    if (on_close && NT_SUCCESS(status)) *on_close = close;
    return nt_result(status);
}
__declspec(dllexport) BOOL WINAPI ObjectCloseAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, BOOL on_close)
{
    UNICODE_STRING sub;
    ustr(&sub, subsystem);
    return nt_result(NtCloseObjectAuditAlarm(&sub, handle_id, on_close));
}
__declspec(dllexport) BOOL WINAPI ObjectDeleteAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, BOOL on_close)
{
    UNICODE_STRING sub;
    ustr(&sub, subsystem);
    return nt_result(NtDeleteObjectAuditAlarm(&sub, handle_id, on_close));
}
__declspec(dllexport) BOOL WINAPI ObjectPrivilegeAuditAlarmW(LPCWSTR subsystem, LPVOID handle_id, HANDLE client, DWORD desired, PPRIVILEGE_SET privileges, BOOL granted)
{
    UNICODE_STRING sub;
    ustr(&sub, subsystem);
    return nt_result(NtPrivilegeObjectAuditAlarm(&sub, handle_id, client, desired, privileges, granted));
}
__declspec(dllexport) BOOL WINAPI PrivilegedServiceAuditAlarmW(LPCWSTR subsystem, LPCWSTR service, HANDLE client, PPRIVILEGE_SET privileges, BOOL granted)
{
    UNICODE_STRING sub, svc;
    ustr(&sub, subsystem); ustr(&svc, service);
    return nt_result(NtPrivilegedServiceAuditAlarm(&sub, &svc, client, privileges, granted));
}
__declspec(dllexport) BOOL WINAPI ObjectOpenAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, LPSTR type_name, LPSTR object_name, PSECURITY_DESCRIPTOR sd, HANDLE client, DWORD desired, DWORD granted,
                                                       PPRIVILEGE_SET privileges, BOOL creation, BOOL access_granted, LPBOOL on_close)
{
    A2W(s, subsystem); A2W(t, type_name); A2W(n, object_name);
    BOOL r = ObjectOpenAuditAlarmW(s, handle_id, t, n, sd, client, desired, granted, privileges, creation, access_granted, on_close);
    DWORD e = GetLastError();
    free(s); free(t); free(n);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI ObjectCloseAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, BOOL on_close)
{
    A2W(s, subsystem);
    BOOL r = ObjectCloseAuditAlarmW(s, handle_id, on_close);
    DWORD e = GetLastError();
    free(s);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI ObjectDeleteAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, BOOL on_close)
{
    A2W(s, subsystem);
    BOOL r = ObjectDeleteAuditAlarmW(s, handle_id, on_close);
    DWORD e = GetLastError();
    free(s);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI ObjectPrivilegeAuditAlarmA(LPCSTR subsystem, LPVOID handle_id, HANDLE client, DWORD desired, PPRIVILEGE_SET privileges, BOOL granted)
{
    A2W(s, subsystem);
    BOOL r = ObjectPrivilegeAuditAlarmW(s, handle_id, client, desired, privileges, granted);
    DWORD e = GetLastError();
    free(s);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI PrivilegedServiceAuditAlarmA(LPCSTR subsystem, LPCSTR service, HANDLE client, PPRIVILEGE_SET privileges, BOOL granted)
{
    A2W(s, subsystem); A2W(v, service);
    BOOL r = PrivilegedServiceAuditAlarmW(s, v, client, privileges, granted);
    DWORD e = GetLastError();
    free(s); free(v);
    SetLastError(e);
    return r;
}

/* ---- trustees ----------------------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) VOID WINAPI BuildImpersonateTrusteeW(PTRUSTEE_W trustee, PTRUSTEE_W impersonate)
{
    if (!trustee) return;
    trustee->pMultipleTrustee = impersonate;
    trustee->MultipleTrusteeOperation = impersonate ? TRUSTEE_IS_IMPERSONATE : NO_MULTIPLE_TRUSTEE;
}
__declspec(dllexport) VOID WINAPI BuildImpersonateTrusteeA(PTRUSTEE_A trustee, PTRUSTEE_A impersonate)
{
    if (!trustee) return;
    trustee->pMultipleTrustee = impersonate;
    trustee->MultipleTrusteeOperation = impersonate ? TRUSTEE_IS_IMPERSONATE : NO_MULTIPLE_TRUSTEE;
}
__declspec(dllexport) VOID WINAPI BuildImpersonateExplicitAccessWithNameW(PEXPLICIT_ACCESS_W access, LPWSTR name, PTRUSTEE_W impersonate, DWORD permissions, ACCESS_MODE mode, DWORD inheritance)
{
    if (!access) return;
    BuildExplicitAccessWithNameW(access, name, permissions, mode, inheritance);
    BuildImpersonateTrusteeW(&access->Trustee, impersonate);
}
__declspec(dllexport) VOID WINAPI BuildImpersonateExplicitAccessWithNameA(PEXPLICIT_ACCESS_A access, LPSTR name, PTRUSTEE_A impersonate, DWORD permissions, ACCESS_MODE mode, DWORD inheritance)
{
    if (!access) return;
    BuildExplicitAccessWithNameA(access, name, permissions, mode, inheritance);
    BuildImpersonateTrusteeA(&access->Trustee, impersonate);
}
__declspec(dllexport) PTRUSTEE_W WINAPI GetMultipleTrusteeW(PTRUSTEE_W trustee) { return trustee ? trustee->pMultipleTrustee : NULL; }
__declspec(dllexport) PTRUSTEE_A WINAPI GetMultipleTrusteeA(PTRUSTEE_A trustee) { return trustee ? trustee->pMultipleTrustee : NULL; }
__declspec(dllexport) MULTIPLE_TRUSTEE_OPERATION WINAPI GetMultipleTrusteeOperationW(PTRUSTEE_W trustee) { return trustee ? trustee->MultipleTrusteeOperation : NO_MULTIPLE_TRUSTEE; }
__declspec(dllexport) MULTIPLE_TRUSTEE_OPERATION WINAPI GetMultipleTrusteeOperationA(PTRUSTEE_A trustee) { return trustee ? trustee->MultipleTrusteeOperation : NO_MULTIPLE_TRUSTEE; }

/* ---- string security descriptors with a domain ---------------------------------------------------------------------------------------------------------------- */

/* A domain's SIDs only matter to the domain-relative account SIDs in a string (DA, EA, ...): there is no domain here, so they are the same as the plain forms. */
__declspec(dllexport) BOOL WINAPI ConvertSDToStringSDDomainW(PSID domain, PSID root_domain, PSECURITY_DESCRIPTOR sd, DWORD revision, SECURITY_INFORMATION info, LPWSTR *out, PULONG length)
{
    (void)domain; (void)root_domain;
    return ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, revision, info, out, length);
}
__declspec(dllexport) BOOL WINAPI ConvertSDToStringSDRootDomainW(PSID root_domain, PSECURITY_DESCRIPTOR sd, DWORD revision, SECURITY_INFORMATION info, LPWSTR *out, PULONG length)
{
    (void)root_domain;
    return ConvertSecurityDescriptorToStringSecurityDescriptorW(sd, revision, info, out, length);
}
__declspec(dllexport) BOOL WINAPI ConvertSDToStringSDRootDomainA(PSID root_domain, PSECURITY_DESCRIPTOR sd, DWORD revision, SECURITY_INFORMATION info, LPSTR *out, PULONG length)
{
    (void)root_domain;
    return ConvertSecurityDescriptorToStringSecurityDescriptorA(sd, revision, info, out, length);
}
__declspec(dllexport) BOOL WINAPI ConvertStringSDToSDDomainW(PSID domain, PSID root_domain, LPCWSTR text, DWORD revision, PSECURITY_DESCRIPTOR *sd, PULONG size)
{
    (void)domain; (void)root_domain;
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(text, revision, sd, size);
}
__declspec(dllexport) BOOL WINAPI ConvertStringSDToSDDomainA(PSID domain, PSID root_domain, LPCSTR text, DWORD revision, PSECURITY_DESCRIPTOR *sd, PULONG size)
{
    (void)domain; (void)root_domain;
    return ConvertStringSecurityDescriptorToSecurityDescriptorA(text, revision, sd, size);
}
__declspec(dllexport) BOOL WINAPI ConvertStringSDToSDRootDomainW(PSID root_domain, LPCWSTR text, DWORD revision, PSECURITY_DESCRIPTOR *sd, PULONG size)
{
    (void)root_domain;
    return ConvertStringSecurityDescriptorToSecurityDescriptorW(text, revision, sd, size);
}
__declspec(dllexport) BOOL WINAPI ConvertStringSDToSDRootDomainA(PSID root_domain, LPCSTR text, DWORD revision, PSECURITY_DESCRIPTOR *sd, PULONG size)
{
    (void)root_domain;
    return ConvertStringSecurityDescriptorToSecurityDescriptorA(text, revision, sd, size);
}

/* ---- the tree forms ----------------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) DWORD WINAPI TreeSetNamedSecurityInfoA(LPSTR name, SE_OBJECT_TYPE type, SECURITY_INFORMATION info, PSID owner, PSID group, PACL dacl, PACL sacl, DWORD action,
                                                            FN_PROGRESS progress, PROG_INVOKE_SETTING setting, PVOID args)
{
    WCHAR *w = strdupAW(name);
    DWORD r;
    if (name && !w) return ERROR_NOT_ENOUGH_MEMORY;
    r = TreeSetNamedSecurityInfoW(w, type, info, owner, group, dacl, sacl, action, progress, setting, args);
    free(w);
    return r;
}
__declspec(dllexport) DWORD WINAPI TreeResetNamedSecurityInfoA(LPSTR name, SE_OBJECT_TYPE type, SECURITY_INFORMATION info, PSID owner, PSID group, PACL dacl, PACL sacl, BOOL keep_explicit,
                                                              FN_PROGRESS progress, PROG_INVOKE_SETTING setting, PVOID args)
{
    WCHAR *w = strdupAW(name);
    DWORD r;
    if (name && !w) return ERROR_NOT_ENOUGH_MEMORY;
    r = TreeResetNamedSecurityInfoW(w, type, info, owner, group, dacl, sacl, keep_explicit, progress, setting, args);
    free(w);
    return r;
}

/* ---- the access-control-list interface of Windows 2000 (ACTRL_ACCESS) ------------------------------------------------------------------------------------------ */
/* These answer ERROR_CALL_NOT_IMPLEMENTED: the interface was never filled in by the objects of Windows either (it works on a provider model of its own), and the ACL functions
 * above them (SetEntriesInAcl, GetNamedSecurityInfo, the SDDL functions) are what programs use. */

#define NOT_IMPLEMENTED(name, ...) __declspec(dllexport) DWORD WINAPI name(__VA_ARGS__) { return ERROR_CALL_NOT_IMPLEMENTED; }
NOT_IMPLEMENTED(ConvertAccessToSecurityDescriptorA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e)
NOT_IMPLEMENTED(ConvertAccessToSecurityDescriptorW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e)
NOT_IMPLEMENTED(ConvertSecurityDescriptorToAccessA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
NOT_IMPLEMENTED(ConvertSecurityDescriptorToAccessW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
NOT_IMPLEMENTED(ConvertSecurityDescriptorToAccessNamedA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
NOT_IMPLEMENTED(ConvertSecurityDescriptorToAccessNamedW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
NOT_IMPLEMENTED(GetAccessPermissionsForObjectA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
NOT_IMPLEMENTED(GetAccessPermissionsForObjectW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g)
NOT_IMPLEMENTED(SetEntriesInAccessListA, PVOID a, PVOID b, PVOID c, PVOID d)
NOT_IMPLEMENTED(SetEntriesInAccessListW, PVOID a, PVOID b, PVOID c, PVOID d)
NOT_IMPLEMENTED(SetEntriesInAuditListA, PVOID a, PVOID b, PVOID c, PVOID d)
NOT_IMPLEMENTED(SetEntriesInAuditListW, PVOID a, PVOID b, PVOID c, PVOID d)
NOT_IMPLEMENTED(TrusteeAccessToObjectA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f)
NOT_IMPLEMENTED(TrusteeAccessToObjectW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f)
NOT_IMPLEMENTED(CancelOverlappedAccess, PVOID a)
NOT_IMPLEMENTED(GetOverlappedAccessResults, PVOID a, PVOID b, PVOID c, PVOID d)
NOT_IMPLEMENTED(SetNamedSecurityInfoExA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g, PVOID h, PVOID i)
NOT_IMPLEMENTED(SetNamedSecurityInfoExW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g, PVOID h, PVOID i)
NOT_IMPLEMENTED(SetSecurityInfoExA, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g, PVOID h, PVOID i)
NOT_IMPLEMENTED(SetSecurityInfoExW, PVOID a, PVOID b, PVOID c, PVOID d, PVOID e, PVOID f, PVOID g, PVOID h, PVOID i)
__declspec(dllexport) DWORD WINAPI LookupSecurityDescriptorPartsA(PTRUSTEE_A *a, PTRUSTEE_A *b, PULONG c, PEXPLICIT_ACCESS_A *d, PULONG e, PEXPLICIT_ACCESS_A *f, PSECURITY_DESCRIPTOR g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return ERROR_CALL_NOT_IMPLEMENTED; }
__declspec(dllexport) DWORD WINAPI LookupSecurityDescriptorPartsW(PTRUSTEE_W *a, PTRUSTEE_W *b, PULONG c, PEXPLICIT_ACCESS_W *d, PULONG e, PEXPLICIT_ACCESS_W *f, PSECURITY_DESCRIPTOR g) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)f; (void)g; return ERROR_CALL_NOT_IMPLEMENTED; }
__declspec(dllexport) DWORD WINAPI GetInheritanceSourceA(LPSTR name, SE_OBJECT_TYPE type, SECURITY_INFORMATION info, BOOL container, GUID **guids, DWORD guid_count, PACL acl, PFN_OBJECT_MGR_FUNCTS functions, PGENERIC_MAPPING mapping, PINHERITED_FROMA out) { (void)name; (void)type; (void)info; (void)container; (void)guids; (void)guid_count; (void)acl; (void)functions; (void)mapping; (void)out; return ERROR_CALL_NOT_IMPLEMENTED; }
__declspec(dllexport) DWORD WINAPI GetInheritanceSourceW(LPWSTR name, SE_OBJECT_TYPE type, SECURITY_INFORMATION info, BOOL container, GUID **guids, DWORD guid_count, PACL acl, PFN_OBJECT_MGR_FUNCTS functions, PGENERIC_MAPPING mapping, PINHERITED_FROMW out) { (void)name; (void)type; (void)info; (void)container; (void)guids; (void)guid_count; (void)acl; (void)functions; (void)mapping; (void)out; return ERROR_CALL_NOT_IMPLEMENTED; }
__declspec(dllexport) DWORD WINAPI FreeInheritedFromArray(PINHERITED_FROMW array, USHORT count, PFN_OBJECT_MGR_FUNCTS functions) { (void)array; (void)count; (void)functions; return ERROR_SUCCESS; }

/* ---- logging on ---------------------------------------------------------------------------------------------------------------------------------------------- */

/* This OS has one local account (the user GetUserName tells) and it has no password, so that is the only logon that works - with an empty password. Others fail the way a wrong
 * password does. The token is a copy of the calling process's own. */
static BOOL local_account(LPCWSTR user, LPCWSTR domain, LPCWSTR password)
{
    WCHAR me[UNLEN + 1], machine[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = UNLEN + 1, m = MAX_COMPUTERNAME_LENGTH + 1;
    if (!user || !GetUserNameW(me, &n)) return FALSE;
    if (wcsicmp(user, me)) return FALSE;
    if (domain && *domain && wcscmp(domain, L".") && !(GetComputerNameW(machine, &m) && !wcsicmp(domain, machine))) return FALSE;
    return !password || !*password;
}
__declspec(dllexport) BOOL WINAPI LogonUserExExW(LPCWSTR user, LPCWSTR domain, LPCWSTR password, DWORD logon_type, DWORD provider, PTOKEN_GROUPS groups, PHANDLE token_out, PSID *logon_sid,
                                                PVOID *profile, LPDWORD profile_length, PQUOTA_LIMITS quotas)
{
    HANDLE process_token = NULL, result = NULL;
    BOOL ok;
    (void)groups;
    if (logon_sid) *logon_sid = NULL;
    if (profile) *profile = NULL;
    if (profile_length) *profile_length = 0;
    if (quotas) memset(quotas, 0, sizeof(*quotas));
    if (token_out) *token_out = NULL;
    if (logon_type < LOGON32_LOGON_INTERACTIVE || logon_type > LOGON32_LOGON_NEW_CREDENTIALS || logon_type == 4 || logon_type == 6 || provider > LOGON32_PROVIDER_WINNT50) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    if (!token_out && !profile) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!local_account(user, domain, password)) { SetLastError(ERROR_LOGON_FAILURE); return FALSE; }
    if (!token_out) return TRUE;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process_token)) return FALSE;
    ok = DuplicateTokenEx(process_token, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, logon_type == LOGON32_LOGON_NETWORK ? TokenImpersonation : TokenPrimary, &result);
    CloseHandle(process_token);
    if (ok) *token_out = result;
    return ok;
}
__declspec(dllexport) BOOL WINAPI LogonUserExW(LPCWSTR user, LPCWSTR domain, LPCWSTR password, DWORD logon_type, DWORD provider, PHANDLE token, PSID *logon_sid, PVOID *profile, LPDWORD profile_length,
                                              PQUOTA_LIMITS quotas)
{
    return LogonUserExExW(user, domain, password, logon_type, provider, NULL, token, logon_sid, profile, profile_length, quotas);
}
__declspec(dllexport) BOOL WINAPI LogonUserW(LPCWSTR user, LPCWSTR domain, LPCWSTR password, DWORD logon_type, DWORD provider, PHANDLE token)
{
    if (!token) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    return LogonUserExExW(user, domain, password, logon_type, provider, NULL, token, NULL, NULL, NULL, NULL);
}
__declspec(dllexport) BOOL WINAPI LogonUserA(LPCSTR user, LPCSTR domain, LPCSTR password, DWORD logon_type, DWORD provider, PHANDLE token)
{
    A2W(u, user); A2W(d, domain); A2W(p, password);
    BOOL r = LogonUserW(u, d, p, logon_type, provider, token);
    DWORD e = GetLastError();
    free(u); free(d); free(p);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI LogonUserExA(LPCSTR user, LPCSTR domain, LPCSTR password, DWORD logon_type, DWORD provider, PHANDLE token, PSID *logon_sid, PVOID *profile, LPDWORD profile_length,
                                              PQUOTA_LIMITS quotas)
{
    A2W(u, user); A2W(d, domain); A2W(p, password);
    BOOL r = LogonUserExW(u, d, p, logon_type, provider, token, logon_sid, profile, profile_length, quotas);
    DWORD e = GetLastError();
    free(u); free(d); free(p);
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI ImpersonateAnonymousToken(HANDLE thread)
{
    return nt_result(NtImpersonateAnonymousToken(thread));
}

/* ---- shutting down ------------------------------------------------------------------------------------------------------------------------------------------- */

typedef struct { DWORD seconds; ULONG action; } PendingShutdown;
static volatile LONG g_shutdown_pending;
static volatile LONG g_shutdown_abort;

static BOOL shutdown_privilege(void)
{
    PRIVILEGE_SET set;
    HANDLE token;
    BOOL held = FALSE;
    if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) && !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return FALSE;
    memset(&set, 0, sizeof(set));
    set.PrivilegeCount = 1;
    set.Privilege[0].Luid.LowPart = 19;      /* SeShutdownPrivilege */
    if (!PrivilegeCheck(token, &set, &held)) held = FALSE;
    CloseHandle(token);
    return held;
}
static DWORD WINAPI shutdown_thread(LPVOID parameter)
{
    PendingShutdown *p = parameter;
    DWORD waited;
    for (waited = 0; waited < p->seconds * 10 && !g_shutdown_abort; ++waited) Sleep(100);
    if (!g_shutdown_abort) NtShutdownSystem(p->action);
    InterlockedExchange(&g_shutdown_pending, 0);
    free(p);
    return 0;
}
static DWORD start_shutdown(LPCWSTR machine, DWORD timeout, ULONG action)
{
    WCHAR name[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    PendingShutdown *p;
    HANDLE t;
    if (machine && *machine) {
        LPCWSTR m = machine[0] == '\\' && machine[1] == '\\' ? machine + 2 : machine;
        if (!GetComputerNameW(name, &n) || (wcsicmp(m, name) && wcsicmp(m, L"localhost"))) return ERROR_BAD_NETPATH;
    }
    if (!shutdown_privilege()) return ERROR_ACCESS_DENIED;
    if (InterlockedCompareExchange(&g_shutdown_pending, 1, 0)) return ERROR_SHUTDOWN_IN_PROGRESS;
    InterlockedExchange(&g_shutdown_abort, 0);
    if (timeout == 0) {
        NTSTATUS status = NtShutdownSystem(action);
        InterlockedExchange(&g_shutdown_pending, 0);
        return NT_SUCCESS(status) ? ERROR_SUCCESS : RtlNtStatusToDosError(status);
    }
    if (!(p = malloc(sizeof(*p)))) { InterlockedExchange(&g_shutdown_pending, 0); return ERROR_NOT_ENOUGH_MEMORY; }
    p->seconds = timeout;
    p->action = action;
    if (!(t = CreateThread(NULL, 0, shutdown_thread, p, 0, NULL))) { DWORD e = GetLastError(); free(p); InterlockedExchange(&g_shutdown_pending, 0); return e; }
    CloseHandle(t);
    return ERROR_SUCCESS;
}
static BOOL shutdown_result(DWORD error)
{
    if (error) SetLastError(error);
    return error == ERROR_SUCCESS;
}
__declspec(dllexport) BOOL WINAPI InitiateSystemShutdownExW(LPWSTR machine, LPWSTR message, DWORD timeout, BOOL force, BOOL reboot, DWORD reason)
{
    (void)message; (void)force; (void)reason;
    return shutdown_result(start_shutdown(machine, timeout, reboot ? 1 : 2));
}
__declspec(dllexport) BOOL WINAPI InitiateSystemShutdownW(LPWSTR machine, LPWSTR message, DWORD timeout, BOOL force, BOOL reboot)
{
    return InitiateSystemShutdownExW(machine, message, timeout, force, reboot, 0);
}
__declspec(dllexport) BOOL WINAPI InitiateSystemShutdownExA(LPSTR machine, LPSTR message, DWORD timeout, BOOL force, BOOL reboot, DWORD reason)
{
    A2W(m, machine);
    BOOL r = InitiateSystemShutdownExW(m, NULL, timeout, force, reboot, reason);
    DWORD e = GetLastError();
    free(m);
    (void)message;
    SetLastError(e);
    return r;
}
__declspec(dllexport) BOOL WINAPI InitiateSystemShutdownA(LPSTR machine, LPSTR message, DWORD timeout, BOOL force, BOOL reboot) { return InitiateSystemShutdownExA(machine, message, timeout, force, reboot, 0); }
__declspec(dllexport) DWORD WINAPI InitiateShutdownW(LPWSTR machine, LPWSTR message, DWORD grace, DWORD flags, DWORD reason)
{
    (void)message; (void)reason;
    if (flags & ~0x3Fu) return ERROR_INVALID_PARAMETER;
    return start_shutdown(machine, grace, (flags & 4 /* SHUTDOWN_RESTART */) ? 1 : 2);
}
__declspec(dllexport) DWORD WINAPI InitiateShutdownA(LPSTR machine, LPSTR message, DWORD grace, DWORD flags, DWORD reason)
{
    A2W(m, machine);
    DWORD r = InitiateShutdownW(m, NULL, grace, flags, reason);
    free(m);
    (void)message;
    return r;
}
__declspec(dllexport) BOOL WINAPI AbortSystemShutdownW(LPWSTR machine)
{
    WCHAR name[MAX_COMPUTERNAME_LENGTH + 2];
    DWORD n = MAX_COMPUTERNAME_LENGTH + 1;
    if (machine && *machine) {
        LPCWSTR m = machine[0] == '\\' && machine[1] == '\\' ? machine + 2 : machine;
        if (!GetComputerNameW(name, &n) || (wcsicmp(m, name) && wcsicmp(m, L"localhost"))) return shutdown_result(ERROR_BAD_NETPATH);
    }
    if (!shutdown_privilege()) return shutdown_result(ERROR_ACCESS_DENIED);
    if (!g_shutdown_pending) return shutdown_result(ERROR_NO_SHUTDOWN_IN_PROGRESS);
    InterlockedExchange(&g_shutdown_abort, 1);
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI AbortSystemShutdownA(LPSTR machine)
{
    A2W(m, machine);
    BOOL r = AbortSystemShutdownW(m);
    DWORD e = GetLastError();
    free(m);
    SetLastError(e);
    return r;
}
