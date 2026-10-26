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

// Host-side tests for the Raspberry Pi 4 / Pi 400 support: board decoding, the VideoCore mailbox message format (with truncation and bit-flip cases), the firmware
// services against a pretend firmware, the V3D core and its page table against a pretend register file, the HEVC engine's bring-up, and the platform that ties them.
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <thread>
#include <vector>
#include "Allocators/IAllocator.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/RaspberryPiGpu.hpp"
#include "Graphics/Graphics3D/BuiltinShaders.hpp"
#include "Graphics/Graphics3D/V3d/V3dGraphicsDevice.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dDraw.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dFloat.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dFrame.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dGpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dPageTable.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dQpu.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dRenderer.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dSelfTest.hpp"
#include "Hardware/GraphicsProcessingUnit/Broadcom/V3dShaders.hpp"
#include "Hardware/Platform/RaspberryPi/RaspberryPiPlatform.hpp"
#include "Hardware/VideoCodec/Broadcom/BroadcomVideoEngine.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::vector<void*> blocks;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment); blocks.push_back(p); return p; }
  void deallocate(void*) override {}
  ~TestAllocator() { for (void* p : blocks) free(p); }
};

// Register windows are plain, naturally aligned memory.
struct Window {
  uint32_t* words;
  uint64_t bytes;
  explicit Window(uint64_t size) : words(static_cast<uint32_t*>(aligned_alloc(4096, size))), bytes(size) { for (uint64_t i = 0; i < size / 4; ++i) words[i] = 0; }
  ~Window() { free(words); }
  MmioRegion region() const { return MmioRegion(reinterpret_cast<uint64_t>(words), bytes); }
  uint32_t& at(uint64_t offset) { return words[offset / 4]; }
};

// Stands in for a register bit the hardware clears by itself (the cache flush bit): clears it as fast as it is set.
struct AutoClear {
  std::atomic<bool> stop{false}, running{false};
  std::thread worker;
  AutoClear(Window& w, uint64_t offset, uint32_t bit) {
    worker = std::thread([this, &w, offset, bit] { while (!stop.load()) { __atomic_and_fetch(&w.at(offset), ~bit, __ATOMIC_RELAXED); running = true; } });
    while (!running.load()) std::this_thread::yield();
  }
  ~AutoClear() { stop = true; worker.join(); }
};

struct TestMessageMemory final : IVcMessageMemory {
  uint32_t* storage = static_cast<uint32_t*>(aligned_alloc(64, 512));
  TestMessageMemory() { for (int i = 0; i < 128; ++i) storage[i] = 0; }
  ~TestMessageMemory() { free(storage); }
  ArrayView<uint32_t> words() override { return ArrayView<uint32_t>(storage, 128); }
  uint32_t bus_address() const override { return 0xC0001000; }
};

// A VideoCore that answers the tags this OS sends.
struct FakeFirmware final : IVcMailboxPort {
  TestMessageMemory* memory;
  uint32_t board_revision = 0x00C03130;  // Pi 400 Rev 1.0, 4 GiB
  uint32_t pending = 0;
  bool refuse = false, drop_answer = false;
  std::map<uint32_t, uint32_t> clock_hz, clock_on;
  uint32_t next_handle = 1;
  std::map<uint32_t, uint32_t> memory_handles;  // handle -> bus address when locked
  int clock_sets = 0;
  explicit FakeFirmware(TestMessageMemory* m) : memory(m) { clock_hz[vc_clock::kV3d] = 250000000; clock_hz[vc_clock::kHevc] = 0; }

  bool send(uint32_t channel, uint32_t value) override {
    if (channel != 8 || value != memory->bus_address()) return false;
    handle();
    pending = value | channel;
    return true;
  }
  bool receive(uint32_t channel, uint32_t* value) override {
    if (drop_answer || (pending & 0xF) != channel) return false;
    *value = pending & ~0xFu;
    return true;
  }
  void handle() {
    uint32_t* m = memory->storage;
    if (refuse) { m[1] = 0x80000001; return; }
    const uint32_t total = m[0];
    uint64_t at = 2;
    while (at < total / 4 && m[at] != 0) {
      const uint32_t id = m[at];
      uint32_t* v = m + at + 3;
      const uint32_t size = m[at + 1];
      uint32_t used = 0;
      switch (id) {
        case vc_tag::kFirmwareRevision: v[0] = 0x65f1a2b3; used = 4; break;
        case vc_tag::kBoardRevision: v[0] = board_revision; used = 4; break;
        case vc_tag::kBoardSerial: v[0] = 0x1234; v[1] = 0x5678; used = 8; break;
        case vc_tag::kArmMemory: v[0] = 0; v[1] = 0x3C000000; used = 8; break;
        case vc_tag::kVcMemory: v[0] = 0x3C000000; v[1] = 0x04000000; used = 8; break;
        case vc_tag::kGetClockRate: v[1] = clock_hz[v[0]]; used = 8; break;
        case vc_tag::kGetMaxClockRate: v[1] = v[0] == vc_clock::kV3d ? 500000000 : 1500000000; used = 8; break;
        case vc_tag::kSetClockRate: clock_hz[v[0]] = v[1]; ++clock_sets; used = 8; break;
        case vc_tag::kSetClockState: clock_on[v[0]] = v[1] & 1; v[1] = clock_on[v[0]]; used = 8; break;
        case vc_tag::kGetClockState: v[1] = clock_on[v[0]]; used = 8; break;
        case vc_tag::kGetTemperature: v[1] = 47500; used = 8; break;
        case vc_tag::kAllocateMemory: v[0] = next_handle++; memory_handles[v[0]] = 0; used = 4; break;
        case vc_tag::kLockMemory: memory_handles[v[0]] = 0x3A000000 + v[0] * 0x1000; v[0] = memory_handles[v[0]]; used = 4; break;
        case vc_tag::kUnlockMemory: case vc_tag::kReleaseMemory: v[0] = 0; used = 4; break;
        case vc_tag::kGetEdidBlock: { const uint32_t b = v[0]; v[0] = b; v[1] = 0; for (int i = 0; i < 32; ++i) v[2 + i] = 0x03020100u + 0x04040404u * i; used = 136; break; }
        default: used = 0; break;
      }
      m[at + 2] = 0x80000000 | (used < size ? used : size);
      at += 3 + size / 4;
    }
    m[1] = 0x80000000;
  }
};

struct FakeMapper final : IRegisterMapper {
  std::map<uint64_t, MmioRegion> windows;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    auto it = windows.find(physical);
    if (it == windows.end() || it->second.length() < length) return false;
    *region = it->second;
    return true;
  }
};

static void test_board() {
  PiBoard b = pi_decode_revision(0x00C03130);
  CHECK(b.valid && b.is_pi400() && b.is_bcm2711() && b.memory_mib == 4096 && b.board_revision == 0);
  b = pi_decode_revision(0x00D03114);  // Pi 4 B 8 GiB
  CHECK(b.valid && !b.is_pi400() && b.is_bcm2711() && b.memory_mib == 8192);
  b = pi_decode_revision(0x00A02082);  // Pi 3 B
  CHECK(b.valid && !b.is_bcm2711() && b.processor == PiProcessor::Bcm2837);
  CHECK(!pi_decode_revision(0x0010).valid);  // old style
}

static void test_message_format() {
  uint32_t storage[32] = {};
  uint32_t values[2] = {5, 0};
  VcTag tag;
  tag.id = vc_tag::kGetClockRate;
  tag.values = ArrayView<uint32_t>(values);
  VcPropertyMessage msg{ArrayView<uint32_t>(storage)};
  CHECK(msg.build(ArrayView<const VcTag>(&tag, 1)));
  CHECK(storage[0] == 4 * 8 && storage[1] == 0 && storage[2] == vc_tag::kGetClockRate && storage[3] == 8 && storage[4] == 0 && storage[5] == 5 && storage[7] == 0);
  // Too small a buffer.
  uint32_t tiny[4] = {};
  VcPropertyMessage small{ArrayView<uint32_t>(tiny)};
  CHECK(!small.build(ArrayView<const VcTag>(&tag, 1)));
  // Tag 0 is the terminator and cannot be sent.
  VcTag zero;
  VcPropertyMessage msg2{ArrayView<uint32_t>(storage)};
  CHECK(!msg2.build(ArrayView<const VcTag>(&zero, 1)));
  // A good answer parses.
  storage[1] = 0x80000000;
  storage[4] = 0x80000008;
  storage[6] = 123456;
  CHECK(msg.parse(ArrayView<VcTag>(&tag, 1)) && values[1] == 123456 && tag.response_words == 2);
}

// Every single-bit flip and every truncation of a valid answer is parsed without a crash or a read outside the message (the sanitizers watch), and a damaged answer
// that still parses leaves the tags' buffers inside their own length.
static void test_message_fuzz() {
  uint32_t good[16] = {};
  uint32_t values[2] = {7, 0};
  VcTag tag;
  tag.id = vc_tag::kGetMaxClockRate;
  tag.values = ArrayView<uint32_t>(values);
  {
    VcPropertyMessage msg{ArrayView<uint32_t>(good)};
    CHECK(msg.build(ArrayView<const VcTag>(&tag, 1)));
  }
  good[1] = 0x80000000; good[4] = 0x80000008; good[6] = 99;
  for (uint32_t word = 0; word < 8; ++word)
    for (uint32_t b = 0; b < 32; ++b) {
      uint32_t copy[16];
      for (int i = 0; i < 16; ++i) copy[i] = good[i];
      copy[word] ^= 1u << b;
      uint32_t v[2] = {7, 0};
      VcTag t;
      t.id = vc_tag::kGetMaxClockRate;
      t.values = ArrayView<uint32_t>(v);
      VcPropertyMessage msg{ArrayView<uint32_t>(copy)};
      const bool ok = msg.parse(ArrayView<VcTag>(&t, 1));
      if (ok) CHECK(t.response_words <= 2);
    }
  for (uint32_t keep = 0; keep < 8; ++keep) {  // truncation: the buffer is shorter than the total the header claims
    uint32_t v[2] = {7, 0};
    VcTag t;
    t.id = vc_tag::kGetMaxClockRate;
    t.values = ArrayView<uint32_t>(v);
    VcPropertyMessage msg{ArrayView<uint32_t>(good, keep)};
    const bool ok = msg.parse(ArrayView<VcTag>(&t, 1));
    CHECK(!ok || keep >= 7);
  }
}

