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
#include "Process/WindowsLocks.hpp"
#include "Process/Objects.hpp"
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsWait.hpp"

namespace process::windowsabi {

namespace {
constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidHandle = 0xC0000008u, kInvalidParameter = 0xC000000Du, kNotImplemented = 0xC0000002u,
                   kLockNotGranted = 0xC0000055u, kRangeNotLocked = 0xC000007Eu, kNoResources = 0xC000009Au, kInvalidDevice = 0xC0000010u;

// The file of a handle as the system names it, and the range asked for.
bool file_and_range(Process& p, const SyscallRequest& r, uint64_t offset_at, uint64_t length_at, String* path, const void** owner, uint64_t* offset, uint64_t* length, uint32_t* status) {
  uint64_t slot = 0;
  if (r.args[0] == 0 || r.args[0] % 4 != 0 || r.args[0] / 4 > Process::kMaxDescriptors) { *status = kInvalidHandle; return false; }
  slot = r.args[0] / 4 - 1;
  if (p.files().kind(slot) == DescriptorKind::Closed) { *status = kInvalidHandle; return false; }
  OpenObject* file = p.files().object(slot);
  if (p.files().kind(slot) != DescriptorKind::File || !file || !p.files().host_path_of(file->path.view(), path)) { *status = kInvalidDevice; return false; }
  *owner = file;
  if (!p.read_user_u64(offset_at, offset) || !p.read_user_u64(length_at, length)) { *status = kAccessViolation; return false; }
  if (*length == 0 || int64_t(*offset) < 0 || int64_t(*length) < 0) { *status = kInvalidParameter; return false; }
  return true;
}
}  // namespace

uint32_t nt_lock_file(Process& p, const SyscallRequest& r) {
  uint64_t status_block = 0, offset_at = 0, length_at = 0, key = 0, fail = 0, exclusive = 0;
  if (!argument(p, r, 4, &status_block) || !argument(p, r, 5, &offset_at) || !argument(p, r, 6, &length_at) || !argument(p, r, 7, &key) || !argument(p, r, 8, &fail) ||
      !argument(p, r, 9, &exclusive))
    return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  String path(p.files().allocator());
  const void* owner = nullptr;
  uint64_t offset = 0, length = 0;
  uint32_t status = kSuccess;
  if (!file_and_range(p, r, offset_at, length_at, &path, &owner, &offset, &length, &status)) return status;
  const bool no_wait = (fail & 0xFF) != 0, write = (exclusive & 0xFF) != 0;
  status = wait_until(p, ~uint64_t{0}, [&]() -> uint64_t {
    switch (objects->lock_range_locked(path.view(), owner, offset, length, write)) {
      case ObjectManager::LockResult::Granted: return kSuccess;
      case ObjectManager::LockResult::Full: return kNoResources;
      default: return no_wait ? uint64_t{kLockNotGranted} : kWaitPending;
    }
  });
  if (status_block != 0) { uint8_t iosb[16] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)}; p.write_user(status_block, ConstByteView(iosb, 16)); }
  return status;
}

uint32_t nt_unlock_file(Process& p, const SyscallRequest& r) {
  uint64_t key = 0;
  if (!argument(p, r, 4, &key)) return kAccessViolation;
  ObjectManager* objects = system_objects();
  if (!objects) return kNotImplemented;
  String path(p.files().allocator());
  const void* owner = nullptr;
  uint64_t offset = 0, length = 0;
  uint32_t status = kSuccess;
  if (!file_and_range(p, r, r.args[2], r.args[3], &path, &owner, &offset, &length, &status)) return status;
  status = objects->unlock_range(path.view(), owner, offset, length) ? kSuccess : kRangeNotLocked;
  if (r.args[1] != 0) { uint8_t iosb[16] = {uint8_t(status), uint8_t(status >> 8), uint8_t(status >> 16), uint8_t(status >> 24)}; p.write_user(r.args[1], ConstByteView(iosb, 16)); }
  return status;
}

}  // namespace process::windowsabi
