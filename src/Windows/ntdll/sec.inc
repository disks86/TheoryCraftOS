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
/* ntdll.dll: security identifiers, access masks and the layout of security descriptors (the parts of the Rtl security API that need no token). Included by ntdll.c. */

#define SEC_STATUS_INVALID_SID 0xC0000078u
#define SEC_STATUS_UNKNOWN_REVISION 0xC0000058u
#define SEC_STATUS_BAD_DESCRIPTOR_FORMAT 0xC00000E7u
#define SEC_SELF_RELATIVE 0x8000u

typedef struct { BYTE Value[6]; } SID_AUTHORITY_;
typedef struct { BYTE Revision, SubAuthorityCount; SID_AUTHORITY_ IdentifierAuthority; DWORD SubAuthority[1]; } SID_;
typedef struct { BYTE AclRevision, Sbz1; WORD AclSize, AceCount, Sbz2; } ACL_;
typedef struct { BYTE AceType, AceFlags; WORD AceSize; } ACE_HEADER_;
typedef struct { BYTE Revision, Sbz1; WORD Control; SID_ *Owner, *Group; ACL_ *Sacl, *Dacl; } SECURITY_DESCRIPTOR_;
typedef struct { BYTE Revision, Sbz1; WORD Control; DWORD Owner, Group, Sacl, Dacl; } SECURITY_DESCRIPTOR_RELATIVE_;

EXPORT BOOLEAN NTAPI RtlAreAllAccessesGranted(DWORD granted, DWORD desired) { return (granted & desired) == desired; }
EXPORT BOOLEAN NTAPI RtlAreAnyAccessesGranted(DWORD granted, DWORD desired) { return (granted & desired) != 0; }