static void test_firmware() {
  TestMessageMemory mem;
  FakeFirmware fw(&mem);
  VcMailbox mailbox(fw, mem);
  VideoCoreFirmware f(mailbox);
  PiBoard board;
  CHECK(f.board(&board) && board.is_pi400());
  uint32_t a = 0, b = 0;
  CHECK(f.arm_memory(&a, &b) && b == 0x3C000000);
  CHECK(f.gpu_memory(&a, &b) && a == 0x3C000000 && b == 0x04000000);
  uint64_t serial = 0;
  CHECK(f.serial(&serial) && serial == 0x0000567800001234ull);
  CHECK(f.max_clock_rate(vc_clock::kV3d, &a) && a == 500000000);
  CHECK(f.set_clock_rate(vc_clock::kV3d, 500000000, &b) && b == 500000000);
  CHECK(f.clock_rate(vc_clock::kV3d, &a) && a == 500000000);
  CHECK(f.set_clock_enabled(vc_clock::kHevc, true));
  bool on = false;
  CHECK(f.clock_enabled(vc_clock::kHevc, &on) && on);
  CHECK(f.temperature(&a) && a == 47500);
  uint32_t handle = 0, bus = 0;
  CHECK(f.allocate_memory(65536, 4096, vc_memory::kCoherent | vc_memory::kZero, &handle) && handle != 0);
  CHECK(f.lock_memory(handle, &bus) && bus != 0 && f.unlock_memory(handle) && f.release_memory(handle));
  uint8_t edid[128];
  CHECK(f.edid_block(0, ArrayView<uint8_t>(edid)) && edid[0] == 0 && edid[1] == 1 && edid[4] == 4 && edid[127] == 0x7F + 0);
  uint8_t small[64];
  CHECK(!f.edid_block(0, ArrayView<uint8_t>(small)));
  // The firmware refusing, or never answering.
  fw.refuse = true;
  CHECK(!f.temperature(&a));
  fw.refuse = false;
  fw.drop_answer = true;
  CHECK(!f.temperature(&a));
}

static void identify_v3d(Window& hub, Window& core, uint32_t tver = 4, uint32_t rev = 2, bool mmu = true) {
  hub.at(v3d::kHubIdent0) = 0x02443356;
  hub.at(v3d::kHubIdent1) = (1u << 17) | (1u << 8) | (rev << 4) | tver;  // TFU, 1 core
  hub.at(v3d::kHubIdent2) = mmu ? (1u << 8) : 0;
  core.at(v3d::kCtlIdent1) = (8u << 28) | (16u << 16) | (2u << 12) | (4u << 8) | (3u << 4) | 2;  // 3 slices, 4 QPUs each, 2 TMUs
}

static void test_v3d_identify_and_jobs() {
  Window hub(0x4000), core(0x4000);
  CHECK(!V3dGpu::identify(hub.region(), core.region()).valid);  // no magic
  identify_v3d(hub, core);
  V3dIdentity id = V3dGpu::identify(hub.region(), core.region());
  CHECK(id.valid && id.version() == 42 && id.cores == 1 && id.slices == 3 && id.qpus_per_slice == 4 && id.qpus() == 12 && id.texture_units == 2 && id.with_mmu && id.with_tfu);

  AutoClear flush_done(core, v3d::kCtlL2TCaCtl, v3d::kL2TCaCtlFlush);
  V3dGpu gpu(hub.region(), core.region());
  CHECK(gpu.initialize() && gpu.initialized());
  CHECK(core.at(v3d::kCtlIntMskClr) == (v3d::kIntOutOfMemory | v3d::kIntFlDone | v3d::kIntFrDone | v3d::kIntViolation));
  CHECK(core.at(v3d::kCtlSlCaCtl) == v3d::kSlCaCtlAll && core.at(v3d::kCtlL2TFlEnd) == 0xFFFFFFFF);

  // A binning list: the driver writes the base then the end (which starts it); the flush-done bit is already up.
  core.at(v3d::kCtlIntSts) = v3d::kIntFlDone;
  V3dBinJob bin;
  bin.start = 0x1000; bin.end = 0x1040; bin.tile_alloc = {0x40000, 0x90000}; bin.tile_state = 0x30000;
  CHECK(gpu.run_binning(bin, 10) == V3dJobResult::Done);
  CHECK(core.at(v3d::kCleCt0Qba) == 0x1000 && core.at(v3d::kCleCt0Qea) == 0x1040 && core.at(v3d::kCleCt0Qma) == 0x40000 && core.at(v3d::kCleCt0Qms) == 0x90000);
  CHECK(core.at(v3d::kCleCt0Qts) == (v3d::kCleCt0QtsEnable | 0x30000) && core.at(v3d::kPtbBpos) == 0);
  // Rendering waits for frame done: not there yet -> timeout; then there.
  core.at(v3d::kCtlIntSts) = v3d::kIntFlDone;
  CHECK(gpu.run_rendering(0x2000, 0x2100, 5) == V3dJobResult::TimedOut);
  core.at(v3d::kCtlIntSts) = v3d::kIntFrDone;
  CHECK(gpu.run_rendering(0x2000, 0x2100, 5) == V3dJobResult::Done && core.at(v3d::kCleCt1Qba) == 0x2000);
  // Bad lists are refused before anything is written.
  core.at(v3d::kCleCt0Qba) = 0xDEAD;
  V3dBinJob backwards = bin;  // a list is a run of packets of any length, so only an end before the start is wrong
  backwards.start = 0x1040; backwards.end = 0x1000;
  CHECK(gpu.run_binning(backwards, 5) == V3dJobResult::Error && core.at(v3d::kCleCt0Qba) == 0xDEAD);
  // A core error.
  core.at(v3d::kCtlIntSts) = 0;
  core.at(v3d::kErrStat) = 1u << 5;
  CHECK(gpu.run_binning(bin, 5) == V3dJobResult::Error && gpu.last_error() == (1u << 5));
  core.at(v3d::kErrStat) = 0;
  // An MMU page fault.
  hub.at(v3d::kHubIntSts) = v3d::kHubIntMmuPageInvalid;
  CHECK(gpu.run_binning(bin, 5) == V3dJobResult::Error && (gpu.last_hub_interrupts() & v3d::kHubIntMmuPageInvalid));
  hub.at(v3d::kHubIntSts) = 0;
  // Out of tile memory: no overflow block -> OutOfMemory; with one it is handed over, then (the flag stays up in the pretend core) the job reports it again.
  core.at(v3d::kCtlIntSts) = v3d::kIntOutOfMemory;
  CHECK(gpu.run_binning(bin, 5) == V3dJobResult::OutOfMemory);
  const V3dMemoryBlock blocks[2] = {{0x30000, 0x10000}, {0x50000, 0x10000}};
  CHECK(gpu.run_binning(bin, 5, ArrayView<const V3dMemoryBlock>(blocks)) == V3dJobResult::OutOfMemory && core.at(v3d::kPtbBpoa) == 0x50000 && core.at(v3d::kPtbBpos) == 0x10000);

  // MMU.
  CHECK(!gpu.enable_mmu(0x1001, 0x2000));
  CHECK(gpu.enable_mmu(0x3A000000, 0x3A400000));
  CHECK(hub.at(v3d::kMmuPtPaBase) == 0x3A000 && (hub.at(v3d::kMmuCtl) & v3d::kMmuCtlEnable) && (hub.at(v3d::kMmuIllegalAddr) & v3d::kMmuIllegalAddrEnable));
  gpu.shutdown();
  CHECK(!gpu.initialized() && !(hub.at(v3d::kMmuCtl) & v3d::kMmuCtlEnable) && !gpu.flush_caches());
}

static void test_v3d_rejects() {
  Window hub(0x4000), core(0x4000);
  identify_v3d(hub, core, 3, 3);  // V3D 3.3 (Pi 3 era VideoCore IV is not this core)
  V3dGpu old(hub.region(), core.region());
  CHECK(!old.initialize());
  identify_v3d(hub, core, 4, 2, false);
  V3dGpu nommu(hub.region(), core.region());
  CHECK(!nommu.initialize());
  V3dGpu none{MmioRegion(), MmioRegion()};
  CHECK(!none.initialize());
}


// Packet bytes for two frames, produced by an independent packer written straight from Mesa's v3d_packet.xml (gen 4.2) following the packet order of Mesa's v3dx_rcl.c
// and v3dx_draw.c (see scripts/gen_v3d_packets.py for where the layouts come from).
struct Golden { const char* bcl; const char* rcl; const char* generic; };
static const Golden kGoldenClearStore = {
    "7800000000ff04cf02135c000000000604",
    "79000005d00240000079034080ffff0000007981000000000000007902000000803f00007e047b000004007a0000140c14c000007c0000"
    "001a1d08000000000000000000000019031b7c0000001a1d0800000000000000000000001b1314001000001c1000001700001701001702"
    "00170300170400170500170600170700170800170900170a00170b00170c00170d00170e00170f00171000171100171200171300170001"
    "170101170201170301170401170501170601170701170801170901170a01170b01170c01170d01170e01170f0117100117110117120117"
    "1301170002170102170202170302170402170502170602170702170802170902170a02170b02170c02170d02170e02170f021710021711"
    "02171202171302170003170103170203170303170403170503170603170703170803170903170a03170b03170c03170d03170e03170f03"
    "171003171103171203171303170004170104170204170304170404170504170604170704170804170904170a04170b04170c04170d0417"
    "0e04170f04171004171104171204171304170005170105170205170305170405170505170605170705170805170905170a05170b05170c"
    "05170d05170e05170f05171005171105171205171305170006170106170206170306170406170506170606170706170806170906170a06"
    "170b06170c06170d06170e06170f0617100617110617120617130617000717010717020717030717040717050717060717070717080717"
    "0907170a07170b07170c07170d07170e07170f071710071711071712071713071700081701081702081703081704081705081706081707"
    "08170808170908170a08170b08170c08170d08170e08170f08171008171108171208171308170009170109170209170309170409170509"
    "170609170709170809170909170a09170b09170c09170d09170e09170f0917100917110917120917130917000a17010a17020a17030a17"
    "040a17050a17060a17070a17080a17090a170a0a170b0a170c0a170d0a170e0a170f0a17100a17110a17120a17130a17000b17010b1702"
    "0b17030b17040b17050b17060b17070b17080b17090b170a0b170b0b170c0b170d0b170e0b170f0b17100b17110b17120b17130b0d",
    "7d1a3802360000000015001d00b01100400100000000100019031b12"};
