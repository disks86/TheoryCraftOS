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

/* ntdll.dll: the rest of the loader's interface: looking up exports (and following forwarders), the resource directory of an image, delay-loaded imports, the options of an image
 * file in the registry, the list of modules for a debugger's sake. Included by ntdll.c at the end, after the loader itself. */

#define LDR_STATUS_PROCEDURE_NOT_FOUND ((NTSTATUS)0xC000007A)
#define LDR_STATUS_ENTRYPOINT_NOT_FOUND ((NTSTATUS)0xC0000139)
#define LDR_STATUS_RESOURCE_DATA_NOT_FOUND ((NTSTATUS)0xC0000089)
#define LDR_STATUS_RESOURCE_TYPE_NOT_FOUND ((NTSTATUS)0xC000008A)
#define LDR_STATUS_RESOURCE_NAME_NOT_FOUND ((NTSTATUS)0xC000008B)
#define LDR_STATUS_RESOURCE_LANG_NOT_FOUND ((NTSTATUS)0xC0000204)

/* ---- exports ---- */

static int ldr_strcmp(const char *a, const char *b) {
  while (*a && *a == *b) { ++a; ++b; }
  return (unsigned char)*a - (unsigned char)*b;
}

/* The export of the image at `base` called `name` (or numbered `ordinal` if there is no name); a forwarder is followed into the library it names, which is loaded. */
static NTSTATUS ldr_export(PVOID base, const char *name, ULONG ordinal, ULONG depth, PVOID *out) {
  ULONG size = 0;
  const BYTE *directory = RtlImageDirectoryEntryToData(base, TRUE, 0 /* IMAGE_DIRECTORY_ENTRY_EXPORT */, &size);
  if (!directory || size < 0x28) return name ? LDR_STATUS_ENTRYPOINT_NOT_FOUND : LDR_STATUS_PROCEDURE_NOT_FOUND;
  const BYTE *image = base;
  const ULONG ordinal_base = *(const ULONG *)(directory + 0x10), function_count = *(const ULONG *)(directory + 0x14), name_count = *(const ULONG *)(directory + 0x18);
  const ULONG *functions = (const ULONG *)(image + *(const ULONG *)(directory + 0x1C));
  const ULONG *names = (const ULONG *)(image + *(const ULONG *)(directory + 0x20));
  const USHORT *ordinals = (const USHORT *)(image + *(const ULONG *)(directory + 0x24));
  ULONG index;
  if (name) {
    LONG low = 0, high = (LONG)name_count - 1;
    LONG found = -1;
    while (low <= high) {
      const LONG middle = (low + high) / 2;
      const int order = ldr_strcmp(name, (const char *)(image + names[middle]));
      if (order == 0) { found = middle; break; }
      if (order < 0) high = middle - 1; else low = middle + 1;
    }
    if (found < 0) { for (ULONG i = 0; i < name_count && found < 0; ++i) if (ldr_strcmp(name, (const char *)(image + names[i])) == 0) found = (LONG)i; }      /* (a table that is not in order) */
    if (found < 0) return LDR_STATUS_ENTRYPOINT_NOT_FOUND;
    index = ordinals[found];
  } else {
    if (ordinal < ordinal_base) return LDR_STATUS_PROCEDURE_NOT_FOUND;
    index = ordinal - ordinal_base;
  }
  if (index >= function_count) return name ? LDR_STATUS_ENTRYPOINT_NOT_FOUND : LDR_STATUS_PROCEDURE_NOT_FOUND;
  const ULONG rva = functions[index];
  if (rva == 0) return name ? LDR_STATUS_ENTRYPOINT_NOT_FOUND : LDR_STATUS_PROCEDURE_NOT_FOUND;
  const ULONG directory_rva = (ULONG)(directory - image);
  if (rva < directory_rva || rva >= directory_rva + size) { *out = (PVOID)(image + rva); return STATUS_SUCCESS; }
  /* a forwarder: "library.function" or "library.#ordinal" */
  if (depth >= 16) return LDR_STATUS_ENTRYPOINT_NOT_FOUND;
  const char *forward = (const char *)(image + rva);
  ULONG dot = 0;
  while (forward[dot] && forward[dot] != '.') ++dot;
  if (!forward[dot] || dot == 0 || dot > 200) return LDR_STATUS_ENTRYPOINT_NOT_FOUND;
  WCHAR library[216];
  for (ULONG i = 0; i < dot; ++i) library[i] = (unsigned char)forward[i];
  library[dot] = '.'; library[dot + 1] = 'd'; library[dot + 2] = 'l'; library[dot + 3] = 'l'; library[dot + 4] = 0;
  UNICODE_STRING text;
  text.Buffer = library;
  text.Length = (USHORT)((dot + 4) * 2);
  text.MaximumLength = text.Length + 2;
  PVOID target = NULL;
  NTSTATUS status = LdrGetDllHandle(NULL, 0, &text, &target);
  if (!NT_SUCCESS(status)) status = LdrLoadDll(NULL, NULL, &text, &target);
  if (!NT_SUCCESS(status)) return status;
  const char *symbol = forward + dot + 1;
  if (symbol[0] == '#') {
    ULONG number = 0;
    for (const char *p = symbol + 1; *p >= '0' && *p <= '9'; ++p) number = number * 10 + (ULONG)(*p - '0');
    return ldr_export(target, NULL, number, depth + 1, out);
  }
  return ldr_export(target, symbol, 0, depth + 1, out);
}