EXPORT DWORD NTAPI RtlLengthRequiredSid(DWORD count) { return (count - 1) * sizeof(DWORD) + sizeof(SID_); }
EXPORT BYTE *NTAPI RtlSubAuthorityCountSid(PVOID sid) { return &((SID_ *)sid)->SubAuthorityCount; }
EXPORT DWORD NTAPI RtlLengthSid(PVOID sid) { return RtlLengthRequiredSid(*RtlSubAuthorityCountSid(sid)); }
EXPORT DWORD *NTAPI RtlSubAuthoritySid(PVOID sid, DWORD index) { return &((SID_ *)sid)->SubAuthority[index]; }
EXPORT SID_AUTHORITY_ *NTAPI RtlIdentifierAuthoritySid(PVOID sid) { return &((SID_ *)sid)->IdentifierAuthority; }
EXPORT BOOLEAN NTAPI RtlValidSid(PVOID sid) {
  const SID_ *s = sid;
  return s && s->Revision == 1 && s->SubAuthorityCount <= 15;
}
EXPORT BOOLEAN NTAPI RtlEqualSid(PVOID a, PVOID b) {
  if (!RtlValidSid(a) || !RtlValidSid(b)) return FALSE;
  if (*RtlSubAuthorityCountSid(a) != *RtlSubAuthorityCountSid(b)) return FALSE;
  return memcmp(a, b, RtlLengthSid(a)) == 0;
}
EXPORT BOOLEAN NTAPI RtlEqualPrefixSid(PVOID a, PVOID b) {
  if (!RtlValidSid(a) || !RtlValidSid(b)) return FALSE;
  if (*RtlSubAuthorityCountSid(a) != *RtlSubAuthorityCountSid(b)) return FALSE;
  return memcmp(a, b, RtlLengthRequiredSid(((SID_ *)a)->SubAuthorityCount - 1)) == 0;
}
EXPORT NTSTATUS NTAPI RtlInitializeSid(PVOID sid, SID_AUTHORITY_ *authority, BYTE count) {
  SID_ *s = sid;
  if (count > 15) return STATUS_INVALID_PARAMETER_;
  s->Revision = 1;
  s->SubAuthorityCount = count;
  if (authority) s->IdentifierAuthority = *authority;
  for (int i = 0; i < count; ++i) s->SubAuthority[i] = 0;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAllocateAndInitializeSid(SID_AUTHORITY_ *authority, BYTE count, DWORD s0, DWORD s1, DWORD s2, DWORD s3, DWORD s4, DWORD s5, DWORD s6, DWORD s7, PVOID *out) {
  if (count > 8) return SEC_STATUS_INVALID_SID;
  SID_ *sid = RtlAllocateHeap(process_heap(), 0, RtlLengthRequiredSid(count));
  if (!sid) return 0xC0000017u;
  sid->Revision = 1;
  if (authority) sid->IdentifierAuthority = *authority;
  sid->SubAuthorityCount = count;
  const DWORD subs[8] = {s0, s1, s2, s3, s4, s5, s6, s7};
  for (int i = 0; i < count; ++i) sid->SubAuthority[i] = subs[i];
  *out = sid;
  return STATUS_SUCCESS;
}
EXPORT PVOID NTAPI RtlFreeSid(PVOID sid) { RtlFreeHeap(process_heap(), 0, sid); return (PVOID)(ULONG_PTR)STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI RtlCopySid(DWORD length, PVOID dest, PVOID source) {
  if (length < RtlLengthSid(source)) return STATUS_BUFFER_TOO_SMALL_;
  memcpy(dest, source, RtlLengthSid(source));
  return STATUS_SUCCESS;
}

EXPORT NTSTATUS NTAPI RtlCreateSecurityDescriptor(PVOID descriptor, DWORD revision) {
  if (revision != 1) return SEC_STATUS_UNKNOWN_REVISION;
  memset(descriptor, 0, sizeof(SECURITY_DESCRIPTOR_));
  ((SECURITY_DESCRIPTOR_ *)descriptor)->Revision = 1;
  return STATUS_SUCCESS;
}
EXPORT BOOLEAN NTAPI RtlValidSecurityDescriptor(PVOID descriptor) {
  const SECURITY_DESCRIPTOR_ *sd = descriptor;
  return sd && sd->Revision == 1;
}
EXPORT BOOLEAN NTAPI RtlFirstFreeAce(ACL_ *acl, ACE_HEADER_ **first) {
  *first = NULL;
  ACE_HEADER_ *ace = (ACE_HEADER_ *)(acl + 1);
  for (int i = 0; i < acl->AceCount; ++i) {
    if ((BYTE *)ace >= (BYTE *)acl + acl->AclSize) return FALSE;
    ace = (ACE_HEADER_ *)((BYTE *)ace + ace->AceSize);
  }
  if ((BYTE *)ace <= (BYTE *)acl + acl->AclSize) *first = ace;
  return TRUE;
}
EXPORT ULONG NTAPI RtlLengthSecurityDescriptor(PVOID descriptor) {
  const SECURITY_DESCRIPTOR_ *sd = descriptor;
  if (!sd) return 0;
  ULONG length = sizeof(SECURITY_DESCRIPTOR_);
  if (sd->Control & SEC_SELF_RELATIVE) {
    const SECURITY_DESCRIPTOR_RELATIVE_ *rel = descriptor;
    length = sizeof(*rel);
    if (rel->Owner) length += RtlLengthSid((BYTE *)descriptor + rel->Owner);
    if (rel->Group) length += RtlLengthSid((BYTE *)descriptor + rel->Group);
    if ((rel->Control & 0x10) && rel->Sacl) length += ((ACL_ *)((BYTE *)descriptor + rel->Sacl))->AclSize;      /* (only when the ACL is marked present) */
    if ((rel->Control & 0x04) && rel->Dacl) length += ((ACL_ *)((BYTE *)descriptor + rel->Dacl))->AclSize;
    return length;
  }
  if (sd->Owner) length += RtlLengthSid(sd->Owner);
  if (sd->Group) length += RtlLengthSid(sd->Group);
  if ((sd->Control & 0x10) && sd->Sacl) length += sd->Sacl->AclSize;
  if ((sd->Control & 0x04) && sd->Dacl) length += sd->Dacl->AclSize;
  return length;
}
EXPORT NTSTATUS NTAPI RtlMakeSelfRelativeSD(PVOID absolute, PVOID relative, DWORD *buffer_length) {
  SECURITY_DESCRIPTOR_ *abs = absolute;
  SECURITY_DESCRIPTOR_RELATIVE_ *rel = relative;
  if (!buffer_length || !abs) return STATUS_INVALID_PARAMETER_;
  ULONG length = RtlLengthSecurityDescriptor(abs);
  if (!(abs->Control & SEC_SELF_RELATIVE)) length -= sizeof(*abs) - sizeof(*rel);
  if (*buffer_length < length) { *buffer_length = length; return STATUS_BUFFER_TOO_SMALL_; }
  if (!rel) return STATUS_INVALID_PARAMETER_;
  if (abs->Control & SEC_SELF_RELATIVE) { memcpy(rel, abs, length); return STATUS_SUCCESS; }
  rel->Revision = abs->Revision;
  rel->Sbz1 = abs->Sbz1;
  rel->Control = abs->Control | SEC_SELF_RELATIVE;
  DWORD offset = sizeof(*rel);
  rel->Owner = 0; rel->Group = 0; rel->Sacl = 0; rel->Dacl = 0;
  if (abs->Owner) { rel->Owner = offset; length = RtlLengthSid(abs->Owner); memcpy((BYTE *)rel + offset, abs->Owner, length); offset += length; }
  if (abs->Group) { rel->Group = offset; length = RtlLengthSid(abs->Group); memcpy((BYTE *)rel + offset, abs->Group, length); offset += length; }
  if (abs->Sacl) { rel->Sacl = offset; length = abs->Sacl->AclSize; memcpy((BYTE *)rel + offset, abs->Sacl, length); offset += length; }
  if (abs->Dacl) { rel->Dacl = offset; length = abs->Dacl->AclSize; memcpy((BYTE *)rel + offset, abs->Dacl, length); }
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI RtlAbsoluteToSelfRelativeSD(PVOID absolute, PVOID relative, ULONG *buffer_length) {
  if (((SECURITY_DESCRIPTOR_ *)absolute)->Control & SEC_SELF_RELATIVE) return SEC_STATUS_BAD_DESCRIPTOR_FORMAT;
  return RtlMakeSelfRelativeSD(absolute, relative, buffer_length);
}
