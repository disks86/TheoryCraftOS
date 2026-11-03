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
#include "Process/Process.hpp"
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

}  // namespace process::windowsabi
