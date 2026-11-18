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

// Host-side tests for the native NVIDIA driver's memory, MMU, channel and copy-engine code (Nvkm/NvFb, NvInstmem, NvMmu, NvBar,
// NvFifo, NvCe), against a model GPU: a register file, VRAM behind the PRAMIN window and the BAR1/BAR2 apertures (translated by
// walking the page tables the driver wrote, independently of NvMmu's own walker), the host (runlists, RAMFC, USERD, GPFIFO, push
// buffer methods) and a copy engine that executes copies, fills and semaphore releases on the model's VRAM and system memory.
//
// Covered: VRAM size decoding per family (and garbage register values, and random bit flips of good ones), the VRAM allocator
// (alignment, fragmentation, exhaustion, double release, several threads at once), PRAMIN window moves (also from several threads),
// the page-table words of each format against hand-computed values, BAR setup register sequences and instance blocks, BAR2 slot
// access, channel instance/RAMFC contents, GPFIFO submission with wrap, copy + fill + semaphore completing on Kepler, Maxwell, Pascal,
// Volta and Turing, a hung engine (every wait times out instead of hanging), and Ampere being refused.
#include <stdio.h>
#include <stdlib.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvBar.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvCe.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFb.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvFifo.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvInstmem.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMmu.hpp"

void bounds_violation(const char* what, uint64_t index, uint64_t size) {
  fprintf(stderr, "BOUNDS VIOLATION: %s (index %llu, size %llu)\n", what, (unsigned long long)index, (unsigned long long)size);
  abort();
}

using namespace hardware;
using namespace hardware::nv;

static std::atomic<int> g_failed{0};
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

static Chip make_chip(Family family, uint32_t chipset, const char* name) {
  Chip c;
  c.family = family;
  c.chipset = chipset;
  c.name = name;
  c.boot0 = chipset << 20;
  c.needs_gsp = family >= Family::Ampere;
  c.display_native = family >= Family::Tesla && family <= Family::Turing;
  return c;
}

struct StdLock final : INvLock {
  std::mutex m;
  void lock() override { m.lock(); }
  void unlock() override { m.unlock(); }
};

constexpr uint64_t kMiB = 1ull << 20;
constexpr uint64_t kSysBase = 0x100000000ull;   // bus addresses of the fake system memory

// ---------------------------------------------------------------------------------------------------------------------------------
// System memory: a contiguous DMA buffer with a CPU view (what the kernel's DMA allocator would give).
struct FakeSystemMemory final : INvMemory {
  std::vector<uint8_t> bytes = std::vector<uint8_t>(4 * kMiB);
  uint64_t next = 0;
  GpuMemory allocate(MemoryTarget target, uint64_t size, uint64_t alignment) override {
    if (target == MemoryTarget::Vram || size == 0) return {};
    if (alignment < 4096) alignment = 4096;
    const uint64_t at = (next + alignment - 1) & ~(alignment - 1);
    const uint64_t rounded = (size + 4095) & ~uint64_t{4095};
    if (at + rounded > bytes.size()) return {};
    next = at + rounded;
    GpuMemory m;
    m.gpu_address = kSysBase + at;
    m.bytes = rounded;
    m.target = target;
    m.cpu = ByteView(bytes.data() + at, rounded);
    return m;
  }
  void release(GpuMemory& m) override { m = GpuMemory{}; }
  void flush(const GpuMemory&) override {}
  uint64_t vram_bytes() const override { return 0; }
};

// ---------------------------------------------------------------------------------------------------------------------------------
// The model GPU.
struct FakeGpu final : INvRegisters {
  Chip chip;
  std::mutex mu;
  std::vector<uint8_t> vram;
  FakeSystemMemory* sys = nullptr;
  std::unordered_map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> log;       // every register write except through the PRAMIN window
  std::vector<std::string> errors;                     // things the model saw the driver do wrong
  uint32_t window = 0;                                 // 0x001700
  uint32_t pbdma_present = 0x7;
  bool hung = false;                                   // the host and copy engine do nothing, preempts never finish
  bool chan_enabled[64] = {};
  std::map<uint32_t, std::set<uint32_t>> runlist_channels;   // runlist -> channel ids, as last committed
  uint32_t launches = 0, invalidates = 0, doorbells = 0, commits = 0;
  struct CeState {
    uint32_t cls[8] = {};
    std::unordered_map<uint32_t, uint32_t> m;
  } ce[64];

  explicit FakeGpu(const Chip& c, uint64_t vram_bytes) : chip(c), vram(vram_bytes) {}

  bool volta_plus() const { return chip.family >= Family::Volta; }
  bool turing() const { return chip.family == Family::Turing; }
  bool gp100() const { return chip.family >= Family::Pascal; }

  uint32_t reg(uint32_t off) { auto it = regs.find(off); return it == regs.end() ? 0 : it->second; }

  // -- physical memory --
  bool phys32(uint32_t aper, uint64_t addr, uint32_t* value, bool write) {
    uint8_t* p = nullptr;
    if (aper == 0) {
      if (addr % 4 || addr + 4 > vram.size()) return false;
      p = vram.data() + addr;
    } else if ((aper == 2 || aper == 3) && sys) {
      if (addr < kSysBase || addr % 4 || addr - kSysBase + 4 > sys->bytes.size()) return false;
      p = sys->bytes.data() + (addr - kSysBase);
    } else {
      return false;
    }
    if (write) for (int i = 0; i < 4; ++i) p[i] = uint8_t(*value >> (8 * i));
    else *value = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
    return true;
  }
  uint32_t v32(uint64_t addr) { uint32_t v = 0; if (!phys32(0, addr, &v, false)) errors.push_back("VRAM read out of range"); return v; }
  uint64_t v64(uint64_t addr) { return uint64_t(v32(addr)) | uint64_t(v32(addr + 4)) << 32; }
  void w32(uint64_t addr, uint32_t v) { if (!phys32(0, addr, &v, true)) errors.push_back("VRAM write out of range"); }

  // -- the MMU, walked from an instance block (written separately from NvMmu's translate) --
  bool walk(uint64_t inst, uint64_t va, uint32_t* aper, uint64_t* pa) {
    const uint64_t pdb = v64(inst + 0x200);
    if (pdb & 3) return false;  // the model keeps page directories in VRAM
    uint64_t table = pdb & ~0xFFFull;
    if (gp100()) {
      if (!(pdb & (1u << 10))) return false;
      const int shifts[3] = {47, 38, 29}, bits[3] = {2, 9, 9};
      for (int l = 0; l < 3; ++l) {
        const uint64_t pde = v64(table + ((va >> shifts[l]) & ((1ull << bits[l]) - 1)) * 8);
        if (((pde >> 1) & 3) != 1) return false;
        table = ((pde >> 8) & ((1ull << 46) - 1)) << 12;
      }
      const uint64_t base = table + ((va >> 21) & 0xFF) * 16;
      const uint64_t small = v64(base + 8), big = v64(base);
      if (((small >> 1) & 3) == 1) {
        const uint64_t pte = v64((((small >> 8) & ((1ull << 46) - 1)) << 12) + ((va >> 12) & 0x1FF) * 8);
        if (pte & 1) { *aper = (pte >> 1) & 3; *pa = (((pte >> 8) & ((1ull << 46) - 1)) << 12) | (va & 0xFFF); return true; }
      }
      if (((big >> 1) & 3) == 1) {
        const uint64_t pte = v64((((big >> 8) & ((1ull << 46) - 1)) << 12) + ((va >> 16) & 0x1F) * 8);
        if (pte & 1) { *aper = (pte >> 1) & 3; *pa = (((pte >> 8) & ((1ull << 46) - 1)) << 12) | (va & 0xFFFF); return true; }
      }
      return false;
    }
    // GF100 with 128 KiB big pages.
    const uint64_t pde = v64(table + ((va >> 27) & 0x1FFF) * 8);
    const uint32_t lo = uint32_t(pde), hi = uint32_t(pde >> 32);
    if ((hi & 3) == 1) {
      const uint64_t pte = v64((uint64_t(hi & ~0xFu) << 8) + ((va >> 12) & 0x7FFF) * 8);
      if (pte & 1) { *aper = (pte >> 33) & 3; *pa = ((pte & 0xFFFFFFF0ull) << 8) | (va & 0xFFF); return true; }
    }
    if ((lo & 3) == 1) {
      const uint64_t pte = v64((uint64_t(lo & ~0xFu) << 8) + ((va >> 17) & 0x3FF) * 8);
      if (pte & 1) { *aper = (pte >> 33) & 3; *pa = ((pte & 0xFFFFFFF0ull) << 8) | (va & 0x1FFFF); return true; }
    }
    return false;
  }
  bool virt32(uint64_t inst, uint64_t va, uint32_t* value, bool write) {
    uint32_t aper = 0;
    uint64_t pa = 0;
    if (!walk(inst, va, &aper, &pa)) { errors.push_back("GPU page fault"); return false; }
    if (!phys32(aper, pa, value, write)) { errors.push_back("GPU access outside memory"); return false; }
    return true;
  }
  bool virt8(uint64_t inst, uint64_t va, uint8_t* value, bool write) {
    uint32_t word = 0;
    if (!virt32(inst, va & ~3ull, &word, false)) return false;
    const int shift = int(va & 3) * 8;
    if (!write) { *value = uint8_t(word >> shift); return true; }
    word = (word & ~(0xFFu << shift)) | uint32_t(*value) << shift;
    return virt32(inst, va & ~3ull, &word, true);
  }

  // -- the BARs --
  uint32_t bar_inst_reg(int bar) const { return turing() ? (bar == 1 ? 0xb80f40 : 0xb80f48) : (bar == 1 ? 0x001704 : 0x001714); }
  bool bar_access(int bar, uint64_t off, uint32_t* value, bool write) {
    std::lock_guard<std::mutex> g(mu);
    const uint32_t r = reg(bar_inst_reg(bar));
    bool ok;
    if (!(r & 0x80000000u)) {
      if (bar == 2) { errors.push_back("BAR2 used before it was set up"); return false; }
      ok = phys32(0, off, value, write);   // BAR1 before setup: VRAM linearly (the firmware's frame buffer)
    } else {
      ok = virt32(uint64_t(r & 0x0FFFFFFF) << 12, off, value, write);
    }
    if (ok && write && bar == 1 && !volta_plus() && (reg(0x002254) & 0x10000000)) {
      const uint64_t userd = uint64_t(reg(0x002254) & 0x0FFFFFFF) << 12;
      if (off >= userd && off < userd + 64 * 0x200 && (off - userd) % 0x200 == 0x8c) kick(uint32_t((off - userd) / 0x200));
    }
    return ok;
  }

