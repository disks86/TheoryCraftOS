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

/* The 32-bit ntdll.dll's way to the kernel (docs/Wow64.md). The kernel's services are written for 64-bit programs; a 32-bit program calls them through
 * INT 2E with the service number in EAX and, in EDX, the address of the arguments, each widened to 64 bits, and with the structures that depend on the
 * pointer size in their 64-bit layout. This file has what the generated Nt* functions (wow64_services.inc) are made of, the services whose buffers need
 * converting by hand, and the start of a 32-bit process: the 32-bit PEB, process parameters and loader lists, made from the 64-bit ones the kernel wrote.
 * Included by ntdll.c when it is built for i386 only. */

#define WOW_HANDLE(h) ((ULONG64)(LONG64)(LONG_PTR)(h))      /* sign-extended: the pseudo handles -1 and -2 stay what they are */
#define WOW_POINTER(p) ((ULONG64)(ULONG_PTR)(p))

static __attribute__((noinline)) NTSTATUS wow_service(ULONG number, const ULONG64 *arguments) {
  NTSTATUS status;
  __asm__ volatile("int $0x2e" : "=a"(status) : "a"(number), "d"(arguments) : "memory", "cc");
  return status;
}

/* The structures nearly every service reads, as a 64-bit program has them. */
typedef struct { USHORT Length, MaximumLength; ULONG pad; ULONG64 Buffer; } UNICODE_STRING64;
typedef struct {
  ULONG Length, pad0;
  ULONG64 RootDirectory, ObjectName;
  ULONG Attributes, pad1;
  ULONG64 SecurityDescriptor, SecurityQualityOfService;
  UNICODE_STRING64 name;      /* (what ObjectName points at) */
} WOW_OBJECT_ATTRIBUTES;

static ULONG64 wow_unicode_string(UNICODE_STRING64 *out, const UNICODE_STRING *in) {
  if (!in) return 0;
  out->Length = in->Length;
  out->MaximumLength = in->MaximumLength;
  out->pad = 0;
  out->Buffer = WOW_POINTER(in->Buffer);
  return WOW_POINTER(out);
}

static ULONG64 wow_object_attributes(WOW_OBJECT_ATTRIBUTES *out, const OBJECT_ATTRIBUTES *in) {
  if (!in) return 0;
  out->Length = 0x30;
  out->pad0 = out->pad1 = 0;
  out->RootDirectory = WOW_HANDLE(in->RootDirectory);
  out->ObjectName = wow_unicode_string(&out->name, in->ObjectName);
  out->Attributes = in->Attributes;
  out->SecurityDescriptor = WOW_POINTER(in->SecurityDescriptor);      /* (a self-relative descriptor is the same for both; an absolute one is not understood) */
  out->SecurityQualityOfService = WOW_POINTER(in->SecurityQualityOfService);
  return WOW_POINTER(out);
}

#include "wow64_services.inc"

/* ---- services written by hand ------------------------------------------------------------------------------------------------------------------------ */

/* The handles are an array of pointer-sized values. */
EXPORT NTSTATUS NTAPI NtWaitForMultipleObjects(ULONG count, const HANDLE *handles, ULONG type, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  ULONG64 wide[64];
  if (count > 64) return (NTSTATUS)0xC00000EFu;      /* STATUS_INVALID_PARAMETER_1 */
  for (ULONG i = 0; i < count; ++i) wide[i] = WOW_HANDLE(handles[i]);
  const ULONG64 v[5] = {count, WOW_POINTER(wide), type, alertable, WOW_POINTER(timeout)};
  return wow_service(NT_SERVICE_NtWaitForMultipleObjects, v);
}
EXPORT NTSTATUS NTAPI ZwWaitForMultipleObjects(ULONG count, const HANDLE *handles, ULONG type, BOOLEAN alertable, PLARGE_INTEGER timeout) {
  return NtWaitForMultipleObjects(count, handles, type, alertable, timeout);
}

