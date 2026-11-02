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
/* advapi32.dll: access tokens, impersonation, the access check and the security of kernel objects and files, over the services of ntdll.dll. Included by kbsec.c. */

NTSTATUS NTAPI NtSetInformationThread(HANDLE, THREADINFOCLASS, PVOID, ULONG);

static DWORD token_error(NTSTATUS status)
{
    if (status == STATUS_NO_TOKEN) return ERROR_NO_TOKEN;
    return RtlNtStatusToDosError(status);
}
static BOOL token_fail(NTSTATUS status)
{
    SetLastError(token_error(status));
    return FALSE;
}

EXP BOOL WINAPI OpenProcessToken(HANDLE process, DWORD access, PHANDLE token)
{
    NTSTATUS status = NtOpenProcessToken(process, access, token);
    return status ? token_fail(status) : TRUE;
}
EXP BOOL WINAPI OpenThreadToken(HANDLE thread, DWORD access, BOOL self, PHANDLE token)
{
    NTSTATUS status = NtOpenThreadToken(thread, access, self, token);
    return status ? token_fail(status) : TRUE;
}
EXP BOOL WINAPI GetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS class_, LPVOID info, DWORD length, LPDWORD returned)
{
    DWORD dummy;
    NTSTATUS status = NtQueryInformationToken(token, class_, info, length, returned ? returned : &dummy);
    return status ? token_fail(status) : TRUE;
}
EXP BOOL WINAPI SetTokenInformation(HANDLE token, TOKEN_INFORMATION_CLASS class_, LPVOID info, DWORD length)
{
    NTSTATUS status = NtSetInformationToken(token, class_, info, length);
    return status ? token_fail(status) : TRUE;
}

EXP BOOL WINAPI DuplicateTokenEx(HANDLE token, DWORD access, LPSECURITY_ATTRIBUTES sa, SECURITY_IMPERSONATION_LEVEL level, TOKEN_TYPE type, PHANDLE out)
{
    OBJECT_ATTRIBUTES attr;
    SECURITY_QUALITY_OF_SERVICE qos;
    NTSTATUS status;
    qos.Length = sizeof(qos);
    qos.ImpersonationLevel = level;
    qos.ContextTrackingMode = SECURITY_DYNAMIC_TRACKING;
    qos.EffectiveOnly = FALSE;
    InitializeObjectAttributes(&attr, NULL, sa && sa->bInheritHandle ? OBJ_INHERIT : 0, NULL, sa ? sa->lpSecurityDescriptor : NULL);
    attr.SecurityQualityOfService = &qos;
    status = NtDuplicateToken(token, access, &attr, FALSE, type, out);
    return status ? token_fail(status) : TRUE;
}
EXP BOOL WINAPI DuplicateToken(HANDLE token, SECURITY_IMPERSONATION_LEVEL level, PHANDLE out)
{
    return DuplicateTokenEx(token, TOKEN_IMPERSONATE | TOKEN_QUERY, NULL, level, TokenImpersonation, out);
}

EXP BOOL WINAPI AdjustTokenPrivileges(HANDLE token, BOOL disable_all, PTOKEN_PRIVILEGES state, DWORD length, PTOKEN_PRIVILEGES previous, PDWORD returned)
{
    DWORD dummy;
    NTSTATUS status = NtAdjustPrivilegesToken(token, disable_all, state, length, previous, returned ? returned : &dummy);
    SetLastError(RtlNtStatusToDosError(status));
    return status == STATUS_SUCCESS || status == STATUS_NOT_ALL_ASSIGNED;
}
EXP BOOL WINAPI AdjustTokenGroups(HANDLE token, BOOL reset, PTOKEN_GROUPS state, DWORD length, PTOKEN_GROUPS previous, PDWORD returned)
{
    DWORD dummy;
    NTSTATUS status = NtAdjustGroupsToken(token, reset, state, length, previous, returned ? returned : &dummy);
    SetLastError(RtlNtStatusToDosError(status));
    return status == STATUS_SUCCESS || status == STATUS_NOT_ALL_ASSIGNED;
}

EXP BOOL WINAPI PrivilegeCheck(HANDLE token, PPRIVILEGE_SET set, LPBOOL result)
{
    BOOLEAN r;
    NTSTATUS status = NtPrivilegeCheck(token, set, &r);
    if (status) return token_fail(status);
    *result = r;
    return TRUE;
}

