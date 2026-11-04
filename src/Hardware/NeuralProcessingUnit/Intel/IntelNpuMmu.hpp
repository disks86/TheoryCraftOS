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
#include "Allocators/IAllocator.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuFirmware.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuHw.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuPageTable.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

// What the NPU's MMU reported in its event queue about a failed translation (ivpu_mmu_dump_event).
struct IntelNpuMmuEvent {
  uint32_t op = 0;    // the fault type, IntelNpuMmu::kEvt*
  uint32_t ssid = 0;  // the context
  uint32_t sid = 0;   // the stream
  uint32_t word2 = 0, word3 = 0;
  uint64_t in_address = 0;     // the NPU address that faulted
  uint64_t fetch_address = 0;  // the table address the walk was reading
};
const char* intel_npu_mmu_event_name(uint32_t op);

// The MMU in front of the NPU's memory port: an ARM SMMUv3 programmed through registers at 0x200000 of the IP BAR, with a stream table (two streams are
// used), a table of context descriptors (one per context, "SSID", pointing at a page table), a command queue the host writes and an event queue the MMU
// writes (ivpu_mmu.c). Every structure is DMA memory from the allocator. Not thread safe: the owner serializes calls (ivpu holds mmu->lock).
class IntelNpuMmu {
 public:
  static constexpr uint32_t kContextDescriptors = 256;
  static constexpr uint32_t kStreams = 4;
  static constexpr uint32_t kQueueEntries = 16;
  static constexpr uint32_t kCommandBytes = 16, kEventBytes = 32;
  // Registers, in the IP BAR.
  static constexpr uint64_t kRegIdr0 = 0x200000, kRegCr0 = 0x200020, kRegCr0Ack = 0x200024, kRegCr1 = 0x200028, kRegIrqCtrl = 0x200050,
                            kRegIrqCtrlAck = 0x200054, kRegGerror = 0x200060, kRegGerrorn = 0x200064, kRegStrtabBase = 0x200080,
                            kRegStrtabBaseCfg = 0x200088, kRegCmdqBase = 0x200090, kRegCmdqProd = 0x200098, kRegCmdqCons = 0x20009C,
                            kRegEvtqBase = 0x2000A0, kRegEvtqProdSec = 0x2000A8 + 0x10000, kRegEvtqConsSec = 0x2000AC + 0x10000;
  static constexpr uint64_t kRegisterEnd = kRegEvtqConsSec + 4;  // the IP BAR window the MMU needs
  static constexpr uint32_t kCr0SmmuEn = 1u << 0, kCr0EvtqEn = 1u << 2, kCr0CmdqEn = 1u << 3, kCr0AtsChk = 1u << 4;
  static constexpr uint32_t kIrqEvtq = 1u << 2, kIrqGerror = 1u << 0;
  static constexpr uint32_t kGerrorErrors = 0xFD;  // CMDQ, EVTQ_ABT, PRIQ_ABT and the MSI aborts: bits 0, 2-7
  // Commands.
  static constexpr uint32_t kCmdCfgiAll = 0x04, kCmdTlbiNhAsid = 0x11, kCmdTlbiNsnhAll = 0x30, kCmdSync = 0x46;
  // Event types.
  static constexpr uint32_t kEvtUnsupportedTransaction = 0x01, kEvtBadStreamId = 0x02, kEvtBadSte = 0x04, kEvtBadSubstreamId = 0x08, kEvtBadCd = 0x0A,
                            kEvtTranslation = 0x10, kEvtAddressSize = 0x11, kEvtAccess = 0x12, kEvtPermission = 0x13;

  IntelNpuMmu(INpuRegisters& ip, allocators::IAllocator& allocator, bool snoop) : ip_(ip), allocator_(allocator), snoop_(snoop) {}
  ~IntelNpuMmu();
  IntelNpuMmu(const IntelNpuMmu&) = delete;
  IntelNpuMmu& operator=(const IntelNpuMmu&) = delete;

  // ivpu_mmu_init(): allocates the stream table, the context descriptor table and the queues, points streams 0 and 3 at the descriptor table and turns the MMU on.
  bool init();
  // ivpu_mmu_enable(): resets the MMU registers and turns it on (after the NPU lost power, too). Needs init().
  bool enable();
  // Only marks the MMU off, so later descriptor and TLB requests do not touch the (powered down) hardware.
  void disable() { on_ = false; }
  bool on() const { return on_; }

  // Points context `ssid` at the page table whose top level is at bus address `root` and makes it valid, or invalidates it. SSIDs 0 (global) and 1 (reserved)
  // fault loudly; the others only abort the offending job.
  bool set_context(uint32_t ssid, uint64_t root);
  bool clear_context(uint32_t ssid);
  // Drops the cached translations of one context; returns when the MMU has done so.
  bool invalidate_tlb(uint32_t ssid);
  // Stops the context from reporting faults (its job is being aborted).
  bool disable_context_events(uint32_t ssid);

