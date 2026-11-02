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
/* The Rtl security functions of ntdll.dll (src/Windows/ntdll/security.c) and the NT services that deal with tokens and security descriptors, for the units built with the mingw
 * headers (mingw declares few of them). ntdll defines NTSEC_EXPORT as dllexport before including this. */
#pragma once
#include <windows.h>
#ifndef NTSEC_EXPORT
#define NTSEC_EXPORT
#endif
#ifndef SE_SELF_RELATIVE
#define SE_SELF_RELATIVE 0x8000
#endif
#define SE_MIN_WELL_KNOWN_PRIVILEGE 2
#define SE_MAX_WELL_KNOWN_PRIVILEGE 36
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef NTAPI
#define NTAPI __stdcall
#endif
/* the functions of security.c (and of ntdll.c that it calls) */
NTSEC_EXPORT NTSTATUS WINAPI RtlAllocateAndInitializeSid ( PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount, DWORD nSubAuthority0, DWORD nSubAuthority1, DWORD nSubAuthority2, DWORD nSubAuthority3, DWORD nSubAuthority4, DWORD nSubAuthority5, DWORD nSubAuthority6, DWORD nSubAuthority7, PSID *pSid );
NTSEC_EXPORT BOOL WINAPI RtlEqualSid( PSID pSid1, PSID pSid2 );
NTSEC_EXPORT BOOL WINAPI RtlEqualPrefixSid (PSID pSid1, PSID pSid2);
NTSEC_EXPORT DWORD WINAPI RtlFreeSid(PSID pSid);
NTSEC_EXPORT DWORD WINAPI RtlLengthRequiredSid(DWORD nrofsubauths);
NTSEC_EXPORT DWORD WINAPI RtlLengthSid(PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlInitializeSid( PSID pSid, PSID_IDENTIFIER_AUTHORITY pIdentifierAuthority, BYTE nSubAuthorityCount);
NTSEC_EXPORT LPDWORD WINAPI RtlSubAuthoritySid( PSID pSid, DWORD nSubAuthority );
NTSEC_EXPORT PSID_IDENTIFIER_AUTHORITY WINAPI RtlIdentifierAuthoritySid( PSID pSid );
NTSEC_EXPORT LPBYTE WINAPI RtlSubAuthorityCountSid(PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlCopySid( DWORD destlen, PSID dest, PSID source );
NTSEC_EXPORT BOOLEAN WINAPI RtlValidSid( PSID pSid );
NTSEC_EXPORT NTSTATUS WINAPI RtlCreateSecurityDescriptor( PSECURITY_DESCRIPTOR lpsd, DWORD rev);
NTSEC_EXPORT NTSTATUS WINAPI RtlCopySecurityDescriptor(PSECURITY_DESCRIPTOR pSourceSD, PSECURITY_DESCRIPTOR pDestinationSD);
NTSEC_EXPORT BOOLEAN WINAPI RtlValidSecurityDescriptor(PSECURITY_DESCRIPTOR descriptor);
NTSEC_EXPORT BOOLEAN WINAPI RtlValidRelativeSecurityDescriptor(PSECURITY_DESCRIPTOR descriptor, ULONG length, SECURITY_INFORMATION info);
NTSEC_EXPORT ULONG WINAPI RtlLengthSecurityDescriptor( PSECURITY_DESCRIPTOR pSecurityDescriptor);
NTSEC_EXPORT NTSTATUS WINAPI RtlGetDaclSecurityDescriptor( IN PSECURITY_DESCRIPTOR pSecurityDescriptor, OUT PBOOLEAN lpbDaclPresent, OUT PACL *pDacl, OUT PBOOLEAN lpbDaclDefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetDaclSecurityDescriptor ( PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOLEAN daclpresent, PACL dacl, BOOLEAN dacldefaulted );
NTSEC_EXPORT NTSTATUS WINAPI RtlGetSaclSecurityDescriptor( IN PSECURITY_DESCRIPTOR pSecurityDescriptor, OUT PBOOLEAN lpbSaclPresent, OUT PACL *pSacl, OUT PBOOLEAN lpbSaclDefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetSaclSecurityDescriptor ( PSECURITY_DESCRIPTOR pSecurityDescriptor, BOOLEAN saclpresent, PACL sacl, BOOLEAN sacldefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlGetOwnerSecurityDescriptor( PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID *Owner, PBOOLEAN OwnerDefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetOwnerSecurityDescriptor( PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID owner, BOOLEAN ownerdefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetGroupSecurityDescriptor ( PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID group, BOOLEAN groupdefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlGetGroupSecurityDescriptor( PSECURITY_DESCRIPTOR pSecurityDescriptor, PSID *Group, PBOOLEAN GroupDefaulted);
NTSEC_EXPORT NTSTATUS WINAPI RtlMakeSelfRelativeSD( IN PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor, IN PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor, IN OUT LPDWORD lpdwBufferLength);
NTSEC_EXPORT NTSTATUS WINAPI RtlSelfRelativeToAbsoluteSD( IN PSECURITY_DESCRIPTOR pSelfRelativeSecurityDescriptor, OUT PSECURITY_DESCRIPTOR pAbsoluteSecurityDescriptor, OUT LPDWORD lpdwAbsoluteSecurityDescriptorSize, OUT PACL pDacl, OUT LPDWORD lpdwDaclSize, OUT PACL pSacl, OUT LPDWORD lpdwSaclSize, OUT PSID pOwner, OUT LPDWORD lpdwOwnerSize, OUT PSID pPrimaryGroup, OUT LPDWORD lpdwPrimaryGroupSize);
NTSEC_EXPORT NTSTATUS WINAPI RtlGetControlSecurityDescriptor( PSECURITY_DESCRIPTOR pSecurityDescriptor, PSECURITY_DESCRIPTOR_CONTROL pControl, LPDWORD lpdwRevision);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetControlSecurityDescriptor( PSECURITY_DESCRIPTOR SecurityDescriptor, SECURITY_DESCRIPTOR_CONTROL ControlBitsOfInterest, SECURITY_DESCRIPTOR_CONTROL ControlBitsToSet);
NTSEC_EXPORT NTSTATUS WINAPI RtlAbsoluteToSelfRelativeSD( PSECURITY_DESCRIPTOR AbsoluteSecurityDescriptor, PSECURITY_DESCRIPTOR SelfRelativeSecurityDescriptor, PULONG BufferLength);
NTSEC_EXPORT NTSTATUS WINAPI RtlNewSecurityObject(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *descr, BOOLEAN is_container, HANDLE token, PGENERIC_MAPPING mapping);
NTSEC_EXPORT NTSTATUS WINAPI RtlNewSecurityObjectEx(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *descr, GUID *type, BOOLEAN is_container, ULONG flags, HANDLE token, PGENERIC_MAPPING mapping );
NTSEC_EXPORT NTSTATUS WINAPI RtlNewSecurityObjectWithMultipleInheritance(PSECURITY_DESCRIPTOR parent, PSECURITY_DESCRIPTOR creator, PSECURITY_DESCRIPTOR *descr, GUID **types, ULONG count, BOOLEAN is_container, ULONG flags, HANDLE token, PGENERIC_MAPPING mapping );
NTSEC_EXPORT NTSTATUS WINAPI RtlDeleteSecurityObject( PSECURITY_DESCRIPTOR *descr );
NTSEC_EXPORT NTSTATUS WINAPI RtlCreateAcl(PACL acl,DWORD size,DWORD rev);
NTSEC_EXPORT BOOLEAN WINAPI RtlFirstFreeAce( PACL acl, PACE_HEADER *x);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAce( PACL acl, DWORD rev, DWORD xnrofaces, PACE_HEADER acestart, DWORD acelen);
NTSEC_EXPORT NTSTATUS WINAPI RtlDeleteAce(PACL pAcl, DWORD dwAceIndex);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAccessAllowedAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD AccessMask, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAccessAllowedAceEx( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD AceFlags, IN DWORD AccessMask, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAccessAllowedObjectAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD dwAceFlags, IN DWORD dwAccessMask, IN GUID* pObjectTypeGuid, IN GUID* pInheritedObjectTypeGuid, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAccessDeniedAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD AccessMask, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAccessDeniedAceEx( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD AceFlags, IN DWORD AccessMask, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAccessDeniedObjectAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD dwAceFlags, IN DWORD dwAccessMask, IN GUID* pObjectTypeGuid, IN GUID* pInheritedObjectTypeGuid, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAuditAccessAceEx( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD dwAceFlags, IN DWORD dwAccessMask, IN PSID pSid, IN BOOL bAuditSuccess, IN BOOL bAuditFailure);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAuditAccessAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD dwAccessMask, IN PSID pSid, IN BOOL bAuditSuccess, IN BOOL bAuditFailure);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddAuditAccessObjectAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD dwAceFlags, IN DWORD dwAccessMask, IN GUID* pObjectTypeGuid, IN GUID* pInheritedObjectTypeGuid, IN PSID pSid, IN BOOL bAuditSuccess, IN BOOL bAuditFailure);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddMandatoryAce( IN OUT PACL pAcl, IN DWORD dwAceRevision, IN DWORD dwAceFlags, IN DWORD dwMandatoryFlags, IN DWORD dwAceType, IN PSID pSid);
NTSEC_EXPORT NTSTATUS WINAPI RtlAddProcessTrustLabelAce( ACL *acl, DWORD revision, DWORD flags, PSID sid, DWORD type, DWORD mask );
NTSEC_EXPORT BOOLEAN WINAPI RtlValidAcl(PACL pAcl);
NTSEC_EXPORT NTSTATUS WINAPI RtlGetAce(PACL pAcl,DWORD dwAceIndex,LPVOID *pAce );
NTSEC_EXPORT BOOLEAN WINAPI RtlAreAllAccessesGranted( ACCESS_MASK granted, ACCESS_MASK desired );
NTSEC_EXPORT BOOLEAN WINAPI RtlAreAnyAccessesGranted( ACCESS_MASK granted, ACCESS_MASK desired );
NTSEC_EXPORT void WINAPI RtlMapGenericMask( ACCESS_MASK *mask, const GENERIC_MAPPING *mapping );
NTSEC_EXPORT void WINAPI RtlCopyLuid( LUID *dest, const LUID *src );
NTSEC_EXPORT BOOLEAN WINAPI RtlEqualLuid( const LUID *luid1, const LUID *luid2 );
NTSEC_EXPORT void WINAPI RtlCopyLuidAndAttributesArray( ULONG count, const LUID_AND_ATTRIBUTES *src, PLUID_AND_ATTRIBUTES dest );
NTSEC_EXPORT NTSTATUS WINAPI RtlAdjustPrivilege(ULONG Privilege, BOOLEAN Enable, BOOLEAN CurrentThread, PBOOLEAN Enabled);
NTSEC_EXPORT NTSTATUS WINAPI RtlImpersonateSelf(SECURITY_IMPERSONATION_LEVEL ImpersonationLevel);
NTSEC_EXPORT NTSTATUS WINAPI RtlConvertSidToUnicodeString( PUNICODE_STRING String, PSID pSid, BOOLEAN AllocateString);
NTSEC_EXPORT NTSTATUS WINAPI RtlQueryInformationAcl( PACL pAcl, LPVOID pAclInformation, DWORD nAclInformationLength, ACL_INFORMATION_CLASS dwAclInformationClass);
NTSEC_EXPORT NTSTATUS WINAPI RtlConvertToAutoInheritSecurityObject( PSECURITY_DESCRIPTOR pdesc, PSECURITY_DESCRIPTOR cdesc, PSECURITY_DESCRIPTOR* ndesc, GUID* objtype, BOOL isdir, PGENERIC_MAPPING genmap );
NTSEC_EXPORT NTSTATUS WINAPI RtlDefaultNpAcl(PACL *pAcl);

NTSTATUS NTAPI NtOpenThreadToken(HANDLE, ACCESS_MASK, BOOLEAN, PHANDLE);
NTSTATUS NTAPI NtOpenProcessToken(HANDLE, ACCESS_MASK, PHANDLE);
NTSTATUS NTAPI NtAdjustPrivilegesToken(HANDLE, BOOLEAN, TOKEN_PRIVILEGES *, ULONG, TOKEN_PRIVILEGES *, ULONG *);
NTSTATUS NTAPI NtDuplicateToken(HANDLE, ACCESS_MASK, OBJECT_ATTRIBUTES *, BOOLEAN, TOKEN_TYPE, PHANDLE);
NTSTATUS NTAPI NtQuerySecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR, ULONG, PULONG);
NTSTATUS NTAPI NtSetSecurityObject(HANDLE, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetInformationAcl(PACL acl, LPVOID info, DWORD length, ACL_INFORMATION_CLASS class);
NTSEC_EXPORT NTSTATUS WINAPI RtlQuerySecurityObject(PSECURITY_DESCRIPTOR object, SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR result, ULONG length, PULONG needed);
NTSEC_EXPORT NTSTATUS WINAPI RtlSetSecurityObject(SECURITY_INFORMATION info, PSECURITY_DESCRIPTOR modification, PSECURITY_DESCRIPTOR *object, PGENERIC_MAPPING mapping, HANDLE token);
NTSTATUS NTAPI NtAllocateLocallyUniqueId(PLUID luid);
NTSTATUS NTAPI NtOpenProcessToken(HANDLE, ACCESS_MASK, PHANDLE);
NTSTATUS NTAPI NtQueryInformationToken(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG, PULONG);
NTSTATUS NTAPI NtSetInformationToken(HANDLE, TOKEN_INFORMATION_CLASS, PVOID, ULONG);
NTSTATUS NTAPI NtFilterToken(HANDLE, ULONG, TOKEN_GROUPS *, TOKEN_PRIVILEGES *, TOKEN_GROUPS *, PHANDLE);
NTSTATUS NTAPI NtAdjustGroupsToken(HANDLE, BOOLEAN, TOKEN_GROUPS *, ULONG, TOKEN_GROUPS *, PULONG);
NTSTATUS NTAPI NtPrivilegeCheck(HANDLE, PPRIVILEGE_SET, PBOOLEAN);
NTSTATUS NTAPI NtAccessCheck(PSECURITY_DESCRIPTOR, HANDLE, ACCESS_MASK, PGENERIC_MAPPING, PPRIVILEGE_SET, PULONG, PACCESS_MASK, PNTSTATUS);
NTSTATUS NTAPI NtAccessCheckByType(PSECURITY_DESCRIPTOR, PSID, HANDLE, ULONG, POBJECT_TYPE_LIST, ULONG, PGENERIC_MAPPING, PPRIVILEGE_SET, PULONG, PACCESS_MASK, PNTSTATUS);
NTSTATUS NTAPI NtAccessCheckByTypeResultList(PSECURITY_DESCRIPTOR, PSID, HANDLE, ULONG, POBJECT_TYPE_LIST, ULONG, PGENERIC_MAPPING, PPRIVILEGE_SET, PULONG, PACCESS_MASK, PNTSTATUS);
