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

/* advapi32.dll: what is left - the hardware profile, idle tasks, and the groups of exports that stand for services this OS does not have (file encryption, WMI, the remote registry
 * and service control interfaces, software installation by policy, Safer / code authorization levels, the DES-based password hash functions). An export that cannot do its work
 * says so with an error, never with success: ERROR_NOT_SUPPORTED, or the error the real one gives when the service it asks is not there. */
#define WIN32_LEAN_AND_MEAN
#define NOCRYPT
#include <windows.h>
#include <winnt.h>

/* (the label is the export: the headers of windows.h declare some of these with types of their own, which these generic stand-ins do not repeat) */
#define STUB(ret, name, ...) ret WINAPI impl_##name(__VA_ARGS__) __asm__(#name); __declspec(dllexport) ret WINAPI impl_##name(__VA_ARGS__)
#define NOT_SUPPORTED_DWORD(name, ...) STUB(DWORD, name, __VA_ARGS__) { return ERROR_NOT_SUPPORTED; }
#define NOT_SUPPORTED_BOOL(name, ...) STUB(BOOL, name, __VA_ARGS__) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
#define NOT_IMPLEMENTED_DWORD(name, ...) STUB(DWORD, name, __VA_ARGS__) { return ERROR_CALL_NOT_IMPLEMENTED; }
#define NOT_SUPPORTED_NT(name, ...) STUB(LONG, name, __VA_ARGS__) { return (LONG)0xC00000BB; }      /* STATUS_NOT_SUPPORTED */
#define WMI_NOT_FOUND(name, ...) STUB(ULONG, name, __VA_ARGS__) { return ERROR_WMI_GUID_NOT_FOUND; }
typedef void *P;

/* ---- the hardware profile ---------------------------------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) BOOL WINAPI GetCurrentHwProfileW(LPHW_PROFILE_INFOW info)
{
    static const WCHAR guid[] = L"{846ee340-7039-11de-9d20-806e6f6e6963}";      /* the profile every installation has */
    static const WCHAR name[] = L"Profile 1";
    if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    info->dwDockInfo = DOCKINFO_DOCKED | DOCKINFO_UNDOCKED ? DOCKINFO_UNDOCKED | DOCKINFO_USER_UNDOCKED : 0;
    memcpy(info->szHwProfileGuid, guid, sizeof(guid));
    memcpy(info->szHwProfileName, name, sizeof(name));
    return TRUE;
}
__declspec(dllexport) BOOL WINAPI GetCurrentHwProfileA(LPHW_PROFILE_INFOA info)
{
    HW_PROFILE_INFOW w;
    if (!info) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    if (!GetCurrentHwProfileW(&w)) return FALSE;
    info->dwDockInfo = w.dwDockInfo;
    WideCharToMultiByte(CP_ACP, 0, w.szHwProfileGuid, -1, info->szHwProfileGuid, HW_PROFILE_GUIDLEN, NULL, NULL);
    WideCharToMultiByte(CP_ACP, 0, w.szHwProfileName, -1, info->szHwProfileName, MAX_PROFILE_LEN, NULL, NULL);
    return TRUE;
}

/* ---- idle tasks ------------------------------------------------------------------------------------------------------------------------------------------------------- */

/* There is no idle scheduler: a task that registers gets its two events (to start and to say it is done) and nothing ever starts it. */
__declspec(dllexport) DWORD WINAPI RegisterIdleTask(DWORD id, HANDLE *ready, HANDLE *done)
{
    if (!ready || !done) return ERROR_INVALID_PARAMETER;
    *ready = CreateEventW(NULL, FALSE, FALSE, NULL);
    *done = CreateEventW(NULL, FALSE, FALSE, NULL);
    (void)id;
    if (!*ready || !*done) { DWORD e = GetLastError(); if (*ready) CloseHandle(*ready); if (*done) CloseHandle(*done); *ready = *done = NULL; return e; }
    return ERROR_SUCCESS;
}
__declspec(dllexport) DWORD WINAPI UnregisterIdleTask(DWORD id, HANDLE ready, HANDLE done)
{
    (void)id;
    if (ready) CloseHandle(ready);
    if (done) CloseHandle(done);
    return ERROR_SUCCESS;
}
__declspec(dllexport) DWORD WINAPI ProcessIdleTasks(void) { return ERROR_SUCCESS; }
__declspec(dllexport) DWORD WINAPI ProcessIdleTasksW(void) { return ERROR_SUCCESS; }
__declspec(dllexport) DWORD WINAPI CheckForHiberboot(PBOOLEAN hiberboot, BOOLEAN clear)
{
    (void)clear;
    if (!hiberboot) return ERROR_INVALID_PARAMETER;
    *hiberboot = FALSE;
    return ERROR_SUCCESS;
}

