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
#include "Process/LinuxVdso.hpp"
#include "Process/LinuxVdso/vdso_data.h"
#include "Utility/ByteStream.hpp"

namespace process {
namespace {

// Built by tools/build_linux_vdso.sh from LinuxVdso/vdso_x86_64.c (an ELF shared object linked at address 0, mapped as it is).
constexpr uint8_t kVdsoX86_64[] = {
#include "Process/LinuxVdso/LinuxVdsoImage.inc"
};

static_assert(TCOS_VDSO_DATA_BYTES == sizeof(tcos_vdso_data), "vdso_data.h: the byte offsets and the structure disagree");
static_assert(TCOS_VDSO_DATA_RATE_AT == __builtin_offsetof(tcos_vdso_data, tsc_ticks_per_us) && TCOS_VDSO_DATA_START_AT == __builtin_offsetof(tcos_vdso_data, tsc_at_start) &&
                  TCOS_VDSO_DATA_OFFSET_AT == __builtin_offsetof(tcos_vdso_data, realtime_offset_ns) && TCOS_VDSO_DATA_FLAGS_AT == __builtin_offsetof(tcos_vdso_data, flags),
              "vdso_data.h: the byte offsets and the structure disagree");

}  // namespace

ConstByteView linux_vdso_image(UserArch arch) {
  if (arch != UserArch::X86_64) return {};
  return ConstByteView(kVdsoX86_64, sizeof(kVdsoX86_64));
}

bool fill_linux_vdso_data(const UserClock* clock, ByteView page) {
  if (page.size() < TCOS_VDSO_DATA_BYTES) return false;
  page.fill(0);
  ByteWriter writer(page.first(TCOS_VDSO_DATA_BYTES));
  writer.le64(TCOS_VDSO_MAGIC);
  writer.le32(TCOS_VDSO_VERSION);
  writer.le32(0);
  writer.le64(clock ? clock->tsc_ticks_per_us : 0);
  writer.le64(clock ? clock->tsc_at_start : 0);
  writer.le64(clock ? clock->realtime_offset_ns : 0);
  return writer.ok();
}

namespace {
bool write_data(AddressSpace& space, uint64_t data_page, IProcessHost& host) {
  uint8_t bytes[TCOS_VDSO_DATA_BYTES];
  UserClock clock;
  const bool has_clock = host.user_clock(&clock) && clock.tsc_ticks_per_us != 0;
  return fill_linux_vdso_data(has_clock ? &clock : nullptr, ByteView(bytes, sizeof(bytes))) && space.write_privileged(data_page, ConstByteView(bytes, sizeof(bytes)));
}
}  // namespace

LoadStatus map_linux_vdso(AddressSpace& space, UserArch arch, IProcessHost& host, uint64_t* image_base) {
  const ConstByteView image = linux_vdso_image(arch);
  if (image.empty()) return LoadStatus::Unsupported;
  uint64_t image_bytes = 0, at = 0;
  if (!AddressSpace::round_up(image.size(), &image_bytes)) return LoadStatus::Malformed;
  const uint64_t total = AddressSpace::kPageSize + image_bytes;
  if (!space.find_free(total, kLinuxVdsoHint, &at) || !space.map(at, total, kProtRead | kProtWrite)) return LoadStatus::OutOfMemory;
  const uint64_t code = at + AddressSpace::kPageSize;
  if (!write_data(space, at, host) || !space.write_privileged(code, image) || !space.protect(at, AddressSpace::kPageSize, kProtRead) ||
      !space.protect(code, image_bytes, kProtRead | kProtExecute)) {
    (void)space.unmap(at, total);
    return LoadStatus::OutOfMemory;
  }
  *image_base = code;
  return LoadStatus::Ok;
}

bool refresh_linux_vdso_data(AddressSpace& space, uint64_t image_base, IProcessHost& host) {
  if (image_base < AddressSpace::kPageSize) return false;
  return write_data(space, image_base - AddressSpace::kPageSize, host);
}

}  // namespace process
