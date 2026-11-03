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

// NT services that do not belong to a bigger group: alerts, keyed events, state queries of events, mutants and semaphores, locale defaults, UUID allocation, power and
// the services that only have to say "nothing to do" (WindowsSyscalls.cpp maps service numbers to these). Each returns an NTSTATUS.
namespace process::windowsabi {

// Threads wait for each other by id (what ntdll builds SRW locks, condition variables and WaitOnAddress from).
// NtWaitForKeyedEvent / NtReleaseKeyedEvent (HANDLE KeyedEvent, PVOID Key, BOOLEAN Alertable, PLARGE_INTEGER Timeout): a release and a wait with the same key meet; whichever comes
// first waits for the other.
uint32_t nt_wait_for_keyed_event(Process& p, const SyscallRequest& r);
uint32_t nt_release_keyed_event(Process& p, const SyscallRequest& r);
uint32_t nt_clear_event(Process& p, const SyscallRequest& r);
uint32_t nt_query_event(Process& p, const SyscallRequest& r);
uint32_t nt_query_mutant(Process& p, const SyscallRequest& r);
uint32_t nt_query_semaphore(Process& p, const SyscallRequest& r);
uint32_t nt_set_event_boost_priority(Process& p, const SyscallRequest& r);
uint32_t nt_get_current_processor_number_ex(Process& p, const SyscallRequest& r);
// NtQueryDefaultLocale, NtSetDefaultLocale, NtQueryDefaultUILanguage, NtSetDefaultUILanguage, NtQueryInstallUILanguage, NtIsUILanguageComitted, NtFlushInstallUILanguage.
uint32_t nt_query_default_locale(Process& p, const SyscallRequest& r);
uint32_t nt_set_default_locale(Process& p, const SyscallRequest& r);
uint32_t nt_query_default_ui_language(Process& p, const SyscallRequest& r);
uint32_t nt_set_default_ui_language(Process& p, const SyscallRequest& r);
uint32_t nt_query_install_ui_language(Process& p, const SyscallRequest& r);
uint32_t nt_allocate_uuids(Process& p, const SyscallRequest& r);
uint32_t nt_set_uuid_seed(Process& p, const SyscallRequest& r);
uint32_t nt_power_information(Process& p, const SyscallRequest& r);
uint32_t nt_set_thread_execution_state(Process& p, const SyscallRequest& r);
uint32_t nt_raise_hard_error(Process& p, const SyscallRequest& r);
uint32_t nt_query_interval_profile(Process& p, const SyscallRequest& r);
// Windows notification facility state (WNF): named pieces of state that programs publish and read. The names are made by NtCreateWnfStateName; each keeps its latest data
// and a change stamp that grows with every update. Subscriptions are not delivered.
uint32_t nt_create_wnf_state_name(Process& p, const SyscallRequest& r);      // (PWNF_STATE_NAME, Lifetime, DataScope, PersistData, TypeId, MaximumStateSize, SecurityDescriptor)
uint32_t nt_update_wnf_state_data(Process& p, const SyscallRequest& r);      // (StateName, Buffer, Length, TypeId, ExplicitScope, MatchingChangeStamp, CheckStamp)
uint32_t nt_query_wnf_state_data(Process& p, const SyscallRequest& r);       // (StateName, TypeId, ExplicitScope, PWNF_CHANGE_STAMP, Buffer, PULONG BufferSize)
uint32_t nt_delete_wnf_state_data(Process& p, const SyscallRequest& r);      // (StateName, ExplicitScope)
uint32_t nt_delete_wnf_state_name(Process& p, const SyscallRequest& r);      // (StateName)
uint32_t nt_query_wnf_state_name_information(Process& p, const SyscallRequest& r);   // (StateName, NameInfoClass, ExplicitScope, Buffer, BufferSize)
// Services that succeed without doing anything (the caches and write buffers they flush do not exist here) or that only validate their arguments.
uint32_t nt_succeed(Process& p, const SyscallRequest& r);
uint32_t nt_make_object_temporary(Process& p, const SyscallRequest& r);
uint32_t nt_make_object_permanent(Process& p, const SyscallRequest& r);
uint32_t nt_privilege_not_held(Process& p, const SyscallRequest& r);
uint32_t nt_not_supported(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