EXPORT NTSTATUS NTAPI LdrGetProcedureAddressEx(PVOID module, PANSI_STRING name, ULONG ordinal, PVOID *address, ULONG flags) {
  (void)flags;
  if (!address) return STATUS_INVALID_PARAMETER;
  if (!module) return STATUS_DLL_NOT_FOUND;
  char text[300];
  const char *symbol = NULL;
  if (name && name->Buffer && name->Length) {
    if (name->Length >= sizeof(text)) return LDR_STATUS_ENTRYPOINT_NOT_FOUND;
    for (USHORT i = 0; i < name->Length; ++i) text[i] = name->Buffer[i];
    text[name->Length] = 0;
    symbol = text;
  }
  return ldr_export(module, symbol, ordinal, 0, address);
}
EXPORT NTSTATUS NTAPI LdrGetProcedureAddress(PVOID module, PANSI_STRING name, ULONG ordinal, PVOID *address) { return LdrGetProcedureAddressEx(module, name, ordinal, address, 0); }
EXPORT NTSTATUS NTAPI LdrGetProcedureAddressForCaller(PVOID module, PANSI_STRING name, ULONG ordinal, PVOID *address, ULONG flags, PVOID *caller) {
  (void)caller;
  return LdrGetProcedureAddressEx(module, name, ordinal, address, flags);
}
/* The address of an export by name; forwarders are not followed. */
EXPORT PVOID NTAPI RtlFindExportedRoutineByName(PVOID base, PCSTR name) {
  ULONG size = 0;
  const BYTE *directory = RtlImageDirectoryEntryToData(base, TRUE, 0, &size);
  if (!directory || size < 0x28 || !name) return NULL;
  const BYTE *image = base;
  const ULONG function_count = *(const ULONG *)(directory + 0x14), name_count = *(const ULONG *)(directory + 0x18);
  const ULONG *functions = (const ULONG *)(image + *(const ULONG *)(directory + 0x1C));
  const ULONG *names = (const ULONG *)(image + *(const ULONG *)(directory + 0x20));
  const USHORT *ordinals = (const USHORT *)(image + *(const ULONG *)(directory + 0x24));
  for (ULONG i = 0; i < name_count; ++i) {
    if (ldr_strcmp(name, (const char *)(image + names[i])) != 0) continue;
    if (ordinals[i] >= function_count) return NULL;
    const ULONG rva = functions[ordinals[i]];
    const ULONG directory_rva = (ULONG)(directory - image);
    return rva >= directory_rva && rva < directory_rva + size ? NULL : (PVOID)(image + rva);
  }
  return NULL;
}

/* ---- the library list ---- */