  // -- the host --
  void commit_runlist(uint32_t rl, uint64_t addr, uint32_t count) {
    ++commits;
    std::set<uint32_t>& chans = runlist_channels[rl];
    chans.clear();
    if (!volta_plus()) {
      for (uint32_t i = 0; i < count; ++i) chans.insert(v32(addr + i * 8) & 0xFFF);
      return;
    }
    for (uint32_t i = 0; i < count;) {
      const uint64_t e = addr + i * 16;
      if (!(v32(e) & 1)) { errors.push_back("runlist: channel without a TSG header"); return; }
      const uint32_t n = v32(e + 4);
      if (n == 0 || i + 1 + n > count) { errors.push_back("runlist: bad TSG channel count"); return; }
      for (uint32_t c = 0; c < n; ++c) {
        const uint64_t ce_ = e + 16 * (c + 1);
        if (v32(ce_) & 1) { errors.push_back("runlist: TSG where a channel was expected"); return; }
        const uint32_t id = v32(ce_ + 8) & 0xFFF;
        const uint64_t inst = (uint64_t(v32(ce_ + 8)) & ~0xFFFull) | uint64_t(v32(ce_ + 12)) << 32;
        if (inst != (uint64_t(reg(0x800000 + id * 8) & 0x0FFFFFFF) << 12)) errors.push_back("runlist: instance does not match the bound one");
        chans.insert(id);
      }
      i += 1 + n;
    }
  }
  bool on_runlist(uint32_t id) {
    for (auto& [rl, set] : runlist_channels) if (set.count(id)) return true;
    return false;
  }

  void kick(uint32_t id) {
    if (hung) return;
    if (id >= 64) { errors.push_back("kick: bad channel id"); return; }
    const uint32_t bind = reg(0x800000 + id * 8);
    if (!(bind & 0x80000000u)) { errors.push_back("kick: channel not bound"); return; }
    if (!chan_enabled[id]) { errors.push_back("kick: channel not enabled"); return; }
    if (!on_runlist(id)) { errors.push_back("kick: channel not on a runlist"); return; }
    const uint64_t inst = uint64_t(bind & 0x0FFFFFFF) << 12;
    if (v32(inst + 0x10) != 0xface) errors.push_back("RAMFC: missing 0xface");
    if (v32(inst + 0xe8) != id) errors.push_back("RAMFC: wrong channel id");
    const uint64_t userd = v64(inst + 0x08);
    const uint64_t gpfifo = v32(inst + 0x48) | uint64_t(v32(inst + 0x4c) & 0xFF) << 32;
    const uint32_t entries = 1u << ((v32(inst + 0x4c) >> 16) & 0x1F);
    const uint32_t put = v32(userd + 0x8c);
    uint32_t get = v32(userd + 0x88);
    if (put >= entries) { errors.push_back("GP_PUT beyond the ring"); return; }
    while (get != put) {
      uint32_t lo = 0, hi = 0;
      if (!virt32(inst, gpfifo + get * 8, &lo, false) || !virt32(inst, gpfifo + get * 8 + 4, &hi, false)) return;
      const uint64_t pb = (lo & ~3ull) | uint64_t(hi & 0xFF) << 32;
      run_push(id, inst, pb, (hi >> 10) & 0x1FFFFF);
      get = (get + 1) % entries;
      w32(userd + 0x88, get);
    }
  }

  void run_push(uint32_t id, uint64_t inst, uint64_t va, uint32_t words) {
    uint32_t i = 0;
    auto word = [&](uint32_t at, uint32_t* v) { return virt32(inst, va + at * 4ull, v, false); };
    while (i < words) {
      uint32_t h = 0;
      if (!word(i++, &h)) return;
      const uint32_t type = h >> 29, subc = (h >> 13) & 7;
      uint32_t mthd = (h & 0x1FFF) << 2;
      if (type == 4) { method(id, inst, subc, mthd, (h >> 16) & 0x1FFF); continue; }
      const uint32_t count = (h >> 16) & 0x1FFF;
      for (uint32_t c = 0; c < count; ++c) {
        uint32_t v = 0;
        if (i >= words || !word(i++, &v)) { errors.push_back("push buffer: method data past the entry"); return; }
        method(id, inst, subc, mthd, v);
        if (type == 1 || (type == 5 && c == 0)) mthd += 4;
        else if (type != 3) { errors.push_back("push buffer: unknown header"); return; }
      }
    }
  }

  void method(uint32_t id, uint64_t inst, uint32_t subc, uint32_t mthd, uint32_t v) {
    CeState& s = ce[id];
    if (mthd == 0) { s.cls[subc] = v; return; }
    if (mthd < 0x100) return;   // other host methods
    if (s.cls[subc] != copy_engine_class(chip)) { errors.push_back("method on a subchannel without the copy class"); return; }
    s.m[mthd] = v;
    if (mthd == ce::kLaunchDma) launch(s, inst, v);
  }

  void launch(CeState& s, uint64_t inst, uint32_t flags) {
    ++launches;
    if ((flags & 3) == 0) return;
    if (!(flags & ce::kSrcPitch) || !(flags & ce::kDstPitch)) { errors.push_back("launch: block-linear layout"); return; }
    if (flags & ce::kMultiLine) { errors.push_back("launch: multi-line not modelled"); return; }
    const uint64_t src = uint64_t(s.m[ce::kOffsetInUpper] & 0x1FFFF) << 32 | s.m[ce::kOffsetInLower];
    const uint64_t dst = uint64_t(s.m[ce::kOffsetOutUpper] & 0x1FFFF) << 32 | s.m[ce::kOffsetOutLower];
    const uint32_t length = s.m[ce::kLineLengthIn];
    if (flags & ce::kRemapEnable) {
      const uint32_t comps = s.m[ce::kRemapComponents];
      if ((comps & 7) != ce::kRemapDstXConstA || ((comps >> 16) & 3) != 3 || ((comps >> 24) & 3) != 0) {
        errors.push_back("launch: remap not a 32-bit constant fill");
        return;
      }
      uint32_t value = s.m[ce::kRemapConstA];
      for (uint32_t i = 0; i < length; ++i)
        if (!virt32(inst, dst + i * 4ull, &value, true)) return;
    } else {
      for (uint32_t i = 0; i < length; ++i) {
        uint8_t b = 0;
        if (!virt8(inst, src + i, &b, false) || !virt8(inst, dst + i, &b, true)) return;
      }
    }
    if (((flags >> 3) & 3) == 1) {
      uint32_t payload = s.m[ce::kSemaphorePayload];
      const uint64_t sem = uint64_t(s.m[ce::kSemaphoreA] & 0x1FFFF) << 32 | s.m[ce::kSemaphoreB];
      (void)virt32(inst, sem, &payload, true);
    }
  }

  // -- registers --
  uint32_t read32(uint32_t off) override {
    std::lock_guard<std::mutex> g(mu);
    if (off >= 0x700000 && off < 0x800000) {
      uint32_t v = 0xFFFFFFFFu;
      const uint64_t addr = (uint64_t(window) << 16) + (off - 0x700000);
      (void)phys32(0, addr, &v, false);
      return v;
    }
    if (off >= 0x800000 && off < 0x800000 + 64 * 8 && off % 8 == 4) return chan_enabled[(off - 0x800004) / 8] ? 1 : 0;
    switch (off) {
      case 0x100c80: return chip.family == Family::Tesla ? (reg(off) & ~1u) : (reg(off) | 0x00FF8000u);
      case 0xb830b0: return reg(off) & ~0x80000000u;
      case 0x070000: return reg(off) & ~2u;
      case 0x001710: case 0xb80f50: return 0;
      case 0x002634: return hung ? (reg(off) | 0x00100000u) : (reg(off) & ~0x00100000u);
      case 0x000204: return reg(off) & pbdma_present;
      default: break;
    }
    if (off >= 0x002284 && off < 0x002284 + 16 * 8) return 0;
    return reg(off);
  }

  void write32(uint32_t off, uint32_t v) override {
    std::lock_guard<std::mutex> g(mu);
    if (off >= 0x700000 && off < 0x800000) {
      const uint64_t addr = (uint64_t(window) << 16) + (off - 0x700000);
      if (!phys32(0, addr, &v, true)) errors.push_back("PRAMIN write beyond VRAM");
      return;
    }
    log.emplace_back(off, v);
    if (off >= 0x800000 && off < 0x800000 + 64 * 8 && off % 8 == 4) {
      const uint32_t id = (off - 0x800004) / 8;
      if (v & 0x400) chan_enabled[id] = true;
      if (v & 0x800) chan_enabled[id] = false;
      return;
    }
    regs[off] = v;
    if (off == 0x001700) window = v;
    if (off == 0x100cbc || off == 0xb830b0) ++invalidates;
    if (off == 0x810090) { ++doorbells; if (volta_plus()) kick(v); else errors.push_back("doorbell before Volta"); }
    if (off == 0x002274) commit_runlist((v >> 20) & 0xF, uint64_t(reg(0x002270) & 0x0FFFFFFF) << 12, v & 0xFFFF);
    if (off >= 0x002b00 && off < 0x002c00 && off % 0x10 == 8) {
      const uint32_t base = off - 8;
      commit_runlist((base - 0x002b00) / 0x10, reg(base) | uint64_t(reg(base + 4)) << 32, v);
    }
  }

  std::vector<std::pair<uint32_t, uint32_t>> writes_to(std::initializer_list<uint32_t> offsets) {
    std::lock_guard<std::mutex> g(mu);
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (auto& w : log)
      for (uint32_t o : offsets) if (w.first == o) out.push_back(w);
    return out;
  }
  void set(uint32_t off, uint32_t v) { std::lock_guard<std::mutex> g(mu); regs[off] = v; }
  bool clean() { std::lock_guard<std::mutex> g(mu); for (auto& e : errors) printf("  model: %s\n", e.c_str()); return errors.empty(); }
};