/* ---- time zones kept by the registry ------------------------------------------------------------------------------------------------------------------------------- */

STUB(DWORD, EnumDynamicTimeZoneInformation, DWORD index, PVOID info) { (void)index; (void)info; return ERROR_NO_MORE_ITEMS; }
STUB(DWORD, GetDynamicTimeZoneInformationEffectiveYears, PVOID info, LPDWORD first, LPDWORD last)
{
    (void)info; (void)first; (void)last;
    return ERROR_FILE_NOT_FOUND;      /* (no time zone has dynamic daylight saving rules here) */
}

/* ---- encrypted files ------------------------------------------------------------------------------------------------------------------------------------------------ */

NOT_SUPPORTED_DWORD(OpenEncryptedFileRawA, P a, ULONG b, P c)
NOT_SUPPORTED_DWORD(OpenEncryptedFileRawW, P a, ULONG b, P c)
NOT_SUPPORTED_DWORD(ReadEncryptedFileRaw, P a, P b, P c)
NOT_SUPPORTED_DWORD(WriteEncryptedFileRaw, P a, P b, P c)
__declspec(dllexport) VOID WINAPI CloseEncryptedFileRaw(P context) { (void)context; }
NOT_SUPPORTED_DWORD(AddUsersToEncryptedFile, P a, P b)
NOT_SUPPORTED_DWORD(AddUsersToEncryptedFileEx, P a, ULONG b, P c, P d)
NOT_SUPPORTED_DWORD(RemoveUsersFromEncryptedFile, P a, P b)
NOT_SUPPORTED_DWORD(QueryUsersOnEncryptedFile, P a, P b)
NOT_SUPPORTED_DWORD(QueryRecoveryAgentsOnEncryptedFile, P a, P b)
NOT_SUPPORTED_DWORD(SetUserFileEncryptionKey, P a)
NOT_SUPPORTED_DWORD(SetUserFileEncryptionKeyEx, P a, ULONG b, ULONG c, P d)
NOT_SUPPORTED_DWORD(DuplicateEncryptionInfoFile, P a, P b, ULONG c, ULONG d, P e)
NOT_SUPPORTED_DWORD(EncryptionDisable, P a, BOOL b)
NOT_SUPPORTED_DWORD(GetEncryptedFileMetadata, P a, P b, P c)
NOT_SUPPORTED_DWORD(SetEncryptedFileMetadata, P a, P b, P c, P d, ULONG e)
NOT_SUPPORTED_DWORD(EncryptedFileKeyInfo, P a, ULONG b, P c)
NOT_SUPPORTED_BOOL(FlushEfsCache, void)
__declspec(dllexport) VOID WINAPI FreeEncryptionCertificateHashList(P list) { (void)list; }
__declspec(dllexport) VOID WINAPI FreeEncryptedFileKeyInfo(P info) { (void)info; }
__declspec(dllexport) VOID WINAPI FreeEncryptedFileMetadata(P metadata) { (void)metadata; }

/* ---- WMI ---------------------------------------------------------------------------------------------------------------------------------------------------------- */
/* The WMI data provider interface (WMI blocks of drivers): there are no driver blocks, so no GUID is found. */