  // The next fault in the event queue, if any.
  bool next_event(IntelNpuMmuEvent* event);
  // Throws away all queued events.
  bool discard_events();
  // The global error handler: acknowledges and returns the active error bits (0 if none).
  uint32_t acknowledge_global_errors();
  // The command queue error code of the last command that timed out (0 none, 1 illegal command, 2 abort, 3 ATC sync).
  uint32_t last_command_error() const { return command_error_; }

 private:
  bool allocate();
  bool write_cr0(uint32_t value);
  bool write_irq_ctrl(uint32_t value);
  bool reset();
  void link_stream(uint32_t sid);
  bool queue_command(uint64_t word0, uint64_t word1);
  bool sync();
  bool cfgi_all_sync();
  bool poll(uint64_t offset, uint32_t mask, uint32_t expected);
  void flush(const void* memory, uint64_t bytes);
  bool evtq_set(bool enable);

  INpuRegisters& ip_;
  allocators::IAllocator& allocator_;
  bool snoop_;
  bool on_ = false;
  bool allocated_ = false;
  ArrayView<uint64_t> context_table_, stream_table_, command_queue_;
  ArrayView<uint32_t> event_queue_;
  uint32_t cmdq_prod_ = 0, cmdq_cons_ = 0, evtq_prod_ = 0, evtq_cons_ = 0;
  uint32_t command_error_ = 0;
};

// A range allocator for the NPU address space of one context (ivpu uses drm_mm): best fit, 64 KiB alignment preferred for buffers of 64 KiB and more so the
// page table can use contiguous entries.
class IntelNpuAddressSpace {
 public:
  IntelNpuAddressSpace(allocators::IAllocator& allocator, uint64_t start, uint64_t end);
  // Finds `size` bytes (a multiple of the page size) inside [lo, hi) of the space. False if there is no room.
  bool allocate(uint64_t lo, uint64_t hi, uint64_t size, uint64_t* address);
  // Takes a specific range (a fixed address the firmware asks for). False, and nothing changes, if any of it is not free.
  bool reserve(uint64_t address, uint64_t size);
  // Gives a range back. Ranges that were not handed out are refused (false), and nothing changes.
  bool release(uint64_t address, uint64_t size);
  uint64_t free_bytes() const;
  uint64_t free_ranges() const { return free_.count(); }

 private:
  struct Range { uint64_t start, end; };
  bool allocate_aligned(uint64_t lo, uint64_t hi, uint64_t size, uint64_t alignment, uint64_t* address);
  List<Range> free_;  // sorted by start, never touching
  uint64_t start_, end_;
};

// One MMU context of the NPU (ivpu_mmu_context): the global one for the firmware (SSID 0), the reserved one (1), and one per process (2 and up).
// Owns the context's page table and address space and keeps the MMU's descriptor and TLB up to date. Outlive-rule: destroy before the IntelNpuMmu.
class IntelNpuMmuContext {
 public:
  static constexpr uint32_t kGlobalSsid = 0, kReservedSsid = 1, kUserMinSsid = 2, kUserMaxSsid = 130;

  IntelNpuMmuContext(allocators::IAllocator& allocator, IntelNpuMmu& mmu, const IntelNpuMemoryMap& map, uint32_t ssid, bool snoop);
  ~IntelNpuMmuContext();
  IntelNpuMmuContext(const IntelNpuMmuContext&) = delete;
  IntelNpuMmuContext& operator=(const IntelNpuMmuContext&) = delete;

  uint32_t ssid() const { return ssid_; }
  bool descriptor_valid() const { return descriptor_valid_; }
  IntelNpuPageTable& page_table() { return table_; }
  IntelNpuAddressSpace& address_space() { return space_; }

  // The reserved context has an empty page table and a descriptor, so that every access through it faults.
  bool init_reserved();
  // Maps a buffer made of `extents` (their lengths must add up to exactly `size`) at `vpu_addr`, creates the context descriptor on first use and invalidates
  // the TLB. On failure nothing stays mapped.
  bool map(uint64_t vpu_addr, ArrayView<const IntelNpuDmaExtent> extents, uint64_t size, IntelNpuPageTable::MapFlags flags);
  void unmap(uint64_t vpu_addr, ArrayView<const IntelNpuDmaExtent> extents);
  bool set_read_only(uint64_t vpu_addr, uint64_t size);

 private:
  allocators::IAllocator& allocator_;
  IntelNpuMmu& mmu_;
  uint32_t ssid_;
  IntelNpuPageTable table_;
  IntelNpuAddressSpace space_;
  bool descriptor_valid_ = false;
};

// The host memory behind the firmware's three fixed regions, as bus extents (ivpu_fw_mem_init).
struct IntelNpuFirmwareMemory {
  IntelNpuDmaExtent boot_params;  // one page
  IntelNpuDmaExtent fw_version;   // one page, written by the firmware
  IntelNpuDmaExtent runtime;      // fw.runtime_size bytes
};

// Maps the firmware's boot parameters, version page and runtime memory in the global context at the addresses its header names, and makes its read-only
// section read-only. All or nothing. Call after IntelNpuMmu::init() and before IntelNpuHw::boot().
bool intel_npu_map_firmware(IntelNpuMmuContext& global, const IntelNpuFirmware& fw, const IntelNpuFirmwareMemory& memory);

}  // namespace hardware