struct FakeAperture final : INvAperture {
  FakeGpu& gpu;
  int bar;
  uint64_t size;
  FakeAperture(FakeGpu& g, int b, uint64_t s) : gpu(g), bar(b), size(s) {}
  uint32_t read32(uint64_t off) override { uint32_t v = 0xFFFFFFFFu; if (off + 4 > size || !gpu.bar_access(bar, off, &v, false)) return 0xFFFFFFFFu; return v; }
  void write32(uint64_t off, uint32_t v) override { if (off + 4 <= size) (void)gpu.bar_access(bar, off, &v, true); }
  uint64_t bytes() const override { return size; }
};

// Board registers for a 64 MiB board of `family`, as each family reports it.
static void set_vram_registers(FakeGpu& g, Family f) {
  switch (f) {
    case Family::Tesla: g.set(0x10020c, 64 * kMiB); g.set(0x100714, 2); break;
    case Family::Fermi: case Family::Kepler:
      g.set(0x022438, 2); g.set(0x022554, 0); g.set(0x11020c, 32); g.set(0x11120c, 32); break;
    case Family::Maxwell:
      g.set(0x022438, 2); g.set(0x022458, 1); g.set(0x021d38, 0); g.set(0x021c14, 0); g.set(0x022450, 2);
      g.set(0x11020c, 32); g.set(0x11120c, 32); break;
    default:
      g.set(0x02243c, 2); g.set(0x021c14, 0); g.set(0x90020c, 32); g.set(0x90420c, 32); g.set(0x1183a4, 64); break;
  }
}

// PTOP: GR on runlist 0, one copy engine on runlist 2 (CE0 before Pascal, LCE instance 0 from Pascal on).
static void set_topology(FakeGpu& g, Family f) {
  const uint32_t chained = 0x80000000u;
  std::vector<uint32_t> e;
  e.push_back(2 | 0x10 | (0u << 21) | chained);                   // GR: runlist 0
  e.push_back(3 | (kTopTypeGr << 2));
  if (f >= Family::Pascal) {
    e.push_back(1 | (0u << 26) | 0x00104000 | chained);           // DATA: instance 0, an engine base
    e.push_back(2 | 0x10 | (2u << 21) | 0x04 | (21u << 9) | chained);
    e.push_back(3 | (kTopTypeLce << 2));
  } else {
    e.push_back(2 | 0x10 | (2u << 21) | chained);                 // no reset field: Kepler's fixed bits are used
    e.push_back(3 | (kTopTypeCe0 << 2));
  }
  for (uint32_t i = 0; i < e.size(); ++i) g.set(0x022700 + i * 4, e[i]);
}

// ---------------------------------------------------------------------------------------------------------------------------------
// The driver's memory stack on a model, as the kernel would build it.
struct Rig {
  Chip chip;
  FakeGpu gpu;
  FakeSystemMemory sys;
  StdLock fb_lock, pramin_lock, tlb_lock, bar1_lock, bar2_lock, inst_lock, fifo_lock, vmm_lock;
  NvTimeouts timeouts;
  VramProbe probe;
  NvFb fb{fb_lock};
  std::unique_ptr<PraminWindow> pramin;
  std::unique_ptr<NvVramMemory> pmem;    // over PRAMIN: page tables, BAR instance blocks
  std::unique_ptr<NvTlb> tlb;
  std::unique_ptr<NvBar> bar;
  std::unique_ptr<NvInstmem> instmem;
  std::unique_ptr<NvVramMemory> mem;     // over NvInstmem: everything else
  std::unique_ptr<FakeAperture> bar1ap, bar2ap;
  std::unique_ptr<NvFifo> fifo;

  Rig(const Chip& c, NvTimeouts t = {}) : chip(c), gpu(c, 64 * kMiB), timeouts(t) {
    gpu.sys = &sys;
    set_vram_registers(gpu, c.family);
    set_topology(gpu, c.family);
    probe = probe_vram(gpu, chip);
    if (!probe.ok() || !fb.init(probe.info)) return;
    pramin = std::make_unique<PraminWindow>(gpu, probe.info.bytes, pramin_lock);
    pmem = std::make_unique<NvVramMemory>(fb, *pramin, &sys);
    tlb = std::make_unique<NvTlb>(gpu, chip, tlb_lock, t);
    bar = std::make_unique<NvBar>(gpu, chip, *pmem, *tlb, bar1_lock, bar2_lock, t);
    instmem = std::make_unique<NvInstmem>(*pramin, probe.info.bytes, inst_lock);
    mem = std::make_unique<NvVramMemory>(fb, *instmem, &sys);
    bar1ap = std::make_unique<FakeAperture>(gpu, 1, 256 * kMiB);
    bar2ap = std::make_unique<FakeAperture>(gpu, 2, 32 * kMiB);
    fifo = std::make_unique<NvFifo>(gpu, chip, *mem, bar.get(), bar1ap.get(), fifo_lock, t);
  }
  ~Rig() {
    fifo.reset();
    if (instmem) instmem->disable_bar2();
  }
  bool bring_up() {
    if (!fb_init_mmu(gpu, chip, 17, 0x200000)) return false;
    if (bar->init(256 * kMiB, 32 * kMiB, 1 * kMiB) != BarStatus::Ok) return false;
    if (!instmem->enable_bar2(*bar, *bar2ap, 4)) return false;
    return fifo->init() == FifoStatus::Ok;
  }
};

// =================================================================================================================================
static void test_vram_probe() {
  struct Case { Family f; uint32_t chipset; } cases[] = {
      {Family::Tesla, 0x84}, {Family::Fermi, 0xc4}, {Family::Kepler, 0xe4}, {Family::Maxwell, 0x124},
      {Family::Pascal, 0x134}, {Family::Volta, 0x140}, {Family::Turing, 0x164},
  };
  for (const Case& c : cases) {
    const Chip chip = make_chip(c.f, c.chipset, "x");
    FakeGpu g(chip, 0);
    set_vram_registers(g, c.f);
    const VramProbe p = probe_vram(g, chip);
    CHECK(p.ok());
    CHECK(p.info.bytes == 64 * kMiB);
    CHECK(p.info.reserved_head == 256 * 1024 && p.info.reserved_tail == kMiB);
    CHECK(p.info.usable_bytes() == 64 * kMiB - 256 * 1024 - kMiB);
    if (c.f != Family::Tesla) CHECK(p.info.partitions == 2 && !p.info.uneven && p.info.partition_bytes == 32 * kMiB);
  }
  // Tesla: the size register's low byte carries address bits 39:32; the type comes from 0x100714.
  {
    const Chip chip = make_chip(Family::Tesla, 0x50, "G80");
    FakeGpu g(chip, 0);
    g.set(0x10020c, 0x40000001);   // 4 GiB + 1 GiB
    g.set(0x100714, 4);
    const VramProbe p = probe_vram(g, chip);
    CHECK(p.ok() && p.info.bytes == 5ull << 30 && p.info.type == RamType::Gddr5 && !p.info.type_guessed);
  }
  // Fermi/Kepler: a fused-off partition is skipped; uneven partitions are reported.
  {
    const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
    FakeGpu g(chip, 0);
    g.set(0x022438, 4); g.set(0x022554, 0x2);
    g.set(0x11020c, 512); g.set(0x11120c, 0xFFFF); g.set(0x11220c, 512); g.set(0x11320c, 256);
    const VramProbe p = probe_vram(g, chip);
    CHECK(p.ok() && p.info.bytes == 1280 * kMiB && p.info.partitions == 3 && p.info.uneven && p.info.partition_bytes == 256 * kMiB);
    CHECK(p.info.type == RamType::Gddr5 && p.info.type_guessed);
    const VramProbe hinted = probe_vram(g, chip, RamType::Gddr3);
    CHECK(hinted.ok() && hinted.info.type == RamType::Gddr3 && !hinted.info.type_guessed);
  }
  // Maxwell: FBPAs per FBP, disabled FBPs and FBPAs.
  {
    const Chip chip = make_chip(Family::Maxwell, 0x124, "GM204");
    FakeGpu g(chip, 0);
    g.set(0x022438, 4); g.set(0x022458, 2); g.set(0x021d38, 0x8); g.set(0x021c14, 0x2);
    for (uint32_t i = 0; i < 8; ++i) g.set(0x11020c + i * 0x1000, 512);
    const VramProbe p = probe_vram(g, chip);   // FBPs 0..2 (FBP 3 off) = FBPAs 0..5, FBPA 1 off: 5 * 512 MiB
    CHECK(p.ok() && p.info.bytes == 2560 * kMiB && p.info.partitions == 5);
  }
  // Turing: the firmware's usable size wins when smaller; the display VGA workspace widens the reserved tail.
  {
    const Chip chip = make_chip(Family::Turing, 0x164, "TU104");
    FakeGpu g(chip, 0);
    g.set(0x02243c, 8); g.set(0x021c14, 0);
    for (uint32_t i = 0; i < 8; ++i) g.set(0x90020c + i * 0x4000, 1024);
    g.set(0x1183a4, 8000);
    g.set(0x625f04, uint32_t(((8000 * kMiB) - 3 * kMiB) >> 8) | 8);
    const VramProbe p = probe_vram(g, chip);
    CHECK(p.ok() && p.info.bytes == 8000 * kMiB && p.info.type == RamType::Gddr6);
    CHECK(p.info.reserved_tail == 3 * kMiB);
  }
  // Ampere: identified, size reported, refused.
  {
    const Chip chip = make_chip(Family::Ampere, 0x172, "GA102");
    FakeGpu g(chip, 0);
    g.set(0x1183a4, 24576);
    const VramProbe p = probe_vram(g, chip);
    CHECK(!p.ok() && p.error == FbError::NeedsGsp && p.info.bytes == 24ull << 30);
  }
  // Unknown family.
  {
    const Chip chip = make_chip(Family::Nv40, 0x40, "NV40");
    FakeGpu g(chip, 0);
    CHECK(probe_vram(g, chip).error == FbError::Unsupported);
  }
}