WMI_NOT_FOUND(WmiOpenBlock, P a, ULONG b, P c)
WMI_NOT_FOUND(WmiQueryAllDataA, P a, P b, P c)
WMI_NOT_FOUND(WmiQueryAllDataW, P a, P b, P c)
WMI_NOT_FOUND(WmiQueryAllDataMultipleA, P a, ULONG b, P c, P d)
WMI_NOT_FOUND(WmiQueryAllDataMultipleW, P a, ULONG b, P c, P d)
WMI_NOT_FOUND(WmiQuerySingleInstanceA, P a, P b, P c, P d)
WMI_NOT_FOUND(WmiQuerySingleInstanceW, P a, P b, P c, P d)
WMI_NOT_FOUND(WmiQuerySingleInstanceMultipleA, P a, P b, ULONG c, P d, P e)
WMI_NOT_FOUND(WmiQuerySingleInstanceMultipleW, P a, P b, ULONG c, P d, P e)
WMI_NOT_FOUND(WmiSetSingleInstanceA, P a, P b, ULONG c, ULONG d, P e)
WMI_NOT_FOUND(WmiSetSingleInstanceW, P a, P b, ULONG c, ULONG d, P e)
WMI_NOT_FOUND(WmiSetSingleItemA, P a, P b, ULONG c, ULONG d, ULONG e, P f)
WMI_NOT_FOUND(WmiSetSingleItemW, P a, P b, ULONG c, ULONG d, ULONG e, P f)
WMI_NOT_FOUND(WmiExecuteMethodA, P a, P b, ULONG c, ULONG d, P e, P f, P g)
WMI_NOT_FOUND(WmiExecuteMethodW, P a, P b, ULONG c, ULONG d, P e, P f, P g)
WMI_NOT_FOUND(WmiQueryGuidInformation, P a, P b)
WMI_NOT_FOUND(WmiNotificationRegistrationA, P a, BOOL b, P c, ULONG_PTR d, ULONG e)
WMI_NOT_FOUND(WmiNotificationRegistrationW, P a, BOOL b, P c, ULONG_PTR d, ULONG e)
WMI_NOT_FOUND(WmiReceiveNotificationsA, ULONG a, P b, P c, P d, P e)
WMI_NOT_FOUND(WmiReceiveNotificationsW, ULONG a, P b, P c, P d, P e)
WMI_NOT_FOUND(WmiEnumerateGuids, P a, P b)
WMI_NOT_FOUND(WmiDevInstToInstanceNameA, P a, ULONG b, P c, ULONG d)
WMI_NOT_FOUND(WmiDevInstToInstanceNameW, P a, ULONG b, P c, ULONG d)
WMI_NOT_FOUND(WmiFileHandleToInstanceNameA, P a, HANDLE b, P c, P d)
WMI_NOT_FOUND(WmiFileHandleToInstanceNameW, P a, HANDLE b, P c, P d)
WMI_NOT_FOUND(WmiMofEnumerateResourcesA, P a, P b, P c)
WMI_NOT_FOUND(WmiMofEnumerateResourcesW, P a, P b, P c)
__declspec(dllexport) ULONG WINAPI WmiCloseBlock(HANDLE block) { (void)block; return ERROR_INVALID_HANDLE; }
__declspec(dllexport) VOID WINAPI WmiFreeBuffer(PVOID buffer) { (void)buffer; }
NOT_SUPPORTED_DWORD(WmiConvertTimestamp, P a, P b, P c)
NOT_SUPPORTED_DWORD(WmiGetFirstTraceOffset, P a)
NOT_SUPPORTED_DWORD(WmiGetNextEvent, P a)
NOT_SUPPORTED_DWORD(WmiGetTraceHeader, P a, ULONG b, P c)
NOT_SUPPORTED_DWORD(WmiParseTraceEvent, P a, ULONG b, ULONG c, P d, P e)
NOT_SUPPORTED_DWORD(WmiOpenTraceWithCursor, P a)
NOT_SUPPORTED_DWORD(WmiCloseTraceWithCursor, P a)
NOT_SUPPORTED_DWORD(WdmWmiServiceMain, P a, P b)
NOT_SUPPORTED_DWORD(EtwLogSysConfigExtension, P a)
NOT_SUPPORTED_DWORD(EventAccessControl, P a, ULONG b, P c, ULONG d, BOOLEAN e)
NOT_SUPPORTED_DWORD(EventAccessQuery, P a, P b, P c)
NOT_SUPPORTED_DWORD(EventAccessRemove, P a)
NOT_SUPPORTED_DWORD(OperationStart, P a)
NOT_SUPPORTED_DWORD(OperationEnd, P a)

