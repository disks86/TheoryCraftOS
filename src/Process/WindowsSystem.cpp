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
#include "Process/WindowsFiles.hpp"
#include "Process/WindowsProcess.hpp"
#include "Utility/ByteBuffer.hpp"

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
      IProcessRuntime::ProcessEntry entries[34];
      uint32_t count = runtime ? runtime->list_processes(entries, 32) : 0;
      {      // the processes of the system itself (session 0), which every list of Windows has
        static const struct { uint64_t pid, parent; const char* name; } kSystemProcesses[2] = {{4, 0, "System"}, {0x1D4, 4, "services.exe"}};
        for (const auto& sys : kSystemProcesses) {
          IProcessRuntime::ProcessEntry& e = entries[count++];
          e = IProcessRuntime::ProcessEntry();
          e.pid = sys.pid; e.parent = sys.parent; e.session = 0;
          for (uint32_t k = 0; sys.name[k] && k + 1 < sizeof e.name; ++k) e.name[k] = sys.name[k];
        }
      }
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
        put(0x64, e.session, 4);                                    // SessionId
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

namespace {

constexpr uint32_t kBufferTooSmall = 0xC0000023u, kInvalidParameter = 0xC000000Du, kInvalidHandleStatus = 0xC0000008u, kNoMemory = 0xC0000017u;
constexpr uint32_t kSystemLogicalProcessorInformationEx = 107, kSystemCpuSetInformation = 175, kSystemSupportedProcessorArchitectures2 = 230;

// Little-endian numbers appended to a byte buffer; a failed append is remembered.
class Writer {
 public:
  explicit Writer(ByteBuffer& out) : out_(out) {}
  bool ok() const { return ok_; }
  void u8(uint32_t v) { ok_ = ok_ && out_.put8(uint8_t(v)); }
  void u16(uint32_t v) { ok_ = ok_ && out_.put16(v & 0xFFFF); }
  void u32(uint32_t v) { ok_ = ok_ && out_.put32(v); }
  void u64(uint64_t v) { u32(uint32_t(v)); u32(uint32_t(v >> 32)); }
  void zeros(uint64_t n) { ok_ = ok_ && out_.put_zeros(n); }
 private:
  ByteBuffer& out_;
  bool ok_ = true;
};

// GROUP_AFFINITY: the mask, the group (always 0) and 6 reserved bytes.
void group_affinity(Writer& w, uint64_t mask) {
  w.u64(mask);
  w.u16(0);
  w.zeros(6);
}

// SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX for the relations asked for (RelationAll is 0xFFFF): every logical processor is a core of its own in one package and one NUMA node.
// A header is a relationship and the size of the entry (8 bytes); the entries are those of Windows 10 for a single processor group.
void logical_processors(Writer& w, uint32_t relation, uint32_t cores) {
  const uint64_t all = cores >= 64 ? ~uint64_t{0} : (uint64_t{1} << cores) - 1;
  const bool every = relation == 0xFFFF;
  auto processor_entry = [&](uint32_t kind, uint64_t mask, uint32_t flags) {      // RelationProcessorCore (0) and RelationProcessorPackage (3)
    w.u32(kind); w.u32(48);
    w.u8(flags); w.u8(0); w.zeros(20); w.u16(1);      // Flags, EfficiencyClass, Reserved, GroupCount
    group_affinity(w, mask);
  };
  auto cache_entry = [&](uint32_t level, uint32_t size, uint32_t type, uint64_t mask) {
    w.u32(2); w.u32(56);
    w.u8(level); w.u8(level == 1 ? 8 : level == 2 ? 4 : 16); w.u16(64); w.u32(size); w.u32(type); w.zeros(18); w.u16(1);      // Level, Associativity, LineSize, CacheSize, Type, Reserved, GroupCount
    group_affinity(w, mask);
  };
  for (uint32_t i = 0; i < cores; ++i) {
    const uint64_t mask = uint64_t{1} << i;
    if (every || relation == 0) processor_entry(0, mask, 0);
    if (every || relation == 2) {
      cache_entry(1, 32 * 1024, 2, mask);      // data
      cache_entry(1, 32 * 1024, 1, mask);      // instruction
      cache_entry(2, 256 * 1024, 0, mask);     // unified
    }
  }
  if (every || relation == 2) cache_entry(3, 8 * 1024 * 1024, 0, all);
  if (every || relation == 1) {      // RelationNumaNode
    w.u32(1); w.u32(48);
    w.u32(0); w.zeros(18); w.u16(1);      // NodeNumber, Reserved, GroupCount
    group_affinity(w, all);
  }
  if (every || relation == 3) processor_entry(3, all, 1);      // (the package: PROCESSOR_RELATION flags 1 is LTP_PC_SMT, not set for a processor without hyper-threads; the package carries none)
  if (every || relation == 4) {      // RelationGroup
    w.u32(4); w.u32(80);
    w.u16(1); w.u16(1); w.zeros(20);      // MaximumGroupCount, ActiveGroupCount, Reserved
    w.u8(cores > 64 ? 64 : cores); w.u8(cores > 64 ? 64 : cores); w.zeros(38); w.u64(all);      // PROCESSOR_GROUP_INFO
  }
}

}  // namespace