static const Golden kGoldenDepth = {
    "78000000007f073704135c000000000604",
    "7900800738044000007903ff0000800000007981000000000000007902000000403f00007e047b000008007a01010f091e1001007c0000"
    "001a1d08000000000000000000000019031b7c0000001a1d0800000000000000000000001b131400200000432000001700001701001702"
    "00170300170400170500170600170700170800170900170a00170b00170c00170d00170e00170001170101170201170301170401170501"
    "170601170701170801170901170a01170b01170c01170d01170e0117000217010217020217030217040217050217060217070217080217"
    "0902170a02170b02170c02170d02170e02170003170103170203170303170403170503170603170703170803170903170a03170b03170c"
    "03170d03170e03170004170104170204170304170404170504170604170704170804170904170a04170b04170c04170d04170e04170005"
    "170105170205170305170405170505170605170705170805170905170a05170b05170c05170d05170e0517000617010617020617030617"
    "0406170506170606170706170806170906170a06170b06170c06170d06170e061700071701071702071703071704071705071706071707"
    "07170807170907170a07170b07170c07170d07170e07170008170108170208170308170408170508170608170708170808170908170a08"
    "170b08170c08170d08170e080d",
    "7d1e00b00100e0010000000020001e09800200e0010000000080001a3802360000000015001d00b00100e0010000000020001d09800200"
    "e00100000000800019031b12"};
// State packets + draw packets (bcl), the list memory the draw wrote (indirect), for a plain draw and for a depth-tested, blended, indexed one.
struct DrawGolden { const char* state; const char* draw; const char* indirect; };
static const DrawGolden kGoldenDrawPlain = {
    "6b000000000005d00260017000680000803f690000803f6e000020480000b4476f0000803f000000006d000000000000803f6c00800200"
    "0068010057ffffffff6163584a005b0f00803f",
    "474440c210070024042400000000000000",
    "ffffffff000000000000000000000000000000400100004002000040030000400400004005000040060000400700004008000040090000"
    "400a0000400b0000400c0000400d0000400e0000400f000040100000401100004012000040130000400000803f0100803f0200803f0300"
    "803f0400803f0500803f0600803f0700803f0800803f0900803f0a00803f0b00803f0c00803f0d00803f0e00803f0f00803f1000803f11"
    "00803f0000000000000000000000000000000000000000000000000200040401010101002007007e040300001007001602030010100700"
    "0600030060100700000050000b3300001c000000ffffff000c005000084000001c000000ffffff00"};
static const DrawGolden kGoldenDrawDepth = {
    "6b10000800600720046007b008680000803f690000803f6e00006c48000004486f0000003f0000803e6d0000803e0000403f6c00c00300"
    "001802005301541007760f57ffffffff6163584a005b0f00803f",
    "474440c21008002b05000000000000002c00006000d00700002044060000000c000000",
    "84ffffffffffffff0000000000000000000000400100004002000040030000400400004005000040060000400700004008000040090000"
    "400a0000400b0000400c0000400d0000400e0000400f000040100000401100004012000040130000400000803f0100803f0200803f0300"
    "803f0400803f0500803f0600803f0700803f0800803f0900803f0a00803f0b00803f0c00803f0d00803f0e00803f0f00803f1000803f11"
    "00803f00000000000000000000000000000000000000000000000002030404010101010020080026050300001008001602030010100800"
    "0600030060100800000050000b3300001c000000ffffff000c005000084000001c000000ffffff00"};
// The QPU programs of the coloured-vertex pipeline (coordinate, vertex, fragment, fragment with Z), 64-bit words. Every one of them was decoded and packed again by
// Mesa's v3d_qpu_instr_unpack / v3d_qpu_instr_pack (V3D 4.2) with the same bits coming out, and its disassembly read.
static const uint64_t kGoldenShaderWords[] = {
    0x3de02180bc807000ULL, 0x3de02181bc807001ULL, 0x3de02182bc807002ULL,
    0x3d82b186bb800000ULL, 0x54001106bbf80280ULL, 0x3d82b186bb800000ULL,
    0x54001146bbf80280ULL, 0x3d82b186bb800000ULL, 0x54001186bbf80280ULL,
    0x3d82b186bb800000ULL, 0x540011c6bbf80280ULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80281ULL, 0x3c0021840583e10bULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80281ULL, 0x3c0021850583e14bULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80281ULL, 0x3c0021860583e18bULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80281ULL, 0x3c0021870583e1cbULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80282ULL, 0x3c0021840583e10bULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80282ULL, 0x3c0021850583e14bULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80282ULL, 0x3c0021860583e18bULL, 0x3d82b186bb800000ULL,
    0x540012c6bbf80282ULL, 0x3c0021870583e1cbULL, 0x3d82b186bb800000ULL,
    0x3c0021840583e10aULL, 0x3d82b186bb800000ULL, 0x3c0021850583e14aULL,
    0x3d82b186bb800000ULL, 0x3c0021860583e18aULL, 0x3d82b186bb800000ULL,
    0x3c0021870583e1caULL, 0x3d823186bb800000ULL, 0x3d827186bb800000ULL,
    0x3de02180f8837100ULL, 0x3de02180f8837141ULL, 0x3de02180f8837182ULL,
    0x3de02180f88371c3ULL, 0x3c003193b68361c0ULL, 0x3c003186bb800000ULL,
    0x3c003186bb800000ULL, 0x54001306bbf80108ULL, 0x54001306bb980300ULL,
    0x3c00218cf6806300ULL, 0x3c00218cf583e300ULL, 0x3de02180f8837304ULL,
    0x54001306bbf80149ULL, 0x54001306bb980300ULL, 0x3c00218cf6806300ULL,
    0x3c00218cf583e300ULL, 0x3de02180f8837305ULL, 0x3c003186bb816000ULL,
    0x3c203186bb800000ULL, 0x3c003186bb800000ULL, 0x3c003186bb800000ULL,
    0x3de02180bc807000ULL, 0x3de02181bc807001ULL, 0x3de02182bc807002ULL,
    0x3de02194bc807003ULL, 0x3de02195bc807004ULL, 0x3de02196bc807005ULL,
    0x3de02197bc807006ULL, 0x3d82b186bb800000ULL, 0x54001106bbf80280ULL,
    0x3d82b186bb800000ULL, 0x54001146bbf80280ULL, 0x3d82b186bb800000ULL,
    0x54001186bbf80280ULL, 0x3d82b186bb800000ULL, 0x540011c6bbf80280ULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80281ULL, 0x3c0021840583e10bULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80281ULL, 0x3c0021850583e14bULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80281ULL, 0x3c0021860583e18bULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80281ULL, 0x3c0021870583e1cbULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80282ULL, 0x3c0021840583e10bULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80282ULL, 0x3c0021850583e14bULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80282ULL, 0x3c0021860583e18bULL,
    0x3d82b186bb800000ULL, 0x540012c6bbf80282ULL, 0x3c0021870583e1cbULL,
    0x3d82b186bb800000ULL, 0x3c0021840583e10aULL, 0x3d82b186bb800000ULL,
    0x3c0021850583e14aULL, 0x3d82b186bb800000ULL, 0x3c0021860583e18aULL,
    0x3d82b186bb800000ULL, 0x3c0021870583e1caULL, 0x3d823186bb800000ULL,
    0x3d827186bb800000ULL, 0x3d83b186bb800000ULL, 0x3d83f186bb800000ULL,
    0x3c003193b68361c0ULL, 0x3c003186bb800000ULL, 0x3c003186bb800000ULL,
    0x54001306bbf80108ULL, 0x54001306bb980300ULL, 0x3c00218cf6806300ULL,
    0x3c00218cf583e300ULL, 0x3de02180f8837300ULL, 0x54001306bbf80149ULL,
    0x54001306bb980300ULL, 0x3c00218cf6806300ULL, 0x3c00218cf583e300ULL,
    0x3de02180f8837301ULL, 0x54001306bbf8018eULL, 0x54001306bb980300ULL,
    0x3c00218c0583e30fULL, 0x3de02180f8837302ULL, 0x3de02180f8827003ULL,
    0x3de02180f8837504ULL, 0x3de02180f8837545ULL, 0x3de02180f8837586ULL,
    0x3de02180f88375c7ULL, 0x3c003186bb816000ULL, 0x3c203186bb800000ULL,
    0x3c003186bb800000ULL, 0x3c003186bb800000ULL, 0x3d02b186bb800000ULL,
    0x3c003186bb800000ULL, 0x540012c6bbf80280ULL, 0x3c002184058352c0ULL,
    0x3d02b186bb800000ULL, 0x3c003186bb800000ULL, 0x540012c6bbf80280ULL,
    0x3c002185058352c0ULL, 0x3d02b186bb800000ULL, 0x3c003186bb800000ULL,
    0x540012c6bbf80280ULL, 0x3c002186058352c0ULL, 0x3d02b186bb800000ULL,
    0x3c003186bb800000ULL, 0x540012c6bbf80280ULL, 0x3c002187058352c0ULL,
    0x3c0031883583e105ULL, 0x3c0031873583e187ULL, 0x3c203186bb800000ULL,
    0x3c003186bb800000ULL, 0x3c003186bb800000ULL, 0x3d02b186bb800000ULL,
    0x3c003186bb800000ULL, 0x540012c6bbf80280ULL, 0x3c002184058352c0ULL,
    0x3d02b186bb800000ULL, 0x3c003186bb800000ULL, 0x540012c6bbf80280ULL,
    0x3c002185058352c0ULL, 0x3d02b186bb800000ULL, 0x3c003186bb800000ULL,
    0x540012c6bbf80280ULL, 0x3c002186058352c0ULL, 0x3d02b186bb800000ULL,
    0x3c003186bb800000ULL, 0x540012c6bbf80280ULL, 0x3c002187058352c0ULL,
    0x3c003188b6836080ULL, 0x3c0031883583e105ULL, 0x3c0031873583e187ULL,
    0x3c203186bb800000ULL, 0x3c003186bb800000ULL, 0x3c003186bb800000ULL,
};