/* ---- the remote registry and service control interfaces (RPC) ------------------------------------------------------------------------------------------------ */
/* The Base* / Remote* / SafeBase* names are the client stubs of RPC interfaces to a registry service in another process; the registry here is called directly. */

NOT_IMPLEMENTED_DWORD(BaseRegCloseKey, P a)
NOT_IMPLEMENTED_DWORD(BaseRegCreateKey, P a, P b, P c, P d, P e, P f, P g, P h)
NOT_IMPLEMENTED_DWORD(BaseRegDeleteKeyEx, P a, P b, ULONG c, ULONG d)
NOT_IMPLEMENTED_DWORD(BaseRegDeleteValue, P a, P b)
NOT_IMPLEMENTED_DWORD(BaseRegFlushKey, P a)
NOT_IMPLEMENTED_DWORD(BaseRegGetVersion, P a, P b)
NOT_IMPLEMENTED_DWORD(BaseRegLoadKey, P a, P b, P c)
NOT_IMPLEMENTED_DWORD(BaseRegOpenKey, P a, P b, ULONG c, ULONG d, P e)
NOT_IMPLEMENTED_DWORD(BaseRegRestoreKey, P a, P b, ULONG c)
NOT_IMPLEMENTED_DWORD(BaseRegSaveKeyEx, P a, P b, P c, ULONG d)
NOT_IMPLEMENTED_DWORD(BaseRegSetKeySecurity, P a, ULONG b, P c)
NOT_IMPLEMENTED_DWORD(BaseRegSetValue, P a, P b, ULONG c, P d, ULONG e)
NOT_IMPLEMENTED_DWORD(BaseRegUnLoadKey, P a, P b)
NOT_IMPLEMENTED_DWORD(SafeBaseRegGetKeySecurity, P a, ULONG b, P c, P d)
NOT_IMPLEMENTED_DWORD(RemoteRegEnumKeyWrapper, P a, ULONG b, P c, P d, P e, P f, P g)
NOT_IMPLEMENTED_DWORD(RemoteRegEnumValueWrapper, P a, ULONG b, P c, P d, P e, P f, P g, P h)
NOT_IMPLEMENTED_DWORD(RemoteRegQueryInfoKeyWrapper, P a, P b, P c, P d, P e, P f, P g, P h, P i, P j, P k, P l)
NOT_IMPLEMENTED_DWORD(RemoteRegQueryMultipleValues2Wrapper, P a, P b, ULONG c, P d, P e, P f)
NOT_IMPLEMENTED_DWORD(RemoteRegQueryMultipleValuesWrapper, P a, P b, ULONG c, P d, P e)
NOT_IMPLEMENTED_DWORD(RemoteRegQueryValueWrapper, P a, P b, P c, P d, P e, P f)
NOT_IMPLEMENTED_DWORD(I_ScGetCurrentGroupStateW, P a, P b, ULONG c)
NOT_IMPLEMENTED_DWORD(I_ScIsSecurityProcess, P a)
NOT_IMPLEMENTED_DWORD(I_ScPnPGetServiceName, P a, P b, ULONG c)
NOT_IMPLEMENTED_DWORD(I_ScQueryServiceConfig, P a, P b, P c)
NOT_IMPLEMENTED_DWORD(I_ScRegisterPreshutdownRestart, P a)
NOT_IMPLEMENTED_DWORD(I_ScSendPnPMessage, ULONG a, ULONG b, ULONG c, P d)
NOT_IMPLEMENTED_DWORD(I_ScSendTSMessage, ULONG a, ULONG b, ULONG c, P d)
NOT_IMPLEMENTED_DWORD(I_ScSetServiceBitsA, P a, ULONG b, BOOL c, BOOL d, P e)
NOT_IMPLEMENTED_DWORD(I_ScSetServiceBitsW, P a, ULONG b, BOOL c, BOOL d, P e)
NOT_IMPLEMENTED_DWORD(I_ScValidatePnPService, P a, P b, P c)
NOT_IMPLEMENTED_DWORD(I_QueryTagInformation, P a, ULONG b, P c)
NOT_SUPPORTED_DWORD(QueryLocalUserServiceName, P a, ULONG b, P c)
NOT_SUPPORTED_DWORD(QueryUserServiceName, P a, P b, ULONG c)
NOT_SUPPORTED_DWORD(QueryUserServiceNameForContext, P a, P b, ULONG c, P d)
NOT_SUPPORTED_DWORD(EnumServiceGroupW, P a, ULONG b, ULONG c, P d, ULONG e, P f, P g, P h, P i)
NOT_SUPPORTED_DWORD(WaitServiceState, P a, ULONG b, ULONG c, P d)
__declspec(dllexport) P WINAPI CreateServiceEx(P manager, LPCWSTR name, LPCWSTR display, ULONG access, ULONG type, ULONG start, ULONG error_control, LPCWSTR binary, LPCWSTR group, P tag,
                                              LPCWSTR deps, LPCWSTR account, LPCWSTR password, P extra)
{
    (void)manager; (void)name; (void)display; (void)access; (void)type; (void)start; (void)error_control; (void)binary; (void)group; (void)tag; (void)deps; (void)account; (void)password; (void)extra;
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return NULL;
}
STUB(DWORD, Wow64Win32ApiEntry, DWORD a, DWORD b, DWORD c) { (void)a; (void)b; (void)c; return ERROR_NOT_SUPPORTED; }
NOT_SUPPORTED_DWORD(NpGetUserName, P a, P b)
NOT_SUPPORTED_DWORD(MSChapSrvChangePassword, P a, P b, P c, P d, P e, P f, P g)
NOT_SUPPORTED_DWORD(MSChapSrvChangePassword2, P a, P b, P c, P d, P e, P f, P g, P h)
__declspec(dllexport) VOID WINAPI MIDL_user_free_Ext(PVOID block) { (void)block; }