static void test_vram_garbage() {
  const Family families[] = {Family::Tesla, Family::Kepler, Family::Maxwell, Family::Pascal, Family::Turing};
  for (Family f : families) {
    const Chip chip = make_chip(f, f == Family::Tesla ? 0x84 : f == Family::Kepler ? 0xe4 : f == Family::Maxwell ? 0x124 : f == Family::Pascal ? 0x134 : 0x164, "x");
    // A GPU that fell off the bus reads all ones everywhere.
    {
      struct Ones final : INvRegisters { uint32_t read32(uint32_t) override { return 0xFFFFFFFFu; } void write32(uint32_t, uint32_t) override {} } ones;
      const VramProbe p = probe_vram(ones, chip);
      CHECK(!p.ok() && p.error == FbError::Garbage && p.info.bytes == 0);
    }
    // All zeros: no memory.
    {
      FakeGpu g(chip, 0);
      const VramProbe p = probe_vram(g, chip);
      CHECK(!p.ok() && (p.error == FbError::NoMemory || p.error == FbError::Garbage));
    }
    // Random bit flips of a good configuration: never out of bounds, never an implausible size.
    std::mt19937 rng(1234 + int(f));
    for (int iter = 0; iter < 3000; ++iter) {
      FakeGpu g(chip, 0);
      set_vram_registers(g, f);
      std::vector<uint32_t> keys;
      for (auto& kv : g.regs) keys.push_back(kv.first);
      std::sort(keys.begin(), keys.end());
      const uint32_t key = keys[rng() % keys.size()];
      g.regs[key] ^= 1u << (rng() % 32);
      if (rng() % 4 == 0) g.regs[keys[rng() % keys.size()]] = rng();
      const VramProbe p = probe_vram(g, chip);
      if (p.ok()) CHECK(p.info.bytes >= 8 * kMiB && p.info.bytes <= (256ull << 30) && p.info.bytes % kMiB == 0 && p.info.usable_bytes() > 0);
      else CHECK(p.info.bytes == 0);
    }
  }
  // Too many partitions and absurd partition sizes.
  const Chip kepler = make_chip(Family::Kepler, 0xe4, "GK104");
  FakeGpu g(kepler, 0);
  g.set(0x022438, 0x40); g.set(0x022554, 0);
  CHECK(probe_vram(g, kepler).error == FbError::Garbage);
  g.set(0x022438, 1); g.set(0x11020c, 0x7FFFFFFF);
  CHECK(probe_vram(g, kepler).error == FbError::Garbage);
  g.set(0x11020c, 4);   // 4 MiB: below anything NVIDIA shipped
  CHECK(probe_vram(g, kepler).error == FbError::Garbage);
}

static VramInfo info_of(uint64_t bytes) {
  VramInfo i;
  i.bytes = bytes;
  i.reserved_head = 256 * 1024;
  i.reserved_tail = kMiB;
  return i;
}

static void test_allocator() {
  StdLock lock;
  NvFb fb(lock);
  CHECK(fb.init(info_of(64 * kMiB)));
  const uint64_t usable = 64 * kMiB - 256 * 1024 - kMiB;
  CHECK(fb.free_bytes() == usable);
  // Alignment, and nothing in the reserved regions.
  const uint64_t aligns[] = {256, 4096, 65536, 1 << 20, 2 << 20};
  std::vector<uint64_t> got;
  for (uint64_t a : aligns) {
    const Optional<uint64_t> at = fb.allocate(1000, a);
    CHECK(at && *at % a == 0 && *at >= 256 * 1024 && *at + 1000 <= 64 * kMiB - kMiB);
    if (at) got.push_back(*at);
  }
  CHECK(!fb.allocate(1000, 3000));   // not a power of two
  CHECK(!fb.allocate(0, 4096));
  CHECK(!fb.allocate(~0ull, 4096));
  for (uint64_t a : got) CHECK(fb.release(a));
  CHECK(fb.free_bytes() == usable && fb.allocation_count() == 0);
  // Double release and releasing an address never handed out.
  const Optional<uint64_t> a = fb.allocate(4096, 4096);
  CHECK(a && fb.release(*a));
  CHECK(!fb.release(*a));
  CHECK(!fb.release(*a + 256));
  CHECK(fb.bad_releases() == 2 && fb.free_bytes() == usable);
  // Exhaustion.
  std::vector<uint64_t> blocks;
  for (;;) {
    const Optional<uint64_t> b = fb.allocate(kMiB, kMiB);
    if (!b) break;
    blocks.push_back(*b);
  }
  CHECK(blocks.size() == 62);   // usable is [256 KiB, 63 MiB): the 1 MiB-aligned blocks start at 1 MiB .. 62 MiB
  CHECK(!fb.allocate(kMiB, kMiB));
  CHECK(fb.allocate(64 * 1024, 4096));   // the space below 1 MiB is still there for small pieces
  // Fragmentation: free every other MiB, a 2 MiB request cannot be met until neighbours are freed too.
  for (size_t i = 0; i < blocks.size(); i += 2) CHECK(fb.release(blocks[i]));
  CHECK(!fb.allocate(2 * kMiB, 4096));
  for (size_t i = 1; i < blocks.size(); i += 2) CHECK(fb.release(blocks[i]));
  const Optional<uint64_t> big = fb.allocate(60 * kMiB, kMiB);
  CHECK(big.has_value());
  // The free-range table filling up must fail cleanly: a tiny allocator with 4 records.
  RangeAllocator<4> tiny;
  CHECK(tiny.reset(0, 1 << 20));
  std::vector<uint64_t> t;
  for (int i = 0; i < 6; ++i) { auto r = tiny.allocate(4096, 4096); if (r) t.push_back(*r); }
  CHECK(t.size() == 4 && tiny.allocation_count() == 4);
  CHECK(tiny.release(t[1]) && tiny.release(t[3]) && !tiny.release(t[3]));
  CHECK(tiny.reserve(512 * 1024, 4096) && !tiny.reserve(512 * 1024, 4096));
  CHECK(tiny.release(t[0]) && tiny.release(t[2]));
  CHECK(tiny.allocation_count() == 0 && tiny.free_bytes() == (1 << 20) - 4096);
}