/* MEMORY_BASIC_INFORMATION (class 0) has pointer-sized fields; the other classes are passed as they are. */
EXPORT NTSTATUS NTAPI NtQueryVirtualMemory(HANDLE process, PVOID address, ULONG information_class, PVOID buffer, SIZE_T size, PSIZE_T returned) {
  ULONG64 length = 0;
  if (information_class != 0) {
    const ULONG64 v[6] = {WOW_HANDLE(process), WOW_POINTER(address), information_class, WOW_POINTER(buffer), size, WOW_POINTER(&length)};
    const NTSTATUS status = wow_service(NT_SERVICE_NtQueryVirtualMemory, v);
    if (returned) *returned = (SIZE_T)length;
    return status;
  }
  struct { ULONG64 BaseAddress, AllocationBase; ULONG AllocationProtect, pad; ULONG64 RegionSize; ULONG State, Protect, Type, pad2; } wide;
  if (size < sizeof(MEMORY_BASIC_INFORMATION)) return (NTSTATUS)0xC0000004u;      /* STATUS_INFO_LENGTH_MISMATCH */
  const ULONG64 v[6] = {WOW_HANDLE(process), WOW_POINTER(address), 0, WOW_POINTER(&wide), sizeof(wide), WOW_POINTER(&length)};
  const NTSTATUS status = wow_service(NT_SERVICE_NtQueryVirtualMemory, v);
  if (status < 0) return status;
  MEMORY_BASIC_INFORMATION *out = (MEMORY_BASIC_INFORMATION *)buffer;
  out->BaseAddress = (PVOID)(ULONG_PTR)wide.BaseAddress;
  out->AllocationBase = (PVOID)(ULONG_PTR)wide.AllocationBase;
  out->AllocationProtect = wide.AllocationProtect;
  out->RegionSize = wide.RegionSize > 0xFFFFFFFFull ? 0xFFFFF000u : (SIZE_T)wide.RegionSize;
  out->State = wide.State;
  out->Protect = wide.Protect;
  out->Type = wide.Type;
  if (returned) *returned = sizeof(MEMORY_BASIC_INFORMATION);
  return status;
}

static PEB *g_wow_peb;      /* the 32-bit PEB (wow64_start_process) */

/* ProcessBasicInformation (class 0) and ThreadBasicInformation (class 0) have pointer-sized fields - and name the PEB and the TEB, which for a 32-bit
 * program are the 32-bit ones. The other classes are passed as they are. */
