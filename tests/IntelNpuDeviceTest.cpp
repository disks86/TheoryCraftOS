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


// Host test for the Intel NPU device object: a pretend NPU with power and reset behaviour, an SMMU that walks the page tables the host built, and a firmware that
// boots from the very boot parameters, image and mappings the host prepared, answers messages, runs job queues and reports jobs done or faults.
#include <stdio.h>
#include <sys/mman.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "FileSystem/Ram/RamFileSystem.hpp"
#include "FileSystem/SystemLayout.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuDevice.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuExecutor.hpp"
#include "Hardware/NeuralProcessingUnit/Intel/IntelNpuFirmwareFile.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  std::map<void*, size_t> dma;
  int dma_budget = -1;
  void* allocate(uint64_t size) override { return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override { const uint64_t al = alignment < 16 ? 16 : alignment; return aligned_alloc(al, (size + al - 1) / al * al); }
  void* allocate_dma(uint64_t size, uint64_t) override {
    if (dma_budget == 0) return nullptr;
    if (dma_budget > 0) --dma_budget;
    const size_t rounded = (size + 4095) / 4096 * 4096;
    void* p = mmap(nullptr, rounded, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED) return nullptr;
    dma[p] = rounded;
    return p;
  }
  void deallocate(void* ptr) override {
    if (!ptr) return;
    auto it = dma.find(ptr);
    if (it != dma.end()) { munmap(ptr, it->second); dma.erase(it); } else free(ptr);
  }
};

struct SimBar final : INpuRegisters {
  std::map<uint64_t, uint32_t> regs;
  std::map<uint64_t, uint64_t> writes64;
  std::set<uint64_t> touched;
  std::vector<std::function<void(uint64_t, uint32_t)>> on_write;
  std::vector<std::function<bool(uint64_t, uint32_t*)>> on_read;
  bool chaos = false;
  uint64_t rng = 0x9E3779B97F4A7C15ull;
  uint32_t read32(uint64_t offset) override {
    touched.insert(offset);
    uint32_t v = 0;
    for (auto& h : on_read) if (h(offset, &v)) return v;
    if (chaos) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return static_cast<uint32_t>(rng); }
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override {
    touched.insert(offset);
    regs[offset] = value;
    for (auto& h : on_write) h(offset, value);
  }
  void write64(uint64_t offset, uint64_t value) override {
    touched.insert(offset);
    writes64[offset] = value;
    regs[offset] = static_cast<uint32_t>(value);
    regs[offset + 4] = static_cast<uint32_t>(value >> 32);
  }
};

static uint32_t ld32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24); }
static uint64_t ld64(const uint8_t* p) { return ld32(p) | (uint64_t{ld32(p + 4)} << 32); }
static void st32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
static void st16(uint8_t* p, uint16_t v) { p[0] = static_cast<uint8_t>(v); p[1] = static_cast<uint8_t>(v >> 8); }
static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) { st32(b.data() + off, v); }
static void put64(std::vector<uint8_t>& b, size_t off, uint64_t v) { st32(b.data() + off, static_cast<uint32_t>(v)); st32(b.data() + off + 4, static_cast<uint32_t>(v >> 32)); }

constexpr uint32_t kSlot = 192;
constexpr uint64_t kCr0 = IntelNpuMmu::kRegCr0;

// The whole pretend NPU.
struct SimNpu {
  IntelNpuPlatform platform;
  SimBar ip, btrs;
  bool mtl;
  // behaviours to provoke
  bool wp_stuck = false, island_stuck = false, cpu_noc_refused = false, mmu_cr0_dead = false, fw_silent = false, fw_no_ready = false, jobs_stall = false;
  uint32_t job_status = 0, fail_type = 0, force_result = 0;
  // state
  bool powered = false, alive = false;
  uint32_t resets = 0, d0i3_entries = 0, cpu_starts = 0, wp_disables = 0, tx_count = 0;
  uint64_t entry_seen = 0;
  bool entry_has_image = false, boot_params_ok = false;
  uint32_t boot_cr0_when_started = 0;
  // smmu
  std::map<uint32_t, uint64_t> cd_w0, cd_ttb;
  std::map<uint64_t, uint64_t> tlb;
  uint32_t evt_prod = 0, evt_cons = 0, icb_extra = 0;
  uint32_t cfgi = 0, tlbi_asid = 0;
  // firmware
  std::deque<uint32_t> fifo;
  uint64_t rx_vpu = 0;
  uint32_t rx_size = 0, rx_next = 0, overruns = 0;
  std::vector<IntelNpuJsmMessage> requests;
  uint32_t resets_engine = 0, resumes = 0, heartbeat = 0x1000;
  struct Db { uint32_t id, ssid; uint64_t base, size; };
  std::vector<Db> dbs;
  std::vector<uint32_t> doorbells;
  std::vector<uint8_t> image_expect;

  explicit SimNpu(uint16_t device_id) : platform(intel_npu_platform(device_id)), mtl(platform.buttress == IntelNpuButtress::Mtl) {
    btrs.regs[mtl ? 0x24 : 0x174] = 0x1008;
    btrs.regs[mtl ? 0x28 : 0x178] = 0x30;
    if (!mtl) btrs.regs[0x150] = 0x1;
    ip.regs[0x108] = 0x00010203;
    ip.regs[0x10C] = 0x42;
    btrs.on_write.push_back([this](uint64_t off, uint32_t v) { btrs_write(off, v); });
    ip.on_write.push_back([this](uint64_t off, uint32_t v) { ip_write(off, v); });
    ip.on_read.push_back([this](uint64_t off, uint32_t* v) { return ip_read(off, v); });
  }

  void btrs_write(uint64_t off, uint32_t v) {
    const uint64_t cmd = mtl ? 0x14 : 0x13C;
    if (off == cmd && (v & 1)) {
      if (wp_stuck) return;
      btrs.regs[cmd] = v & ~1u;
      const uint32_t p1 = btrs.regs[mtl ? 0x0C : 0x134];
      const bool enable = (p1 & 0xFFFF) != 0 && (mtl || (btrs.regs[0x138] >> 16) != 0);
      const uint64_t status = mtl ? 0x44 : 0x154;
      btrs.regs[status] = (btrs.regs[status] & ~1u) | (enable ? 1u : 0u) | (mtl ? 2u : 0u);
      if (!mtl) btrs.regs[status] |= 2u;
      if (mtl) btrs.regs[0x40] = enable ? 2u : 0u; else btrs.regs[0x14C] = btrs.regs[0x138] >> 16;
      if (!mtl && enable) btrs.regs[0x154] |= (1u << 6);
      powered = enable;
      if (!enable) ++wp_disables;
    }
    if (off == (mtl ? 0x60u : 0x164u)) {
      btrs.regs[off] = v & ~1u;
      if (v & 4) ++d0i3_entries;
    }
    if (off == (mtl ? 0x50u : 0x160u) && (v & 1)) {   // IP reset: the chip loses everything
      btrs.regs[off] = v & ~1u;
      ++resets;
      powered = false; alive = false;
      fifo.clear();
      cd_w0.clear(); cd_ttb.clear(); tlb.clear();
      for (uint64_t r : {kCr0, uint64_t{IntelNpuMmu::kRegCr0Ack}, uint64_t{IntelNpuMmu::kRegIrqCtrl}, uint64_t{IntelNpuMmu::kRegIrqCtrlAck}}) ip.regs[r] = 0;
      evt_prod = evt_cons = 0; icb_extra = 0;
      ip.regs[IntelNpuMmu::kRegEvtqProdSec] = 0;
      ip.regs[0x154] = ip.regs[0x158] = ip.regs[0x160] = ip.regs[0x164] = 0;
      ip.regs[0x30024] = ip.regs[0x3002C] = 0;
      ip.regs[0x01010030] = ip.regs[0x01010034] = 0;
    }
  }