/* ---- software installation by policy, wait chains -------------------------------------------------------------------------------------------------------------- */

__declspec(dllexport) DWORD WINAPI GetManagedApplications(P category, DWORD query_flags, DWORD level, LPDWORD count, P *apps)
{
    (void)category; (void)query_flags; (void)level;
    if (!count || !apps) return ERROR_INVALID_PARAMETER;
    *count = 0;
    *apps = NULL;
    return ERROR_SUCCESS;      /* (no policy installs any application) */
}
__declspec(dllexport) DWORD WINAPI GetLocalManagedApplications(BOOL user, LPDWORD count, P *apps)
{
    (void)user;
    if (!count || !apps) return ERROR_INVALID_PARAMETER;
    *count = 0;
    *apps = NULL;
    return ERROR_SUCCESS;
}
NOT_SUPPORTED_DWORD(GetLocalManagedApplicationData, P a, P b, P c)
NOT_SUPPORTED_DWORD(GetManagedApplicationCategories, DWORD a, P b)
NOT_SUPPORTED_DWORD(InstallApplication, P a)
NOT_SUPPORTED_DWORD(UninstallApplication, P a, DWORD b)
NOT_SUPPORTED_DWORD(CommandLineFromMsiDescriptor, P a, P b, P c)
__declspec(dllexport) P WINAPI OpenThreadWaitChainSession(DWORD flags, P callback)
{
    (void)flags; (void)callback;
    SetLastError(ERROR_NOT_SUPPORTED);
    return NULL;
}
__declspec(dllexport) VOID WINAPI CloseThreadWaitChainSession(P session) { (void)session; }
NOT_SUPPORTED_BOOL(GetThreadWaitChain, P a, DWORD_PTR b, DWORD c, DWORD d, P e, P f, P g)
NOT_SUPPORTED_BOOL(RegisterWaitChainCOMCallback, P a, P b)
NOT_SUPPORTED_DWORD(GetStringConditionFromBinary, P a, ULONG b, ULONG c, P d)