// NtQuerySystemInformationEx(Class, Input, InputLength, Buffer, Length, ReturnLength). SystemLogicalProcessorInformationEx takes the relationship wanted as a DWORD; SystemCpuSetInformation
// the handle of a process, and answers one SYSTEM_CPU_SET_INFORMATION (32 bytes) for each processor. The other classes are the ones of NtQuerySystemInformation.
uint32_t nt_query_system_information_ex(Process& p, const SyscallRequest& r) {
  uint64_t buffer_length = 0, returned = 0;
  if (!argument(p, r, 4, &buffer_length) || !argument(p, r, 5, &returned)) return kAccessViolation;
  const uint32_t info_class = uint32_t(r.args[0]), input_length = uint32_t(r.args[2]), length = uint32_t(buffer_length);
  IProcessRuntime* runtime = p.runtime();
  const uint32_t processors = runtime ? runtime->processor_count() : 1;
  const uint32_t cores = processors > 255 ? 255 : processors;
  if (info_class != kSystemLogicalProcessorInformationEx && info_class != kSystemCpuSetInformation && info_class != kSystemSupportedProcessorArchitectures2) {
    SyscallRequest plain = r;      // the same question without an input
    plain.args[0] = info_class;
    plain.args[1] = r.args[3];
    plain.args[2] = buffer_length;
    plain.args[3] = returned;
    return nt_query_system_information(p, plain);
  }
  uint8_t input[8] = {};
  const uint32_t wanted_input = info_class == kSystemLogicalProcessorInformationEx ? 4 : 8;
  if (input_length != wanted_input) return kInvalidParameter;
  if (!p.read_user(r.args[1], ByteView(input, wanted_input))) return kAccessViolation;
  uint64_t first = 0;
  for (uint32_t i = 0; i < wanted_input; ++i) first |= uint64_t{input[i]} << (8 * i);
  ByteBuffer answer(p.files().allocator());
  Writer w(answer);
  if (info_class == kSystemSupportedProcessorArchitectures2) {      // the machines of this system: x86-64 only, in kernel and user mode, for processes too
    if (first != ~uint64_t{0} && !names_own_process(p, first)) {
      bool exited = false;
      uint32_t code = 0;
      uint64_t pid = 0;
      if (!process_handle_info(p, first, &exited, &code, &pid)) return kInvalidHandleStatus;
    }
    w.u32(0x8664u | (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19));      // Machine, KernelMode, UserMode, Native, Process
    w.u32(0);
  } else if (info_class == kSystemCpuSetInformation) {
    if (first != ~uint64_t{0} && !names_own_process(p, first)) {
      bool exited = false;
      uint32_t code = 0;
      uint64_t pid = 0;
      if (!process_handle_info(p, first, &exited, &code, &pid)) return kInvalidHandleStatus;
    }
    for (uint32_t i = 0; i < cores; ++i) {
      w.u32(32); w.u32(0);      // Size, Type: CpuSetInformation
      w.u32(0x100 + i); w.u16(0); w.u8(i); w.u8(i); w.u8(0); w.u8(0); w.u8(0); w.u8(0);      // Id, Group, LogicalProcessorIndex, CoreIndex, LastLevelCacheIndex, NumaNodeIndex, EfficiencyClass, flags
      w.zeros(4); w.u64(0);      // Reserved, AllocationTag
    }
  } else {
    const uint32_t relation = uint32_t(first);
    if (relation > 4 && relation != 0xFFFF) return kInvalidParameter;
    logical_processors(w, relation, cores > 64 ? 64 : cores);
  }
  if (!w.ok()) return kNoMemory;
  if (returned != 0 && !p.write_user_u32(returned, uint32_t(answer.size()))) return kAccessViolation;
  if (length < answer.size()) return info_class == kSystemCpuSetInformation ? kBufferTooSmall : kInfoLengthMismatch;
  return p.write_user(r.args[3], answer.view()) ? kSuccess : kAccessViolation;
}

}  // namespace process::windowsabi
