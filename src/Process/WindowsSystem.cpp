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
#include "Process/WindowsSystem.hpp"

namespace process::windowsabi {

namespace {

constexpr uint32_t kSuccess = 0, kAccessViolation = 0xC0000005u, kInvalidInfoClass = 0xC0000003u, kInfoLengthMismatch = 0xC0000004u;
constexpr uint64_t kUnixToNt = 11644473600ull * 10000000ull;

// A buffer the answer is built in; every field is little-endian at a fixed offset.
struct Answer {
  uint8_t bytes[320] = {};
  void put(uint32_t at, uint64_t value, uint32_t size) { for (uint32_t i = 0; i < size; ++i) bytes[at + i] = uint8_t(value >> (8 * i)); }
};

}  // namespace

uint32_t nt_query_system_information(Process& p, const SyscallRequest& r) {
  const uint32_t info_class = uint32_t(r.args[0]), length = uint32_t(r.args[2]);
  const uint64_t buffer = r.args[1], returned = r.args[3];
  IProcessRuntime* runtime = p.runtime();
  const uint32_t processors = runtime ? runtime->processor_count() : 1;
  uint64_t total = 0, available = 0;
  if (runtime) runtime->memory_status(&total, &available);
  Answer a;
  uint32_t size = 0;
  switch (info_class) {
    case 0: {      // SYSTEM_BASIC_INFORMATION
      const uint64_t pages = total / 4096;
      a.put(4, 156250, 4);               // TimerResolution (100 ns units)
      a.put(8, 4096, 4);                 // PageSize
      a.put(12, pages, 4);               // NumberOfPhysicalPages
      a.put(16, 1, 4);                   // LowestPhysicalPageNumber
      a.put(20, pages, 4);               // HighestPhysicalPageNumber
      a.put(24, 0x10000, 4);             // AllocationGranularity
      a.put(32, 0x10000, 8);             // MinimumUserModeAddress
      a.put(40, 0x00007FFFFFFEFFFFull, 8);      // MaximumUserModeAddress
      a.put(48, processors >= 64 ? ~uint64_t{0} : (uint64_t{1} << processors) - 1, 8);      // ActiveProcessorsAffinityMask
      a.put(56, processors > 255 ? 255 : processors, 1);      // NumberOfProcessors
      size = 64;
      break;
    }
    case 1:      // SYSTEM_PROCESSOR_INFORMATION
      a.put(0, 9, 2);                    // PROCESSOR_ARCHITECTURE_AMD64
      a.put(2, 6, 2);                    // ProcessorLevel
      a.put(4, 0, 2);                    // ProcessorRevision
      a.put(6, processors, 2);           // MaximumProcessors
      a.put(8, 0x3F, 8);                 // ProcessorFeatureBits (the ones IsProcessorFeaturePresent reports)
      size = 16;
      break;
    case 2:      // SYSTEM_PERFORMANCE_INFORMATION
      a.put(44, available / 4096, 4);                        // AvailablePages
      a.put(48, (total > available ? total - available : 0) / 4096, 4);      // CommittedPages
      a.put(52, total / 4096, 4);                            // CommitLimit
      size = 312;
      break;
    case 3: {    // SYSTEM_TIMEOFDAY_INFORMATION
      const uint64_t now = p.host().realtime_ns() / 100 + kUnixToNt;
      const uint64_t since_boot = p.host().monotonic_ns() / 100;
      a.put(0, now - since_boot, 8);     // BootTime
      a.put(8, now, 8);                  // CurrentTime
      size = 48;
      break;
    }
    case 5: {    // SYSTEM_PROCESS_INFORMATION for each program, one after the other (the threads' entries are left out); each is 0x100 bytes and the image name's characters
      IProcessRuntime::ProcessEntry entries[32];
      const uint32_t count = runtime ? runtime->list_processes(entries, 32) : 0;
      constexpr uint32_t kEntry = 0x100, kName = 24 * 2 + 8;
      const uint32_t total_size = count * (kEntry + kName);
      if (returned != 0 && !p.write_user_u32(returned, total_size)) return kAccessViolation;
      if (length < total_size) return kInfoLengthMismatch;
      for (uint32_t i = 0; i < count; ++i) {
        uint8_t entry[kEntry + kName] = {};
        auto put = [&](uint32_t at, uint64_t value, uint32_t size) { for (uint32_t k = 0; k < size; ++k) entry[at + k] = uint8_t(value >> (8 * k)); };
        const IProcessRuntime::ProcessEntry& e = entries[i];
        uint32_t name_length = 0;
        while (name_length < sizeof e.name && e.name[name_length]) ++name_length;
        const uint64_t at = buffer + uint64_t(i) * (kEntry + kName);
        put(0, i + 1 < count ? kEntry + kName : 0, 4);        // NextEntryOffset
        put(4, e.threads, 4);                                    // NumberOfThreads
        put(0x38, name_length * 2, 2);                           // ImageName.Length
        put(0x3A, kName, 2);                                     // .MaximumLength
        put(0x40, at + kEntry, 8);                               // .Buffer: the characters follow the entry
        put(0x48, 8, 4);                                         // BasePriority
        put(0x50, e.pid, 8);                                     // UniqueProcessId
        put(0x58, e.parent, 8);                                  // InheritedFromUniqueProcessId
        for (uint32_t k = 0; k < name_length; ++k) put(kEntry + k * 2, uint8_t(e.name[k]), 2);
        if (!p.write_user(at, ConstByteView(entry, sizeof entry))) return kAccessViolation;
      }
      return kSuccess;
    }
    default: return kInvalidInfoClass;
  }
  if (returned != 0 && !p.write_user_u32(returned, size)) return kAccessViolation;
  if (length < size) return kInfoLengthMismatch;
  return p.write_user(buffer, ConstByteView(a.bytes, size)) ? kSuccess : kAccessViolation;
}

}  // namespace process::windowsabi
