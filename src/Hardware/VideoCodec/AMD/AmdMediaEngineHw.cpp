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

#include "Hardware/VideoCodec/AMD/AmdMediaEngineHw.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

namespace {
// Appends `bytes` bytes of the window, from its start, as little-endian words.
bool fetch(const MmioRegion& window, uint32_t bytes, ByteBuffer& out) {
  for (uint32_t offset = 0; offset < bytes; offset += 4)
    if (!out.put32(window.read32(offset))) return false;
  return true;
}
}  // namespace

bool AmdMediaEngineHw::supported(AmdGpuArchitecture architecture) {
  switch (architecture) {
    case AmdGpuArchitecture::Rdna1:
    case AmdGpuArchitecture::Rdna2:
    case AmdGpuArchitecture::Rdna3:
    case AmdGpuArchitecture::Rdna3_5:
    case AmdGpuArchitecture::Rdna4: return true;
    default: return false;
  }
}

namespace {
// Reads and parses the table at the start of `window`; `probed` is false if there is none there.
AmdMediaEngineStatus read_table(allocators::IAllocator& allocator, const MmioRegion& window) {
  AmdMediaEngineStatus status;
  if (window.length() < kAmdIpDiscoveryHeaderBytes) return status;
  ByteBuffer table(allocator);
  if (!fetch(window, kAmdIpDiscoveryHeaderBytes, table)) return status;
  const uint32_t declared = amd_ip_discovery_declared_size(table.view());
  if (declared == 0 || declared > window.length() || declared > AmdMediaEngineHw::kDiscoveryWindow) return status;   // no table here
  const uint32_t rounded = (declared + 3) & ~3u;
  if (rounded > window.length()) return status;
  table.clear();
  if (!fetch(window, rounded, table)) return status;
  status.discovery = amd_parse_ip_discovery(table.view());
  status.probed = status.discovery.valid;
  return status;
}
}  // namespace

AmdMediaEngineStatus AmdMediaEngineHw::probe(allocators::IAllocator& allocator, const MmioRegion& window, AmdGpuArchitecture architecture) {
  if (!supported(architecture)) return AmdMediaEngineStatus();
  return read_table(allocator, window);
}

AmdMediaEngineStatus AmdMediaEngineHw::probe_aperture(allocators::IAllocator& allocator, const MmioRegion& aperture, AmdGpuArchitecture architecture) {
  constexpr uint64_t kMiB = 1ull << 20;
  constexpr uint32_t kSignature = 0x28211407;   // the first word of the table
  if (!supported(architecture) || aperture.length() < kDiscoveryWindow) return AmdMediaEngineStatus();
  for (uint64_t boundary = aperture.length() / kMiB * kMiB; boundary >= kMiB; boundary -= kMiB) {
    const uint64_t offset = boundary - kDiscoveryWindow;
    if (aperture.read32(offset) != kSignature) continue;
    AmdMediaEngineStatus status = read_table(allocator, aperture.subregion(offset, kDiscoveryWindow));
    if (!status.probed || !status.discovery.checksum_ok) continue;   // a stale or damaged copy: keep looking lower
    status.carve_out_bytes = boundary;
    return status;
  }
  return AmdMediaEngineStatus();
}

}  // namespace hardware
