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
#pragma once
#include <stdint.h>
#include "ObjectContainer/List.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"
#include "Process/Process.hpp"
#include "Process/RegistryStore.hpp"
#include "Process/Syscall.hpp"

// The NT registry services (WindowsSyscalls.cpp maps service numbers to these) over the tree of RegistryTree.hpp. A key handle is a descriptor of kind Key whose token is the
// id of the key. Each service returns an NTSTATUS.
namespace process::windowsabi {

uint32_t nt_create_key(Process& p, const SyscallRequest& r);
uint32_t nt_open_key(Process& p, const SyscallRequest& r);
uint32_t nt_open_key_ex(Process& p, const SyscallRequest& r);
uint32_t nt_query_key(Process& p, const SyscallRequest& r);
uint32_t nt_enumerate_key(Process& p, const SyscallRequest& r);
uint32_t nt_enumerate_value_key(Process& p, const SyscallRequest& r);
uint32_t nt_query_value_key(Process& p, const SyscallRequest& r);
uint32_t nt_set_value_key(Process& p, const SyscallRequest& r);
uint32_t nt_delete_key(Process& p, const SyscallRequest& r);
uint32_t nt_delete_value_key(Process& p, const SyscallRequest& r);
uint32_t nt_flush_key(Process& p, const SyscallRequest& r);
uint32_t nt_rename_key(Process& p, const SyscallRequest& r);
uint32_t nt_query_multiple_value_key(Process& p, const SyscallRequest& r);
uint32_t nt_set_information_key(Process& p, const SyscallRequest& r);
uint32_t nt_notify_change_key(Process& p, const SyscallRequest& r);
uint32_t nt_notify_change_multiple_keys(Process& p, const SyscallRequest& r);
uint32_t nt_save_key(Process& p, const SyscallRequest& r);
uint32_t nt_save_key_ex(Process& p, const SyscallRequest& r);
uint32_t nt_restore_key(Process& p, const SyscallRequest& r);
uint32_t nt_replace_key(Process& p, const SyscallRequest& r);
uint32_t nt_load_key(Process& p, const SyscallRequest& r);       // NtLoadKey, NtLoadKey2
uint32_t nt_load_key_ex(Process& p, const SyscallRequest& r);    // NtLoadKeyEx, NtLoadKey3
uint32_t nt_unload_key(Process& p, const SyscallRequest& r);     // NtUnloadKey, NtUnloadKey2, NtUnloadKeyEx
uint32_t nt_query_open_sub_keys(Process& p, const SyscallRequest& r);
uint32_t nt_compact_keys(Process& p, const SyscallRequest& r);
uint32_t nt_lock_registry_key(Process& p, const SyscallRequest& r);
uint32_t nt_create_key_transacted(Process& p, const SyscallRequest& r);
uint32_t nt_open_key_transacted(Process& p, const SyscallRequest& r);
// Closing, duplicating or inheriting a key handle.
void key_token_add(uint64_t token);
void key_token_release(uint64_t token);
// The full path of the key a key handle's token names (\Registry\Machine\...); false if there is none.
bool key_full_name(Process& p, uint64_t token, String* out);

// The programs that setup programs registered as installed: every subkey of the Uninstall keys (HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall, the same
// under WOW6432Node, and each user's Software\...\Uninstall), with the values a package manager needs. For kernel code (the App Manager), not a system service.
struct RegistryUninstallEntry {
  explicit RegistryUninstallEntry(allocators::IAllocator& allocator) : key(allocator), display_name(allocator), display_version(allocator), uninstall(allocator), quiet_uninstall(allocator) {}
  String key;                // the key's full name ("\REGISTRY\MACHINE\SOFTWARE\...\Uninstall\{product code}")
  String display_name, display_version, uninstall, quiet_uninstall;      // DisplayName, DisplayVersion, UninstallString, QuietUninstallString ("" if absent)
};
// Appends them to `out`. False only if memory ran out. Before any Windows program has run there is no registry, and nothing is appended.
[[nodiscard]] bool list_uninstall_entries(allocators::IAllocator& allocator, List<UniquePointer<RegistryUninstallEntry>>* out);

// The registry for kernel code that installs Windows programs itself (the App Manager's Windows Installer engine, src/Msi): values read, written and
// deleted by the path of their key under one of the roots a program names (HKEY_CLASSES_ROOT is the machine's SOFTWARE\Classes, HKEY_CURRENT_USER the
// key of the one user programs run as). Text is UTF-8 here; `data` is what the registry keeps (text as UTF-16LE with its terminator). Not system services.
enum class RegistryHive : uint8_t { ClassesRoot, CurrentUser, LocalMachine, Users };
// Sets a value, making its key (and the registry itself, if no program has run yet: with `allocator`, which must be the kernel's own, as the registry
// lives as long as the system). False if the name or the path is not valid, or memory ran out.
[[nodiscard]] bool registry_set_value(allocators::IAllocator& allocator, RegistryHive hive, StringView key, StringView name, uint32_t type, ConstByteView data);
// False if there is no registry, no such key or no such value.
[[nodiscard]] bool registry_get_value(RegistryHive hive, StringView key, StringView name, uint32_t* type, ByteBuffer* data);
void registry_delete_value(RegistryHive hive, StringView key, StringView name);
// Deletes the key with everything under it; with `only_if_empty`, only a key that has no values and no keys under it.
void registry_delete_key(RegistryHive hive, StringView key, bool only_if_empty);

// The registry on disk (RegistryStore.hpp, filesystem::layout::kWindowsRegistryFile). registry_attach_storage() is called once at boot, before any
// Windows program runs: it makes the registry (with `allocator`, the kernel's own) and merges the saved image into it; from then on registry_save()
// writes it, and RegFlushKey on any key does too. `unix_seconds` is the time now (the write time of the keys the registry starts with). After NotMerged
// nothing is attached: the registry is in memory only until the next boot, and the file stays as it is.
[[nodiscard]] RegistryLoad registry_attach_storage(allocators::IAllocator& allocator, filesystem::IFileSystem& fs, uint64_t unix_seconds);
// Writes the registry's keys that are not volatile to disk; with `only_if_changed`, only if something changed since the last save. True if there was
// nothing to do or it was written; false if the file system refused (the previous file is then still there). Called every few seconds from a kernel
// thread (main.cpp) and by NtFlushKey; may be called from any thread.
[[nodiscard]] bool registry_save(bool only_if_changed);
// The last save, before the file system goes away (shutdown). From then on the registry is in memory only.
[[nodiscard]] bool registry_detach_storage();

}  // namespace process::windowsabi
