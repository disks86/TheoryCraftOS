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
    case 5: {    // SYSTEM_PROCESS_INFORMATION for each program, one after the other: the entry (0x100 bytes), its threads (SYSTEM_THREAD_INFORMATION, 0x50 bytes each), the image name's characters
      IProcessRuntime::ProcessEntry entries[32];
      const uint32_t count = runtime ? runtime->list_processes(entries, 32) : 0;
      for (uint32_t i = 1; i < count; ++i) {      // (in the order of their ids: a program just started comes after the ones that started it)
        IProcessRuntime::ProcessEntry moved = entries[i];
        uint32_t k = i;
        for (; k > 0 && entries[k - 1].pid > moved.pid; --k) entries[k] = entries[k - 1];
        entries[k] = moved;
      }
      constexpr uint32_t kEntry = 0x100, kThread = 0x50, kName = 24 * 2 + 8, kMaxThreadsListed = 64;
      auto entry_size = [&](const IProcessRuntime::ProcessEntry& e) { return kEntry + (e.threads < kMaxThreadsListed ? e.threads : kMaxThreadsListed) * kThread + kName; };
      uint64_t total_size = 0;
      for (uint32_t i = 0; i < count; ++i) total_size += entry_size(entries[i]);
      if (returned != 0 && !p.write_user_u32(returned, uint32_t(total_size))) return kAccessViolation;
      if (length < total_size) return kInfoLengthMismatch;
      uint64_t at = buffer;
      for (uint32_t i = 0; i < count; ++i) {
        const IProcessRuntime::ProcessEntry& e = entries[i];
        const uint32_t thread_count = e.threads < kMaxThreadsListed ? e.threads : kMaxThreadsListed, size = entry_size(e);
        uint8_t entry[kEntry + kMaxThreadsListed * kThread + kName] = {};
        auto put = [&](uint32_t where, uint64_t value, uint32_t bytes) { for (uint32_t k = 0; k < bytes; ++k) entry[where + k] = uint8_t(value >> (8 * k)); };
        uint32_t name_length = 0;
        while (name_length < sizeof e.name && e.name[name_length]) ++name_length;
        put(0, i + 1 < count ? size : 0, 4);                       // NextEntryOffset
        put(4, thread_count, 4);                                    // NumberOfThreads
        put(0x20, e.create_time, 8);                                // CreateTime
        put(0x38, name_length * 2, 2);                              // ImageName.Length
        put(0x3A, kName, 2);                                        // .MaximumLength
        put(0x40, at + kEntry + thread_count * kThread, 8);         // .Buffer: the characters follow the threads
        put(0x48, 8, 4);                                            // BasePriority
        put(0x50, e.pid, 8);                                        // UniqueProcessId
        put(0x58, e.parent, 8);                                     // InheritedFromUniqueProcessId
        put(0x78, e.memory_bytes, 8);                               // VirtualSize
        put(0x90, e.memory_bytes, 8);                               // WorkingSetSize
        put(0xB8, e.memory_bytes, 8);                               // PagefileUsage
        put(0xC0, e.memory_bytes, 8);                               // PeakPagefileUsage
        put(0xC8, e.memory_bytes, 8);                               // PrivatePageCount
        IProcessRuntime::ThreadEntry threads[kMaxThreadsListed];
        const uint32_t listed = runtime && thread_count != 0 ? runtime->list_threads(e.pid, threads, thread_count) : 0;
        for (uint32_t k = 0; k < listed; ++k) {
          const uint32_t base = kEntry + k * kThread;
          put(base + 0x20, threads[k].start_address, 8);            // StartAddress
          put(base + 0x28, e.pid, 8);                               // ClientId.UniqueProcess
          put(base + 0x30, threads[k].id, 8);                       // ClientId.UniqueThread
          put(base + 0x38, uint32_t(8 + threads[k].priority), 4);   // Priority
          put(base + 0x3C, uint32_t(8 + threads[k].priority), 4);   // BasePriority
          put(base + 0x44, 5, 4);                                   // ThreadState: waiting
        }
        for (uint32_t k = 0; k < name_length; ++k) put(kEntry + thread_count * kThread + k * 2, uint8_t(e.name[k]), 2);
        if (!p.write_user(at, ConstByteView(entry, size))) return kAccessViolation;
        at += size;
      }
      return kSuccess;
    }
    case 8: {    // SYSTEM_PROCESSOR_PERFORMANCE_INFORMATION for each processor (48 bytes): the time idle, in the kernel (idle included) and in programs, in 100 ns units
      constexpr uint32_t kEntry = 48, kMax = 256;
      const uint32_t cpus = processors > kMax ? kMax : processors, total_size = cpus * kEntry;
      if (returned != 0 && !p.write_user_u32(returned, total_size)) return kAccessViolation;
      if (length < total_size) return kInfoLengthMismatch;
      const uint64_t idle = p.host().monotonic_ns() / 100;
      uint8_t out[kMax * kEntry] = {};
      for (uint32_t i = 0; i < cpus; ++i) {
        for (uint32_t k = 0; k < 8; ++k) { out[i * kEntry + k] = uint8_t(idle >> (8 * k)); out[i * kEntry + 8 + k] = uint8_t(idle >> (8 * k)); }      // IdleTime, KernelTime
      }
      return p.write_user(buffer, ConstByteView(out, total_size)) ? kSuccess : kAccessViolation;
    }
    case 0x80: {    // this OS's own class: the loaded modules of the process whose id is in the first 8 bytes of the buffer (0: this one)
      // Reply: u32 count, u32 record size (112), then per module: base (8), size (8), entry (8), flags (4: bit 0 the program), name length in characters (4), name (40 UTF-16 characters).
      constexpr uint32_t kRecord = 112, kMaxModules = 64;
      uint8_t id_bytes[8] = {};
      uint64_t pid = 0;
      if (length >= 8 && p.read_user(buffer, ByteView(id_bytes, 8))) for (uint32_t k = 0; k < 8; ++k) pid |= uint64_t{id_bytes[k]} << (8 * k);
      if (pid == 0) pid = p.id();
      IProcessRuntime::ModuleEntry modules[kMaxModules];
      const uint32_t count = runtime ? runtime->list_modules(pid, modules, kMaxModules) : 0;
      const uint32_t total_size = 8 + count * kRecord;
      if (returned != 0 && !p.write_user_u32(returned, total_size)) return kAccessViolation;
      if (length < total_size) return kInfoLengthMismatch;
      uint8_t out[8 + kMaxModules * kRecord] = {};
      auto put = [&](uint32_t where, uint64_t value, uint32_t bytes) { for (uint32_t k = 0; k < bytes; ++k) out[where + k] = uint8_t(value >> (8 * k)); };
      put(0, count, 4);
      put(4, kRecord, 4);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t base = 8 + i * kRecord;
        uint32_t name_length = 0;
        while (name_length < sizeof modules[i].name && modules[i].name[name_length]) ++name_length;
        put(base, modules[i].base, 8); put(base + 8, modules[i].size, 8); put(base + 16, modules[i].entry, 8);
        put(base + 24, modules[i].program ? 1 : 0, 4); put(base + 28, name_length, 4);
        for (uint32_t k = 0; k < name_length; ++k) put(base + 32 + k * 2, uint8_t(modules[i].name[k]), 2);
      }
      return p.write_user(buffer, ConstByteView(out, total_size)) ? kSuccess : kAccessViolation;
    }
    default: return kInvalidInfoClass;
  }
  if (returned != 0 && !p.write_user_u32(returned, size)) return kAccessViolation;
  if (length < size) return kInfoLengthMismatch;
  return p.write_user(buffer, ConstByteView(a.bytes, size)) ? kSuccess : kAccessViolation;
}

}  // namespace process::windowsabi