  bool ip_read(uint64_t off, uint32_t* v) {
    if (off == IntelNpuIpc::kRxFifoStatus) { *v = static_cast<uint32_t>(fifo.size() > 255 ? 255 : fifo.size()) << 16; return true; }
    if (off == IntelNpuIpc::kRxFifoAddress) { if (fifo.empty()) *v = 0; else { *v = fifo.front(); fifo.pop_front(); } return true; }
    if (off == IntelNpuIrq::kIcbStatus0) { *v = (fifo.empty() ? 0u : 0x10u) | (evt_prod != evt_cons ? 0x20u : 0u) | icb_extra; return true; }
    return false;
  }

  void ip_write(uint64_t off, uint32_t v) {
    // the Hw sequences (power island, NoC queues, host subsystem)
    if (off == 0x154) ip.regs[0x158] = v;
    if (off == 0x160 && !(platform.ip == IntelNpuIp::Ip37xx && false)) ip.regs[0x164] = v;
    if (off == 0x30024 && !island_stuck) ip.regs[0x3002C] = v;
    if (off == 0x01010030 && !cpu_noc_refused) ip.regs[0x01010034] = v;
    if (off == IntelNpuIrq::kIcbClear0) icb_extra &= ~v;
    // SMMU
    if (off == kCr0 && !mmu_cr0_dead) ip.regs[IntelNpuMmu::kRegCr0Ack] = v;
    if (off == IntelNpuMmu::kRegIrqCtrl) ip.regs[IntelNpuMmu::kRegIrqCtrlAck] = v;
    if (off == IntelNpuMmu::kRegCmdqProd) process_commands();
    if (off == IntelNpuMmu::kRegEvtqConsSec) evt_cons = v & 0x1F;
    // CPU start
    if (platform.ip == IntelNpuIp::Ip37xx) {
      if (off == 0x41040 && (v & 1)) cpu_start(uint64_t{v & ~1u} << 9);
    } else {
      if (off == 0x40040 && (v & 1)) cpu_start(ip.writes64[0x40040] >> 3);
    }
    // TX FIFO
    if (off == (platform.ip == IntelNpuIp::Ip37xx ? IntelNpuIpc::kTx37xx : IntelNpuIpc::kTxLater)) on_tx(v);
    // doorbells
    const uint64_t db_base = platform.ip == IntelNpuIp::Ip37xx ? IntelNpuDoorbells::kBase37xx : IntelNpuDoorbells::kBaseLater;
    if (off >= db_base && off < db_base + 256 * IntelNpuDoorbells::kStride && ((off - db_base) % IntelNpuDoorbells::kStride) == 0 && v == 1)
      on_doorbell(static_cast<uint32_t>((off - db_base) / IntelNpuDoorbells::kStride));
  }

  // ---- SMMU ----
  void process_commands() {
    if (!(ip.regs[kCr0] & IntelNpuMmu::kCr0CmdqEn) || mmu_cr0_dead) return;
    const uint64_t base = ip.writes64[IntelNpuMmu::kRegCmdqBase] & (((1ull << 52) - 1) & ~0x1Full);
    const uint32_t prod = ip.regs[IntelNpuMmu::kRegCmdqProd] & 0x1F;
    uint32_t cons = ip.regs[IntelNpuMmu::kRegCmdqCons] & 0xFFFFFF;
    while (cons != prod) {
      const uint64_t* q = reinterpret_cast<const uint64_t*>(base) + (cons & 15) * 2;
      const uint32_t op = q[0] & 0xFF;
      if (op == IntelNpuMmu::kCmdCfgiAll) { ++cfgi; cd_w0.clear(); cd_ttb.clear(); }
      else if (op == IntelNpuMmu::kCmdTlbiNsnhAll) tlb.clear();
      else if (op == IntelNpuMmu::kCmdTlbiNhAsid) {
        ++tlbi_asid;
        const uint32_t asid = static_cast<uint32_t>(q[0] >> 48);
        for (auto it = tlb.begin(); it != tlb.end();) it = (it->first >> 48) == asid ? tlb.erase(it) : std::next(it);
      } else if (op != IntelNpuMmu::kCmdSync) { ip.regs[IntelNpuMmu::kRegCmdqCons] = cons | (1u << 24); return; }
      cons = (cons + 1) & 0x1F;
    }
    ip.regs[IntelNpuMmu::kRegCmdqCons] = cons;
  }
  void report(uint32_t op, uint32_t sid, uint32_t ssid, uint64_t va) {
    if (!(ip.regs[kCr0] & IntelNpuMmu::kCr0EvtqEn)) return;
    const uint64_t base = ip.writes64[IntelNpuMmu::kRegEvtqBase] & (((1ull << 52) - 1) & ~0x1Full);
    uint32_t* e = reinterpret_cast<uint32_t*>(base) + (evt_prod & 15) * 8;
    e[0] = op | (ssid << 12); e[1] = sid; e[2] = e[3] = 0; e[4] = static_cast<uint32_t>(va); e[5] = static_cast<uint32_t>(va >> 32); e[6] = 0x1000; e[7] = 0;
    evt_prod = (evt_prod + 1) & 0x1F;
    ip.regs[IntelNpuMmu::kRegEvtqProdSec] = evt_prod;
  }
  bool translate(uint32_t sid, uint32_t ssid, uint64_t va, bool write, uint64_t* pa, bool quiet = false) {
    auto fault = [&](uint32_t op) { if (!quiet) report(op, sid, ssid, va); return false; };
    if (!(ip.regs[kCr0] & IntelNpuMmu::kCr0SmmuEn)) return false;
    const uint64_t key = (uint64_t{ssid} << 48) | (va >> 12);
    uint64_t leaf = 0;
    auto cached = tlb.find(key);
    if (cached != tlb.end()) leaf = cached->second;
    else {
      if (sid >= IntelNpuMmu::kStreams) return fault(IntelNpuMmu::kEvtBadStreamId);
      const uint64_t strtab = ip.writes64[IntelNpuMmu::kRegStrtabBase] & (((1ull << 52) - 1) & ~0x3Full);
      const uint64_t* ste = reinterpret_cast<const uint64_t*>(strtab) + sid * 8;
      if (!(ste[0] & 1) || ((ste[0] >> 1) & 7) != 5) return fault(IntelNpuMmu::kEvtBadSte);
      if (ssid >= (1u << (ste[0] >> 59))) return fault(IntelNpuMmu::kEvtBadSubstreamId);
      if (!cd_w0.count(ssid)) {
        const uint64_t* cd = reinterpret_cast<const uint64_t*>(ste[0] & (((1ull << 52) - 1) & ~0x3Full)) + ssid * 8;
        cd_w0[ssid] = cd[0]; cd_ttb[ssid] = cd[1] & (((1ull << 52) - 1) & ~0xFull);
      }
      if (!((cd_w0[ssid] >> 31) & 1)) return fault(IntelNpuMmu::kEvtBadCd);
      uint64_t table = cd_ttb[ssid];
      for (int shift : {39, 30, 21}) {
        const uint64_t e = reinterpret_cast<const uint64_t*>(table)[(va >> shift) & 511];
        if ((e & 3) != 3) return fault(IntelNpuMmu::kEvtTranslation);
        table = e & IntelNpuPageTable::kAddressMask;
      }
      leaf = reinterpret_cast<const uint64_t*>(table)[(va >> 12) & 511];
      if ((leaf & 3) != 3) return fault(IntelNpuMmu::kEvtTranslation);
      tlb[key] = leaf;
    }
    if (write && (leaf & IntelNpuPageTable::kReadOnly)) return fault(IntelNpuMmu::kEvtPermission);
    *pa = (leaf & IntelNpuPageTable::kAddressMask) | (va & 0xFFF);
    return true;
  }
  // The firmware's view of memory: a range in the global context that must be contiguous in host memory.
  uint8_t* host(uint64_t vpu, uint32_t size, uint32_t ssid = 0) {
    uint64_t a = 0, b = 0;
    if (size == 0 || !translate(0, ssid, vpu, false, &a, true) || !translate(0, ssid, vpu + size - 1, false, &b, true) || b != a + size - 1) return nullptr;
    return reinterpret_cast<uint8_t*>(a);
  }