EXPORT NTSTATUS NTAPI NtQueryInformationProcess(HANDLE process, ULONG information_class, PVOID buffer, ULONG size, PULONG returned) {
  if (information_class != 0) {
    const ULONG64 v[5] = {WOW_HANDLE(process), information_class, WOW_POINTER(buffer), size, WOW_POINTER(returned)};
    return wow_service(NT_SERVICE_NtQueryInformationProcess, v);
  }
  struct { ULONG ExitStatus, pad; ULONG64 PebBaseAddress, AffinityMask; ULONG BasePriority, pad2; ULONG64 UniqueProcessId, InheritedFromUniqueProcessId; } wide;
  struct Narrow { ULONG ExitStatus; PVOID PebBaseAddress; ULONG_PTR AffinityMask; ULONG BasePriority; ULONG_PTR UniqueProcessId, InheritedFromUniqueProcessId; };
  if (size != sizeof(struct Narrow)) return (NTSTATUS)0xC0000004u;
  const ULONG64 v[5] = {WOW_HANDLE(process), 0, WOW_POINTER(&wide), sizeof(wide), 0};
  const NTSTATUS status = wow_service(NT_SERVICE_NtQueryInformationProcess, v);
  if (status < 0) return status;
  struct Narrow *out = (struct Narrow *)buffer;
  out->ExitStatus = wide.ExitStatus;
  out->PebBaseAddress = process == NtCurrentProcess() ? (PVOID)g_wow_peb : (PVOID)(ULONG_PTR)wide.PebBaseAddress;      /* (another process's: the PEB the kernel knows) */
  out->AffinityMask = (ULONG_PTR)wide.AffinityMask;
  out->BasePriority = wide.BasePriority;
  out->UniqueProcessId = (ULONG_PTR)wide.UniqueProcessId;
  out->InheritedFromUniqueProcessId = (ULONG_PTR)wide.InheritedFromUniqueProcessId;
  if (returned) *returned = sizeof(struct Narrow);
  return status;
}
EXPORT NTSTATUS NTAPI NtQueryInformationThread(HANDLE thread, ULONG information_class, PVOID buffer, ULONG size, PULONG returned) {
  if (information_class != 0) {
    const ULONG64 v[5] = {WOW_HANDLE(thread), information_class, WOW_POINTER(buffer), size, WOW_POINTER(returned)};
    return wow_service(NT_SERVICE_NtQueryInformationThread, v);
  }
  struct { ULONG ExitStatus, pad; ULONG64 TebBaseAddress, UniqueProcess, UniqueThread, AffinityMask; ULONG Priority, BasePriority; } wide;
  struct Narrow { ULONG ExitStatus; PVOID TebBaseAddress; ULONG_PTR UniqueProcess, UniqueThread, AffinityMask; ULONG Priority, BasePriority; };
  if (size != sizeof(struct Narrow)) return (NTSTATUS)0xC0000004u;
  const ULONG64 v[5] = {WOW_HANDLE(thread), 0, WOW_POINTER(&wide), sizeof(wide), 0};
  const NTSTATUS status = wow_service(NT_SERVICE_NtQueryInformationThread, v);
  if (status < 0) return status;
  struct Narrow *out = (struct Narrow *)buffer;
  out->ExitStatus = wide.ExitStatus;
  out->TebBaseAddress = wide.TebBaseAddress ? (PVOID)((ULONG_PTR)wide.TebBaseAddress + 0x2000) : NULL;      /* the 32-bit TEB follows the one the kernel names */
  out->UniqueProcess = (ULONG_PTR)wide.UniqueProcess;
  out->UniqueThread = (ULONG_PTR)wide.UniqueThread;
  out->AffinityMask = (ULONG_PTR)wide.AffinityMask;
  out->Priority = wide.Priority;
  out->BasePriority = wide.BasePriority;
  if (returned) *returned = sizeof(struct Narrow);
  return status;
}

/* Passed as they are: their buffers hold no pointers in the classes these libraries ask for. (The classes that do are not converted yet.) */
EXPORT NTSTATUS NTAPI NtQuerySystemInformation(ULONG information_class, PVOID buffer, ULONG size, PULONG returned) {
  const ULONG64 v[4] = {information_class, WOW_POINTER(buffer), size, WOW_POINTER(returned)};
  return wow_service(NT_SERVICE_NtQuerySystemInformation, v);
}
EXPORT NTSTATUS NTAPI NtQueryObject(HANDLE handle, ULONG information_class, PVOID buffer, ULONG size, PULONG returned) {
  const ULONG64 v[5] = {WOW_HANDLE(handle), information_class, WOW_POINTER(buffer), size, WOW_POINTER(returned)};
  return wow_service(NT_SERVICE_NtQueryObject, v);
}

/* ---- the start of a 32-bit process ------------------------------------------------------------------------------------------------------------------- */

/* The kernel wrote the PEB, the process parameters and the loader lists as a 64-bit program reads them (it reads them itself). A 32-bit program - and
 * every line of these libraries, built for i386 - reads the 32-bit ones, which are made here, once, before anything looks: the first thread calls this
 * first thing. The strings stay where they are (below 4 GiB, like everything of this process); only the headers that point at them are new. */
static ULONG64 wide_at(const BYTE *p) { return *(const ULONG64 *)p; }
static void narrow_string(UNICODE_STRING *out, const BYTE *in) {
  out->Length = *(const USHORT *)in;
  out->MaximumLength = *(const USHORT *)(in + 2);
  out->Buffer = (PWSTR)(ULONG_PTR)wide_at(in + 8);
}