EXPORT NTSTATUS NTAPI LdrDisableThreadCalloutsForDll(PVOID base) {
  loader_lock();
  LDR_DATA_TABLE_ENTRY *e = ldr_find_base(base);
  NTSTATUS status = STATUS_DLL_NOT_FOUND;
  if (e) {
    status = STATUS_SUCCESS;
    e->Flags |= LDRP_DONT_CALL_FOR_THREADS;
  }
  loader_unlock();
  return status;
}
EXPORT NTSTATUS NTAPI LdrGetDllHandleByMapping(PVOID base, PVOID *handle) {
  loader_lock();
  LDR_DATA_TABLE_ENTRY *e = ldr_find_base(base);
  loader_unlock();
  if (!e) return STATUS_DLL_NOT_FOUND;
  *handle = e->DllBase;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrGetDllHandleByName(PUNICODE_STRING base_name, PUNICODE_STRING full_name, PVOID *handle) {
  if (!handle || (!base_name && !full_name)) return STATUS_INVALID_PARAMETER;
  loader_lock();
  LDR_DATA_TABLE_ENTRY *found = NULL;
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  for (LIST_ENTRY *l = ldr->InLoadOrderModuleList.Flink; l != &ldr->InLoadOrderModuleList && !found; l = l->Flink) {
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)l;
    if (full_name && RtlEqualUnicodeString(full_name, &m->FullDllName, TRUE)) found = m;
    else if (base_name && RtlEqualUnicodeString(base_name, &m->BaseDllName, TRUE)) found = m;
  }
  loader_unlock();
  if (!found) return STATUS_DLL_NOT_FOUND;
  *handle = found->DllBase;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrGetKnownDllSectionHandle(PCWSTR name, BOOLEAN wow64, PHANDLE section) { (void)name; (void)wow64; (void)section; return STATUS_DLL_NOT_FOUND; }

/* RTL_PROCESS_MODULES: a count, then one record of 0x128 bytes per library (the first at offset 8). */
EXPORT NTSTATUS NTAPI LdrQueryProcessModuleInformation(PVOID buffer, ULONG size, PULONG needed) {
  loader_lock();
  PEB_LDR_DATA *ldr = NtCurrentPeb()->Ldr;
  ULONG count = 0;
  for (LIST_ENTRY *l = ldr->InLoadOrderModuleList.Flink; l != &ldr->InLoadOrderModuleList; l = l->Flink) ++count;
  const ULONG total = 8 + count * 0x128;
  if (needed) *needed = total;
  if (!buffer || size < 8) { loader_unlock(); return (NTSTATUS)0xC0000004; }
  BYTE *out = buffer;
  memset(out, 0, size < total ? size : total);
  ULONG written = 0, index = 0;
  for (LIST_ENTRY *l = ldr->InLoadOrderModuleList.Flink; l != &ldr->InLoadOrderModuleList; l = l->Flink, ++index) {
    if (8 + (index + 1) * 0x128 > size) break;
    LDR_DATA_TABLE_ENTRY *m = (LDR_DATA_TABLE_ENTRY *)l;
    BYTE *r = out + 8 + index * 0x128;
    *(PVOID *)(r + 0x08) = m->DllBase;      /* MappedBase */
    *(PVOID *)(r + 0x10) = m->DllBase;      /* ImageBase */
    *(ULONG *)(r + 0x18) = m->SizeOfImage;
    *(ULONG *)(r + 0x1C) = m->Flags;
    *(USHORT *)(r + 0x20) = (USHORT)index;
    *(USHORT *)(r + 0x22) = (USHORT)index;
    *(USHORT *)(r + 0x24) = m->LoadCount;
    USHORT name_at = 0;
    char *path = (char *)(r + 0x28);
    const USHORT length = m->FullDllName.Length / 2;
    USHORT i = 0;
    for (; i < length && i < 255; ++i) {
      const WCHAR c = m->FullDllName.Buffer[i];
      path[i] = c < 0x80 ? (char)c : '?';
      if (c == '\\') name_at = (USHORT)(i + 1);
    }
    path[i] = 0;
    *(USHORT *)(r + 0x26) = name_at;
    ++written;
  }
  *(ULONG *)out = written;
  loader_unlock();
  return written < count ? (NTSTATUS)0xC0000004 : STATUS_SUCCESS;
}

EXPORT void NTAPI LdrProcessInitializationComplete(void) {}
EXPORT NTSTATUS NTAPI LdrShutdownThread(void) { return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI LdrSetImplicitPathOptions(ULONG options) { (void)options; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI LdrSetMUICacheType(ULONG type) { (void)type; return STATUS_SUCCESS; }
EXPORT NTSTATUS NTAPI LdrSetDllManifestProber(PVOID prober, PVOID a, PVOID b) { (void)prober; (void)a; (void)b; return STATUS_SUCCESS; }
EXPORT void NTAPI LdrSetAppCompatDllRedirectionCallback(ULONG flags, PVOID callback, PVOID context) { (void)flags; (void)callback; (void)context; }
EXPORT NTSTATUS NTAPI LdrUpdatePackageSearchPath(PCWSTR path) { (void)path; return STATUS_SUCCESS; }
EXPORT BOOLEAN NTAPI LdrControlFlowGuardEnforced(void) { return FALSE; }
EXPORT BOOLEAN NTAPI LdrIsModuleSxsRedirected(PVOID base) { (void)base; return FALSE; }
EXPORT BOOLEAN NTAPI LdrAlternateResourcesEnabled(void) { return FALSE; }
EXPORT NTSTATUS NTAPI LdrLoadAlternateResourceModule(PVOID module, PWSTR path) { (void)module; (void)path; return STATUS_NOT_IMPLEMENTED; }
EXPORT NTSTATUS NTAPI LdrLoadAlternateResourceModuleEx(PVOID module, ULONG language, PVOID a, PVOID b, ULONG flags) { (void)module; (void)language; (void)a; (void)b; (void)flags; return STATUS_NOT_IMPLEMENTED; }
EXPORT BOOLEAN NTAPI LdrUnloadAlternateResourceModule(PVOID module) { (void)module; return TRUE; }
EXPORT BOOLEAN NTAPI LdrUnloadAlternateResourceModuleEx(PVOID module, ULONG flags) { (void)module; (void)flags; return TRUE; }
EXPORT void NTAPI LdrFlushAlternateResourceModules(void) {}
EXPORT void NTAPI LdrFastFailInLoaderCallout(void) { RtlExitUserProcess(0xC0000409); }

/* ---- resources: a tree of directories (type, then name, then language) of data entries ---- */

typedef struct LDR_RESOURCE_INFO_ { ULONG_PTR type, name, language; } LDR_RESOURCE_INFO_;
#define RES_DIRECTORY_SIZE 16
#define RES_ENTRY_SIZE 8

static const BYTE *res_entry_target(const BYTE *root, const BYTE *entry, BOOLEAN want_directory) {
  const ULONG offset = *(const ULONG *)(entry + 4);
  const BOOLEAN is_directory = (offset & 0x80000000u) != 0;
  if (is_directory != (want_directory != 0)) return NULL;
  return root + (offset & 0x7FFFFFFFu);
}
/* In a directory, the entry for a name (a string, or "#number" for an id) or for an id. */
static const BYTE *res_find_by_name(const BYTE *root, const BYTE *directory, ULONG_PTR name, BOOLEAN want_directory) {
  const USHORT named = *(const USHORT *)(directory + 12), ids = *(const USHORT *)(directory + 14);
  const BYTE *entries = directory + RES_DIRECTORY_SIZE;
  if (name > 0xFFFF) {
    const WCHAR *text = (const WCHAR *)name;
    if (text[0] == '#') {      /* a number written out */
      ULONG number = 0;
      const WCHAR *p = text + 1;
      if (*p == 0) return NULL;
      for (; *p; ++p) { if (*p < '0' || *p > '9') { number = 0xFFFFFFFFu; break; } number = number * 10 + (ULONG)(*p - '0'); }
      if (number != 0xFFFFFFFFu && number <= 0xFFFF) return res_find_by_name(root, directory, number, want_directory);
    }
    SIZE_T length = 0;
    while (text[length]) ++length;
    for (USHORT i = 0; i < named; ++i) {
      const BYTE *entry = entries + (SIZE_T)i * RES_ENTRY_SIZE;
      const BYTE *string = root + (*(const ULONG *)entry & 0x7FFFFFFFu);
      const USHORT string_length = *(const USHORT *)string;
      const WCHAR *characters = (const WCHAR *)(string + 2);
      if (string_length != length) continue;
      if (RtlCompareUnicodeStrings(characters, string_length, text, length, TRUE) == 0) return res_entry_target(root, entry, want_directory);
    }
    return NULL;
  }
  for (USHORT i = 0; i < ids; ++i) {
    const BYTE *entry = entries + (SIZE_T)(named + i) * RES_ENTRY_SIZE;
    if (*(const ULONG *)entry == (ULONG)name) return res_entry_target(root, entry, want_directory);
  }
  return NULL;
}
static const BYTE *res_first(const BYTE *root, const BYTE *directory, BOOLEAN want_directory) {
  const USHORT count = (USHORT)(*(const USHORT *)(directory + 12) + *(const USHORT *)(directory + 14));
  if (count == 0) return NULL;
  return res_entry_target(root, directory + RES_DIRECTORY_SIZE, want_directory);
}

/* Down the tree as far as `level` says (0: the root directory, 1: the type's, 2: the name's, 3: the entry for the language). */
static NTSTATUS res_find(PVOID base, const LDR_RESOURCE_INFO_ *info, ULONG level, const BYTE **found, BOOLEAN want_directory) {
  ULONG size = 0;
  const BYTE *root = RtlImageDirectoryEntryToData((PVOID)((ULONG_PTR)base & ~(ULONG_PTR)3), TRUE, 2 /* IMAGE_DIRECTORY_ENTRY_RESOURCE */, &size);
  if (!root || size < RES_DIRECTORY_SIZE) return LDR_STATUS_RESOURCE_DATA_NOT_FOUND;
  const BYTE *directory = root;
  if (level == 0) { if (!want_directory) return LDR_STATUS_RESOURCE_DATA_NOT_FOUND; *found = directory; return STATUS_SUCCESS; }
  if (level > 3) level = 3;
  directory = res_find_by_name(root, directory, info->type, want_directory || level > 1);
  if (!directory) return LDR_STATUS_RESOURCE_TYPE_NOT_FOUND;
  if (level == 1) { *found = directory; return STATUS_SUCCESS; }
  directory = res_find_by_name(root, directory, info->name, want_directory || level > 2);
  if (!directory) return LDR_STATUS_RESOURCE_NAME_NOT_FOUND;
  if (level == 2) { *found = directory; return STATUS_SUCCESS; }
  const BYTE *entry = NULL;
  if (info->language != 0 && info->language != (ULONG_PTR)-1) {
    entry = res_find_by_name(root, directory, info->language & 0xFFFF, want_directory);      /* the language itself */
    if (!entry) {      /* the same primary language */
      const USHORT named = *(const USHORT *)(directory + 12), ids = *(const USHORT *)(directory + 14);
      for (USHORT i = 0; i < ids && !entry; ++i) {
        const BYTE *candidate = directory + RES_DIRECTORY_SIZE + (SIZE_T)(named + i) * RES_ENTRY_SIZE;
        if ((*(const ULONG *)candidate & 0x3FF) == (info->language & 0x3FF)) entry = res_entry_target(root, candidate, want_directory);
      }
    }
    if (!entry) entry = res_find_by_name(root, directory, 0 /* neutral */, want_directory);
  }
  if (!entry) entry = res_first(root, directory, want_directory);
  if (!entry) return LDR_STATUS_RESOURCE_LANG_NOT_FOUND;
  *found = entry;
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrFindResourceDirectory_U(PVOID base, const LDR_RESOURCE_INFO_ *info, ULONG level, PVOID *directory) {
  const BYTE *found = NULL;
  const NTSTATUS status = res_find(base, info, level, &found, TRUE);
  if (NT_SUCCESS(status)) *directory = (PVOID)found;
  return status;
}
EXPORT NTSTATUS NTAPI LdrFindResource_U(PVOID base, const LDR_RESOURCE_INFO_ *info, ULONG level, PVOID *entry) {
  const BYTE *found = NULL;
  const NTSTATUS status = res_find(base, info, level, &found, FALSE);
  if (NT_SUCCESS(status)) *entry = (PVOID)found;
  return status;
}
EXPORT NTSTATUS NTAPI LdrFindResourceEx_U(ULONG flags, PVOID base, const LDR_RESOURCE_INFO_ *info, ULONG level, PVOID *entry) { (void)flags; return LdrFindResource_U(base, info, level, entry); }
/* The bytes of a data entry (IMAGE_RESOURCE_DATA_ENTRY: OffsetToData, Size, CodePage, Reserved). A library loaded as data (the low bit of its handle) is a file as it lies on disk. */
EXPORT NTSTATUS NTAPI LdrAccessResource(PVOID base, const BYTE *entry, PVOID *address, PULONG size) {
  const BOOLEAN as_data = ((ULONG_PTR)base & 1) != 0;
  BYTE *image = (BYTE *)((ULONG_PTR)base & ~(ULONG_PTR)3);
  const ULONG rva = *(const ULONG *)entry;
  if (address) {
    if (as_data) {
      PVOID headers = RtlImageNtHeader(image);
      PVOID va = headers ? RtlImageRvaToVa(headers, image, rva, NULL) : NULL;
      if (!va) return STATUS_INVALID_PARAMETER;
      *address = va;
    } else *address = image + rva;
  }
  if (size) *size = *(const ULONG *)(entry + 4);
  return STATUS_SUCCESS;
}

/* ---- delay-loaded imports: the descriptor is IMAGE_DELAYLOAD_DESCRIPTOR (Attributes, DllNameRVA, ModuleHandleRVA, ImportAddressTableRVA, ImportNameTableRVA, ...) ---- */

EXPORT PVOID NTAPI LdrResolveDelayLoadedAPI(PVOID parent, const BYTE *descriptor, PVOID failure_dll_hook, PVOID failure_system_hook, ULONG_PTR *thunk, ULONG flags) {
  (void)failure_dll_hook; (void)failure_system_hook; (void)flags;
  BYTE *image = parent;
  const ULONG attributes = *(const ULONG *)descriptor;
  const BOOLEAN rva_based = (attributes & 1) != 0;
  if (!rva_based) return NULL;
  const char *library_name = (const char *)(image + *(const ULONG *)(descriptor + 4));
  PVOID *module_slot = (PVOID *)(image + *(const ULONG *)(descriptor + 8));
  ULONG_PTR *table = (ULONG_PTR *)(image + *(const ULONG *)(descriptor + 12));
  const ULONG_PTR *names = (const ULONG_PTR *)(image + *(const ULONG *)(descriptor + 16));
  PVOID module = *module_slot;
  if (!module) {
    WCHAR wide[260];
    ULONG length = 0;
    while (library_name[length] && length < 255) { wide[length] = (unsigned char)library_name[length]; ++length; }
    wide[length] = 0;
    UNICODE_STRING text;
    text.Buffer = wide;
    text.Length = (USHORT)(length * 2);
    text.MaximumLength = text.Length + 2;
    if (!NT_SUCCESS(LdrLoadDll(NULL, NULL, &text, &module))) return NULL;
    *module_slot = module;
  }
  const ULONG_PTR index = (ULONG_PTR)(thunk - table);
  const ULONG_PTR entry = names[index];
  PVOID address = NULL;
  NTSTATUS status;
  if (entry & ((ULONG_PTR)1 << 63)) status = ldr_export(module, NULL, (ULONG)(entry & 0xFFFF), 0, &address);
  else status = ldr_export(module, (const char *)(image + (entry & 0x7FFFFFFFu) + 2), 0, 0, &address);
  if (!NT_SUCCESS(status)) return NULL;
  *thunk = (ULONG_PTR)address;
  return address;
}
EXPORT NTSTATUS NTAPI LdrResolveDelayLoadsFromDll(PVOID parent, PCSTR target, ULONG flags) { (void)parent; (void)target; (void)flags; return STATUS_SUCCESS; }

/* ---- the options of an image file (HKLM\Software\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\<file name>) ---- */

NTSTATUS NTAPI NtQueryValueKey(HANDLE key, PUNICODE_STRING name, ULONG information_class, PVOID information, ULONG length, PULONG result_length);

static NTSTATUS ldr_open_options(const UNICODE_STRING *image, PHANDLE key) {
  static const WCHAR prefix[] = L"\\Registry\\Machine\\Software\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\";
  WCHAR path[600];
  SIZE_T n = 0;
  for (; prefix[n]; ++n) path[n] = prefix[n];
  SIZE_T start = 0;
  const SIZE_T length = image->Length / 2;
  for (SIZE_T i = 0; i < length; ++i) if (image->Buffer[i] == '\\' || image->Buffer[i] == '/') start = i + 1;
  if (length - start == 0 || length - start > 200) return STATUS_OBJECT_NAME_NOT_FOUND;
  for (SIZE_T i = start; i < length; ++i) path[n++] = image->Buffer[i];
  path[n] = 0;
  UNICODE_STRING name;
  name.Buffer = path;
  name.Length = (USHORT)(n * 2);
  name.MaximumLength = (USHORT)(n * 2 + 2);
  OBJECT_ATTRIBUTES attributes = {sizeof(attributes), NULL, &name, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL};
  return NtOpenKey(key, 0x20019 /* KEY_READ */, &attributes);
}
EXPORT NTSTATUS NTAPI LdrOpenImageFileOptionsKey(PUNICODE_STRING image, BOOLEAN wow64, PHANDLE key) { (void)wow64; return ldr_open_options(image, key); }
EXPORT NTSTATUS NTAPI LdrQueryImageFileKeyOption(HANDLE key, PCWSTR option, ULONG type, PVOID buffer, ULONG size, PULONG returned) {
  UNICODE_STRING name;
  RtlInitUnicodeString(&name, option);
  BYTE information[1024 + 16];
  ULONG got = 0;
  NTSTATUS status = NtQueryValueKey(key, &name, 2 /* KeyValuePartialInformation */, information, sizeof(information), &got);
  if (!NT_SUCCESS(status)) return status;
  const ULONG value_type = *(const ULONG *)(information + 4), data_length = *(const ULONG *)(information + 8);
  const BYTE *data = information + 12;
  if (value_type != type) {
    if (type == 4 /* REG_DWORD */ && value_type == 1 /* REG_SZ */) {      /* a number written as text */
      ULONG number = 0;
      const WCHAR *text = (const WCHAR *)data;
      ULONG base = 10;
      if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { base = 16; text += 2; }
      for (; *text; ++text) {
        ULONG digit = *text >= '0' && *text <= '9' ? (ULONG)(*text - '0') : (*text >= 'a' && *text <= 'f' ? (ULONG)(*text - 'a' + 10) : (*text >= 'A' && *text <= 'F' ? (ULONG)(*text - 'A' + 10) : 99));
        if (digit >= base) break;
        number = number * base + digit;
      }
      if (size < 4) return (NTSTATUS)0xC0000023;
      *(ULONG *)buffer = number;
      if (returned) *returned = 4;
      return STATUS_SUCCESS;
    }
    return (NTSTATUS)0xC0000024;      /* STATUS_OBJECT_TYPE_MISMATCH */
  }
  if (returned) *returned = data_length;
  if (size < data_length) return (NTSTATUS)0xC0000023;
  memcpy(buffer, data, data_length);
  return STATUS_SUCCESS;
}
EXPORT NTSTATUS NTAPI LdrQueryImageFileExecutionOptionsEx(PUNICODE_STRING image, PCWSTR option, ULONG type, PVOID buffer, ULONG size, PULONG returned, BOOLEAN wow64) {
  HANDLE key = NULL;
  NTSTATUS status = LdrOpenImageFileOptionsKey(image, wow64, &key);
  if (!NT_SUCCESS(status)) return status;
  status = LdrQueryImageFileKeyOption(key, option, type, buffer, size, returned);
  NtClose(key);
  return status;
}
EXPORT NTSTATUS NTAPI LdrQueryImageFileExecutionOptions(PUNICODE_STRING image, PCWSTR option, ULONG type, PVOID buffer, ULONG size, PULONG returned) {
  return LdrQueryImageFileExecutionOptionsEx(image, option, type, buffer, size, returned, FALSE);
}

/* ---- message tables (RT_MESSAGETABLE resources): MESSAGE_RESOURCE_DATA { NumberOfBlocks; blocks of { LowId, HighId, OffsetToEntries } } of entries { Length, Flags (1: unicode), Text } ---- */

EXPORT NTSTATUS NTAPI RtlFindMessage(PVOID module, ULONG type, ULONG language, ULONG id, const BYTE **entry) {
  LDR_RESOURCE_INFO_ info = {type, 1 /* RT_MESSAGETABLE ids start at 1 */, language};
  const BYTE *data_entry = NULL;
  NTSTATUS status = LdrFindResource_U(module, &info, 3, (PVOID *)&data_entry);
  if (!NT_SUCCESS(status)) return status;
  const BYTE *table = NULL;
  ULONG size = 0;
  status = LdrAccessResource(module, data_entry, (PVOID *)&table, &size);
  if (!NT_SUCCESS(status)) return status;
  const ULONG blocks = *(const ULONG *)table;
  for (ULONG i = 0; i < blocks; ++i) {
    const BYTE *block = table + 4 + 12 * (SIZE_T)i;
    const ULONG low = *(const ULONG *)block, high = *(const ULONG *)(block + 4), offset = *(const ULONG *)(block + 8);
    if (id < low || id > high) continue;
    const BYTE *p = table + offset;
    for (ULONG k = low; k < id; ++k) p += *(const USHORT *)p;
    *entry = p;
    return STATUS_SUCCESS;
  }
  return (NTSTATUS)0xC0000109;      /* STATUS_MESSAGE_NOT_FOUND */
}