  // ---- firmware ----
  void cpu_start(uint64_t entry) {
    ++cpu_starts;
    entry_seen = entry;
    boot_cr0_when_started = ip.regs[kCr0];
    if (!powered) return;
    alive = true;
    uint64_t pa = 0;
    entry_has_image = translate(0, 0, entry, false, &pa, true) && !image_expect.empty() && std::equal(image_expect.begin(), image_expect.begin() + 16, reinterpret_cast<const uint8_t*>(pa));
    // The boot parameters sit in the first page of the runtime range, as the header says; the sim finds them through the MMU like the real firmware.
    const uint8_t* bp = host(boot_params_addr, 4096);
    boot_params_ok = bp && ld32(bp) == 0x10000 && ld32(bp + 800) == 8 && ld32(bp + 804) == 0x30;
    if (!boot_params_ok || fw_no_ready) return;
    rx_vpu = ld64(bp + 256);
    rx_size = ld32(bp + 264) * 2;
    rx_next = 0;
    deliver(IntelNpuIpc::kBootChannel, nullptr, IntelNpuIpc::kBootMessageData);
  }
  uint64_t boot_params_addr = 0;

  bool deliver(uint32_t channel, const IntelNpuJsmMessage* msg, uint32_t boot_data = 0) {
    const uint32_t slots = rx_size / kSlot;
    if (slots == 0) return false;
    const uint32_t slot = rx_next;
    uint8_t* base = host(rx_vpu + uint64_t{slot} * kSlot, kSlot);
    if (!base) return false;
    if (base[12] != 0 || base[64 + 12] != 0) { ++overruns; return false; }
    rx_next = (rx_next + 1) % slots;
    std::fill(base, base + kSlot, 0);
    st32(base, msg ? static_cast<uint32_t>(rx_vpu + slot * kSlot + 64) : boot_data);
    st32(base + 4, msg ? 128 : 0);
    st16(base + 8, static_cast<uint16_t>(channel));
    base[10] = 1; base[11] = 0; base[12] = 1;
    if (msg) { std::copy(msg->bytes, msg->bytes + 128, base + 64); st32(base + 64 + 12, 1); }
    fifo.push_back(static_cast<uint32_t>(rx_vpu + slot * kSlot));
    return true;
  }

  void on_tx(uint32_t slot_vpu) {
    ++tx_count;
    if (!alive || fw_silent) return;
    const uint8_t* p = host(slot_vpu, kSlot);
    if (!p) return;
    IntelNpuJsmMessage req;
    std::copy(p + 64, p + 64 + 128, req.bytes);
    requests.push_back(req);
    IntelNpuJsmMessage rsp;
    rsp.set_type(IntelNpuJsm::response_type(req.type()));
    rsp.set_request_id(req.request_id());
    rsp.set_result(req.type() == fail_type ? 2 : force_result);
    switch (req.type()) {
      case IntelNpuJsm::kQueryEngineHeartbeat: rsp.put64(8, ++heartbeat); break;
      case IntelNpuJsm::kRegisterDb:
        dbs.erase(std::remove_if(dbs.begin(), dbs.end(), [&](const Db& d) { return d.id == req.get32(0); }), dbs.end());
        dbs.push_back({req.get32(0), req.get32(20), req.get64(8), req.get32(16)});
        break;
      case IntelNpuJsm::kHwsRegisterDb:
        dbs.erase(std::remove_if(dbs.begin(), dbs.end(), [&](const Db& d) { return d.id == req.get32(0); }), dbs.end());
        dbs.push_back({req.get32(0), req.get32(4), req.get64(16), req.get64(24)});
        break;
      case IntelNpuJsm::kUnregisterDb:
        dbs.erase(std::remove_if(dbs.begin(), dbs.end(), [&](const Db& d) { return d.id == req.get32(0); }), dbs.end());
        break;
      case IntelNpuJsm::kEngineReset: ++resets_engine; break;
      case IntelNpuJsm::kHwsEngineResume: ++resumes; break;
      default: break;
    }
    deliver(p[8] | (p[9] << 8), &rsp);
  }

  void on_doorbell(uint32_t id) {
    doorbells.push_back(id);
    if (!alive || jobs_stall) return;
    const Db* db = nullptr;
    for (const Db& d : dbs) if (d.id == id) db = &d;
    if (!db) return;
    uint8_t* q = host(db->base, static_cast<uint32_t>(db->size));
    if (!q) return;
    const uint32_t entries = static_cast<uint32_t>((db->size - 64) / 64);
    uint32_t head = ld32(q + 4);
    const uint32_t tail = ld32(q + 8);
    while (head != tail && head < entries) {
      const uint8_t* e = q + 64 + head * 64;
      uint64_t pa = 0;
      if (!translate(0, db->ssid, ld64(e), false, &pa)) break;   // the job's commands are not mapped in its context: a fault, no completion
      IntelNpuJsmMessage done;
      done.set_type(IntelNpuJsm::kJobDone);
      done.put32(4, id); done.put32(8, ld32(e + 8)); done.put32(12, job_status); done.put32(16, db->ssid); done.put64(24, 7);
      deliver(IntelNpuJsm::kChannelJobReturn, &done);
      head = (head + 1) % entries;
    }
    st32(q + 4, head);
  }
};

// A firmware file for `ip`, as in IntelNpuHwTest.
static std::vector<uint8_t> make_firmware(IntelNpuIp ip) {
  const uint64_t base = IntelNpuMemoryMap::for_ip(ip).runtime.start;
  const uint32_t image_size = 0x4000;
  std::vector<uint8_t> f(IntelNpuFirmware::kFileImageOffset + image_size);
  put32(f, 0, 1); put32(f, 4, 1);
  put64(f, 8, base + 0x2000); put32(f, 16, image_size); put64(f, 20, base + 0x2000);
  put64(f, 64, base + 0x1000); put32(f, 72, 32); put64(f, 76, base);
  put32(f, 84, (3u << 16) | 29); put32(f, 84 + 16, (3u << 16) | 34);
  put32(f, 148, 0x2000 + 0x10000); put32(f, 152, 0x1800);
  put32(f, 156, 0x3000); put32(f, 160, 0x5000);
  put64(f, 188, base + 0x2000); put32(f, 196, 0x1000);
  const char* v = "FW 2026.1 test";
  for (size_t i = 0; v[i]; ++i) f[4096 + i] = static_cast<uint8_t>(v[i]);
  for (size_t i = 0; i < image_size; ++i) f[IntelNpuFirmware::kFileImageOffset + i] = static_cast<uint8_t>(i * 7 + 1);
  return f;
}