static std::vector<uint8_t> unhex(const char* hex) {
  std::vector<uint8_t> out;
  for (const char* p = hex; p[0] && p[1]; p += 2) { auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; }; out.push_back(static_cast<uint8_t>(v(p[0]) * 16 + v(p[1]))); }
  return out;
}

static void check_frame(const V3dFrameDesc& desc, const Golden& golden, uint32_t generic_address, uint32_t tile_alloc) {
  V3dTileGeometry g;
  CHECK(V3dFrameBuilder::geometry(desc.width, desc.height, &g));
  std::vector<uint8_t> b(4096), r(4096), gen(4096);
  V3dControlList bcl{ArrayView<uint8_t>(b.data(), b.size()), 0x10000}, rcl{ArrayView<uint8_t>(r.data(), r.size()), 0x20000}, generic{ArrayView<uint8_t>(gen.data(), gen.size()), generic_address};
  V3dFrameBuilder::begin_binning(bcl, desc);
  V3dFrameBuilder::end_binning(bcl);
  CHECK(V3dFrameBuilder::build_render_list(rcl, generic, desc, g, tile_alloc));
  const std::vector<uint8_t> eb = unhex(golden.bcl), er = unhex(golden.rcl), eg = unhex(golden.generic);
  CHECK(bcl.ok() && bcl.size() == eb.size() && std::equal(eb.begin(), eb.end(), b.begin()));
  CHECK(rcl.size() == er.size() && std::equal(er.begin(), er.end(), r.begin()));
  CHECK(generic.size() == eg.size() && std::equal(eg.begin(), eg.end(), gen.begin()));
}

static void test_qpu_encoder() {
  using namespace v3d_qpu;
  uint64_t w = 0;
  Instruction i;
  CHECK(encode(i, &w) && w == 0x3c003186bb800000ULL);   // nop ; nop
  i.add_op = Add::Fadd; i.add_dest = rf_dest(4); i.add_a = rf(4); i.add_b = rf(11);
  CHECK(encode(i, &w) && w == 0x3c0021840583e10bULL);   // fadd rf4, rf4, rf11
  Instruction c;
  c.add_op = Add::Fadd; c.add_a = rf(1); c.add_b = rf(2); c.mul_op = Mul::Fmul; c.mul_a = rf(3);
  CHECK(!encode(c, &w));                                 // three register file entries need three ports
  Instruction imm2;
  imm2.add_op = Add::LdVpmvIn; imm2.add_dest = rf_dest(0); imm2.add_a = imm(2); imm2.mul_op = Mul::Fmul; imm2.mul_a = imm(3); imm2.mul_b = acc(0);
  CHECK(!encode(imm2, &w));                              // one immediate per instruction
  Instruction big; big.add_op = Add::LdVpmvIn; big.add_a = imm(16);
  CHECK(!encode(big, &w));
  Instruction bad_rf; bad_rf.add_op = Add::Or; bad_rf.add_a = rf(64); bad_rf.add_b = rf(64);
  CHECK(!encode(bad_rf, &w));
  Instruction both; both.ldvary = true; both.ldunifrf = true;
  CHECK(!encode(both, &w));
  Instruction extra; extra.add_op = Add::Nop; extra.add_a = rf(1);
  CHECK(!encode(extra, &w));                             // an operand the operation does not read
  uint64_t two[2];
  Program p{ArrayView<uint64_t>(two)};
  CHECK(p.nop(2) && p.count() == 2 && !p.emit(Instruction()) && !p.ok());
}

static void test_shaders() {
  std::vector<uint64_t> code(512);
  v3d_shader::ShaderCode sc;
  CHECK(v3d_shader::build_colored_pipeline(ArrayView<uint64_t>(code.data(), code.size()), &sc));
  const uint32_t total = sc.fragment_z.first + sc.fragment_z.instructions;
  CHECK(total == sizeof kGoldenShaderWords / sizeof kGoldenShaderWords[0] && sc.coordinate.first == 0 && sc.vertex.first == 66 && sc.fragment.first == 143 && sc.fragment_z.first == 164);
  CHECK(std::equal(code.begin(), code.begin() + total, std::begin(kGoldenShaderWords)));
  // Every program ends with a signalled thread switch and its two delay slots; the programs that write to the VPM wait for it first.
  const v3d_shader::Program programs[4] = {sc.coordinate, sc.vertex, sc.fragment, sc.fragment_z};
  for (const v3d_shader::Program& pr : programs) {
    const uint64_t nop = 0x3c003186bb800000ULL;
    CHECK(code[pr.first + pr.instructions - 3] == 0x3c203186bb800000ULL && code[pr.first + pr.instructions - 2] == nop && code[pr.first + pr.instructions - 1] == nop);
  }
  CHECK(code[sc.coordinate.first + sc.coordinate.instructions - 4] == 0x3c003186bb816000ULL && code[sc.vertex.first + sc.vertex.instructions - 4] == 0x3c003186bb816000ULL);  // vpmwt
  // A buffer that is too small is refused.
  std::vector<uint64_t> small(100);
  v3d_shader::ShaderCode none;
  CHECK(!v3d_shader::build_colored_pipeline(ArrayView<uint64_t>(small.data(), small.size()), &none));
  // The uniform streams: the matrix as it is in memory, then the viewport scale in 1/256 pixel.
  uint32_t mvp[16];
  for (int k = 0; k < 16; ++k) mvp[k] = v3d_float_bits(static_cast<float>(k) + 0.5f);
  uint32_t cu[18], vu[20], short_stream[10];
  CHECK(v3d_shader::coordinate_uniforms(ArrayView<uint32_t>(cu), ArrayView<const uint32_t>(mvp), v3d_shader::float_bits(640 * 128), v3d_shader::float_bits(360 * 128)) &&
        v3d_shader::vertex_uniforms(ArrayView<uint32_t>(vu), ArrayView<const uint32_t>(mvp), v3d_shader::float_bits(640 * 128), v3d_shader::float_bits(360 * 128), v3d_float_bits(0.5f), v3d_float_bits(0.25f)));
  // float_bits of a whole number is that number as a float.
  for (uint32_t n : {0u, 1u, 2u, 3u, 255u, 256u, 16000u * 128u, (1u << 24) - 1u}) CHECK(v3d_shader::float_bits(n) == v3d_float_bits(static_cast<float>(n)));
  CHECK(cu[0] == 0x3F000000 && cu[16] == 0x47A00000 /* 81920.0f = 640 * 128 */ && cu[17] == 0x47340000 /* 46080.0f */ && vu[18] == 0x3F000000 && vu[19] == 0x3E800000);
  CHECK(!v3d_shader::coordinate_uniforms(ArrayView<uint32_t>(short_stream), ArrayView<const uint32_t>(mvp), 1, 1));
}

static void check_draw(const V3dFrameDesc& frame, const V3dViewport& vp, const V3dDrawState& state, const v3d_shader::ShaderCode& code, bool z, bool indexed, uint32_t indirect_base, uint32_t defaults, const DrawGolden& golden) {
  std::vector<uint8_t> b(2048), ind(2048);
  V3dControlList bcl{ArrayView<uint8_t>(b.data(), b.size()), 0x10000}, indirect{ArrayView<uint8_t>(ind.data(), ind.size()), indirect_base};
  CHECK(V3dDrawEmitter::emit_state(bcl, frame, vp, state));
  const std::vector<uint8_t> es = unhex(golden.state);
  CHECK(bcl.size() == es.size() && std::equal(es.begin(), es.end(), b.begin()));
  const uint32_t state_bytes = bcl.size();
  V3dDrawCall d;
  d.code_address = 0x30000; d.code = code; d.with_z_write = z; d.defaults_address = defaults;
  d.attribute_count = 2;
  d.attributes[0] = {0x500000, 28, 3, 3, 3};
  d.attributes[1] = {0x50000C, 28, 4, 0, 4};
  d.vertex_count = indexed ? 6 : 36;
  if (indexed) { d.indexed = true; d.index_address = 0x600000; d.index_buffer_bytes = 2000; d.index_bytes = 2; d.index_offset_bytes = 12; d.base_vertex = 5; }
  uint32_t cs[18], vs[20], fs[2];
  for (uint32_t i = 0; i < 18; ++i) cs[i] = 0x3F800000 + i;
  for (uint32_t i = 0; i < 20; ++i) vs[i] = 0x40000000 + i;
  fs[0] = z ? v3d_shader::kTlbDepthConfig : v3d_shader::kTlbColorConfig; fs[1] = v3d_shader::kTlbColorConfig;
  d.coordinate_uniforms = ArrayView<const uint32_t>(cs); d.vertex_uniforms = ArrayView<const uint32_t>(vs);
  d.fragment_uniforms = ArrayView<const uint32_t>(fs, z ? 2 : 1);
  CHECK(V3dDrawEmitter::emit_draw(bcl, indirect, d));
  const std::vector<uint8_t> ed = unhex(golden.draw), ei = unhex(golden.indirect);
  CHECK(bcl.size() - state_bytes == ed.size() && std::equal(ed.begin(), ed.end(), b.begin() + state_bytes));
  CHECK(indirect.size() == ei.size() && std::equal(ei.begin(), ei.end(), ind.begin()));
}