static void wow64_start_process(const BYTE *peb64) {
  const BYTE *parameters64 = (const BYTE *)(ULONG_PTR)wide_at(peb64 + 0x20);
  const BYTE *ldr64 = (const BYTE *)(ULONG_PTR)wide_at(peb64 + 0x18);
  /* How many modules the kernel listed. */
  ULONG modules = 0;
  if (ldr64) {
    const BYTE *head = ldr64 + 0x10;
    for (const BYTE *e = (const BYTE *)(ULONG_PTR)wide_at(head); e != head && modules < 4096; e = (const BYTE *)(ULONG_PTR)wide_at(e)) ++modules;
  }
  const SIZE_T entry_bytes = (sizeof(LDR_DATA_TABLE_ENTRY) + 15) & ~(SIZE_T)15;
  PVOID base = NULL;
  SIZE_T size = 0x2000 + sizeof(PEB_LDR_DATA) + 16 + (SIZE_T)modules * entry_bytes;
  if (NtAllocateVirtualMemory(NtCurrentProcess(), &base, 0, &size, 0x3000, 4) < 0) NtTerminateProcess(NtCurrentProcess(), (NTSTATUS)0xC0000017u);      /* MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE */
  BYTE *block = (BYTE *)base;
  PEB *peb = (PEB *)block;
  RTL_USER_PROCESS_PARAMETERS *parameters = (RTL_USER_PROCESS_PARAMETERS *)(block + 0x1000);
  PEB_LDR_DATA *ldr = (PEB_LDR_DATA *)(block + 0x2000);
  LDR_DATA_TABLE_ENTRY *entries = (LDR_DATA_TABLE_ENTRY *)((BYTE *)ldr + ((sizeof(PEB_LDR_DATA) + 15) & ~(SIZE_T)15));

  /* The PEB: what the structure names, then the fields programs read by their (x86) offsets. */
  peb->BeingDebugged = peb64[2];
  peb->Mutant = (PVOID)(LONG_PTR)-1;
  peb->ImageBaseAddress = (PVOID)(ULONG_PTR)wide_at(peb64 + 0x10);
  *(ULONG *)(block + 0x64) = *(const ULONG *)(peb64 + 0xB8);       /* NumberOfProcessors */
  *(ULONG *)(block + 0x68) = *(const ULONG *)(peb64 + 0xBC);       /* NtGlobalFlag */
  *(ULONG *)(block + 0xA4) = *(const ULONG *)(peb64 + 0x118);      /* OSMajorVersion */
  *(ULONG *)(block + 0xA8) = *(const ULONG *)(peb64 + 0x11C);      /* OSMinorVersion */
  *(USHORT *)(block + 0xAC) = *(const USHORT *)(peb64 + 0x120);    /* OSBuildNumber */
  *(ULONG *)(block + 0xB0) = *(const ULONG *)(peb64 + 0x124);      /* OSPlatformId */
  *(ULONG *)(block + 0x1D4) = *(const ULONG *)(peb64 + 0x2C0);     /* SessionId */

  if (parameters64) {
    parameters->MaximumLength = parameters->Length = sizeof(*parameters);
    parameters->Flags = *(const ULONG *)(parameters64 + 0x08);
    parameters->ConsoleHandle = (HANDLE)(ULONG_PTR)wide_at(parameters64 + 0x10);
    parameters->ConsoleFlags = *(const ULONG *)(parameters64 + 0x18);
    parameters->StandardInput = (HANDLE)(ULONG_PTR)wide_at(parameters64 + 0x20);
    parameters->StandardOutput = (HANDLE)(ULONG_PTR)wide_at(parameters64 + 0x28);
    parameters->StandardError = (HANDLE)(ULONG_PTR)wide_at(parameters64 + 0x30);
    narrow_string(&parameters->CurrentDirectoryPath, parameters64 + 0x38);
    parameters->CurrentDirectoryHandle = (HANDLE)(ULONG_PTR)wide_at(parameters64 + 0x48);
    narrow_string(&parameters->DllPath, parameters64 + 0x50);
    narrow_string(&parameters->ImagePathName, parameters64 + 0x60);
    narrow_string(&parameters->CommandLine, parameters64 + 0x70);
    parameters->Environment = (PVOID)(ULONG_PTR)wide_at(parameters64 + 0x80);
    const ULONG *numbers = (const ULONG *)(parameters64 + 0x88);      /* StartingX ... ShowWindowFlags */
    parameters->StartingX = numbers[0]; parameters->StartingY = numbers[1]; parameters->CountX = numbers[2]; parameters->CountY = numbers[3];
    parameters->CountCharsX = numbers[4]; parameters->CountCharsY = numbers[5]; parameters->FillAttribute = numbers[6];
    parameters->WindowFlags = numbers[7]; parameters->ShowWindowFlags = numbers[8];
    narrow_string(&parameters->WindowTitle, parameters64 + 0xB0);
    narrow_string(&parameters->DesktopInfo, parameters64 + 0xC0);
    peb->ProcessParameters = parameters;
  }

  if (ldr64) {
    ldr->Length = sizeof(*ldr);
    ldr->Initialized = 1;
    LIST_ENTRY *load = &ldr->InLoadOrderModuleList, *memory = &ldr->InMemoryOrderModuleList, *init = &ldr->InInitializationOrderModuleList;
    load->Flink = load->Blink = load;
    memory->Flink = memory->Blink = memory;
    init->Flink = init->Blink = init;
    /* The modules in the order they were loaded (and the same order in memory). */
    const BYTE *head = ldr64 + 0x10;
    ULONG i = 0;
    for (const BYTE *e = (const BYTE *)(ULONG_PTR)wide_at(head); e != head && i < modules; e = (const BYTE *)(ULONG_PTR)wide_at(e), ++i) {
      LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)((BYTE *)entries + i * entry_bytes);
      m->DllBase = (PVOID)(ULONG_PTR)wide_at(e + 0x30);
      m->EntryPoint = (PVOID)(ULONG_PTR)wide_at(e + 0x38);
      m->SizeOfImage = *(const ULONG *)(e + 0x40);
      narrow_string(&m->FullDllName, e + 0x48);
      narrow_string(&m->BaseDllName, e + 0x58);
      m->Flags = *(const ULONG *)(e + 0x68);
      m->LoadCount = *(const USHORT *)(e + 0x6C);
      m->TlsIndex = *(const USHORT *)(e + 0x6E);
      m->InLoadOrderLinks.Flink = load; m->InLoadOrderLinks.Blink = load->Blink; load->Blink->Flink = &m->InLoadOrderLinks; load->Blink = &m->InLoadOrderLinks;
      m->InMemoryOrderLinks.Flink = memory; m->InMemoryOrderLinks.Blink = memory->Blink; memory->Blink->Flink = &m->InMemoryOrderLinks; memory->Blink = &m->InMemoryOrderLinks;
      m->InInitializationOrderLinks.Flink = m->InInitializationOrderLinks.Blink = &m->InInitializationOrderLinks;      /* (not in that list unless the kernel put it there) */
    }
    /* The order their entry points run in: the kernel's list links the entries at +0x20; each is found again by its place in the load order. */
    const BYTE *init_head = ldr64 + 0x30;
    for (const BYTE *link = (const BYTE *)(ULONG_PTR)wide_at(init_head); link != init_head; link = (const BYTE *)(ULONG_PTR)wide_at(link)) {
      const BYTE *wanted = link - 0x20;
      ULONG k = 0;
      const BYTE *e = (const BYTE *)(ULONG_PTR)wide_at(head);
      for (; e != head && e != wanted && k < modules; e = (const BYTE *)(ULONG_PTR)wide_at(e)) ++k;
      if (e != wanted || k >= modules) continue;
      LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)((BYTE *)entries + k * entry_bytes);
      m->InInitializationOrderLinks.Flink = init; m->InInitializationOrderLinks.Blink = init->Blink; init->Blink->Flink = &m->InInitializationOrderLinks; init->Blink = &m->InInitializationOrderLinks;
    }
    peb->Ldr = ldr;
  }
  g_wow_peb = peb;
  *(PEB **)TEB_BYTE(NtCurrentTeb(), 0x30) = peb;      /* TEB.ProcessEnvironmentBlock: NtCurrentPeb() from here on */
}

/* A thread made later finds the PEB the first one made. */
static void wow64_start_thread(void) { *(PEB **)TEB_BYTE(NtCurrentTeb(), 0x30) = g_wow_peb; }