struct Rig {
  TestAllocator alloc;
  SimNpu npu;
  std::vector<uint8_t> file;
  IntelNpuDeviceConfig config;
  UniquePointer<IntelNpuDevice> dev;
  explicit Rig(uint16_t id) : npu(id) {
    file = make_firmware(npu.platform.ip);
    npu.boot_params_addr = IntelNpuMemoryMap::for_ip(npu.platform.ip).runtime.start;
    npu.image_expect.assign(file.begin() + IntelNpuFirmware::kFileImageOffset, file.begin() + IntelNpuFirmware::kFileImageOffset + 16);
    config.ip = &npu.ip; config.buttress = &npu.btrs; config.allocator = &alloc;
    config.device_id = id; config.revision = 4; config.pci_bus = 3; config.snoop = true;
    config.boot_tries = 200; config.reply_tries = 200;
    dev = UniquePointer<IntelNpuDevice>::make(alloc, config);
  }
  IntelNpuDevice& d() { return *dev; }
  ConstByteView fw() const { return ConstByteView(file.data(), file.size()); }
  bool boot() { return d().load_firmware(fw()) == IntelNpuDeviceError::None && d().start(1789000000000000ull) == IntelNpuDeviceError::None; }
};

static const uint16_t kIds[] = {0x7D1D, 0x643E, 0xB03E, 0xD71D};

static void test_load_errors() {
  { Rig r(0x1234); CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::Platform && r.d().state() == IntelNpuDeviceState::Created); }
  {
    Rig r(0x643E);
    r.npu.btrs.regs[0x178] = 0xFFFFFFFF;   // a device that is off the bus
    CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::Registers);
    r.npu.btrs.regs[0x178] = 0x30;
    std::vector<uint8_t> bad = r.file;
    bad[0] = 9;
    CHECK(r.d().load_firmware(ConstByteView(bad.data(), bad.size())) == IntelNpuDeviceError::Firmware && r.d().firmware_error() == IntelNpuFirmwareError::HeaderVersion);
    CHECK(r.d().load_firmware(ConstByteView(bad.data(), 100)) == IntelNpuDeviceError::Firmware && r.d().firmware_error() == IntelNpuFirmwareError::TooSmall);
    CHECK(r.d().state() == IntelNpuDeviceState::Created && r.d().start() == IntelNpuDeviceError::BadState && r.d().suspend() == IntelNpuDeviceError::BadState && r.d().recover() == IntelNpuDeviceError::BadState);
    CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::None && r.d().state() == IntelNpuDeviceState::Loaded && r.d().load_firmware(r.fw()) == IntelNpuDeviceError::BadState);
    CHECK(r.d().info().valid && !r.alloc.dma.empty() && r.npu.cpu_starts == 0 && !r.npu.powered);   // loading touches nothing but reads
    uint32_t ctx = 0;
    CHECK(r.d().create_context(&ctx) == IntelNpuDeviceError::BadState && !r.d().service().handled);
  }
  // Out of DMA memory while loading: nothing leaks.
  for (int budget = 0; budget < 7; ++budget) {
    Rig r(0x643E);
    r.alloc.dma_budget = budget;
    CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::Memory && r.d().state() == IntelNpuDeviceState::Created);
    CHECK(r.alloc.dma.empty());
  }
}

static void test_boot_each_platform() {
  for (uint16_t id : kIds) {
    Rig r(id);
    const IntelNpuIp ip = r.npu.platform.ip;
    CHECK(r.boot());
    IntelNpuDevice& d = r.d();
    CHECK(d.state() == IntelNpuDeviceState::Running && d.boot_count() == 1 && d.last_error() == IntelNpuDeviceError::None);
    CHECK(r.npu.powered && r.npu.alive && r.npu.cpu_starts == 1);
    // The firmware found its image and its boot parameters through the MMU, by the addresses in the header, and the MMU was on when the CPU started.
    CHECK(r.npu.entry_seen == d.firmware().cold_boot_entry_point && r.npu.entry_has_image && r.npu.boot_params_ok);
    CHECK((r.npu.boot_cr0_when_started & 1) == 1);
    CHECK(r.npu.ip.regs[IntelNpuIrq::kFwSocIrqEnable] == 0x7F && r.npu.ip.writes64[IntelNpuIrq::kIcbEnable0] != 0);   // interrupts on
    CHECK(d.firmware().sched_mode == kIntelNpuSchedHw);
    CHECK(!r.npu.requests.empty() && r.npu.requests[0].type() == IntelNpuJsm::kSetPriorityBandSetup && r.npu.requests[0].get32(48) == 10);
    CHECK(r.npu.requests[0].get32(4) == 50000 && r.npu.requests[0].get32(16 + 4) == 300000 && r.npu.requests[0].get32(32) == 50000);
    // Every ready message and request was released: nothing is left in the receive area.
    CHECK(d.service().handled == false);
    CHECK(r.npu.fifo.empty());
    // The TBU stream-id valid bits and snoop override were set before the CPU started.
    CHECK(r.npu.ip.regs[0x00360004] == (ip == IntelNpuIp::Ip37xx ? 0x33u : 0x3Fu));
    CHECK((r.npu.ip.regs[0x00360000] & 8) != 0);
    // Power down on destruction, and every DMA page freed.
    r.dev.reset();
    CHECK(r.npu.resets == 1 && !r.npu.powered && r.npu.d0i3_entries >= 1 && r.alloc.dma.empty());
  }
}

struct Buf {
  void* p;
  IntelNpuDmaExtent e;
};
static Buf dma_buf(Rig& r, uint64_t size) {
  void* p = r.alloc.allocate_dma(size, 4096);
  return {p, {intel_npu_bus_address(p), size}};
}