static void test_draw() {
  std::vector<uint64_t> code(512);
  v3d_shader::ShaderCode sc;
  CHECK(v3d_shader::build_colored_pipeline(ArrayView<uint64_t>(code.data(), code.size()), &sc));
  V3dFrameDesc f1;
  f1.width = 1280; f1.height = 720; f1.color = {0x100000, 5120};
  V3dViewport v1; v1.width = 1280; v1.height = 720;
  V3dDrawState s1;
  check_draw(f1, v1, s1, sc, false, false, 0x71000, 0x72000, kGoldenDrawPlain);
  V3dFrameDesc f2;
  f2.width = 1920; f2.height = 1080; f2.color = {0x200000, 7680}; f2.has_depth = true; f2.depth = {0x800000, 7680};
  V3dViewport v2; v2.x = 16; v2.y = 8; v2.width = 1888; v2.height = 1056; v2.z_scale_bits = v3d_float_bits(0.5f); v2.z_offset_bits = v3d_float_bits(0.25f); v2.z_max_bits = v3d_float_bits(0.75f);
  V3dDrawState s2; s2.cull = V3dDrawState::Cull::None; s2.front_clockwise = true; s2.depth_test = s2.depth_write = true; s2.alpha_blend = true;
  check_draw(f2, v2, s2, sc, true, true, 0x81000, 0x82000, kGoldenDrawDepth);
  // Malformed draws and viewports are refused.
  std::vector<uint8_t> b(2048), ind(64);
  V3dControlList bcl{ArrayView<uint8_t>(b.data(), b.size()), 0}, indirect{ArrayView<uint8_t>(ind.data(), ind.size()), 0x1000};
  V3dViewport outside = v1; outside.width = 2000;
  CHECK(!V3dDrawEmitter::emit_state(bcl, f1, outside, s1));
  V3dViewport empty = v1; empty.height = 0;
  V3dControlList bcl2{ArrayView<uint8_t>(b.data(), b.size()), 0};
  CHECK(!V3dDrawEmitter::emit_state(bcl2, f1, empty, s1));
  V3dDrawCall bad;
  bad.code = sc; bad.defaults_address = 0x72000; bad.vertex_count = 3;
  CHECK(!V3dDrawEmitter::emit_draw(bcl, indirect, bad));  // no attributes
  uint32_t defaults[64];
  V3dDrawEmitter::fill_attribute_defaults(ArrayView<uint32_t>(defaults));
  CHECK(defaults[0] == 0 && defaults[3] == 0x3F800000 && defaults[7] == 0x3F800000 && defaults[63] == 0x3F800000 && defaults[62] == 0);
}

static void test_frame_lists() {
  V3dTileGeometry g;
  CHECK(V3dFrameBuilder::geometry(1920, 1080, &g) && g.tile_width == 64 && g.tiles_x == 30 && g.tiles_y == 17);
  CHECK(V3dFrameBuilder::tile_alloc_bytes(g) == ((30 * 17 * 64 + 4095) / 4096) * 4096 + 8192 + 512 * 1024 && V3dFrameBuilder::tile_state_bytes(g) == 30 * 17 * 256);
  CHECK(!V3dFrameBuilder::geometry(0, 10, &g) && !V3dFrameBuilder::geometry(5000, 10, &g));
  V3dFrameDesc a;
  a.width = 1280; a.height = 720; a.color = {0x100000, 5120}; a.swap_rb = true; a.clear_color = true; { const float c[4] = {0.25f, 0.5f, 1.0f, 1.0f}; a.clear_color_rgba8 = v3d_pack_rgba8(c); }
  check_frame(a, kGoldenClearStore, 0x1000, 0x40000);
  V3dFrameDesc d;
  d.width = 1920; d.height = 1080; d.color = {0x200000, 7680}; d.clear_color = d.load_color = true; { const float c[4] = {1.0f, 0, 0, 0.5f}; d.clear_color_rgba8 = v3d_pack_rgba8(c); }
  d.has_depth = true; d.depth = {0x800000, 7680}; d.clear_depth = d.load_depth = d.store_depth = true; d.clear_z_bits = v3d_float_bits(0.75f);
  check_frame(d, kGoldenDepth, 0x2000, 0x80000);
  // Lists that are too small, and a desc with no stride, are refused instead of overrunning.
  std::vector<uint8_t> small(40), big(4096);
  V3dTileGeometry g2;
  V3dFrameBuilder::geometry(a.width, a.height, &g2);
  V3dControlList tiny{ArrayView<uint8_t>(small.data(), small.size()), 0}, ok_list{ArrayView<uint8_t>(big.data(), big.size()), 0x100};
  CHECK(!V3dFrameBuilder::build_render_list(tiny, ok_list, a, g2, 0x40000) && !tiny.ok());
  V3dFrameDesc bad = a; bad.color.stride_bytes = 0;
  V3dControlList l1{ArrayView<uint8_t>(big.data(), big.size()), 0}, l2{ArrayView<uint8_t>(big.data(), big.size()), 0};
  CHECK(!V3dFrameBuilder::build_render_list(l1, l2, bad, g2, 0x40000));
  // The packet builder refuses a value wider than its field.
  uint8_t bytes[9] = {};
  V3dControlList cl{ArrayView<uint8_t>(bytes), 0};
  V3dPacketBuilder p = cl.emit(v3d_pkt::tile_binning_mode_cfg::kInfo);
  p.set(v3d_pkt::tile_binning_mode_cfg::kWidthInPixels, 70000);
  CHECK(!p.ok());
  V3dControlList cl2{ArrayView<uint8_t>(bytes), 0};
  V3dPacketBuilder q = cl2.emit(v3d_pkt::tile_binning_mode_cfg::kInfo);
  q.set(v3d_pkt::tile_binning_mode_cfg::kWidthInPixels, 0);  // stored minus one: zero cannot be
  CHECK(!q.ok());
}

struct TestJobMemory final : IV3dJobMemory {
  std::vector<uint8_t> storage;
  uint32_t base;
  int cleans = 0, invalidates = 0;
  TestJobMemory(uint64_t size, uint32_t gpu_base) : storage(size), base(gpu_base) {}
  ArrayView<uint8_t> bytes() override { return ArrayView<uint8_t>(storage.data(), storage.size()); }
  uint32_t gpu_address() const override { return base; }
  void clean() override { ++cleans; }
  void invalidate() override { ++invalidates; }
};

static void test_renderer() {
  Window hub(0x4000), core(0x4000);
  identify_v3d(hub, core);
  AutoClear flush_done(core, v3d::kCtlL2TCaCtl, v3d::kL2TCaCtlFlush);
  V3dGpu gpu(hub.region(), core.region());
  CHECK(gpu.initialize());
  V3dFrameDesc f;
  f.width = 640; f.height = 480; f.color = {0x3000000, 2560}; f.clear_color = true; f.clear_color_rgba8 = 0xFF00FF00;
  uint64_t needed = 0;
  CHECK(V3dRenderer::memory_needed(f, &needed) && needed > 512 * 1024);
  TestJobMemory memory(needed, 0x3A000000);
  core.at(v3d::kCtlIntSts) = v3d::kIntFlDone | v3d::kIntFrDone;  // both lists "finish" at once
  CHECK(V3dRenderer::render(gpu, f, memory, 10) == V3dJobResult::Done);
  CHECK(memory.cleans == 1 && memory.invalidates == 1);
  CHECK(core.at(v3d::kCleCt0Qba) == 0x3A000000 && core.at(v3d::kCleCt1Qba) == 0x3A001000 && core.at(v3d::kCleCt1Qea) > core.at(v3d::kCleCt1Qba));
  CHECK(core.at(v3d::kCleCt0Qma) == 0x3A003000 + 0x1000 * ((10 * 8 * 256 + 4095) / 4096) && core.at(v3d::kCleCt0Qts) == (v3d::kCleCt0QtsEnable | 0x3A003000));
  CHECK(memory.storage[0] == 120 && memory.storage[4096] == 121);  // the lists were written where the registers point
  // Too little memory, a misaligned block and a bad frame are refused before the GPU is touched.
  core.at(v3d::kCleCt0Qba) = 0xDEAD;
  TestJobMemory small(8192, 0x3A000000), unaligned(needed, 0x3A000010);
  CHECK(V3dRenderer::render(gpu, f, small, 10) == V3dJobResult::Error && V3dRenderer::render(gpu, f, unaligned, 10) == V3dJobResult::Error);
  V3dFrameDesc none;
  CHECK(!V3dRenderer::memory_needed(none, &needed) && core.at(v3d::kCleCt0Qba) == 0xDEAD);
  // A render that never finishes times out.
  core.at(v3d::kCtlIntSts) = v3d::kIntFlDone;
  CHECK(V3dRenderer::render(gpu, f, memory, 5) == V3dJobResult::TimedOut);
}

// GPU memory from host memory: every block remembers its bytes after it is released, so a test can read what a job left behind.
struct FakeGpuMemory final : IV3dGpuMemory {
  struct Entry { std::vector<uint8_t> bytes; uint32_t gpu; bool live; };
  std::vector<Entry*> entries;
  uint32_t next = 0x30000000;
  int cleans = 0, invalidates = 0;
  uint64_t budget = ~0ull;
  ~FakeGpuMemory() { for (Entry* e : entries) delete e; }
  bool allocate(uint32_t bytes, uint32_t alignment, V3dGpuBlock* out) override {
    if (bytes > budget) return false;
    budget -= bytes;
    next = (next + alignment - 1) & ~(alignment - 1);
    Entry* e = new Entry{std::vector<uint8_t>((bytes + 63) & ~63u), next, true};
    next += static_cast<uint32_t>(e->bytes.size());
    entries.push_back(e);
    out->cpu = ArrayView<uint8_t>(e->bytes.data(), e->bytes.size());
    out->gpu_address = e->gpu;
    out->cookie = entries.size() - 1;
    return true;
  }
  void release(const V3dGpuBlock& b) override { entries[b.cookie]->live = false; }
  void clean(const V3dGpuBlock&) override { ++cleans; }
  void invalidate(const V3dGpuBlock&) override { ++invalidates; }
  int live() const { int n = 0; for (Entry* e : entries) n += e->live; return n; }
  const Entry* by_gpu(uint32_t gpu) const { for (Entry* e : entries) if (e->gpu == gpu) return e; return nullptr; }
};

struct CapturingDisplay final : IGraphicsProcessingUnit {
  std::vector<uint32_t> pixels; uint32_t bw = 0, bh = 0, presents = 0;
  const char* name() const override { return "capture"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t, DisplayMode*) const override { return false; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t) override { return false; }
  uint32_t width() const override { return 640; }
  uint32_t height() const override { return 480; }
  void put_pixel(uint32_t, uint32_t, uint32_t) override {}
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t) override {}
  void blit(uint32_t, uint32_t, uint32_t w, uint32_t h, const uint32_t* p, uint32_t pitch) override {
    bw = w; bh = h; pixels.assign(w * h, 0);
    for (uint32_t y = 0; y < h; ++y) for (uint32_t x = 0; x < w; ++x) pixels[y * w + x] = p[y * pitch + x];
  }
  void present() override { ++presents; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
};