// Several threads allocating and releasing at once; an ownership map at 256-byte granularity catches any overlap.
static void test_allocator_threads() {
  StdLock lock;
  NvFb fb(lock);
  CHECK(fb.init(info_of(64 * kMiB)));
  std::vector<std::atomic<uint8_t>> owner(64 * kMiB / 256);
  std::atomic<int> overlaps{0}, misaligned{0}, outside{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(77 + t);
      std::vector<std::pair<uint64_t, uint64_t>> mine;
      for (int i = 0; i < 3000; ++i) {
        if (mine.size() < 16 && rng() % 3) {
          const uint64_t bytes = 256 + rng() % (256 * 1024);
          const uint64_t align = 256ull << (rng() % 8);
          const Optional<uint64_t> at = fb.allocate(bytes, align);
          if (!at) continue;
          if (*at % align) ++misaligned;
          if (*at < 256 * 1024 || *at + bytes > 63 * kMiB) ++outside;
          for (uint64_t u = *at / 256; u < (*at + bytes + 255) / 256; ++u) {
            uint8_t expect = 0;
            if (!owner[u].compare_exchange_strong(expect, uint8_t(t + 1))) ++overlaps;
          }
          mine.emplace_back(*at, bytes);
        } else if (!mine.empty()) {
          const size_t k = rng() % mine.size();
          const auto [at, bytes] = mine[k];
          for (uint64_t u = at / 256; u < (at + bytes + 255) / 256; ++u) owner[u].store(0);
          if (!fb.release(at)) ++overlaps;
          mine.erase(mine.begin() + k);
        }
      }
      for (auto& [at, bytes] : mine) {
        for (uint64_t u = at / 256; u < (at + bytes + 255) / 256; ++u) owner[u].store(0);
        if (!fb.release(at)) ++overlaps;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(overlaps == 0 && misaligned == 0 && outside == 0);
  CHECK(fb.allocation_count() == 0 && fb.free_bytes() == 64 * kMiB - 256 * 1024 - kMiB && fb.bad_releases() == 0);
}

static void test_pramin() {
  const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
  FakeGpu g(chip, 64 * kMiB);
  StdLock lock;
  PraminWindow w(g, 64 * kMiB, lock);
  CHECK(w.write32(0x0, 0x11111111));
  CHECK(w.write32(0xFFFFC, 0x22222222));
  CHECK(w.write32(0x100000, 0x33333333));
  CHECK(w.write32(0x123454, 0x44444444));
  CHECK(w.write32(64 * kMiB - 4, 0x55555555));
  CHECK(!w.write32(64 * kMiB, 1));
  CHECK(!w.write32(0x2, 1));
  uint32_t v = 0;
  CHECK(!w.read32(~0ull - 3, &v));
  const auto moves = g.writes_to({0x001700});
  CHECK(moves.size() == 3 && moves[0].second == 0x0 && moves[1].second == 0x10 && moves[2].second == 0x3f0);
  CHECK(w.moves() == 3);
  CHECK(g.v32(0) == 0x11111111 && g.v32(0xFFFFC) == 0x22222222 && g.v32(0x100000) == 0x33333333 && g.v32(0x123454) == 0x44444444 &&
        g.v32(64 * kMiB - 4) == 0x55555555);
  CHECK(w.read32(0x100000, &v) && v == 0x33333333);
  CHECK(w.read32(0xFFFFC, &v) && v == 0x22222222);
  // A fill across a window boundary moves the window once more and writes every word.
  CHECK(w.fill32(0x2FF000, 0x2000, 0xABCDEF01));
  bool all = true;
  for (uint64_t a = 0x2FF000; a < 0x301000; a += 4) all = all && g.v32(a) == 0xABCDEF01;
  CHECK(all && g.v32(0x2FEFFC) == 0 && g.v32(0x301000) == 0);
  CHECK(!w.fill32(64 * kMiB - 8, 16, 0));
  // forget(): the next access programs the window again even if it seems to be in place.
  const size_t before = g.writes_to({0x001700}).size();
  w.forget();
  CHECK(w.read32(0x300000, &v) && v == 0xABCDEF01);
  CHECK(g.writes_to({0x001700}).size() == before + 1);
}

// Threads that each work in their own MiB: without the lock, one thread's window move would redirect another's access.
static void test_pramin_threads() {
  const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
  FakeGpu g(chip, 16 * kMiB);
  StdLock lock;
  PraminWindow w(g, 16 * kMiB, lock);
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 6; ++t) {
    threads.emplace_back([&, t] {
      const uint64_t base = uint64_t(t + 1) * 2 * kMiB;
      for (uint32_t i = 0; i < 4000; ++i) {
        const uint64_t a = base + (i % 1024) * 4;
        const uint32_t value = uint32_t(t) << 24 | i;
        if (!w.write32(a, value)) ++wrong;
        uint32_t back = 0;
        if (!w.read32(a, &back) || back != value) ++wrong;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(wrong == 0);
  CHECK(g.clean());
}

// ---- Page-table words, hand-computed ----
static void test_entry_encodings() {
  MapFlags none;
  MapFlags all;
  all.privileged = true;
  all.read_only = true;
  all.kind = 0xFE;
  CHECK(encode_pte(VmmFormat::Gp100, 0x12345000, MemoryTarget::Vram, none) == 0x0000000001234501ull);
  CHECK(encode_pte(VmmFormat::Gp100, 0x100002000ull, MemoryTarget::SystemCoherent, none) == 0x000000001000020Dull);
  CHECK(encode_pte(VmmFormat::Gp100, 0x100002000ull, MemoryTarget::SystemNoncoherent, none) == 0x0000000010000207ull);
  CHECK(encode_pte(VmmFormat::Gp100, 0x1000, MemoryTarget::Vram, all) == 0xFE00000000000161ull);
  CHECK(encode_pte(VmmFormat::Gf100, 0x12345000, MemoryTarget::Vram, none) == 0x0000000000123451ull);
  CHECK(encode_pte(VmmFormat::Gf100, 0x100002000ull, MemoryTarget::SystemCoherent, none) == 0x0000000501000021ull);
  CHECK(encode_pte(VmmFormat::Gf100, 0x1000, MemoryTarget::Vram, all) == 0x00000FE000000017ull);
  CHECK(encode_pte(VmmFormat::Nv50, 0x12345000, MemoryTarget::Vram, none) == 0x0000000012345001ull);
  CHECK(encode_pte(VmmFormat::Nv50, 0x100002000ull, MemoryTarget::SystemCoherent, none) == 0x0000000100002021ull);
  CHECK(encode_pte(VmmFormat::Nv50, 0x1000, MemoryTarget::Vram, all) == 0x00007E0000001049ull);

  GpuMemory vt{0x40000, 4096, MemoryTarget::Vram, {}};
  GpuMemory st{0x100000000ull, 4096, MemoryTarget::SystemCoherent, {}};
  CHECK(encode_gp100_pde(vt) == 0x4002);
  CHECK(encode_gp100_pde(st) == 0x1000000Cull);
  GpuMemory big{0x20000, 8192, MemoryTarget::Vram, {}}, small{0x30000, 262144, MemoryTarget::Vram, {}};
  CHECK(encode_gf100_pde(&big, &small) == 0x0000030100000201ull);
  CHECK(encode_gf100_pde(&big, nullptr) == 0x201);
  CHECK(encode_gf100_pde(nullptr, &small) == 0x0000030100000000ull);
  GpuMemory n50{0x100000, 1 << 20, MemoryTarget::Vram, {}};
  CHECK(encode_nv50_pde(n50, PageSize::Small) == 0x100003);
  CHECK(encode_nv50_pde(n50, PageSize::Big) == 0x100001);

  const VmmLayout gp = vmm_layout(make_chip(Family::Pascal, 0x134, "GP104"));
  const VmmLayout gf = vmm_layout(make_chip(Family::Kepler, 0xe4, "GK104"));
  const VmmLayout gm16 = vmm_layout(make_chip(Family::Maxwell, 0x124, "GM204"), 16);
  GpuMemory root{0x5000, 4096, MemoryTarget::Vram, {}};
  CHECK(encode_pdb(gp, root) == 0x5C00);
  CHECK(encode_pdb(gf, root) == 0x5000);
  CHECK(encode_pdb(gm16, root) == 0x5800);
  GpuMemory sroot{0x100005000ull, 4096, MemoryTarget::SystemCoherent, {}};
  CHECK(encode_pdb(gf, sroot) == 0x100005006ull);
  CHECK(gp.va_bits == 49 && gp.dir_levels == 4 && gf.va_bits == 40 && gf.big_shift == 17 && gm16.big_shift == 16 && gm16.dir[0].bits == 14);
  CHECK(vmm_layout(make_chip(Family::Ampere, 0x172, "GA102")).format == VmmFormat::None);
}

struct VmmRig {
  FakeGpu gpu;
  FakeSystemMemory sys;
  StdLock fb_lock, w_lock, vmm_lock;
  NvFb fb{fb_lock};
  std::unique_ptr<PraminWindow> w;
  std::unique_ptr<NvVramMemory> mem;
  explicit VmmRig(const Chip& c) : gpu(c, 64 * kMiB) {
    CHECK(fb.init(info_of(64 * kMiB)));
    w = std::make_unique<PraminWindow>(gpu, 64 * kMiB, w_lock);
    mem = std::make_unique<NvVramMemory>(fb, *w, &sys);
  }
};

static void test_vmm_gp100() {
  const Chip chip = make_chip(Family::Turing, 0x164, "TU104");
  VmmRig r(chip);
  NvVmm vmm(*r.mem, chip, r.vmm_lock, nullptr);
  CHECK(vmm.init(1 << 20, 1ull << 49));
  CHECK(vmm.root().gpu_address == 0x40000);
  const uint64_t va = (1ull << 47) | (2ull << 38) | (3ull << 29) | (4ull << 21) | (5ull << 12);
  GpuMemory target{0x800000, 4096, MemoryTarget::Vram, {}};
  CHECK(vmm.map(va, target, 0, 4096));
  // The tables were allocated in order: PD2 0x41000, PD1 0x42000, PD0 0x43000, PT 0x44000.
  CHECK(r.gpu.v64(0x40000 + 1 * 8) == 0x4102);
  CHECK(r.gpu.v64(0x41000 + 2 * 8) == 0x4202);
  CHECK(r.gpu.v64(0x42000 + 3 * 8) == 0x4302);
  CHECK(r.gpu.v64(0x43000 + 4 * 16) == 0 && r.gpu.v64(0x43000 + 4 * 16 + 8) == 0x4402);
  CHECK(r.gpu.v64(0x44000 + 5 * 8) == 0x80001);
  CHECK(vmm.table_count() == 5);
  // A big page in the same 2 MiB adds the big table (0x45000) to the dual PD0 entry.
  GpuMemory big{0x1000000, 65536, MemoryTarget::Vram, {}};
  const uint64_t bva = (va & ~((1ull << 21) - 1)) + 3 * 65536;
  CHECK(vmm.map(bva, big, 0, 65536, PageSize::Big));
  CHECK(r.gpu.v64(0x43000 + 4 * 16) == 0x4502 && r.gpu.v64(0x43000 + 4 * 16 + 8) == 0x4402);
  CHECK(r.gpu.v64(0x45000 + 3 * 8) == 0x100001);
  // System memory.
  GpuMemory sm = r.sys.allocate(MemoryTarget::SystemCoherent, 8192, 4096);
  CHECK(vmm.map(va + 4096, sm, 0, 8192));
  CHECK(r.gpu.v64(0x44000 + 6 * 8) == ((sm.gpu_address >> 4) | 0xD));
  // translate() walks what was written.
  auto t = vmm.translate(va + 0x123);
  CHECK(t && t->address == 0x800123 && t->target == MemoryTarget::Vram && t->page == PageSize::Small);
  t = vmm.translate(bva + 0x4567);
  CHECK(t && t->address == 0x1004567 && t->page == PageSize::Big);
  t = vmm.translate(va + 4096 + 8);
  CHECK(t && t->address == sm.gpu_address + 8 && t->target == MemoryTarget::SystemCoherent);
  CHECK(!vmm.translate(va + 3 * 4096));
  // Misaligned, out of range and oversized requests.
  CHECK(!vmm.map(va + 1, target, 0, 4096));
  CHECK(!vmm.map(va, target, 0, 8192));
  CHECK(!vmm.map(1ull << 49, target, 0, 4096));
  CHECK(!vmm.map(bva + 4096, big, 0, 65536, PageSize::Big));
  CHECK(vmm.unmap(va, 4096));
  CHECK(r.gpu.v64(0x44000 + 5 * 8) == 0 && !vmm.translate(va));
  // Join: PDB, limit and the subcontexts (GV100+).
  GpuMemory inst = r.mem->allocate(MemoryTarget::Vram, 4096, 4096);
  CHECK(vmm.join(inst));
  CHECK(r.gpu.v64(inst.gpu_address + 0x200) == 0x40C00);
  CHECK(r.gpu.v64(inst.gpu_address + 0x208) == (1ull << 49) - 1);
  CHECK(r.gpu.v32(inst.gpu_address + 0x2a0) == 0x40C00 && r.gpu.v32(inst.gpu_address + 0x2a4) == 0);
  CHECK(r.gpu.v32(inst.gpu_address + 0x2b0) == 1 && r.gpu.v32(inst.gpu_address + 0x2b4) == 1);
  CHECK(r.gpu.v32(inst.gpu_address + 0x298) == 1 && r.gpu.v32(inst.gpu_address + 0x29c) == 0);
  // VA allocation.
  const Optional<uint64_t> v1 = vmm.allocate_va(10000, 65536);
  CHECK(v1 && *v1 % 65536 == 0 && *v1 >= (1 << 20));
  CHECK(vmm.free_va(*v1) && !vmm.free_va(*v1));
  // Pascal joins without subcontexts.
  const Chip pascal = make_chip(Family::Pascal, 0x134, "GP104");
  VmmRig r2(pascal);
  NvVmm v2(*r2.mem, pascal, r2.vmm_lock, nullptr);
  CHECK(v2.init(1 << 20, 1ull << 49));
  GpuMemory inst2 = r2.mem->allocate(MemoryTarget::Vram, 4096, 4096);
  CHECK(v2.join(inst2) && r2.gpu.v32(inst2.gpu_address + 0x2a0) == 0 && r2.gpu.v32(inst2.gpu_address + 0x298) == 0);
}

static void test_vmm_gf100() {
  const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
  VmmRig r(chip);
  NvVmm vmm(*r.mem, chip, r.vmm_lock, nullptr);
  CHECK(vmm.init(1 << 20, 1ull << 40));
  CHECK(vmm.root().gpu_address == 0x40000);   // 8192 entries: 64 KiB
  const uint64_t va = (3ull << 27) | (5ull << 12);
  GpuMemory target{0x12345000, 4096, MemoryTarget::Vram, {}};
  r.gpu.vram.resize(512 * kMiB);   // the target lies beyond 64 MiB: page tables only, nothing reads it
  CHECK(vmm.map(va, target, 0, 4096));
  CHECK(r.gpu.v64(0x40000 + 3 * 8) == 0x0000050100000000ull);   // small PT at 0x50000 (256 KiB)
  CHECK(r.gpu.v64(0x50000 + 5 * 8) == 0x0000000000123451ull);
  GpuMemory big{0x1000000, 1 << 17, MemoryTarget::Vram, {}};
  CHECK(vmm.map((3ull << 27) | (2ull << 17), big, 0, 1 << 17, PageSize::Big));
  CHECK(r.gpu.v64(0x40000 + 3 * 8) == 0x0000050100000901ull);   // big PT at 0x90000 (8 KiB)
  CHECK(r.gpu.v64(0x90000 + 2 * 8) == 0x10001);
  auto t = vmm.translate((3ull << 27) | (2ull << 17) | 0x1ABCD);
  CHECK(t && t->address == 0x101ABCD && t->page == PageSize::Big);
  t = vmm.translate(va | 0x10);
  CHECK(t && t->address == 0x12345010);
  GpuMemory inst = r.mem->allocate(MemoryTarget::Vram, 4096, 4096);
  CHECK(vmm.join(inst));
  CHECK(r.gpu.v64(inst.gpu_address + 0x200) == 0x40000 && r.gpu.v64(inst.gpu_address + 0x208) == (1ull << 40) - 1);
  CHECK(r.gpu.v32(inst.gpu_address + 0x298) == 0);
}

static void test_vmm_nv50() {
  const Chip chip = make_chip(Family::Tesla, 0x84, "G84");
  VmmRig r(chip);
  NvVmm vmm(*r.mem, chip, r.vmm_lock, nullptr);
  CHECK(vmm.init(1 << 20, 1ull << 40));
  CHECK(vmm.root().gpu_address == 0x40000 && vmm.layout().nv50_pd_offset == 0x200);   // 2048 entries: 16 KiB
  GpuMemory target{0x2345000, 4096, MemoryTarget::Vram, {}};
  CHECK(vmm.map((1ull << 29) | (7ull << 12), target, 0, 4096));
  CHECK(r.gpu.v64(0x40000 + 1 * 8) == 0x44003);                  // small PT at 0x44000 (1 MiB)
  CHECK(r.gpu.v64(0x44000 + 7 * 8) == 0x2345001);
  GpuMemory inst = r.mem->allocate(MemoryTarget::Vram, 0x10000, 4096);   // at 0x144000
  CHECK(inst.gpu_address == 0x144000);
  CHECK(vmm.join(inst));
  CHECK(r.gpu.v64(inst.gpu_address + 0x200 + 8) == 0x44003);
  // One page size per PDE on NV50.
  GpuMemory big{0x1000000, 65536, MemoryTarget::Vram, {}};
  CHECK(!vmm.map((1ull << 29) | (1ull << 16), big, 0, 65536, PageSize::Big));
  CHECK(vmm.map((2ull << 29) | (1ull << 16), big, 0, 65536, PageSize::Big));
  CHECK(r.gpu.v64(0x40000 + 2 * 8) == 0x154001);                 // big PT at 0x154000 (64 KiB), copied into the joined instance
  CHECK(r.gpu.v64(inst.gpu_address + 0x200 + 16) == 0x154001);
  CHECK(r.gpu.v64(0x154000 + 1 * 8) == 0x1000001);
  auto t = vmm.translate((2ull << 29) | (1ull << 16) | 0x42);
  CHECK(t && t->address == 0x1000042 && t->page == PageSize::Big);
}

static void test_tlb_sequences() {
  GpuMemory root{0x123000, 4096, MemoryTarget::Vram, {}};
  {
    const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
    FakeGpu g(chip, 0);
    StdLock l;
    NvTlb tlb(g, chip, l);
    CHECK(tlb.invalidate(root, true));
    const auto w = g.writes_to({0x100cb8, 0x100cbc, 0x100cec});
    CHECK(w.size() == 2 && w[0] == std::make_pair(0x100cb8u, 0x1230u) && w[1] == std::make_pair(0x100cbcu, 0x80000005u));
  }
  {
    const Chip chip = make_chip(Family::Pascal, 0x134, "GP104");
    FakeGpu g(chip, 0);
    StdLock l;
    NvTlb tlb(g, chip, l);
    CHECK(tlb.invalidate(root, false));
    const auto w = g.writes_to({0x100cb8, 0x100cbc, 0x100cec});
    CHECK(w.size() == 3 && w[0].second == 0x1230 && w[1] == std::make_pair(0x100cecu, 0u) && w[2].second == 0x85000001u);
  }
  {
    const Chip chip = make_chip(Family::Turing, 0x164, "TU104");
    FakeGpu g(chip, 0);
    StdLock l;
    NvTlb tlb(g, chip, l);
    CHECK(tlb.invalidate(root, true));
    const auto w = g.writes_to({0xb830a0, 0xb830a4, 0x100e68, 0xb830b0});
    CHECK(w.size() == 4 && w[0].second == 0x1230 && w[1].second == 0 && w[2].second == 0 && w[3].second == 0x85000005u);
  }
  // An MMU that never reports progress: the invalidate gives up.
  {
    const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
    struct Stuck final : INvRegisters { uint32_t read32(uint32_t) override { return 0; } void write32(uint32_t, uint32_t) override {} } stuck;
    StdLock l;
    NvTimeouts t;
    t.register_us = 1000;
    NvTlb tlb(stuck, chip, l, t);
    CHECK(!tlb.invalidate(root, false));
  }
}

static void test_fb_init_mmu() {
  {
    const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
    FakeGpu g(chip, 0);
    g.set(0x100c80, 0x1);
    CHECK(fb_init_mmu(g, chip, 17, 0x7654000));
    CHECK((g.reg(0x100c80) & 1) == 0 && g.reg(0x100c10) == 0x76540);
    CHECK(!fb_init_mmu(g, chip, 15, 0));
    CHECK(!fb_init_mmu(g, chip, 17, 0x1001));
  }
  {
    const Chip chip = make_chip(Family::Maxwell, 0x124, "GM204");
    FakeGpu g(chip, 0);
    CHECK(fb_init_mmu(g, chip, 16, 0));
    CHECK((g.reg(0x100c80) & 0x1801) == 0x1001);
  }
  CHECK(!fb_init_mmu(*std::make_unique<FakeGpu>(make_chip(Family::Ampere, 0x172, "GA102"), 0), make_chip(Family::Ampere, 0x172, "GA102"), 16, 0));
}

static void test_bar_setup() {
  struct Case { Family f; uint32_t chipset; uint32_t bar1, bar2; bool wait; } cases[] = {
      {Family::Kepler, 0xe4, 0x001704, 0x001714, false},
      {Family::Maxwell, 0x117, 0x001704, 0x001714, true},
      {Family::Pascal, 0x134, 0x001704, 0x001714, true},
      {Family::Turing, 0x164, 0xb80f40, 0xb80f48, true},
  };
  for (const Case& c : cases) {
    const Chip chip = make_chip(c.f, c.chipset, "x");
    Rig r(chip);
    CHECK(r.probe.ok());
    CHECK(r.bar->init(256 * kMiB, 32 * kMiB, kMiB) == BarStatus::Ok);
    // BAR2's instance block is allocated first, BAR1's second; BAR2 is bound first.
    const auto w = r.gpu.writes_to({c.bar1, c.bar2});
    CHECK(w.size() == 2);
    if (w.size() == 2) {
      CHECK(w[0] == std::make_pair(c.bar2, 0x80000040u));
      CHECK(w[1] == std::make_pair(c.bar1, 0x80000041u));
    }
    CHECK(r.bar->bar2_instance().gpu_address == 0x40000 && r.bar->bar1_instance().gpu_address == 0x41000);
    CHECK(r.gpu.v64(0x40000 + 0x208) == 32 * kMiB - 1 && r.gpu.v64(0x41000 + 0x208) == 256 * kMiB - 1);
    CHECK((r.gpu.v64(0x40000 + 0x200) & ~0xFFFull) == r.bar->bar2().root().gpu_address);
    // BAR1 keeps the firmware's linear first MiB: a write through BAR1 lands at the same VRAM offset.
    r.bar1ap->write32(0x1230, 0xFEEDF00D);
    CHECK(r.gpu.v32(0x1230) == 0xFEEDF00D);
    // A BAR2 mapping reaches VRAM through the page tables (model walks them from 0x001714 / 0xb80f48).
    GpuMemory obj = r.pmem->allocate(MemoryTarget::Vram, 65536, 65536);
    const Optional<uint64_t> va = r.bar->bar2().allocate_va(65536, 65536);
    CHECK(obj.valid() && va && r.bar->bar2().map(*va, obj, 0, 65536) && r.bar->bar2().flush());
    r.bar2ap->write32(*va + 0x44, 0xC0FFEE);
    CHECK(r.gpu.v32(obj.gpu_address + 0x44) == 0xC0FFEE);
    CHECK(r.bar->flush_writes());
    CHECK(r.gpu.writes_to({0x070000}).size() == 1);
    CHECK(r.gpu.invalidates >= 1);
    r.pmem->release(obj);
    r.bar->fini();
    CHECK((r.gpu.reg(c.bar1) & 0x80000000u) == 0 && (r.gpu.reg(c.bar2) & 0x80000000u) == 0);
    CHECK(r.gpu.clean());
  }
  // Tesla's BARs are DMA objects: not done here, refused.
  const Chip tesla = make_chip(Family::Tesla, 0x84, "G84");
  Rig t(tesla);
  CHECK(t.bar->init(256 * kMiB, 32 * kMiB) == BarStatus::Unsupported);
  const Chip kepler = make_chip(Family::Kepler, 0xe4, "GK104");
  Rig k(kepler);
  CHECK(k.bar->init(3 * kMiB, 32 * kMiB) == BarStatus::BadSize);
}

static void test_instmem_bar2() {
  const Chip chip = make_chip(Family::Kepler, 0xe4, "GK104");
  Rig r(chip);
  CHECK(r.bar->init(256 * kMiB, 32 * kMiB, kMiB) == BarStatus::Ok);
  CHECK(r.instmem->enable_bar2(*r.bar, *r.bar2ap, 2));
  CHECK(r.instmem->bar2_active());
  const size_t window_moves = r.gpu.writes_to({0x001700}).size();
  // Through BAR2: each new 64 KiB chunk remaps a slot (two slots: the third chunk evicts the first).
  const uint64_t addrs[] = {0x400000, 0x400040, 0x500000, 0x600000, 0x400080};
  uint32_t n = 0;
  for (uint64_t a : addrs) CHECK(r.instmem->write32(a, 0xA0000000u | n++));
  CHECK(r.instmem->remaps() == 4);
  n = 0;
  for (uint64_t a : addrs) CHECK(r.gpu.v32(a) == (0xA0000000u | n++));
  uint32_t v = 0;
  CHECK(r.instmem->read32(0x500000, &v) && v == 0xA0000002u);
  CHECK(!r.instmem->write32(64 * kMiB, 0));
  // Page tables are still written through PRAMIN, but the data words did not move the PRAMIN window.
  CHECK(r.gpu.writes_to({0x001700}).size() >= window_moves);
  // The VRAM INvMemory over it: allocate (zeroed), word access, bounds.
  GpuMemory small = r.mem->allocate(MemoryTarget::Vram, 1000, 256);
  CHECK(small.valid() && small.bytes == 1024);
  r.mem->release(small);
  GpuMemory m = r.mem->allocate(MemoryTarget::Vram, 1000, 1024);
  CHECK(m.valid() && m.bytes == 1024 && m.gpu_address % 1024 == 0);
  CHECK(r.mem->write32(m, 1020, 7) && !r.mem->write32(m, 1024, 7) && !r.mem->write32(m, 2, 7));
  CHECK(r.mem->read32(m, 1020, &v) && v == 7 && r.gpu.v32(m.gpu_address + 1020) == 7);
  GpuMemory copy = m;
  r.mem->release(m);
  CHECK(!m.valid());
  r.mem->release(copy);   // the same object again: detected and counted, nothing else happens
  CHECK(r.fb.bad_releases() == 1);
  // System memory objects go through their CPU view.
  GpuMemory s = r.mem->allocate(MemoryTarget::SystemCoherent, 4096, 4096);
  CHECK(s.valid() && r.mem->write32(s, 8, 0x12345678) && r.sys.bytes[s.gpu_address - kSysBase + 8] == 0x78);
  CHECK(r.gpu.clean());
}

static void test_instmem_threads() {
  const Chip chip = make_chip(Family::Pascal, 0x134, "GP104");
  Rig r(chip);
  CHECK(r.bar->init(256 * kMiB, 32 * kMiB, kMiB) == BarStatus::Ok);
  CHECK(r.instmem->enable_bar2(*r.bar, *r.bar2ap, 3));
  std::atomic<int> wrong{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 5; ++t) {
    threads.emplace_back([&, t] {
      for (uint32_t i = 0; i < 600; ++i) {
        const uint64_t a = 0x800000 + uint64_t(t) * 0x30000 + (i % 3) * 0x10000 + (i % 64) * 4;
        const uint32_t value = uint32_t(t) << 24 | i;
        if (!r.instmem->write32(a, value)) ++wrong;
        uint32_t back = 0;
        if (!r.instmem->read32(a, &back) || back != value) ++wrong;
      }
    });
  }
  for (auto& th : threads) th.join();
  CHECK(wrong == 0);
  CHECK(r.gpu.clean());
}

static void test_topology() {
  const Chip chip = make_chip(Family::Turing, 0x164, "TU104");
  FakeGpu g(chip, 0);
  set_topology(g, Family::Turing);
  Topology t;
  CHECK(read_topology(g, &t));
  CHECK(t.count == 2 && t.engines[0].type == kTopTypeGr && t.engines[0].runlist == 0);
  CHECK(t.engines[1].type == kTopTypeLce && t.engines[1].runlist == 2 && t.engines[1].reset == 21 && t.engines[1].addr == 0x104000);
  const Optional<TopEngine> ce = find_copy_engine(t, 0);
  CHECK(ce && ce->runlist == 2 && ce->instance == 0);
  CHECK(!find_copy_engine(t, 1));
  struct Ones final : INvRegisters { uint32_t read32(uint32_t) override { return 0xFFFFFFFFu; } void write32(uint32_t, uint32_t) override {} } ones;
  CHECK(!read_topology(ones, &t));
  // Random topology words: decoding stays inside its tables.
  std::mt19937 rng(9);
  for (int iter = 0; iter < 500; ++iter) {
    FakeGpu r(chip, 0);
    for (uint32_t i = 0; i < 64; ++i) r.set(0x022700 + i * 4, rng() & 0x7FFFFFFFu);
    Topology x;
    CHECK(read_topology(r, &x) && x.count <= Topology::kMaxEngines);
  }
}

// The end-to-end path: channel, GPFIFO, copy engine, semaphore.
static void test_channel_and_copy(Family family, uint32_t chipset, const char* name) {
  printf("  copy engine on %s\n", name);
  const Chip chip = make_chip(family, chipset, name);
  Rig r(chip);
  CHECK(r.probe.ok());
  CHECK(r.bring_up());
  if (family < Family::Volta) CHECK(r.gpu.reg(0x002254) == (0x10000000u | uint32_t(kMiB >> 12)));   // USERD right after BAR1's linear MiB
  CHECK((r.gpu.reg(0x000200) & 0x100) != 0);
  const Optional<TopEngine> ce_engine = find_copy_engine(r.fifo->topology(), 0);
  CHECK(ce_engine && ce_engine->runlist == 2);
  if (family >= Family::Pascal) CHECK(r.gpu.reg(0x000200) & (1u << 21));
  else if (family == Family::Kepler) CHECK(r.gpu.reg(0x000200) & (1u << 6));   // no PTOP reset field: Kepler's fixed CE0 bit

  StdLock vmm_lock;
  NvVmm vmm(*r.mem, chip, vmm_lock, r.tlb.get());
  CHECK(vmm.init(1 << 20, 1ull << 40));
  NvChannel* ch = nullptr;
  CHECK(r.fifo->create_channel(vmm, 2, &ch) == FifoStatus::Ok && ch);
  if (!ch) return;
  CHECK(ch->id() == 0 && ch->active());
  // The instance block: RAMFC and the page directory.
  const GpuMemory& inst = ch->instance();
  auto word = [&](uint32_t off) { uint32_t x = 0; CHECK(r.mem->read32(inst, off, &x)); return x; };
  CHECK(word(0x10) == 0xface && word(0x30) == (family >= Family::Volta ? 0x7ffff902u : 0xfffff902u));
  CHECK(word(0x08) == uint32_t(ch->userd_address()) && word(0x0c) == uint32_t(ch->userd_address() >> 32));
  CHECK(word(0x48) == uint32_t(ch->gpfifo_va()) && word(0x4c) == (uint32_t(ch->gpfifo_va() >> 32) | 9u << 16));
  CHECK(word(0x84) == 0x20400000 && word(0x94) == 0x30000001 && word(0xe8) == ch->id());
  if (family < Family::Volta) CHECK(word(0x9c) == 0x100 && word(0xac) == 0x1f && word(0xb8) == 0xf8000000 && word(0xfc) == 0x10000010);
  else CHECK(word(0xf4) == 0x1000 && word(0x2a0) == word(0x200) && word(0x298) == 1);
  CHECK((uint64_t(word(0x200)) & ~0xFFFull) == vmm.root().gpu_address);
  CHECK(r.gpu.reg(0x800000) == (0x80000000u | uint32_t(inst.gpu_address >> 12)) && r.gpu.chan_enabled[0]);
  CHECK(r.gpu.runlist_channels[2].count(0) == 1);

  // Buffers: VRAM source and destination, a semaphore page, and a system-memory source.
  const uint64_t kBytes = 3 * 65536 + 100;
  GpuMemory src = r.mem->allocate(MemoryTarget::Vram, kBytes, 65536);
  GpuMemory dst = r.mem->allocate(MemoryTarget::Vram, kBytes, 65536);
  GpuMemory sem = r.mem->allocate(MemoryTarget::Vram, 4096, 4096);
  GpuMemory sys = r.mem->allocate(MemoryTarget::SystemCoherent, 8192, 4096);
  CHECK(src.valid() && dst.valid() && sem.valid() && sys.valid());
  for (uint64_t i = 0; i < src.bytes; i += 4) CHECK(r.mem->write32(src, i, uint32_t(i * 2654435761u)));
  for (uint64_t i = 0; i < sys.bytes; i += 4) CHECK(r.mem->write32(sys, i, uint32_t(~i)));
  auto map = [&](const GpuMemory& m) {
    const uint64_t bytes = (m.bytes + 4095) & ~4095ull;
    const Optional<uint64_t> va = vmm.allocate_va(bytes, 65536);
    CHECK(va && vmm.map(*va, m, 0, bytes));
    return va ? *va : 0;
  };
  const uint64_t src_va = map(src), dst_va = map(dst), sem_va = map(sem), sys_va = map(sys);
  CHECK(vmm.flush());

  NvCopyEngine ce(*ch, *r.mem, chip);
  CHECK(ce.class_id() == copy_engine_class(chip));
  CHECK(ce.copy(dst_va, src_va, 64, sem_va, 1) == CeStatus::BadArgument);   // not bound yet
  CHECK(ce.bind() == CeStatus::Ok);
  CHECK(ce.copy(dst_va, src_va, kBytes, sem_va, 0x1001) == CeStatus::Ok);
  CHECK(ce.wait(sem, 0, 0x1001) == CeStatus::Ok);
  bool same = true;
  for (uint64_t i = 0; i + 4 <= kBytes; i += 4) same = same && r.gpu.v32(dst.gpu_address + i) == uint32_t(i * 2654435761u);
  CHECK(same);
  CHECK(r.gpu.v32(dst.gpu_address + ((kBytes + 3) & ~3ull)) == 0);   // nothing past the end
  // Fill, then a system-memory source.
  CHECK(ce.fill(dst_va + 4096, 8192, 0xDEADBEEF, sem_va, 0x1002) == CeStatus::Ok);
  CHECK(ce.wait(sem, 0, 0x1002) == CeStatus::Ok);
  bool filled = true;
  for (uint64_t i = 4096; i < 4096 + 8192; i += 4) filled = filled && r.gpu.v32(dst.gpu_address + i) == 0xDEADBEEF;
  CHECK(filled && r.gpu.v32(dst.gpu_address + 4092) == uint32_t(4092 * 2654435761u) && r.gpu.v32(dst.gpu_address + 4096 + 8192) != 0xDEADBEEF);
  CHECK(ce.fill(dst_va, 6, 0, sem_va, 1) == CeStatus::BadArgument);
  CHECK(ce.copy(dst_va + 65536, sys_va, 8192, sem_va + 4, 0x1003) == CeStatus::Ok);
  CHECK(ce.wait(sem, 4, 0x1003) == CeStatus::Ok);
  CHECK(r.gpu.v32(dst.gpu_address + 65536 + 40) == uint32_t(~40ull));
  // Many submissions: the GPFIFO (512 entries) and the push buffer (64 KiB) both wrap.
  for (uint32_t i = 0; i < 1500; ++i) {
    if (ce.copy(dst_va + (i % 16) * 16, src_va, 16, sem_va, 0x2000 + i) != CeStatus::Ok) { CHECK(false); break; }
  }
  CHECK(ce.wait(sem, 0, 0x2000 + 1499) == CeStatus::Ok);
  CHECK(ch->wait_idle() == FifoStatus::Ok && ch->gp_get() == ch->gp_put());
  CHECK(r.gpu.launches == 1503);
  if (family >= Family::Volta) CHECK(r.gpu.doorbells == 1504);   // one per submission, bind included
  // A second channel shares the runlist.
  NvChannel* ch2 = nullptr;
  CHECK(r.fifo->create_channel(vmm, 2, &ch2) == FifoStatus::Ok && ch2 && ch2->id() == 1);
  CHECK(r.gpu.runlist_channels[2].size() == 2);
  if (ch2) {
    NvCopyEngine ce2(*ch2, *r.mem, chip);
    CHECK(ce2.bind() == CeStatus::Ok && ce2.fill(dst_va, 4096, 0x55AA55AA, sem_va + 8, 7) == CeStatus::Ok && ce2.wait(sem, 8, 7) == CeStatus::Ok);
    CHECK(r.gpu.v32(dst.gpu_address) == 0x55AA55AA);
    CHECK(r.fifo->destroy_channel(*ch2) == FifoStatus::Ok);
  }
  CHECK(r.fifo->destroy_channel(*ch) == FifoStatus::Ok);
  CHECK(!ch->active() && r.gpu.reg(0x800000) == 0 && r.gpu.runlist_channels[2].empty());
  CHECK(r.fifo->destroy_channel(*ch) == FifoStatus::BadArgument);
  CHECK(r.gpu.writes_to({0x002634}).size() == 2);
  for (GpuMemory* m : {&src, &dst, &sem, &sys}) r.mem->release(*m);
  CHECK(r.gpu.clean());
}

static void test_hung_engine() {
  const Chip chip = make_chip(Family::Turing, 0x164, "TU104");
  NvTimeouts t;
  t.register_us = 2000;
  t.fence_us = 2000;
  Rig r(chip, t);
  CHECK(r.bring_up());
  StdLock vmm_lock;
  NvVmm vmm(*r.mem, chip, vmm_lock, r.tlb.get());
  CHECK(vmm.init(1 << 20, 1ull << 40));
  NvChannel* ch = nullptr;
  CHECK(r.fifo->create_channel(vmm, 2, &ch) == FifoStatus::Ok && ch);
  if (!ch) return;
  GpuMemory sem = r.mem->allocate(MemoryTarget::Vram, 4096, 4096);
  const Optional<uint64_t> sem_va = vmm.allocate_va(4096, 4096);
  CHECK(sem_va && vmm.map(*sem_va, sem, 0, 4096) && vmm.flush());
  NvCopyEngine ce(*ch, *r.mem, chip, t);
  CHECK(ce.bind() == CeStatus::Ok);
  r.gpu.hung = true;
  CHECK(ce.fill(*sem_va + 64, 64, 1, *sem_va, 42) == CeStatus::Ok);   // submitted...
  CHECK(ce.wait(sem, 0, 42) == CeStatus::Timeout);                    // ...never completes
  CHECK(ch->wait_idle() == FifoStatus::Timeout);
  // Filling the GPFIFO: once 511 entries are outstanding, submit waits for the host, which never comes.
  CeStatus last = CeStatus::Ok;
  uint32_t submitted = 0;
  for (uint32_t i = 0; i < 600 && last == CeStatus::Ok; ++i, ++submitted) last = ce.fill(*sem_va + 64, 64, 1, *sem_va, 43);
  printf("    hung: %u submissions, last %s\n", submitted, ce_status_name(last));
  CHECK(last == CeStatus::Timeout && submitted == 511);   // 1 + 510 outstanding fill the 511 usable entries; the next one times out
  // Tear-down: the preempt does not finish, so the channel's memory is kept, not handed out again.
  const uint32_t allocations = r.fb.allocation_count();
  CHECK(r.fifo->destroy_channel(*ch) == FifoStatus::Timeout);
  CHECK(r.fb.allocation_count() == allocations && !ch->active() && r.gpu.reg(0x800000) == 0);
  r.mem->release(sem);
  CHECK(r.gpu.clean());
}

static void test_refused() {
  // Ampere and later need GSP: identified and refused, without touching the hardware.
  const Chip ga102 = make_chip(Family::Ampere, 0x172, "GA102");
  FakeGpu g(ga102, 16 * kMiB);
  StdLock lock;
  FakeSystemMemory sys;
  NvFifo fifo(g, ga102, sys, nullptr, nullptr, lock);
  CHECK(fifo.init() == FifoStatus::NeedsGsp);
  CHECK(g.log.empty());
  CHECK(copy_engine_class(ga102) == kAmpereDmaCopyB && copy_engine_class(make_chip(Family::Ampere, 0x170, "GA100")) == kAmpereDmaCopyA);
  CHECK(copy_engine_class(make_chip(Family::Ada, 0x192, "AD102")) == kAmpereDmaCopyB);
  CHECK(copy_engine_class(make_chip(Family::Kepler, 0xe4, "GK104")) == kKeplerDmaCopyA);
  CHECK(copy_engine_class(make_chip(Family::Maxwell, 0x117, "GM107")) == kMaxwellDmaCopyA);
  CHECK(copy_engine_class(make_chip(Family::Pascal, 0x130, "GP100")) == kPascalDmaCopyA);
  CHECK(copy_engine_class(make_chip(Family::Pascal, 0x132, "GP102")) == kPascalDmaCopyB);
  CHECK(copy_engine_class(make_chip(Family::Volta, 0x140, "GV100")) == kVoltaDmaCopyA);
  CHECK(copy_engine_class(make_chip(Family::Turing, 0x166, "TU106")) == kTuringDmaCopyA);
  CHECK(copy_engine_class(make_chip(Family::Fermi, 0xc4, "GF104")) == 0);
  // Fermi and Tesla FIFOs are not done here.
  const Chip fermi = make_chip(Family::Fermi, 0xc4, "GF104");
  FakeGpu gf(fermi, 16 * kMiB);
  NvFifo ff(gf, fermi, sys, nullptr, nullptr, lock);
  CHECK(ff.init() == FifoStatus::Unsupported);
  // Kepler without BAR1 cannot reach USERD.
  const Chip kepler = make_chip(Family::Kepler, 0xe4, "GK104");
  Rig k(kepler);
  CHECK(k.fifo->init() == FifoStatus::NotReady);
  // No copy engine in the topology.
  Rig n(kepler);
  for (uint32_t i = 0; i < 64; ++i) n.gpu.set(0x022700 + i * 4, 0);
  CHECK(n.bar->init(256 * kMiB, 32 * kMiB, kMiB) == BarStatus::Ok);
  CHECK(n.fifo->init() == FifoStatus::NoEngine);
  // Push builder overflow.
  PushBuilder p;
  std::vector<uint32_t> many(300, 1);
  p.method(0, 0x100, ArrayView<const uint32_t>(many.data(), many.size()));
  CHECK(p.overflow() && p.words().size() == 0);
  CHECK(method_incr(4, 0x300, 1) == 0x200180C0u && method_immd(1, 0x10, 5) == 0x80052004u);
}

int main() {
  test_vram_probe();
  test_vram_garbage();
  test_allocator();
  test_allocator_threads();
  test_pramin();
  test_pramin_threads();
  test_entry_encodings();
  test_vmm_gp100();
  test_vmm_gf100();
  test_vmm_nv50();
  test_tlb_sequences();
  test_fb_init_mmu();
  test_bar_setup();
  test_instmem_bar2();
  test_instmem_threads();
  test_topology();
  test_channel_and_copy(Family::Kepler, 0xe4, "GK104");
  test_channel_and_copy(Family::Maxwell, 0x124, "GM204");
  test_channel_and_copy(Family::Pascal, 0x134, "GP104");
  test_channel_and_copy(Family::Volta, 0x140, "GV100");
  test_channel_and_copy(Family::Turing, 0x164, "TU104");
  test_hung_engine();
  test_refused();
  if (g_failed) {
    printf("NvMemoryTest: %d failed\n", g_failed.load());
    return 1;
  }
  printf("NvMemoryTest passed\n");
  return 0;
}