static void test_contexts_and_jobs() {
  for (uint16_t id : {0x643E, 0x7D1D}) {
    Rig r(id);
    CHECK(r.boot());
    IntelNpuDevice& d = r.d();
    uint32_t ssid = 0, queue = 0, job = 0;
    CHECK(d.create_context(&ssid) == IntelNpuDeviceError::None && ssid == 2);
    Buf data = dma_buf(r, 0x6000);
    uint64_t va = 0;
    const IntelNpuDmaExtent ex[1] = {data.e};
    CHECK(d.map_buffer(ssid, ArrayView<const IntelNpuDmaExtent>(ex), 0x6000, IntelNpuPageTable::MapFlags(), &va) == IntelNpuDeviceError::None);
    uint64_t pa = 0;
    CHECK(r.npu.translate(0, ssid, va + 0x10, false, &pa) && pa == data.e.bus_addr + 0x10);
    CHECK(d.create_queue(ssid, IntelNpuJsm::kBandNormal, &queue) == IntelNpuDeviceError::None && queue == 1);
    // Submitting registers the queue with the firmware (the hardware scheduler's three messages), rings the doorbell, and the job comes back done.
    IntelNpuJob j; j.batch_buffer = va;
    const size_t before = r.npu.requests.size();
    CHECK(d.submit(ssid, queue, j, &job) == IntelNpuDeviceError::None && job == (ssid - 1) << 8 && d.jobs_in_flight() == 1);
    CHECK(r.npu.requests.size() == before + 3 && r.npu.requests[before].type() == IntelNpuJsm::kCreateCmdQueue && r.npu.requests[before + 1].type() == IntelNpuJsm::kSetContextSchedProperties &&
          r.npu.requests[before + 2].type() == IntelNpuJsm::kHwsRegisterDb);
    CHECK(r.npu.requests[before].get32(8) == ssid && r.npu.requests[before].get64(24) == 1);
    CHECK(r.npu.doorbells.size() == 1);
    IntelNpuJobResult res;
    CHECK(!d.take_job_result(job, &res));
    const IntelNpuServiceResult s = d.service();
    CHECK(s.handled && !s.recovery_needed && s.mmu_faults == 0);
    CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Completed && res.status == 0 && res.ssid == ssid && d.jobs_in_flight() == 0);
    // More jobs reuse the registration.
    uint32_t j2 = 0, j3 = 0;
    CHECK(d.submit(ssid, queue, j, &j2) == IntelNpuDeviceError::None && d.submit(ssid, queue, j, &j3) == IntelNpuDeviceError::None && j2 == job + 1 && j3 == job + 2);
    CHECK(r.npu.requests.size() == before + 3 && r.npu.doorbells.size() == 3);
    d.service();
    CHECK(d.take_job_result(j2, &res) && d.take_job_result(j3, &res) && d.jobs_in_flight() == 0);
    uint64_t hb = 0;
    CHECK(d.heartbeat(&hb) == IntelNpuDeviceError::None && hb == 0x1001);
    // Errors of the API.
    CHECK(d.submit(ssid, 2, j, &job) == IntelNpuDeviceError::NoSuchQueue && d.submit(ssid, 0, j, &job) == IntelNpuDeviceError::NoSuchQueue && d.submit(9, queue, j, &job) == IntelNpuDeviceError::NoSuchContext);
    CHECK(d.create_queue(ssid, 9, &queue) == IntelNpuDeviceError::Invalid && d.create_queue(77, 1, &queue) == IntelNpuDeviceError::NoSuchContext);
    CHECK(d.destroy_queue(ssid, 5) == IntelNpuDeviceError::NoSuchQueue && d.destroy_context(77) == IntelNpuDeviceError::NoSuchContext);
    CHECK(d.unmap_buffer(ssid, va + 0x1000, ArrayView<const IntelNpuDmaExtent>(ex)) == IntelNpuDeviceError::Invalid);   // not an address we handed out
    CHECK(r.npu.translate(0, ssid, va, false, &pa, true));                                                           // ... and nothing was unmapped
    const IntelNpuDmaExtent whole[1] = {{data.e.bus_addr, 0x6000}};
    CHECK(d.unmap_buffer(ssid, va, ArrayView<const IntelNpuDmaExtent>(whole)) == IntelNpuDeviceError::None && !r.npu.translate(0, ssid, va, false, &pa, true));
    // Queue full, a stalled NPU.
    r.npu.jobs_stall = true;
    CHECK(d.map_buffer(ssid, ArrayView<const IntelNpuDmaExtent>(whole), 0x6000, IntelNpuPageTable::MapFlags(), &va) == IntelNpuDeviceError::None);
    j.batch_buffer = va;
    uint32_t n = 0;
    IntelNpuDeviceError e;
    while ((e = d.submit(ssid, queue, j, &job)) == IntelNpuDeviceError::None) ++n;
    CHECK(e == IntelNpuDeviceError::QueueFull && n == 62 && d.jobs_in_flight() == n);
    // Destroying the context releases it with the firmware and aborts what was queued.
    const size_t reqs = r.npu.requests.size();
    CHECK(d.destroy_context(ssid) == IntelNpuDeviceError::None);
    CHECK(r.npu.requests.size() == reqs + 3 && r.npu.requests[reqs].type() == IntelNpuJsm::kUnregisterDb && r.npu.requests[reqs + 1].type() == IntelNpuJsm::kDestroyCmdQueue &&
          r.npu.requests[reqs + 2].type() == IntelNpuJsm::kSsidRelease);
    uint32_t aborted = 0;
    for (uint32_t k = 0; k < 256; ++k) { IntelNpuJobResult x; if (d.take_job_result(((ssid - 1) << 8) | k, &x) && x.outcome == IntelNpuJobOutcome::Aborted) ++aborted; }
    CHECK(aborted == n);
    r.alloc.deallocate(data.p);
  }
  // The OS scheduler (no hardware scheduler messages): a firmware whose JSM API is older than 3.24.
  {
    Rig r(0x643E);
    put32(r.file, 84 + 16, (3u << 16) | 20);
    CHECK(r.boot() && r.d().firmware().sched_mode == kIntelNpuSchedOs);
    CHECK(r.npu.requests.empty());   // no priority-band setup either
    uint32_t ssid, queue, job;
    CHECK(r.d().create_context(&ssid) == IntelNpuDeviceError::None && r.d().create_queue(ssid, 1, &queue) == IntelNpuDeviceError::None);
    Buf b = dma_buf(r, 0x1000);
    const IntelNpuDmaExtent ex[1] = {b.e};
    uint64_t va;
    CHECK(r.d().map_buffer(ssid, ArrayView<const IntelNpuDmaExtent>(ex), 0x1000, IntelNpuPageTable::MapFlags(), &va) == IntelNpuDeviceError::None);
    IntelNpuJob j; j.batch_buffer = va;
    CHECK(r.d().submit(ssid, queue, j, &job) == IntelNpuDeviceError::None && r.npu.requests.size() == 1 && r.npu.requests[0].type() == IntelNpuJsm::kRegisterDb);
    r.d().service();
    IntelNpuJobResult res;
    CHECK(r.d().take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Completed);
    r.alloc.deallocate(b.p);
  }
}