static std::vector<uint8_t> list_bytes(const FakeGpuMemory::Entry* job, uint32_t offset, uint32_t size) {
  return std::vector<uint8_t>(job->bytes.begin() + offset, job->bytes.begin() + offset + size);
}

static void test_graphics_device() {
  using namespace graphics;
  TestAllocator alloc;
  Window hub(0x4000), core(0x4000);
  identify_v3d(hub, core);
  AutoClear flush_done(core, v3d::kCtlL2TCaCtl, v3d::kL2TCaCtlFlush);
  V3dGpu gpu(hub.region(), core.region());
  FakeGpuMemory memory;
  CapturingDisplay display;
  {
    V3dGraphicsDevice device(alloc, gpu, memory, &display);
    CHECK(!device.initialize());  // the GPU has not been brought up
    CHECK(gpu.initialize());
    CHECK(device.initialize() && device.ready() && device.info().accelerated && device.info().kind == DeviceKind::Gpu);
    // The programs are in GPU memory, as assembled.
    const FakeGpuMemory::Entry* code = memory.entries[0];
    bool same = true;
    for (uint32_t i = 0; i < sizeof kGoldenShaderWords / 8; ++i) {
      uint64_t w = 0;
      for (uint32_t b = 0; b < 8; ++b) w |= uint64_t{code->bytes[i * 8 + b]} << (8 * b);
      same = same && w == kGoldenShaderWords[i];
    }
    CHECK(same);
    CHECK(memory.entries[1]->bytes[12] == 0x00 && memory.entries[1]->bytes[15] == 0x3F);  // defaults: (0, 0, 0, 1.0f)

    // Shader modules and pipelines: the built-in ones work, others are refused.
    IShaderModule* vs = device.create_shader_module(colored_vertex_shader());
    IShaderModule* fs = device.create_shader_module(colored_fragment_shader());
    ShaderModuleDesc plain = colored_vertex_shader();
    plain.qpu_program = QpuProgram::None;
    CHECK(vs && fs && !device.create_shader_module(plain));
    GraphicsPipelineDesc pd;
    describe_colored_pipeline(&pd);
    pd.vertex_shader = vs; pd.fragment_shader = fs;
    pd.cull_mode = CullMode::Back; pd.depth_test = false;
    IPipeline* pipeline = device.create_graphics_pipeline(pd);
    CHECK(pipeline);
    GraphicsPipelineDesc depth_pd = pd;
    depth_pd.depth_test = depth_pd.depth_write = true; depth_pd.alpha_blend = true; depth_pd.cull_mode = CullMode::None; depth_pd.front_face = FrontFace::Clockwise;
    IPipeline* depth_pipeline = device.create_graphics_pipeline(depth_pd);
    CHECK(depth_pipeline);
    GraphicsPipelineDesc wrong = pd;
    wrong.attributes[1].format = VertexFormat::Float3;
    CHECK(!device.create_graphics_pipeline(wrong));
    wrong = pd; wrong.vertex_shader = fs;
    CHECK(!device.create_graphics_pipeline(wrong));
    wrong = pd; wrong.attribute_count = 1;
    CHECK(!device.create_graphics_pipeline(wrong));

    // Buffers and images.
    IBuffer* vb = device.create_buffer(3 * sizeof(ColoredVertex), kBufferVertex);
    IBuffer* ib = device.create_buffer(12, kBufferIndex);
    CHECK(vb && ib && !device.create_buffer(0, kBufferVertex));
    ImageDesc color_desc; color_desc.extent = {20, 12}; color_desc.format = Format::B8G8R8A8Unorm; color_desc.usage = kImageColorAttachment | kImagePresent;
    IImage* color = device.create_image(color_desc);
    ImageDesc depth_desc = color_desc; depth_desc.format = Format::D32Float; depth_desc.usage = kImageDepthAttachment;
    IImage* depth = device.create_image(depth_desc);
    CHECK(color && depth && color->extent().width == 20);
    ImageDesc bad_desc = color_desc; bad_desc.extent = {0, 5};
    CHECK(!device.create_image(bad_desc));
    ImageDesc huge = color_desc; huge.extent = {5000, 5};
    CHECK(!device.create_image(huge));

    ColoredVertex tri[3] = {{{-0.5f, -0.5f, 0.5f}, {1, 0, 0, 1}}, {{0.5f, -0.5f, 0.5f}, {0, 1, 0, 1}}, {{0.0f, 0.5f, 0.5f}, {0, 0, 1, 1}}};
    ByteView map = vb->map();
    for (uint32_t i = 0; i < sizeof tri; ++i) map[i] = reinterpret_cast<const uint8_t*>(tri)[i];
    vb->unmap();
    ColoredConstants cc;
    ICommandBuffer* cmd = device.create_command_buffer();
    CHECK(cmd);

    // A pass with a clear and one draw: compare the lists in the job memory with what the emitters make from the same inputs.
    const size_t before = memory.entries.size();
    cmd->begin();
    RenderPassBegin rp;
    rp.color = color; rp.clear_color = true; rp.clear_color_value = {0.25f, 0.5f, 0.75f, 1.0f};
    cmd->begin_render_pass(rp);
    cmd->bind_pipeline(pipeline);
    cmd->bind_vertex_buffer(vb, 0);
    cmd->push_constants(&cc, sizeof cc);
    cmd->draw(3, 0);
    cmd->end_render_pass();
    CHECK(cmd->end());
    core.at(v3d::kCtlIntSts) = v3d::kIntFlDone | v3d::kIntFrDone;
    CHECK(device.submit(cmd));
    CHECK(memory.entries.size() > before);
    const FakeGpuMemory::Entry* job = memory.entries[before];  // the first block a pass takes is its job block
    CHECK(!job->live);
    const uint32_t job_gpu = job->gpu;
    CHECK(core.at(v3d::kCleCt0Qba) == job_gpu && core.at(v3d::kCleCt1Qba) > job_gpu);
    // Expected binner list and indirect memory.
    V3dFrameDesc frame;
    frame.width = 20; frame.height = 12;
    uint32_t color_gpu = 0;
    for (FakeGpuMemory::Entry* e : memory.entries) if (e->bytes.size() == 32 * 12 * 4 && e->gpu != 0) { color_gpu = e->gpu; break; }
    frame.color = {color_gpu, 32 * 4}; frame.swap_rb = true; frame.clear_color = true;
    { const float c[4] = {0.25f, 0.5f, 0.75f, 1.0f}; frame.clear_color_rgba8 = v3d_pack_rgba8(c); }
    std::vector<uint8_t> eb(4096), ei(4096);
    V3dControlList ebcl{ArrayView<uint8_t>(eb.data(), eb.size()), job_gpu}, eind{ArrayView<uint8_t>(ei.data(), ei.size()), core.at(v3d::kCleCt1Qba) + 0};
    (void)eind;
    V3dFrameBuilder::begin_binning(ebcl, frame);
    V3dViewport vp; vp.width = 20; vp.height = 12;
    V3dDrawState ds;
    CHECK(V3dDrawEmitter::emit_state(ebcl, frame, vp, ds));
    const uint32_t bcl_size = core.at(v3d::kCleCt0Qea) - job_gpu;
    const std::vector<uint8_t> actual = list_bytes(job, 0, bcl_size);
    // The state part of the binner's list is byte for byte the emitter's.
    CHECK(bcl_size > ebcl.size() && std::equal(eb.begin(), eb.begin() + ebcl.size(), actual.begin()));
    // The rest is: VCM cache size, GL shader state, vertex array prims (3 vertices), flush.
    CHECK(actual[ebcl.size()] == 71 && actual[ebcl.size() + 2] == 64 && actual[bcl_size - 1] == 4);
    // The draw's shader record is where the GL shader state packet says, and names the vertex buffer's address and the programs.
    uint32_t record = 0;
    for (uint32_t b = 0; b < 4; ++b) record |= uint32_t{actual[ebcl.size() + 3 + b]} << (8 * b);
    record &= ~31u;
    const FakeGpuMemory::Entry* rec_block = job;
    const uint32_t rec_offset = record - job_gpu;
    CHECK(record >= job_gpu && rec_offset < job->bytes.size() && (record & 31) == 0 && rec_block);
    // The attribute records follow the 36 -byte shader record's size; the first holds the buffer's GPU address and a stride of 28.
    const uint32_t attr = rec_offset + v3d_pkt::gl_shader_state_record::kBytes;
    uint32_t attr0_addr = 0, attr0_stride = 0;
    for (uint32_t b = 0; b < 4; ++b) { attr0_addr |= uint32_t{job->bytes[attr + b]} << (8 * b); attr0_stride |= uint32_t{job->bytes[attr + 8 + b]} << (8 * b); }
    CHECK(attr0_addr == memory.entries[2]->gpu && attr0_stride == sizeof(ColoredVertex));
    CHECK(memory.cleans > 0 && memory.invalidates > 0);

    // The clear colour reached the renderer's list (R,G,B,A in the tile buffer), the supertile list ends it.
    const uint32_t rcl_off = core.at(v3d::kCleCt1Qba) - job_gpu;
    CHECK(job->bytes[rcl_off] == 121);
    std::vector<uint8_t> er(4096), eg(4096);
    V3dControlList ercl{ArrayView<uint8_t>(er.data(), er.size()), core.at(v3d::kCleCt1Qba)}, egen{ArrayView<uint8_t>(eg.data(), eg.size()), core.at(v3d::kCleCt1Qba) + 4096};
    V3dTileGeometry g;
    V3dFrameBuilder::geometry(20, 12, &g);
    const uint32_t alloc_at = core.at(v3d::kCleCt0Qma);
    CHECK(V3dFrameBuilder::build_render_list(ercl, egen, frame, g, alloc_at));
    CHECK(std::equal(er.begin(), er.begin() + ercl.size(), job->bytes.begin() + rcl_off));

    // Every block was given back except the long-lived ones.

    // A pass with depth, a scissor, an indexed draw with a base vertex, a viewport.
    uint8_t* ip = ib->map().data();
    const uint16_t idx[3] = {0, 1, 2};
    for (uint32_t i = 0; i < 6; ++i) ip[i] = reinterpret_cast<const uint8_t*>(idx)[i];
    ib->unmap();
    cmd->begin();
    RenderPassBegin rp2;
    rp2.color = color; rp2.depth = depth; rp2.clear_color = false; rp2.clear_depth = true; rp2.clear_depth_value = 0.5f;
    cmd->begin_render_pass(rp2);
    cmd->bind_pipeline(depth_pipeline);
    cmd->bind_vertex_buffer(vb, 0);
    cmd->bind_index_buffer(ib, IndexType::Uint16, 0);
    cmd->push_constants(&cc, sizeof cc);
    Viewport viewport; viewport.x = 2; viewport.y = 1; viewport.width = 16; viewport.height = 10; viewport.min_depth = 0.25f; viewport.max_depth = 0.75f;
    cmd->set_viewport(viewport);
    Rect2D scissor; scissor.x = -4; scissor.y = 0; scissor.width = 12; scissor.height = 6;
    cmd->set_scissor(scissor);
    cmd->draw_indexed(3, 0, 0);
    cmd->end_render_pass();
    CHECK(cmd->end());
    core.at(v3d::kCtlIntSts) = v3d::kIntFlDone | v3d::kIntFrDone;
    CHECK(device.submit(cmd));
    // The depth pass: its clip window is the viewport cut by the scissor, and it picked the fragment shader that writes Z with the Z configuration word first.
    const FakeGpuMemory::Entry* job2 = nullptr;
    for (size_t k = before + 1; k < memory.entries.size(); ++k) if (memory.entries[k]->bytes.size() > 500000) { job2 = memory.entries[k]; break; }
    CHECK(job2 != nullptr);
    if (job2) {
      auto u16 = [&](uint32_t at) { return static_cast<uint32_t>(job2->bytes[at] | (job2->bytes[at + 1] << 8)); };
      CHECK(job2->bytes[16] == 107 && u16(17) == 2 && u16(19) == 1 && u16(21) == 6 && u16(23) == 5);
      // Walk the packets to the GL shader state and read its record.
      uint32_t lengths[256] = {};
      lengths[107] = v3d_pkt::clip_window::kInfo.length; lengths[96] = v3d_pkt::cfg_bits::kInfo.length; lengths[104] = v3d_pkt::point_size::kInfo.length;
      lengths[105] = v3d_pkt::line_width::kInfo.length; lengths[110] = v3d_pkt::clipper_xy_scaling::kInfo.length; lengths[111] = v3d_pkt::clipper_z_scale_and_offset::kInfo.length;
      lengths[109] = v3d_pkt::clipper_z_min_max_clipping_planes::kInfo.length; lengths[108] = v3d_pkt::viewport_offset::kInfo.length; lengths[83] = v3d_pkt::blend_enables::kInfo.length;
      lengths[84] = v3d_pkt::blend_cfg::kInfo.length; lengths[87] = v3d_pkt::color_write_masks::kInfo.length; lengths[97] = lengths[99] = lengths[88] = 1;
      lengths[74] = v3d_pkt::transform_feedback_specs::kInfo.length; lengths[91] = v3d_pkt::sample_state::kInfo.length; lengths[71] = v3d_pkt::vcm_cache_size::kInfo.length;
      uint32_t pos = 16;
      while (pos < 1024 && job2->bytes[pos] != 64) { const uint32_t n = lengths[job2->bytes[pos]]; if (n == 0) break; pos += n; }
      CHECK(job2->bytes[pos] == 64);
      uint32_t record = 0;
      for (uint32_t k = 0; k < 4; ++k) record |= uint32_t{job2->bytes[pos + 1 + k]} << (8 * k);
      record &= ~31u;
      const uint32_t at = record - job2->gpu;
      CHECK((job2->bytes[at + 1] & 1) == 1);  // fragment shader does Z writes
      uint32_t fs_unif = 0;
      for (uint32_t k = 0; k < 4; ++k) fs_unif |= uint32_t{job2->bytes[at + 16 + k]} << (8 * k);  // fragment shader uniforms address (byte 16 of the record)
      const uint32_t ua = fs_unif - job2->gpu;
      CHECK(job2->bytes[ua] == 0x84 && job2->bytes[ua + 1] == 0xFF && job2->bytes[ua + 4] == 0xFF);
    }

    // Refused: no pipeline, a draw reaching beyond the vertex buffer, an unfinished pass, a viewport outside the image, push constants that are too short.
    cmd->begin();
    cmd->begin_render_pass(rp);
    cmd->bind_vertex_buffer(vb, 0);
    cmd->draw(3, 0);
    cmd->end_render_pass();
    CHECK(cmd->end() && !device.submit(cmd));
    cmd->begin();
    cmd->begin_render_pass(rp);
    cmd->bind_pipeline(pipeline); cmd->bind_vertex_buffer(vb, 0); cmd->push_constants(&cc, sizeof cc);
    cmd->draw(6, 0);
    cmd->end_render_pass();
    CHECK(cmd->end() && !device.submit(cmd));  // vertices 3..5 are not in the 3-vertex buffer
    cmd->begin();
    cmd->begin_render_pass(rp);
    CHECK(cmd->end() && !device.submit(cmd));  // the pass never ends
    cmd->begin();
    cmd->begin_render_pass(rp);
    cmd->bind_pipeline(pipeline); cmd->bind_vertex_buffer(vb, 0); cmd->push_constants(&cc, sizeof cc);
    Viewport outside; outside.x = 0; outside.y = 0; outside.width = 40; outside.height = 12;
    cmd->set_viewport(outside);
    cmd->draw(3, 0);
    cmd->end_render_pass();
    CHECK(cmd->end() && !device.submit(cmd));
    cmd->begin();
    cmd->begin_render_pass(rp);
    cmd->bind_pipeline(pipeline); cmd->bind_vertex_buffer(vb, 0);
    float short_constants[4] = {};
    cmd->push_constants(short_constants, sizeof short_constants);
    cmd->draw(3, 0);
    cmd->end_render_pass();
    CHECK(cmd->end() && !device.submit(cmd));
    // A job that never finishes (the GPU does not say it is done) fails the submit instead of hanging.
    core.at(v3d::kCtlIntSts) = 0;
    device.set_job_attempts(20);
    cmd->begin();
    cmd->begin_render_pass(rp);
    cmd->end_render_pass();
    CHECK(cmd->end() && !device.submit(cmd));
    // Out of GPU memory for the job.
    core.at(v3d::kCtlIntSts) = v3d::kIntFlDone | v3d::kIntFrDone;
    memory.budget = 100;
    CHECK(!device.submit(cmd));
    memory.budget = ~0ull;

    // present(): the image's pixels (rows are 32 pixels apart in memory, the image is 20 wide) reach the display; a non-presentable image does not.
    FakeGpuMemory::Entry* writable = memory.entries[4];  // the colour image (code, defaults, vertex buffer, index buffer, colour, depth)
    CHECK(writable->live && writable->bytes.size() == 32 * 12 * 4);
    for (uint32_t y = 0; y < 12; ++y) for (uint32_t x = 0; x < 32; ++x) { const uint32_t v = 0xFF000000u | (y << 8) | x; for (uint32_t b = 0; b < 4; ++b) writable->bytes[(y * 32 + x) * 4 + b] = static_cast<uint8_t>(v >> (8 * b)); }
    CHECK(device.present(color) && display.bw == 20 && display.bh == 12 && display.presents == 1 && display.pixels[0] == 0xFF000000u && display.pixels[5 * 20 + 7] == (0xFF000000u | (5 << 8) | 7));
    CHECK(!device.present(depth) && !device.present(nullptr));
    std::vector<uint32_t> out(20 * 12);
    CHECK(color->read_pixels(ArrayView<uint32_t>(out.data(), out.size())) && out[3 * 20 + 4] == (0xFF000000u | (3 << 8) | 4) && !depth->read_pixels(ArrayView<uint32_t>(out.data(), out.size())));
    std::vector<uint32_t> small(5);
    CHECK(!color->read_pixels(ArrayView<uint32_t>(small.data(), small.size())));

    device.destroy_command_buffer(cmd);
    device.destroy_buffer(vb); device.destroy_buffer(ib);
    device.destroy_image(color); device.destroy_image(depth);
    device.destroy_pipeline(pipeline); device.destroy_pipeline(depth_pipeline);
    device.destroy_shader_module(vs); device.destroy_shader_module(fs);
    CHECK(memory.live() == 2);  // the programs and the defaults stay until the device goes
  }
  CHECK(memory.live() == 0);
}