/* ---- Safer (code authorization levels) ---------------------------------------------------------------------------------------------------------------------------- */

NOT_SUPPORTED_BOOL(SaferGetLevelInformation, P a, ULONG b, P c, ULONG d, P e)
NOT_SUPPORTED_BOOL(SaferSetPolicyInformation, ULONG a, ULONG b, ULONG c, P d, P e)
NOT_SUPPORTED_BOOL(SaferRecordEventLogEntry, P a, P b, P c)
NOT_SUPPORTED_DWORD(SaferiChangeRegistryScope, P a, ULONG b)
NOT_SUPPORTED_DWORD(SaferiCompareTokenLevels, P a, P b, P c)
NOT_SUPPORTED_DWORD(SaferiIsDllAllowed, P a, P b, P c)
NOT_SUPPORTED_DWORD(SaferiPopulateDefaultsInRegistry, P a, P b)
NOT_SUPPORTED_DWORD(SaferiRecordEventLogEntry, P a, P b, P c)
NOT_SUPPORTED_DWORD(SaferiReplaceProcessThreadTokens, P a, P b, P c)
NOT_SUPPORTED_DWORD(SaferiSearchMatchingHashRules, ULONG a, P b, ULONG c, P d, P e, P f)
NOT_SUPPORTED_DWORD(CreateCodeAuthzLevel, P a, P b, P c)
NOT_SUPPORTED_DWORD(CloseCodeAuthzLevel, P a)
NOT_SUPPORTED_DWORD(ComputeAccessTokenFromCodeAuthzLevel, P a, P b, P c, P d)
NOT_SUPPORTED_DWORD(GetInformationCodeAuthzLevelW, P a, ULONG b, P c, ULONG d, P e)
NOT_SUPPORTED_DWORD(GetInformationCodeAuthzPolicyW, ULONG a, ULONG b, ULONG c, P d, P e)
NOT_SUPPORTED_DWORD(IdentifyCodeAuthzLevelW, ULONG a, P b, P c, P d)
NOT_SUPPORTED_DWORD(SetInformationCodeAuthzLevelW, P a, ULONG b, P c, ULONG d)
NOT_SUPPORTED_DWORD(SetInformationCodeAuthzPolicyW, ULONG a, ULONG b, ULONG c, P d, ULONG e)

/* ---- the DES-based functions of the password hash interface (SystemFunction001 ...) ------------------------------------------------------------------------- */
/* The classic password-hash helpers of NT are not part of this OS (it keeps no password database): they answer STATUS_NOT_SUPPORTED. SystemFunction032/033 (RC4),
 * 036 (random bytes), 040/041 (memory encryption) are in crypt.c. */

#define SYSFUNC(n) NOT_SUPPORTED_NT(SystemFunction##n, P a, P b, P c)
SYSFUNC(001) SYSFUNC(002) SYSFUNC(003) SYSFUNC(004) SYSFUNC(005) SYSFUNC(006) SYSFUNC(007) SYSFUNC(008) SYSFUNC(009) SYSFUNC(010)
SYSFUNC(011) SYSFUNC(012) SYSFUNC(013) SYSFUNC(014) SYSFUNC(015) SYSFUNC(016) SYSFUNC(017) SYSFUNC(018) SYSFUNC(019) SYSFUNC(020)
SYSFUNC(021) SYSFUNC(022) SYSFUNC(023) SYSFUNC(024) SYSFUNC(025) SYSFUNC(026) SYSFUNC(027) SYSFUNC(028) SYSFUNC(029) SYSFUNC(030)
SYSFUNC(031) SYSFUNC(034) SYSFUNC(035)

/* ---- the service-notification alias ------------------------------------------------------------------------------------------------------------------------------------ */

STUB(DWORD, NotifyServiceStatusChange, P h, DWORD mask, P notify) { return NotifyServiceStatusChangeW(h, mask, notify); }
NOT_SUPPORTED_DWORD(UsePinForEncryptedFilesA, P a, P b, P c)
NOT_SUPPORTED_DWORD(UsePinForEncryptedFilesW, P a, P b, P c)