static void test_faults() {
  Rig r(0x643E);
  CHECK(r.boot());
  IntelNpuDevice& d = r.d();
  uint32_t a = 0, b = 0, qa = 0, qb = 0, job = 0;
  CHECK(d.create_context(&a) == IntelNpuDeviceError::None && d.create_context(&b) == IntelNpuDeviceError::None && a == 2 && b == 3);
  CHECK(d.create_queue(a, 1, &qa) == IntelNpuDeviceError::None && d.create_queue(b, 1, &qb) == IntelNpuDeviceError::None);
  Buf buf = dma_buf(r, 0x1000);
  const IntelNpuDmaExtent ex[1] = {buf.e};
  uint64_t va_a = 0, va_b = 0;
  CHECK(d.map_buffer(a, ArrayView<const IntelNpuDmaExtent>(ex), 0x1000, IntelNpuPageTable::MapFlags(), &va_a) == IntelNpuDeviceError::None);
  CHECK(d.map_buffer(b, ArrayView<const IntelNpuDmaExtent>(ex), 0x1000, IntelNpuPageTable::MapFlags(), &va_b) == IntelNpuDeviceError::None);
  // A job whose commands are not mapped in its context: the MMU reports a translation fault for that context, and only that one stops.
  IntelNpuJob bad; bad.batch_buffer = va_a + 0x10000;
  CHECK(d.submit(a, qa, bad, &job) == IntelNpuDeviceError::None);
  IntelNpuServiceResult s = d.service();
  CHECK(s.handled && s.mmu_faults == 1 && s.faulted_contexts == 1 && !s.recovery_needed && d.context_faulted(a) && !d.context_faulted(b) && d.total_mmu_faults() == 1);
  IntelNpuJobResult res;
  CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Aborted && res.status == IntelNpuJsm::kStatusAborted);
  IntelNpuJob ok; ok.batch_buffer = va_b;
  CHECK(d.submit(a, qa, ok, &job) == IntelNpuDeviceError::Faulted && d.create_queue(a, 1, &qa) == IntelNpuDeviceError::Faulted &&
        d.map_buffer(a, ArrayView<const IntelNpuDmaExtent>(ex), 0x1000, IntelNpuPageTable::MapFlags(), &va_a) == IntelNpuDeviceError::Faulted);
  CHECK(d.submit(b, qb, ok, &job) == IntelNpuDeviceError::None);
  d.service();
  CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Completed);
  // The firmware reports a job that needs an engine reset: its context faults, the engine is reset and resumed.
  r.npu.job_status = 0xE;
  CHECK(d.submit(b, qb, ok, &job) == IntelNpuDeviceError::None);
  s = d.service();
  CHECK(s.faulted_contexts == 1 && s.engine_resets == 1 && !s.recovery_needed && d.context_faulted(b) && r.npu.resets_engine == 1 && r.npu.resumes == 1 && d.engine_reset_count() == 1);
  CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::ContextFault && res.status == 0xE);   // the job that faulted keeps the firmware's verdict
  // A fault in the firmware's own memory needs the whole NPU recovered.
  r.npu.report(IntelNpuMmu::kEvtTranslation, 0, 0, 0x90000000);
  s = d.service();
  CHECK(s.mmu_faults == 1 && s.recovery_needed);
  // The engine reset itself fails: recovery.
  uint32_t c = 0, qc = 0;
  CHECK(d.create_context(&c) == IntelNpuDeviceError::None && d.create_queue(c, 1, &qc) == IntelNpuDeviceError::None);
  uint64_t va_c = 0;
  CHECK(d.map_buffer(c, ArrayView<const IntelNpuDmaExtent>(ex), 0x1000, IntelNpuPageTable::MapFlags(), &va_c) == IntelNpuDeviceError::None);
  ok.batch_buffer = va_c;
  CHECK(d.submit(c, qc, ok, &job) == IntelNpuDeviceError::None);
  r.npu.fail_type = IntelNpuJsm::kEngineReset;
  s = d.service();
  CHECK(s.faulted_contexts == 1 && s.recovery_needed);
  r.npu.fail_type = 0;
  // Watchdog and buttress errors through the interrupt controller.
  r.npu.icb_extra = 0x40000000;   // management subsystem watchdog
  s = d.service();
  CHECK(s.handled && s.recovery_needed);
  r.alloc.deallocate(buf.p);
}

static void test_suspend_resume_recover() {
  for (uint16_t id : {0x643E, 0x7D1D, 0xB03E}) {
    Rig r(id);
    CHECK(r.boot());
    IntelNpuDevice& d = r.d();
    uint32_t ssid = 0, queue = 0, job = 0;
    CHECK(d.create_context(&ssid) == IntelNpuDeviceError::None && d.create_queue(ssid, 2, &queue) == IntelNpuDeviceError::None);
    Buf buf = dma_buf(r, 0x2000);
    const IntelNpuDmaExtent ex[1] = {buf.e};
    uint64_t va = 0;
    CHECK(d.map_buffer(ssid, ArrayView<const IntelNpuDmaExtent>(ex), 0x2000, IntelNpuPageTable::MapFlags(), &va) == IntelNpuDeviceError::None);
    IntelNpuJob j; j.batch_buffer = va;
    r.npu.jobs_stall = true;
    CHECK(d.submit(ssid, queue, j, &job) == IntelNpuDeviceError::None);
    // Suspend: the NPU is reset and off; the job in flight is aborted; nothing more goes out.
    CHECK(d.suspend() == IntelNpuDeviceError::None && d.state() == IntelNpuDeviceState::Suspended);
    CHECK(r.npu.resets == 1 && !r.npu.powered && !r.npu.alive && r.npu.d0i3_entries == 1);
    IntelNpuJobResult res;
    CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Aborted);
    uint64_t hb_unused = 0;
    CHECK(d.submit(ssid, queue, j, &job) == IntelNpuDeviceError::BadState && d.heartbeat(&hb_unused) == IntelNpuDeviceError::BadState);
    CHECK(d.suspend() == IntelNpuDeviceError::BadState && !d.service().handled);
    // Resume: a cold boot again. The context, its mapping and its queue are still there; the queue registers again on its next job.
    r.npu.jobs_stall = false;
    r.npu.requests.clear();
    r.npu.dbs.clear();
    CHECK(d.start(5) == IntelNpuDeviceError::None && d.state() == IntelNpuDeviceState::Running && d.boot_count() == 2 && r.npu.cpu_starts == 2);
    CHECK(r.npu.entry_has_image && r.npu.boot_params_ok);
    uint64_t pa = 0;
    CHECK(r.npu.translate(0, ssid, va, false, &pa) && pa == buf.e.bus_addr);
    const size_t before = r.npu.requests.size();
    CHECK(d.submit(ssid, queue, j, &job) == IntelNpuDeviceError::None && r.npu.requests.size() == before + 3);
    d.service();
    CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Completed);
    // recover(): a hung NPU stops answering; the reset brings it back.
    r.npu.fw_silent = true;
    CHECK(d.heartbeat(&pa) == IntelNpuDeviceError::Ipc && d.ipc_status() == IntelNpuIpcStatus::Timeout);
    r.npu.fw_silent = false;
    r.npu.jobs_stall = true;
    CHECK(d.submit(ssid, queue, j, &job) == IntelNpuDeviceError::None);
    CHECK(d.recover(9) == IntelNpuDeviceError::None && d.state() == IntelNpuDeviceState::Running && d.boot_count() == 3 && r.npu.resets == 2);
    CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Aborted);
    r.npu.jobs_stall = false;
    CHECK(d.submit(ssid, queue, j, &job) == IntelNpuDeviceError::None);
    d.service();
    CHECK(d.take_job_result(job, &res) && res.outcome == IntelNpuJobOutcome::Completed);
    r.alloc.deallocate(buf.p);
  }
}