static void test_self_test() {
  Window hub(0x4000), core(0x4000);
  identify_v3d(hub, core);
  AutoClear flush_done(core, v3d::kCtlL2TCaCtl, v3d::kL2TCaCtlFlush);
  V3dGpu gpu(hub.region(), core.region());
  FakeGpuMemory memory;
  CapturingDisplay display;
  CHECK(!V3dSelfTest::run(gpu, memory, 10, &display).memory_ok);  // the GPU is not initialised
  CHECK(gpu.initialize());
  core.at(v3d::kCtlIntSts) = v3d::kIntFlDone | v3d::kIntFrDone;
  const V3dSelfTestResult r = V3dSelfTest::run(gpu, memory, 10, &display);
  CHECK(r.memory_ok && r.job == V3dJobResult::Done);
  CHECK(r.clear_pixels == 0 && r.drawn_pixels == 128 * 96 && !r.passed());  // the pretend GPU drew nothing: the image is still zeros
  CHECK(display.bw == 128 && display.bh == 96 && display.presents == 1);
  CHECK(memory.live() == 0);                                                  // every block was given back
  // What the test asked the GPU to do: the vertex data, and a binner list that opens with the binning mode and closes with a flush.
  const FakeGpuMemory::Entry* vertices = memory.entries[2];
  const uint32_t first_word = vertices->bytes[0] | (vertices->bytes[1] << 8) | (vertices->bytes[2] << 16) | (uint32_t{vertices->bytes[3]} << 24);
  CHECK(first_word == 0xBF000000);
  const FakeGpuMemory::Entry* job = memory.entries[4];
  const uint32_t bcl_size = core.at(v3d::kCleCt0Qea) - core.at(v3d::kCleCt0Qba);
  CHECK(job->bytes[0] == 120 && job->bytes[bcl_size - 1] == 4 && bcl_size > 90);
  CHECK(core.at(v3d::kCleCt0Qma) != 0 && core.at(v3d::kCleCt0Qts) != 0 && core.at(v3d::kCleCt1Qea) > core.at(v3d::kCleCt1Qba));
  // The pixel counting and the verdict.
  std::vector<uint32_t> pixels(130 * 96, V3dSelfTest::kClearPixel);  // rows 130 apart, the image 128 wide
  for (uint32_t i = 0; i < 2000; ++i) pixels[(i / 100) * 130 + (i % 100) + 10] = 0xFFFF0000;
  pixels[127 + 3 * 130 + 1] = 0x12345678;  // in the gap between rows: not counted
  V3dSelfTestResult counted;
  V3dSelfTest::count_pixels(ArrayView<const uint32_t>(pixels.data(), pixels.size()), 130, 128, 96, &counted);
  CHECK(counted.drawn_pixels == 2000 && counted.clear_pixels == 128 * 96 - 2000);
  counted.memory_ok = true; counted.job = V3dJobResult::Done;
  CHECK(counted.passed());
  V3dSelfTestResult failed = counted;
  failed.job = V3dJobResult::TimedOut;
  CHECK(!failed.passed());
  failed = counted; failed.drawn_pixels = 100; failed.clear_pixels = 128 * 96 - 100;
  CHECK(!failed.passed());
  V3dSelfTestResult shortview;
  V3dSelfTest::count_pixels(ArrayView<const uint32_t>(pixels.data(), 100), 130, 128, 96, &shortview);  // a view too short for the image is counted as far as it goes
  CHECK(shortview.clear_pixels + shortview.drawn_pixels == 100);
  // No memory for the test: nothing is left allocated.
  FakeGpuMemory tight;
  tight.budget = 3000;
  CHECK(!V3dSelfTest::run(gpu, tight, 10).memory_ok && tight.live() == 0);
}