EXP BOOL WINAPI IsTokenRestricted(HANDLE token)
{
    DWORD size = 0;
    TOKEN_GROUPS *groups;
    BOOL restricted;
    if (!GetTokenInformation(token, TokenRestrictedSids, NULL, 0, &size) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) return FALSE;
    groups = malloc(size);
    if (!groups) return FALSE;
    if (!GetTokenInformation(token, TokenRestrictedSids, groups, size, &size)) { free(groups); return FALSE; }
    restricted = groups->GroupCount > 0;
    free(groups);
    return restricted;
}
EXP BOOL WINAPI IsTokenUntrusted(HANDLE token) { (void)token; return FALSE; }

EXP BOOL WINAPI CreateRestrictedToken(HANDLE token, DWORD flags, DWORD disable_count, PSID_AND_ATTRIBUTES disable, DWORD delete_count, PLUID_AND_ATTRIBUTES delete_privileges,
    DWORD restrict_count, PSID_AND_ATTRIBUTES restrict_sids, PHANDLE out)
{
    TOKEN_GROUPS *disable_groups = NULL, *restrict_groups = NULL;
    TOKEN_PRIVILEGES *privileges = NULL;
    NTSTATUS status;
    DWORD i;
    if (disable_count) {
        disable_groups = malloc(sizeof(TOKEN_GROUPS) + disable_count * sizeof(SID_AND_ATTRIBUTES));
        if (!disable_groups) return win_fail(ERROR_NOT_ENOUGH_MEMORY);
        disable_groups->GroupCount = disable_count;
        for (i = 0; i < disable_count; ++i) disable_groups->Groups[i] = disable[i];
    }
    if (restrict_count) {
        restrict_groups = malloc(sizeof(TOKEN_GROUPS) + restrict_count * sizeof(SID_AND_ATTRIBUTES));
        if (!restrict_groups) { free(disable_groups); return win_fail(ERROR_NOT_ENOUGH_MEMORY); }
        restrict_groups->GroupCount = restrict_count;
        for (i = 0; i < restrict_count; ++i) restrict_groups->Groups[i] = restrict_sids[i];
    }
    if (delete_count) {
        privileges = malloc(sizeof(TOKEN_PRIVILEGES) + delete_count * sizeof(LUID_AND_ATTRIBUTES));
        if (!privileges) { free(disable_groups); free(restrict_groups); return win_fail(ERROR_NOT_ENOUGH_MEMORY); }
        privileges->PrivilegeCount = delete_count;
        for (i = 0; i < delete_count; ++i) privileges->Privileges[i] = delete_privileges[i];
    }
    status = NtFilterToken(token, flags, disable_groups, privileges, restrict_groups, out);
    free(disable_groups);
    free(restrict_groups);
    free(privileges);
    return status ? token_fail(status) : TRUE;
}

/* ---- impersonation ------------------------------------------------------------------------------------------------------------------------------------ */

static BOOL set_thread_token(HANDLE thread, HANDLE token)
{
    NTSTATUS status = NtSetInformationThread(thread ? thread : GetCurrentThread(), ThreadImpersonationToken, &token, sizeof(token));
    return status ? token_fail(status) : TRUE;
}
EXP BOOL WINAPI SetThreadToken(PHANDLE thread, HANDLE token)
{
    HANDLE target = thread ? *thread : GetCurrentThread(), duplicate = NULL;
    BOOL ret;
    if (token) {
        DWORD type = 0, size = sizeof(type);
        if (GetTokenInformation(token, TokenType, &type, sizeof(type), &size) && type == TokenPrimary) {
            if (!DuplicateTokenEx(token, TOKEN_IMPERSONATE | TOKEN_QUERY, NULL, SecurityImpersonation, TokenImpersonation, &duplicate)) return FALSE;
            token = duplicate;
        }
    }
    ret = set_thread_token(target, token);
    if (duplicate) CloseHandle(duplicate);
    return ret;
}
EXP BOOL WINAPI RevertToSelf(void) { return set_thread_token(GetCurrentThread(), NULL); }
EXP BOOL WINAPI ImpersonateSelf(SECURITY_IMPERSONATION_LEVEL level)
{
    NTSTATUS status = RtlImpersonateSelf(level);
    return status ? token_fail(status) : TRUE;
}
EXP BOOL WINAPI ImpersonateLoggedOnUser(HANDLE token)
{
    HANDLE copy = NULL;
    DWORD type = 0, size = sizeof(type);
    BOOL ret;
    if (!GetTokenInformation(token, TokenType, &type, sizeof(type), &size)) return FALSE;
    if (type == TokenPrimary) { if (!DuplicateTokenEx(token, TOKEN_IMPERSONATE | TOKEN_QUERY, NULL, SecurityImpersonation, TokenImpersonation, &copy)) return FALSE; token = copy; }
    ret = set_thread_token(GetCurrentThread(), token);
    if (copy) CloseHandle(copy);
    return ret;
}