static void test_boot_failures() {
  struct Case { const char* name; std::function<void(Rig&)> sabotage; IntelNpuDeviceError error; IntelNpuStep step; };
  const Case cases[] = {
    {"work point", [](Rig& r) { r.npu.wp_stuck = true; }, IntelNpuDeviceError::PowerUp, IntelNpuStep::WorkPoint},
    {"power island", [](Rig& r) { r.npu.island_stuck = true; }, IntelNpuDeviceError::PowerUp, IntelNpuStep::PowerDomain},
    {"mmu", [](Rig& r) { r.npu.mmu_cr0_dead = true; }, IntelNpuDeviceError::Mmu, IntelNpuStep::None},
    {"no ready message", [](Rig& r) { r.npu.fw_no_ready = true; }, IntelNpuDeviceError::BootTimeout, IntelNpuStep::None},
  };
  for (const Case& c : cases) {
    Rig r(0x643E);
    c.sabotage(r);
    CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::None);
    CHECK(r.d().start() == c.error && r.d().state() == IntelNpuDeviceState::Failed && r.d().hw_step() == c.step);
    CHECK(!r.npu.powered && !r.npu.alive);   // left off
    CHECK(r.d().submit(2, 1, IntelNpuJob(), &r.npu.resets) == IntelNpuDeviceError::BadState);
    // Fixing the cause and trying again works.
    r.npu.wp_stuck = r.npu.island_stuck = r.npu.mmu_cr0_dead = r.npu.fw_no_ready = false;
    r.npu.btrs.regs[0x14] &= ~1u; r.npu.btrs.regs[0x13C] &= ~1u;   // the request the stuck work point never took
    const IntelNpuDeviceError again = r.d().start();
    if (again != IntelNpuDeviceError::None || r.d().boot_count() != 1) printf("case %s: error %s state %d boots %u\n", c.name, intel_npu_device_error_name(again), static_cast<int>(r.d().state()), r.d().boot_count());
    CHECK(again == IntelNpuDeviceError::None && r.d().state() == IntelNpuDeviceState::Running && r.d().boot_count() == 1);
  }
  {
    Rig r(0xB03E);
    r.npu.cpu_noc_refused = true;
    CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::None && r.d().start() == IntelNpuDeviceError::CpuBoot && r.d().hw_step() == IntelNpuStep::CpuNoc && r.d().state() == IntelNpuDeviceState::Failed);
    CHECK(r.d().recover() == IntelNpuDeviceError::CpuBoot);   // still refusing
    r.npu.cpu_noc_refused = false;
    CHECK(r.d().recover() == IntelNpuDeviceError::None && r.d().state() == IntelNpuDeviceState::Running);
  }
  {
    // The firmware is silent after booting: the priority-band message gets no answer.
    Rig r(0x643E);
    r.npu.fw_silent = true;
    CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::None && r.d().start() == IntelNpuDeviceError::Ipc && r.d().ipc_status() == IntelNpuIpcStatus::Timeout);
  }
  {
    // Out of memory in the first boot: the device ends up Failed and leaks nothing.
    for (int budget = 0; budget < 40; ++budget) {
      Rig r(0x643E);
      CHECK(r.d().load_firmware(r.fw()) == IntelNpuDeviceError::None);
      r.alloc.dma_budget = budget;
      const IntelNpuDeviceError e = r.d().start();
      r.alloc.dma_budget = -1;
      if (e == IntelNpuDeviceError::None) { CHECK(budget > 8); break; }
      CHECK(r.d().state() == IntelNpuDeviceState::Failed && !r.npu.powered);
      r.dev.reset();
      CHECK(r.alloc.dma.empty());
    }
  }
}

static void test_limits() {
  Rig r(0x643E);
  CHECK(r.boot());
  IntelNpuDevice& d = r.d();
  uint32_t ssid = 0;
  uint32_t made = 0;
  while (d.create_context(&ssid) == IntelNpuDeviceError::None) ++made;
  CHECK(made == IntelNpuDevice::kMaxUserContexts && made == 129 && d.last_error() == IntelNpuDeviceError::NoRoom);
  CHECK(d.destroy_context(50) == IntelNpuDeviceError::None && d.create_context(&ssid) == IntelNpuDeviceError::None && ssid == 50);
  uint32_t q = 0, n = 0;
  while (d.create_queue(2, 1, &q) == IntelNpuDeviceError::None) ++n;
  CHECK(n == IntelNpuDevice::kMaxQueuesPerContext && d.last_error() == IntelNpuDeviceError::NoRoom);
  CHECK(d.destroy_queue(2, 3) == IntelNpuDeviceError::None && d.create_queue(2, 1, &q) == IntelNpuDeviceError::None && q == 3);
  // A buffer larger than the user range, or an extent list that does not add up.
  const IntelNpuDmaExtent huge[1] = {{0x1000, 1ull << 50}};
  uint64_t va;
  CHECK(d.map_buffer(2, ArrayView<const IntelNpuDmaExtent>(huge), 1ull << 50, IntelNpuPageTable::MapFlags(), &va) != IntelNpuDeviceError::None);
  CHECK(d.map_buffer(2, ArrayView<const IntelNpuDmaExtent>(huge), 0x1000, IntelNpuPageTable::MapFlags(), &va) == IntelNpuDeviceError::Mmu);
  CHECK(r.npu.requests.size() > 0);
  r.dev.reset();
  CHECK(r.alloc.dma.empty());
}


// ---- firmware files and the executor ----

static bool put_file(filesystem::IFileSystem& fs, const char* path, const std::vector<uint8_t>& data) {
  UniquePointer<filesystem::IFile> f;
  if (fs.open_file(StringView(path), filesystem::OpenMode::CreateOrOpen, &f) != filesystem::Status::Ok || !f) return false;
  uint64_t n = 0;
  return data.empty() || f->write(0, ConstByteView(data.data(), data.size()), &n) == filesystem::Status::Ok;
}

static void test_firmware_file() {
  TestAllocator alloc;
  filesystem::RamFileSystem fs(alloc);
  for (const char* dir : {"/Linux", "/Linux/lib", "/Linux/lib/firmware", "/Linux/lib/firmware/intel", "/Linux/lib/firmware/intel/vpu"}) CHECK(fs.create_directory(StringView(dir)) == filesystem::Status::Ok);
  CHECK(std::string(filesystem::layout::kLinuxFirmwareIntelVpu) == "/Linux/lib/firmware/intel/vpu");
  uint32_t count = 0;
  const char* const* names = intel_npu_firmware_names(IntelNpuIp::Ip40xx, &count);
  CHECK(count == 2 && std::string(names[0]) == "vpu_40xx_v1.bin" && std::string(names[1]) == "vpu_40xx_v0.0.bin");
  CHECK(intel_npu_firmware_names(IntelNpuIp::Ip60xx, &count) && count == 1 && !intel_npu_firmware_names(IntelNpuIp::Unknown, &count) && count == 0);
  ByteBuffer out(alloc);
  FixedString<48> used;
  CHECK(!intel_npu_read_firmware(fs, alloc, IntelNpuIp::Ip40xx, out, &used));   // nothing there
  const std::vector<uint8_t> old_fw(100, 0x11), new_fw(5000, 0x22);
  CHECK(put_file(fs, "/Linux/lib/firmware/intel/vpu/vpu_40xx_v0.0.bin", old_fw));
  CHECK(intel_npu_read_firmware(fs, alloc, IntelNpuIp::Ip40xx, out, &used) && out.size() == 100 && std::string(used.c_str()) == "vpu_40xx_v0.0.bin" && out.view()[0] == 0x11);
  CHECK(put_file(fs, "/Linux/lib/firmware/intel/vpu/vpu_40xx_v1.bin", new_fw));
  CHECK(intel_npu_read_firmware(fs, alloc, IntelNpuIp::Ip40xx, out, &used) && out.size() == 5000 && std::string(used.c_str()) == "vpu_40xx_v1.bin" && out.view()[4999] == 0x22);   // the newest wins
  CHECK(!intel_npu_read_firmware(fs, alloc, IntelNpuIp::Ip37xx, out, &used) && !intel_npu_read_firmware(fs, alloc, IntelNpuIp::Unknown, out, &used));
  CHECK(put_file(fs, "/Linux/lib/firmware/intel/vpu/vpu_50xx_v1.bin", {}));   // an empty file is not firmware ...
  CHECK(put_file(fs, "/Linux/lib/firmware/intel/vpu/vpu_50xx_v0.0.bin", old_fw));
  CHECK(intel_npu_read_firmware(fs, alloc, IntelNpuIp::Ip50xx, out, nullptr) && out.size() == 100);   // ... the next name is tried
}

struct StepClock final : IClock {
  mutable uint64_t t = 0;
  uint64_t now_us() const override { return t += 100; }
  const char* name() const override { return "step"; }
  bool initialize() override { return true; }
  void shutdown() override {}
};

static uint64_t fake_wall(void* context) { ++*static_cast<int*>(context); return 1789000000000000ull; }

