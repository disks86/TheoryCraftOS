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
/* ntdll.dll: process creation with the Windows interface of NtCreateUserProcess (the system's own service, with its own arguments, is TcosCreateUserProcess; kernel32 uses that).
   Included by ntdll.c after atom.inc. */

__asm__(".text\n.globl TcosCreateUserProcess\nTcosCreateUserProcess:\n mov %rcx, %r10\n mov $" _STR(NT_SERVICE_NtCreateUserProcess) ", %eax\n syscall\n ret\n"
        ".section .drectve\n.ascii \" -export:TcosCreateUserProcess\"\n.text\n");
NTSTATUS NTAPI TcosCreateUserProcess(PHANDLE process, PHANDLE thread, PUNICODE_STRING image, PUNICODE_STRING command, PUNICODE_STRING cwd, PVOID environment, ULONG flags,
                                     PULONG process_id, PULONG thread_id, HANDLE std_in, HANDLE std_out, HANDLE std_err, PVOID startup);

typedef struct { ULONG_PTR Attribute; SIZE_T Size; ULONG_PTR Value; PSIZE_T ReturnLength; } PS_ATTRIBUTE_;
typedef struct { SIZE_T TotalLength; PS_ATTRIBUTE_ Attributes[1]; } PS_ATTRIBUTE_LIST_;

/* NtCreateUserProcess(ProcessHandle, ThreadHandle, ProcessAccess, ThreadAccess, ProcessAttributes, ThreadAttributes, ProcessFlags, ThreadFlags, Parameters, CreateInfo, Attributes):
   the image comes from the PS_ATTRIBUTE_IMAGE_NAME attribute (an NT path), the rest from the process parameters. Only a suspended first thread is made here. */
EXPORT NTSTATUS NTAPI NtCreateUserProcess(PHANDLE process, PHANDLE thread, ULONG process_access, ULONG thread_access, PVOID process_attributes, PVOID thread_attributes,
                                          ULONG process_flags, ULONG thread_flags, RTL_USER_PROCESS_PARAMETERS *params, PVOID create_info, PS_ATTRIBUTE_LIST_ *attributes) {
  (void)process_access; (void)thread_access; (void)process_attributes; (void)thread_attributes; (void)create_info;
  if (!process || !thread || !params || !attributes) return STATUS_INVALID_PARAMETER;
  if (thread_flags & ~1u) return STATUS_INVALID_PARAMETER;      /* (only THREAD_CREATE_FLAGS_CREATE_SUSPENDED is taken at creation) */
  const WCHAR *path = NULL;
  SIZE_T chars = 0;
  const SIZE_T count = attributes->TotalLength >= sizeof(SIZE_T) ? (attributes->TotalLength - sizeof(SIZE_T)) / sizeof(PS_ATTRIBUTE_) : 0;
  for (SIZE_T i = 0; i < count; ++i) {
    if ((attributes->Attributes[i].Attribute & 0xFFFF) == 5 /* PsAttributeImageName */) { path = (const WCHAR *)attributes->Attributes[i].Value; chars = attributes->Attributes[i].Size / 2; }
  }
  if (!path) return STATUS_INVALID_PARAMETER;
  RtlNormalizeProcessParams(params);
  if (chars >= 4 && path[0] == '\\' && path[1] == '?' && path[2] == '?' && path[3] == '\\') { path += 4; chars -= 4; }      /* (the system takes a Win32 path) */
  UNICODE_STRING image;
  image.Buffer = (PWSTR)path;
  image.Length = image.MaximumLength = (USHORT)(chars * 2);
  const RTL_USER_PROCESS_PARAMETERS *own = NtCurrentPeb()->ProcessParameters;
  PVOID environment = params->Environment ? params->Environment : own->Environment;
  ULONG pid = 0, tid = 0;
  const ULONG nt_flags = ((thread_flags & 1) ? 1u : 0u) | ((process_flags & 4) ? 2u : 0u);      /* (suspended; PROCESS_CREATE_FLAGS_INHERIT_HANDLES) */
  return TcosCreateUserProcess(process, thread, &image, params->CommandLine.Buffer ? &params->CommandLine : NULL, params->CurrentDirectoryPath.Buffer ? &params->CurrentDirectoryPath : NULL,
                               environment, nt_flags, &pid, &tid, NULL, NULL, NULL, NULL);
}
EXPORT NTSTATUS NTAPI ZwCreateUserProcess(PHANDLE process, PHANDLE thread, ULONG pa, ULONG ta, PVOID po, PVOID to, ULONG pf, ULONG tf, RTL_USER_PROCESS_PARAMETERS *params, PVOID info, PS_ATTRIBUTE_LIST_ *attributes) {
  return NtCreateUserProcess(process, thread, pa, ta, po, to, pf, tf, params, info, attributes);
}
