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

AmdMediaEngineStatus AmdMediaEngineHw::probe(allocators::IAllocator& allocator, const MmioRegion& window, AmdGpuArchitecture architecture) {
  AmdMediaEngineStatus status;
  if (!supported(architecture) || window.length() < kAmdIpDiscoveryHeaderBytes) return status;
  ByteBuffer table(allocator);
  if (!fetch(window, kAmdIpDiscoveryHeaderBytes, table)) return status;
  const uint32_t declared = amd_ip_discovery_declared_size(table.view());
  if (declared == 0 || declared > window.length() || declared > kDiscoveryWindow) return status;   // no table here
  const uint32_t rounded = (declared + 3) & ~3u;
  if (rounded > window.length()) return status;
  table.clear();
  if (!fetch(window, rounded, table)) return status;
  status.discovery = amd_parse_ip_discovery(table.view());
  status.probed = status.discovery.valid;
  return status;
}

}  // namespace hardware