static void test_executor() {
  for (uint16_t id : {0x643E, 0x7D1D}) {
    Rig r(id);
    CHECK(r.boot());
    StepClock clock;
    int wall_calls = 0;
    IntelNpuExecutor::Config cfg;
    cfg.clock = &clock; cfg.wall_clock_us = &fake_wall; cfg.wall_context = &wall_calls; cfg.wait_tries = 50; cfg.idle_polls = 3;
    {
      IntelNpuExecutor ex(r.d(), r.alloc, cfg);
      std::vector<uint8_t> stream(6000, 0xC3);
      using St = IntelNpuExecutor::Status;
      // A command stream goes to the NPU as one job: its pages are mapped read-only in a context of the executor's, and unmapped and freed afterwards.
      const size_t dma_before = r.alloc.dma.size();
      IntelNpuExecutor::Result res = ex.execute(ConstByteView(stream.data(), stream.size()));
      CHECK(res.status == St::Ok && res.firmware_status == 0 && res.elapsed_us > 0 && ex.jobs_run() == 1 && ex.jobs_failed() == 0 && ex.busy_us() > 0);
      CHECK(r.npu.doorbells.size() == 1 && r.d().jobs_in_flight() == 0);
      const size_t after_first = r.alloc.dma.size();
      CHECK(after_first > dma_before);   // the context's tables and the queue are kept ...
      CHECK(ex.execute(ConstByteView(stream.data(), stream.size())).status == St::Ok && r.alloc.dma.size() == after_first);   // ... and the stream's pages are not
      const uint64_t busy = ex.busy_us();
      // The firmware reports an error status: Failed, with its code.
      r.npu.job_status = 7;
      res = ex.execute(ConstByteView(stream.data(), 100));
      CHECK(res.status == St::Failed && res.firmware_status == 7 && ex.jobs_failed() == 1 && ex.busy_us() > busy);
      // A status that needs an engine reset faults the executor's context; the next job gets a new one.
      r.npu.job_status = 0xE;
      res = ex.execute(ConstByteView(stream.data(), 100));
      CHECK(res.status == St::Faulted && r.npu.resets_engine == 1);
      r.npu.job_status = 0;
      CHECK(ex.execute(ConstByteView(stream.data(), 100)).status == St::Ok);
      // Nothing to run, or too much.
      CHECK(ex.execute(ConstByteView()).status == St::Empty);
      IntelNpuExecutor::Config small = cfg; small.max_command_bytes = 1000;
      IntelNpuExecutor tiny(r.d(), r.alloc, small);
      CHECK(tiny.execute(ConstByteView(stream.data(), 1001)).status == St::Empty && tiny.execute(ConstByteView(stream.data(), 1000)).status == St::Ok);
      // The NPU stalls: after the wait it is reset (so that it cannot read freed memory) and the job reported as timed out.
      r.npu.jobs_stall = true;
      const uint32_t resets = r.npu.resets;
      res = ex.execute(ConstByteView(stream.data(), 100));
      CHECK(res.status == St::Timeout && r.npu.resets == resets + 1 && ex.recoveries() == 1 && wall_calls == 1 && r.d().state() == IntelNpuDeviceState::Running);
      r.npu.jobs_stall = false;
      CHECK(ex.execute(ConstByteView(stream.data(), 100)).status == St::Ok);   // working again after the reset
      // Out of DMA memory for the stream.
      r.alloc.dma_budget = 0;
      CHECK(ex.execute(ConstByteView(stream.data(), 100)).status == St::NoMemory);
      r.alloc.dma_budget = -1;
      // poll(): looks at the device on an interrupt, now and then without one, and always when interrupts are not delivered.
      const uint32_t before = r.npu.tx_count;
      r.npu.icb_extra = 0;
      IntelNpuServiceResult polled = ex.poll();
      CHECK(!polled.handled);
      r.npu.icb_extra = 0x40000000;   // the management watchdog
      CHECK(!ex.poll().handled);      // no interrupt notified, idle count not reached: not looked at
      ex.notify_interrupt();
      polled = ex.poll();
      CHECK(polled.handled && polled.recovery_needed && ex.recoveries() == 2 && r.d().state() == IntelNpuDeviceState::Running && r.d().boot_count() >= 3);
      (void)before;
      r.npu.icb_extra = 0x40000000;
      IntelNpuExecutor::Config always = cfg; always.interrupts = false;
      IntelNpuExecutor polling(r.d(), r.alloc, always);
      CHECK(polling.poll().recovery_needed && polling.recoveries() == 1);
      // A suspended device cannot run work.
      CHECK(r.d().suspend() == IntelNpuDeviceError::None);
      CHECK(ex.execute(ConstByteView(stream.data(), 100)).status == St::NotRunning && !ex.poll().handled);
      CHECK(r.d().start() == IntelNpuDeviceError::None);
      CHECK(ex.execute(ConstByteView(stream.data(), 100)).status == St::Ok);   // the executor's context survived the power cycle
    }
    r.dev.reset();
    CHECK(r.alloc.dma.empty());
  }
  // Two threads: execute() from one, poll() from the other, take turns without breaking the device.
  {
    Rig r(0x643E);
    CHECK(r.boot());
    IntelNpuExecutor::Config cfg; cfg.wait_tries = 50; cfg.interrupts = false;
    IntelNpuExecutor ex(r.d(), r.alloc, cfg);
    std::vector<uint8_t> stream(3000, 1);
    std::atomic<bool> stop{false};
    std::thread poller([&] { while (!stop.load()) ex.poll(); });
    uint32_t ok = 0;
    for (int i = 0; i < 200; ++i) ok += ex.execute(ConstByteView(stream.data(), stream.size())).status == IntelNpuExecutor::Status::Ok;
    stop = true;
    poller.join();
    CHECK(ok == 200 && ex.jobs_run() == 200);
  }
}

static void test_chaos() {
  for (uint32_t seed = 1; seed <= 4; ++seed) {
    Rig r(kIds[seed % 4]);
    r.npu.ip.chaos = r.npu.btrs.chaos = true;
    r.npu.ip.rng = 0x1234567ull * seed; r.npu.btrs.rng = 0x7654321ull * seed;
    (void)r.d().load_firmware(r.fw());
    (void)r.d().start();
    (void)r.d().service();
    uint32_t ssid, queue, job;
    if (r.d().create_context(&ssid) == IntelNpuDeviceError::None) {
      (void)r.d().create_queue(ssid, 1, &queue);
      (void)r.d().submit(ssid, 1, IntelNpuJob(), &job);
    }
    (void)r.d().suspend();
    (void)r.d().recover();
    r.dev.reset();
    CHECK(r.alloc.dma.empty());
  }
  // Registers that go bad after a good boot.
  Rig r(0x643E);
  CHECK(r.boot());
  uint32_t ssid, queue, job;
  CHECK(r.d().create_context(&ssid) == IntelNpuDeviceError::None && r.d().create_queue(ssid, 1, &queue) == IntelNpuDeviceError::None);
  r.npu.ip.chaos = r.npu.btrs.chaos = true;
  for (int i = 0; i < 5; ++i) { (void)r.d().service(); (void)r.d().submit(ssid, queue, IntelNpuJob(), &job); }
  (void)r.d().destroy_context(ssid);
  r.dev.reset();
  CHECK(r.alloc.dma.empty());
}

int main() {
  test_load_errors();
  test_boot_each_platform();
  test_contexts_and_jobs();
  test_faults();
  test_suspend_resume_recover();
  test_boot_failures();
  test_limits();
  test_firmware_file();
  test_executor();
  test_chaos();
  if (g_failed) { printf("IntelNpuDeviceTest: %d failed\n", g_failed); return 1; }
  printf("IntelNpuDeviceTest: ok\n");
  return 0;
}