EXP BOOL WINAPI CheckTokenMembership(HANDLE token, PSID sid, PBOOL is_member)
{
    HANDLE own = NULL;
    DWORD size = 0, type = 0, i;
    TOKEN_GROUPS *groups;
    TOKEN_USER *user;
    BOOL member = FALSE, restricted = FALSE;
    if (!sid || !is_member) return win_fail(ERROR_INVALID_PARAMETER);
    if (!RtlValidSid(sid)) return win_fail(ERROR_INVALID_SID);
    if (!token) {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY | TOKEN_DUPLICATE, TRUE, &own)) {
            HANDLE process;
            if (GetLastError() != ERROR_NO_TOKEN) return FALSE;
            if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process)) return FALSE;
            if (!DuplicateTokenEx(process, TOKEN_QUERY, NULL, SecurityImpersonation, TokenImpersonation, &own)) { CloseHandle(process); return FALSE; }
            CloseHandle(process);
        }
        token = own;
    } else {
        size = sizeof(type);
        if (!GetTokenInformation(token, TokenType, &type, sizeof(type), &size)) return FALSE;
        if (type != TokenImpersonation) return win_fail(ERROR_NO_IMPERSONATION_TOKEN);
    }
    size = 0;
    GetTokenInformation(token, TokenGroups, NULL, 0, &size);
    groups = malloc(size);
    if (groups && GetTokenInformation(token, TokenGroups, groups, size, &size)) {
        for (i = 0; i < groups->GroupCount; ++i)
            if ((groups->Groups[i].Attributes & SE_GROUP_ENABLED) && !(groups->Groups[i].Attributes & SE_GROUP_USE_FOR_DENY_ONLY) && RtlEqualSid(groups->Groups[i].Sid, sid)) member = TRUE;
    }
    free(groups);
    size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    user = malloc(size);
    if (user && GetTokenInformation(token, TokenUser, user, size, &size) && RtlEqualSid(user->User.Sid, sid) && !(user->User.Attributes & SE_GROUP_USE_FOR_DENY_ONLY)) member = TRUE;
    free(user);
    (void)restricted;
    if (own) CloseHandle(own);
    *is_member = member;
    return TRUE;
}

/* ---- the access check ----------------------------------------------------------------------------------------------------------------------------------- */

EXP BOOL WINAPI AccessCheck(PSECURITY_DESCRIPTOR sd, HANDLE token, DWORD desired, PGENERIC_MAPPING mapping, PPRIVILEGE_SET privileges, LPDWORD privileges_length, LPDWORD granted, LPBOOL status_out)
{
    NTSTATUS access_status, status;
    status = NtAccessCheck(sd, token, desired, mapping, privileges, privileges_length, granted, &access_status);
    if (status) return nt_fail(status);
    *status_out = access_status == STATUS_SUCCESS;
    if (!*status_out) SetLastError(RtlNtStatusToDosError(access_status));
    return TRUE;
}
EXP BOOL WINAPI AccessCheckByType(PSECURITY_DESCRIPTOR sd, PSID principal, HANDLE token, DWORD desired, POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping,
    PPRIVILEGE_SET privileges, LPDWORD privileges_length, LPDWORD granted, LPBOOL status_out)
{
    NTSTATUS access_status, status;
    status = NtAccessCheckByType(sd, principal, token, desired, types, type_count, mapping, privileges, privileges_length, granted, &access_status);
    if (status) return nt_fail(status);
    *status_out = access_status == STATUS_SUCCESS;
    if (!*status_out) SetLastError(RtlNtStatusToDosError(access_status));
    return TRUE;
}
EXP BOOL WINAPI AccessCheckByTypeResultList(PSECURITY_DESCRIPTOR sd, PSID principal, HANDLE token, DWORD desired, POBJECT_TYPE_LIST types, DWORD type_count, PGENERIC_MAPPING mapping,
    PPRIVILEGE_SET privileges, LPDWORD privileges_length, LPDWORD granted, LPDWORD status_out)
{
    NTSTATUS status = NtAccessCheckByTypeResultList(sd, principal, token, desired, types, type_count, mapping, privileges, privileges_length, (PACCESS_MASK)granted, (PNTSTATUS)status_out);
    if (status) return nt_fail(status);
    return TRUE;
}

