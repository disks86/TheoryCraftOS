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
/* The form in which an access token is kept: a token object holds one blob, the kernel (src/Process/WindowsTokens.cpp) and ntdll.dll (src/Windows/ntdll/token.c) agree on
 * its layout. The kernel builds the first token of the system and reads the blob to decide on access; ntdll does everything else (the information classes, privileges, copies,
 * restricted tokens) on a copy of the blob and writes it back. All offsets count from the start of the blob; all numbers are little endian. */
#ifndef TCOS_TOKEN_H
#define TCOS_TOKEN_H

#define TCOS_TOKEN_MAGIC 0x4B4F5454u      /* "TTOK" */
#define TCOS_TOKEN_MAX_BYTES 8192u
#define TCOS_TOKEN_MAX_GROUPS 32u
#define TCOS_TOKEN_MAX_PRIVILEGES 40u
#define TCOS_TOKEN_MAX_RESTRICTED 16u

/* The operations of the NtTcosToken service: NtTcosToken(operation, a, b, c, d, e). */
enum {
  TCOS_TOKEN_CREATE = 0,          /* (blob, length, access, inherit, &handle): a new token object holding a copy of the blob */
  TCOS_TOKEN_READ = 1,            /* (handle, required access, buffer, capacity, &needed) */
  TCOS_TOKEN_WRITE = 2,           /* (handle, required access, blob, length) */
  TCOS_TOKEN_OPEN_PROCESS = 3,    /* (process handle, access, &handle): the token of a process (made from the default when it has none) */
  TCOS_TOKEN_OPEN_THREAD = 4,     /* (thread handle, access, &handle): the impersonation token of a thread, STATUS_NO_TOKEN when it has none */
  TCOS_TOKEN_SET_THREAD = 5,      /* (thread handle, token handle or 0 to revert) */
  TCOS_OBJECT_SD_GET = 6,         /* (handle, required access, buffer, capacity, &needed): the security descriptor kept for the object, needed 0 if none */
  TCOS_OBJECT_SD_SET = 7,         /* (handle, required access, descriptor, length) */
  TCOS_TOKEN_SET_PROCESS = 8,     /* (process handle, token handle): the token a process runs as (a process that has not started, as CreateProcessAsUser makes it) */
  TCOS_TOKEN_CALLER = 9,          /* (&handle): the token the caller's access is decided by: the impersonation token of the thread, else the token of the process */
  TCOS_THREAD_SD_SET = 12,        /* (handle of a thread, 0, descriptor, length): like TCOS_OBJECT_SD_SET for a thread that is named by a process handle (the first thread has the id of its process) */
  TCOS_FILE_PARENT_SD = 11        /* (handle of a file, buffer, capacity, &needed): the descriptor kept for the directory the file is in, needed 0 if none */
};

#pragma pack(push, 1)
typedef struct {
  unsigned int magic, size;            /* TCOS_TOKEN_MAGIC; the length of the whole blob */
  unsigned int type, level;            /* TokenPrimary (1) or TokenImpersonation (2); SECURITY_IMPERSONATION_LEVEL */
  unsigned long long token_id, auth_id, modified_id, expiration;
  unsigned int session, elevation_type, flags, mandatory_policy;      /* flags: 1 sandbox inert, 2 has restrictions (write restricted: 4), 8 virtualization allowed, 16 virtualization enabled, 32 UIAccess */
  unsigned int user_off, user_attrs;
  unsigned int owner_off, pgroup_off, dacl_off;      /* 0: none (the default DACL is an ACL) */
  unsigned int group_count, groups_off;               /* each entry: unsigned sid offset, unsigned attributes */
  unsigned int privilege_count, privileges_off;       /* each entry: unsigned low, int high, unsigned attributes */
  unsigned int restricted_count, restricted_off;      /* like the groups */
  unsigned char source_name[8];
  unsigned long long source_id, origin;
  unsigned int reserved[4];
} TcosTokenBlob;
#pragma pack(pop)

#define TCOS_TOKEN_FLAG_SANDBOX_INERT 1u
#define TCOS_TOKEN_FLAG_HAS_RESTRICTIONS 2u
#define TCOS_TOKEN_FLAG_WRITE_RESTRICTED 4u
#define TCOS_TOKEN_FLAG_VIRT_ALLOWED 8u
#define TCOS_TOKEN_FLAG_VIRT_ENABLED 16u
#define TCOS_TOKEN_FLAG_UIACCESS 32u

#endif