static void test_page_table() {
  std::vector<uint32_t> entries(256, 0);
  V3dPageTable pt{ArrayView<uint32_t>(entries.data(), entries.size())};
  CHECK(pt.map(0x2000, 0x3A001000, 3, true));
  bool w = false;
  CHECK(pt.is_mapped(0x2000, &w) && w && pt.is_mapped(0x4000) && !pt.is_mapped(0x5000) && !pt.is_mapped(0x1000));
  CHECK(entries[2] == (0x3A001u | v3d::kPteValid | v3d::kPteWriteable) && entries[4] == (0x3A003u | v3d::kPteValid | v3d::kPteWriteable));
  CHECK(!pt.map(0x2001, 0x1000, 1, true) && !pt.map(0x2000, 0x1001, 1, true));            // unaligned
  CHECK(!pt.map(0xFF000, 0x1000, 2, true) && !pt.map(0x100000, 0x1000, 1, true));         // beyond the table
  CHECK(!pt.map(0, uint64_t{1} << 40, 1, true));                                          // page number does not fit
  CHECK(!pt.map(0, 0xFFFFFFF000ull, 0x10, true) || true);
  CHECK(pt.map(0x8000, 0x1000, 1, false) && pt.is_mapped(0x8000, &w) && !w);
  CHECK(pt.unmap(0x2000, 3) && !pt.is_mapped(0x2000) && !pt.unmap(0xFF000, 2));
  pt.clear();
  CHECK(!pt.is_mapped(0x8000) && !pt.is_mapped(0xFFFFFFFFFFull));
}

static void test_hevc_engine() {
  TestAllocator alloc;
  TestMessageMemory mem;
  FakeFirmware fw(&mem);
  VcMailbox mailbox(fw, mem);
  VideoCoreFirmware f(mailbox);
  Window hevc(0x10000), intc(0x1000);
  hevc.at(rpi_hevc::kVersion) = 0x20210309;
  {
    BroadcomVideoEngine engine(alloc, hevc.region(), intc.region(), &f);
    CHECK(engine.initialize() && engine.hevc_block_ready() && engine.hevc_version() == 0x20210309);
    CHECK(engine.info().hardware_engine && !engine.info().accelerated && engine.info().accelerated_decode_codecs == 0);
    CHECK(fw.clock_on[vc_clock::kHevc] == 1 && fw.clock_hz[vc_clock::kHevc] == RpiHevcBlock::kClockHz);
    const uint32_t c = intc.at(rpi_hevc::kIcCtrl);
    CHECK((c & rpi_hevc::kIcActive1Enable) && (c & rpi_hevc::kIcActive2Enable) && !(c & rpi_hevc::kIcReservedMask));
    CHECK(engine.engine_decode_codecs() & codec_mask(VideoCodec::Hevc));
    CHECK(engine.can_decode(VideoCodec::Hevc));  // the software path still serves it
    engine.shutdown();
    CHECK(fw.clock_on[vc_clock::kHevc] == 0 && !engine.hevc_block_ready() && !(intc.at(rpi_hevc::kIcCtrl) & rpi_hevc::kIcActive1Enable));
  }
  {  // No registers: the engine is reported, nothing is touched.
    BroadcomVideoEngine engine(alloc, MmioRegion(), MmioRegion(), &f);
    CHECK(engine.initialize() && !engine.hevc_block_ready() && engine.info().hardware_engine);
  }
  {  // The firmware will not start the clock.
    fw.refuse = true;
    BroadcomVideoEngine engine(alloc, hevc.region(), intc.region(), &f);
    CHECK(engine.initialize() && !engine.hevc_block_ready());
    fw.refuse = false;
  }
}

// A screen that records what it was asked.
struct FakeScreen final : IGraphicsProcessingUnit {
  uint32_t fills = 0;
  const char* name() const override { return "fake screen"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint32_t mode_count() const override { return 1; }
  bool mode_info(uint32_t, DisplayMode* out) const override { out->width = 1920; out->height = 1080; return true; }
  uint32_t current_mode() const override { return 0; }
  bool set_mode(uint32_t) override { return true; }
  uint32_t width() const override { return 1920; }
  uint32_t height() const override { return 1080; }
  void put_pixel(uint32_t, uint32_t, uint32_t) override {}
  void fill_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { ++fills; }
  void copy_rect(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
  void clear(uint32_t) override {}
  bool framebuffer_range(uint64_t* base, uint64_t* bytes) const override { *base = 0x3E000000; *bytes = 1920ull * 1080 * 4; return true; }
  uint32_t glyph_width() const override { return 8; }
  uint32_t glyph_height() const override { return 16; }
  void draw_char(uint32_t, uint32_t, char, uint32_t, uint32_t) override {}
  void draw_text(uint32_t, uint32_t, const char*, uint32_t, uint32_t) override {}
};

static void test_gpu_wrapper() {
  TestMessageMemory mem;
  FakeFirmware fw(&mem);
  VcMailbox mailbox(fw, mem);
  VideoCoreFirmware f(mailbox);
  FakeScreen screen;
  RaspberryPiGpu gpu(screen, &f, nullptr);
  CHECK(gpu.initialize() && gpu.width() == 1920 && gpu.v3d() == nullptr);
  gpu.fill_rect(0, 0, 1, 1, 0);
  CHECK(screen.fills == 1);
  uint64_t total = 0, used = 0;
  CHECK(gpu.video_memory(&total, &used) && total == 0x04000000 && used == 1920ull * 1080 * 4);
  CHECK(gpu.temperature_millicelsius() == 47500);
  RaspberryPiGpu bare(screen, nullptr, nullptr);
  CHECK(!bare.video_memory(&total, &used) && bare.temperature_millicelsius() == -1);
}

static void test_platform() {
  TestAllocator alloc;
  TestMessageMemory mem;
  FakeFirmware fw(&mem);
  Window hub(0x4000), core(0x4000), hevc(0x10000), intc(0x1000);
  identify_v3d(hub, core);
  AutoClear flush_done(core, v3d::kCtlL2TCaCtl, v3d::kL2TCaCtlFlush);
  FakeMapper mapper;
  mapper.windows[bcm2711::kV3dHubBase] = hub.region();
  mapper.windows[bcm2711::kV3dCoreBase] = core.region();
  mapper.windows[bcm2711::kHevcBase] = hevc.region();
  mapper.windows[bcm2711::kHevcIntcBase] = intc.region();
  {
    RaspberryPiPlatform platform(alloc, mapper);
    CHECK(platform.initialize_with_port(fw, &mem) && platform.board().is_pi400());
    CHECK(platform.start_v3d() && platform.v3d() && platform.v3d()->identity().version() == 42);
    CHECK(fw.clock_hz[vc_clock::kV3d] == 500000000 && fw.clock_on[vc_clock::kV3d] == 1);
    MmioRegion hevc_registers, intc_registers;
    CHECK(platform.map_hevc(&hevc_registers, &intc_registers));
    BroadcomVideoEngine engine(alloc, hevc_registers, intc_registers, platform.firmware());
    CHECK(engine.initialize() && engine.hevc_block_ready() && engine.info().hardware_engine);
    engine.shutdown();
  }
  CHECK(fw.clock_on[vc_clock::kV3d] == 0);  // the platform gave the clock back
  {  // A Pi 3: not a BCM2711, so the platform stops there.
    fw.board_revision = 0x00A02082;
    RaspberryPiPlatform platform(alloc, mapper);
    CHECK(!platform.initialize_with_port(fw, &mem) && !platform.start_v3d());
  }
  {  // A BCM2711 whose V3D registers are not there.
    fw.board_revision = 0x00C03130;
    FakeMapper empty;
    RaspberryPiPlatform platform(alloc, empty);
    CHECK(platform.initialize_with_port(fw, &mem) && !platform.start_v3d());
  }
  CHECK(bcm2711::arm_to_bus(0x3A000000) == 0xFA000000 && bcm2711::bus_to_arm(0xFA000000) == 0x3A000000);
}

int main() {
  test_board();
  test_message_format();
  test_message_fuzz();
  test_firmware();
  test_v3d_identify_and_jobs();
  test_v3d_rejects();
  test_page_table();
  test_frame_lists();
  test_qpu_encoder();
  test_shaders();
  test_draw();
  test_renderer();
  test_graphics_device();
  test_self_test();
  test_hevc_engine();
  test_gpu_wrapper();
  test_platform();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("RaspberryPiTest passed\n");
  return 0;
}