/* ---- the security of objects and files ------------------------------------------------------------------------------------------------------------------- */

EXP BOOL WINAPI GetKernelObjectSecurity(HANDLE handle, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd, DWORD length, LPDWORD needed)
{
    NTSTATUS status = NtQuerySecurityObject(handle, info, sd, length, needed);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI SetKernelObjectSecurity(HANDLE handle, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd)
{
    NTSTATUS status = NtSetSecurityObject(handle, info, sd);
    return status ? nt_fail(status) : TRUE;
}

static DWORD file_security_access(SECURITY_INFORMATION info, BOOL set)
{
    DWORD access = 0;
    if (set) {
        if (info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION)) access |= WRITE_OWNER;
        if (info & DACL_SECURITY_INFORMATION) access |= WRITE_DAC;
        if (info & SACL_SECURITY_INFORMATION) access |= ACCESS_SYSTEM_SECURITY;
    } else {
        if (info & (OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION)) access |= READ_CONTROL;
        if (info & SACL_SECURITY_INFORMATION) access |= ACCESS_SYSTEM_SECURITY;
    }
    return access;
}
EXP BOOL WINAPI GetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd, DWORD length, LPDWORD needed)
{
    HANDLE file = CreateFileW(name, file_security_access(info, FALSE), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BOOL ret;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = GetKernelObjectSecurity(file, info, sd, length, needed);
    CloseHandle(file);
    return ret;
}
EXP BOOL WINAPI SetFileSecurityW(LPCWSTR name, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR sd)
{
    HANDLE file = CreateFileW(name, file_security_access(info, TRUE), FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    BOOL ret;
    if (file == INVALID_HANDLE_VALUE) return FALSE;
    ret = SetKernelObjectSecurity(file, info, sd);
    CloseHandle(file);
    return ret;
}

EXP BOOL WINAPI CreatePrivateObjectSecurityEx(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *out, GUID *type, BOOL container, ULONG flags,
    HANDLE token, PGENERIC_MAPPING mapping)
{
    NTSTATUS status = RtlNewSecurityObjectEx(parent, creator, out, type, container, flags, token, mapping);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI CreatePrivateObjectSecurity(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *out, BOOL container, HANDLE token, PGENERIC_MAPPING mapping)
{
    return CreatePrivateObjectSecurityEx(parent, creator, out, NULL, container, 0, token, mapping);
}
EXP BOOL WINAPI CreatePrivateObjectSecurityWithMultipleInheritance(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *out, GUID **types, ULONG count,
    BOOL container, ULONG flags, HANDLE token, PGENERIC_MAPPING mapping)
{
    (void)types; (void)count;
    return CreatePrivateObjectSecurityEx(parent, creator, out, NULL, container, flags, token, mapping);
}
EXP BOOL WINAPI SetPrivateObjectSecurityEx(SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR modification, PSECURITY_DESCRIPTOR *object, ULONG flags, PGENERIC_MAPPING mapping, HANDLE token)
{
    NTSTATUS status;
    (void)flags;
    status = RtlSetSecurityObject(info, modification, object, mapping, token);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI SetPrivateObjectSecurity(SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR modification, PSECURITY_DESCRIPTOR *object, PGENERIC_MAPPING mapping, HANDLE token)
{
    return SetPrivateObjectSecurityEx(info, modification, object, 0, mapping, token);
}
EXP BOOL WINAPI GetPrivateObjectSecurity(PSECURITY_DESCRIPTOR object, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR result, DWORD length, PDWORD needed)
{
    NTSTATUS status = RtlQuerySecurityObject(object, info, result, length, needed);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI DestroyPrivateObjectSecurity(PSECURITY_DESCRIPTOR *object)
{
    NTSTATUS status = RtlDeleteSecurityObject(object);
    return status ? nt_fail(status) : TRUE;
}
EXP BOOL WINAPI ConvertToAutoInheritPrivateObjectSecurity(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR current, PSECURITY_DESCRIPTOR *out, GUID *type, BOOLEAN directory, PGENERIC_MAPPING mapping)
{
    NTSTATUS status = RtlConvertToAutoInheritSecurityObject(parent, current, out, type, directory, mapping);
    return status ? nt_fail(status) : TRUE;
}

/* ---- a process that runs as another token -------------------------------------------------------------------------------------------------------------- */

#include "../tcos_token.h"
NTSTATUS NTAPI NtTcosToken(ULONG op, ULONG_PTR a, ULONG_PTR b, ULONG_PTR c, ULONG_PTR d, ULONG_PTR e);

/* The token must be a real handle with the right to be assigned and to be queried. */
static BOOL check_process_token(HANDLE token)
{
    ULONG basic[14];
    if ((LONG_PTR)token >= -6 && (LONG_PTR)token <= -4) return win_fail(ERROR_INVALID_HANDLE);
    if (NtQueryObject(token, 0, basic, sizeof(basic), NULL)) return win_fail(ERROR_INVALID_HANDLE);
    if ((basic[1] & (TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY)) != (TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY)) return win_fail(ERROR_ACCESS_DENIED);
    return TRUE;
}

/* The process is made suspended, given the token, and runs on (or stays suspended if it was asked to). */
static BOOL give_process_token(HANDLE token, DWORD flags, PROCESS_INFORMATION *pi)
{
    NTSTATUS status = NtTcosToken(TCOS_TOKEN_SET_PROCESS, (ULONG_PTR)pi->hProcess, (ULONG_PTR)token, 0, 0, 0);
    if (status) {
        DWORD error = RtlNtStatusToDosError(status);
        TerminateProcess(pi->hProcess, 1);
        CloseHandle(pi->hProcess);
        CloseHandle(pi->hThread);
        SetLastError(error);
        return FALSE;
    }
    if (!(flags & CREATE_SUSPENDED)) ResumeThread(pi->hThread);
    return TRUE;
}
EXP BOOL WINAPI CreateProcessAsUserW(HANDLE token, LPCWSTR application, LPWSTR command, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit,
    DWORD flags, LPVOID environment, LPCWSTR directory, LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information)
{
    PROCESS_INFORMATION local;
    if (!information) information = &local;
    if (!token) return CreateProcessW(application, command, process_attributes, thread_attributes, inherit, flags, environment, directory, startup, information);
    if (!check_process_token(token)) return FALSE;
    if (!CreateProcessW(application, command, process_attributes, thread_attributes, inherit, flags | CREATE_SUSPENDED, environment, directory, startup, information)) return FALSE;
    return give_process_token(token, flags, information);
}
EXP BOOL WINAPI CreateProcessAsUserA(HANDLE token, LPCSTR application, LPSTR command, LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes, BOOL inherit,
    DWORD flags, LPVOID environment, LPCSTR directory, LPSTARTUPINFOA startup, LPPROCESS_INFORMATION information)
{
    PROCESS_INFORMATION local;
    if (!information) information = &local;
    if (!token) return CreateProcessA(application, command, process_attributes, thread_attributes, inherit, flags, environment, directory, startup, information);
    if (!check_process_token(token)) return FALSE;
    if (!CreateProcessA(application, command, process_attributes, thread_attributes, inherit, flags | CREATE_SUSPENDED, environment, directory, startup, information)) return FALSE;
    return give_process_token(token, flags, information);
}

/* ImpersonateNamedPipeClient: the server thread takes on the identity of the client of the pipe. Every program of this system runs as the one user, so that identity is the token of
 * this process (the clients' own tokens are not passed along the connection); the call is refused for what is not a pipe that has a client. */
EXP BOOL WINAPI ImpersonateNamedPipeClient(HANDLE pipe)
{
    DWORD available;
    HANDLE process_token, copy;
    BOOL ret;
    if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL)) {
        DWORD error = GetLastError();
        if (error == ERROR_INVALID_HANDLE) return FALSE;
        if (error != ERROR_BROKEN_PIPE) return FALSE;
    }
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &process_token)) return FALSE;
    ret = DuplicateTokenEx(process_token, TOKEN_ALL_ACCESS, NULL, SecurityImpersonation, TokenImpersonation, &copy);
    CloseHandle(process_token);
    if (!ret) return FALSE;
    ret = set_thread_token(GetCurrentThread(), copy);
    CloseHandle(copy);
    return ret;
}
