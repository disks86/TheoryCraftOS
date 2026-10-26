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
#include "Process/WindowsJobs.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"

namespace process::windowsabi {

namespace {
constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u,
                   kInfoLengthMismatch = 0xC0000004u, kInvalidInfoClass = 0xC0000003u, kNotSupported = 0xC00000BBu, kBufferOverflow = 0x80000005u,
                   kProcessInJob = 0x124, kProcessNotInJob = 0x123, kProcessNotInJobStatus = 0xC0000022u;      // (access denied, as Windows answers a query without a handle when the process is in no job)
constexpr uint64_t kCurrentProcess = ~uint64_t{0};

uint32_t job_of_handle(Process& p, uint64_t handle) {
  if (handle == 0 || handle % 4 != 0 || handle / 4 > Process::kMaxDescriptors) return 0;
  const uint64_t slot = handle / 4 - 1;
  return p.files().kind(slot) == DescriptorKind::Job ? uint32_t(p.files().token(slot)) : 0;
}
bool pid_of_handle(Process& p, uint64_t handle, uint64_t* pid) {
  if (handle == kCurrentProcess) { *pid = p.id(); return true; }
  bool exited = false;
  uint32_t code = 0;
  return process_handle_info(p, handle, &exited, &code, pid);
}
void put(uint8_t* b, uint32_t at, uint64_t value, uint32_t size) { for (uint32_t i = 0; i < size; ++i) b[at + i] = uint8_t(value >> (8 * i)); }
uint64_t get(const uint8_t* b, uint32_t at, uint32_t size) { uint64_t v = 0; for (uint32_t i = 0; i < size; ++i) v |= uint64_t{b[at + i]} << (8 * i); return v; }
}  // namespace

uint32_t nt_assign_process_to_job_object(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t job = job_of_handle(p, r.args[0]);
  uint64_t pid = 0;
  if (job == 0 || !pid_of_handle(p, r.args[1], &pid)) return kInvalidHandle;
  return objects->job_assign(job, pid);
}

uint32_t nt_terminate_job_object(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t job = job_of_handle(p, r.args[0]);
  if (job == 0) return kInvalidHandle;
  return objects->job_terminate(job, uint32_t(r.args[1]));
}

uint32_t nt_is_process_in_job(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint64_t pid = 0;
  if (!pid_of_handle(p, r.args[0], &pid)) return kInvalidHandle;
  uint32_t asked = 0;
  if (r.args[1] != 0 && (asked = job_of_handle(p, r.args[1])) == 0) return kInvalidHandle;
  const uint32_t actual = objects->job_of_process(pid);
  return actual != 0 && (asked == 0 || asked == actual) ? kProcessInJob : kProcessNotInJob;
}

uint32_t nt_query_information_job_object(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  uint64_t returned = 0;
  if (!argument(p, r, 4, &returned)) return kAccessViolation;
  const uint32_t job = r.args[0] == 0 ? objects->job_of_process(p.id()) : job_of_handle(p, r.args[0]);      // (no handle: the job of this process)
  if (job == 0) return r.args[0] == 0 ? kProcessNotInJobStatus : kInvalidHandle;
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);      // (ULONGs: the upper halves are not defined)
  ObjectManager::JobInfo info;
  uint64_t pids[ObjectManager::kJobMaxMembers];
  uint32_t pid_count = 0;
  if (!objects->job_info(job, &info, pids, ObjectManager::kJobMaxMembers, &pid_count)) return kInvalidHandle;
  uint8_t b[160] = {};
  uint32_t size = 0;
  auto basic_limits = [&]() {
    put(b, 0, info.per_process_time, 8); put(b, 8, info.per_job_time, 8); put(b, 16, info.limit_flags, 4);
    put(b, 24, info.min_working_set, 8); put(b, 32, info.max_working_set, 8); put(b, 40, info.active_limit, 4);
    put(b, 48, info.affinity, 8); put(b, 56, info.priority_class, 4); put(b, 60, info.scheduling_class, 4);
  };
  auto basic_accounting = [&](uint32_t at) {
    put(b, at + 32, 0, 4);                                  // TotalPageFaultCount
    put(b, at + 36, info.total_processes, 4);
    put(b, at + 40, info.active_processes, 4);
    put(b, at + 44, info.terminated_processes, 4);
  };
  switch (info_class) {
    case 1: basic_accounting(0); size = 48; break;
    case 2: basic_limits(); size = 64; break;
    case 3: {
      const uint32_t fit = length >= 8 ? (length - 8) / 8 : 0, listed = pid_count < fit ? pid_count : fit;
      if (length < 8) return kInfoLengthMismatch;
      uint8_t list[8 + 8 * ObjectManager::kJobMaxMembers] = {};
      put(list, 0, pid_count, 4);
      put(list, 4, listed, 4);
      for (uint32_t i = 0; i < listed; ++i) put(list, 8 + 8 * i, pids[i], 8);
      const uint32_t bytes = 8 + 8 * listed;
      if (returned != 0 && !p.write_user_u32(returned, bytes)) return kAccessViolation;
      if (!p.write_user(r.args[2], ConstByteView(list, bytes))) return kAccessViolation;
      return listed < pid_count ? kBufferOverflow : kSuccess;
    }
    case 4: put(b, 0, info.ui_flags, 4); size = 4; break;
    case 8: basic_accounting(0); size = 96; break;
    case 9:
      basic_limits();
      put(b, 112, info.process_memory_limit, 8); put(b, 120, info.job_memory_limit, 8);
      size = 144;
      break;
    default: return info_class == 7 ? kNotSupported : kInvalidInfoClass;
  }
  if (returned != 0 && !p.write_user_u32(returned, size)) return kAccessViolation;
  if (length < size) return kInfoLengthMismatch;
  return p.write_user(r.args[2], ConstByteView(b, size)) ? kSuccess : kAccessViolation;
}

uint32_t nt_set_information_job_object(Process& p, const SyscallRequest& r) {
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  const uint32_t job = job_of_handle(p, r.args[0]);
  if (job == 0) return kInvalidHandle;
  const uint32_t info_class = uint32_t(r.args[1]), length = uint32_t(r.args[3]);
  uint8_t b[144] = {};
  switch (info_class) {
    case 2:
    case 9: {
      const uint32_t size = info_class == 2 ? 64 : 144;
      if (length != size) return kInfoLengthMismatch;
      if (!p.read_user(r.args[2], ByteView(b, size))) return kAccessViolation;
      ObjectManager::JobInfo in;
      in.per_process_time = get(b, 0, 8); in.per_job_time = get(b, 8, 8); in.limit_flags = uint32_t(get(b, 16, 4));
      in.min_working_set = get(b, 24, 8); in.max_working_set = get(b, 32, 8); in.active_limit = uint32_t(get(b, 40, 4));
      in.affinity = get(b, 48, 8); in.priority_class = uint32_t(get(b, 56, 4)); in.scheduling_class = uint32_t(get(b, 60, 4));
      if (info_class == 9) { in.process_memory_limit = get(b, 112, 8); in.job_memory_limit = get(b, 120, 8); }
      return objects->job_set(job, in, info_class == 9);
    }
    case 4:
    case 6:
      if (length != 4) return kInfoLengthMismatch;
      return p.read_user(r.args[2], ByteView(b, 4)) ? kSuccess : kAccessViolation;      // (accepted; nothing about windows or time is enforced)
    case 7: return kNotSupported;
    default: return kInvalidInfoClass;
  }
}

}  // namespace process::windowsabi
