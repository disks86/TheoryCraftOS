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

// Host tests for the native NVIDIA display code (src/Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay*.cpp) against a model of the display
// engine: registers (ownership handshake, channel control, the user areas' PUT/GET, the readback of assembly and armed state, the supervisor)
// and memory (the model fetches the push buffers from the fake video memory and decodes methods and data as EVO does, looks context DMA
// handles up through the RAMHT in the instance memory and writes notifiers where the context DMA points). The register and method numbers
// below are typed in here separately from NvDisplayHw.hpp; both come from the same (remembered) nouveau sources, so the model checks that the
// driver is consistent with itself and with nouveau's sequences, not that the numbers are right on hardware.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayMode.hpp"

using namespace hardware;
using namespace hardware::nv;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

namespace {

constexpr uint64_t kVramBytes = 32ull << 20;
constexpr uint64_t kFirstAllocation = 8ull << 20;    // below it: the firmware's framebuffer(s)

struct FakeMemory final : INvMemory {
  std::vector<uint8_t> vram = std::vector<uint8_t>(kVramBytes, 0);
  uint64_t next = kFirstAllocation;
  bool fail = false;
  int allocations = 0, releases = 0, flushes = 0;
  std::vector<uint64_t> released;
  GpuMemory allocate(MemoryTarget target, uint64_t bytes, uint64_t alignment) override {
    if (fail || target != MemoryTarget::Vram) return GpuMemory();
    const uint64_t at = (next + alignment - 1) & ~(alignment - 1);
    if (at + bytes > vram.size()) return GpuMemory();
    next = at + bytes;
    memset(vram.data() + at, 0, bytes);
    ++allocations;
    GpuMemory m;
    m.gpu_address = at;
    m.bytes = bytes;
    m.target = target;
    m.cpu = ByteView(vram.data() + at, bytes);
    return m;
  }
  void release(GpuMemory& memory) override {
    ++releases;
    released.push_back(memory.gpu_address);
    memory = GpuMemory();
  }
  void flush(const GpuMemory&) override { ++flushes; }
  uint64_t vram_bytes() const override { return kVramBytes; }
  uint32_t rd(uint64_t a) const { uint32_t v; memcpy(&v, vram.data() + a, 4); return v; }
  void wr(uint64_t a, uint32_t v) { memcpy(vram.data() + a, &v, 4); }
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The display engine model
// ---------------------------------------------------------------------------------------------------------------------------------------
struct Model final : INvRegisters {
  DisplayClass cls;
  FakeMemory& mem;
  std::map<uint32_t, uint32_t> regs;
  std::vector<std::pair<uint32_t, uint32_t>> writes;
  struct Chan {
    std::map<uint32_t, uint32_t> assy, arm;
    uint32_t put = 0, get = 0;
    std::vector<std::pair<uint32_t, uint32_t>> methods;   // decoded, in order
    uint32_t updates = 0, jumps = 0;
  };
  Chan core;
  Chan win[32];
  // behaviour
  bool vbios_never_releases = false, never_start = false, never_fetch = false, never_notify = false, never_latch = false;
  bool supervisor = true;                 // GF119: a mode set UPDATE goes through supervisor stages 1..3
  // results
  int errors = 0;
  uint64_t scanout[8] = {};               // per head: the VRAM address the armed ISO + offset resolves to (~0 unknown)
  uint64_t window_scanout[32] = {};
  uint32_t super_stage = 0;               // 0 none, 1..3 the stage waiting for its ack
  bool super_commit_notify = false;
  uint64_t super_notify_at = 0;
  uint32_t super_acks = 0;

  Model(DisplayClass c, FakeMemory& m) : cls(c), mem(m) {
    for (auto& s : scanout) s = ~0ull;
    for (auto& s : window_scanout) s = ~0ull;
    switch (cls) {
      case DisplayClass::Nv50:
        regs[0x610184] = 0x03000000;      // two SORs
        regs[0x610024] = 0x100;           // the VBIOS owns the display
        regs[0x6194e8] = 0x3;
        break;
      case DisplayClass::Gf119:
        regs[0x022448] = 4;
        regs[0x612004] = 0x0f | 0x10 | 0x0f00;   // four heads, a DAC, four SORs
        regs[0x6100ac] = 0x100;
        regs[0x6194e8] = 0x3;
        break;
      default:
        regs[0x610074] = 4 | 4 << 8 | 8 << 20;
        regs[0x610060] = 0x0f | 0x0f00;
        regs[0x610064] = 0xff;
        regs[0x6254e8] = 0x3;
        regs[0x610068] = 0x1234;
        break;
    }
  }

  bool pre() const { return cls == DisplayClass::Nv50 || cls == DisplayClass::Gf119; }

  // NV50's per-method state registers (assembly; armed = +4), head stride 0x540
  static uint32_t nv50_state(uint32_t method) {
    switch (method) {
      case 0x0860: return 0x610a84;
      case 0x0868: return 0x610b18;
      case 0x086c: return 0x610b20;
      case 0x0870: return 0x610ac8;
      case 0x0874: return 0x610a38;
      case 0x08c0: return 0x610a58;
      case 0x0814: return 0x610af8;
      case 0x0804: return 0x610ad0;
      default: return 0;
    }
  }

  uint32_t read32(uint32_t off) override {
    if (cls == DisplayClass::Nv50) {
      for (uint32_t h = 0; h < 2; ++h)
        for (uint32_t m = 0x0800; m < 0x0900; m += 4) {
          const uint32_t reg = nv50_state(m);
          if (!reg) continue;
          if (off == reg + h * 0x540) return value(core.assy, m + h * 0x400);
          if (off == reg + h * 0x540 + 4) return value(core.arm, m + h * 0x400);
        }
      if (off == 0x640000) return core.put;
      if (off == 0x640004) return core.get;
    } else if (cls == DisplayClass::Gf119) {
      if (off == 0x640000) return core.put;
      if (off == 0x640004) return core.get;
      if (off >= 0x640000 && off < 0x660000) return value(core.arm, off - 0x640000);
      if (off >= 0x660000 && off < 0x680000) return value(core.assy, off - 0x660000);
    } else {
      if (off == 0x680000) return core.put;
      if (off == 0x680004) return core.get;
      if (off >= 0x680000 && off < 0x688000) return value(core.assy, off - 0x680000);
      if (off >= 0x688000 && off < 0x690000) return value(core.arm, off - 0x688000);
      if (off >= 0x690000 && off < 0x690000 + 32 * 0x1000) {
        Chan& w = win[(off - 0x690000) / 0x1000];
        const uint32_t rel = off & 0xfff;
        if (rel == 0) return w.put;
        if (rel == 4) return w.get;
        return rel < 0x800 ? value(w.assy, rel) : value(w.arm, rel - 0x800);
      }
    }
    auto it = regs.find(off);
    return it == regs.end() ? 0 : it->second;
  }
  static uint32_t value(const std::map<uint32_t, uint32_t>& m, uint32_t k) {
    auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
  }

  void write32(uint32_t off, uint32_t v) override {
    writes.push_back({off, v});
    // ownership
    if ((cls == DisplayClass::Nv50 && off == 0x610024) || (cls == DisplayClass::Gf119 && off == 0x6100ac)) {
      if (v & 0x100) regs[off] &= ~0x100u;
      regs[off] &= ~(v & 0x7);           // the supervisor status bits are cleared by writing them
      return;
    }
    if (off == 0x6194e8 || off == 0x6254e8) {
      regs[off] = v;
      if (!(v & 1) && !vbios_never_releases) regs[off] &= ~2u;
      return;
    }
    // channel control
    const uint32_t core_ctrl = cls == DisplayClass::Nv50 ? 0x610200 : 0x610490;
    if (pre() && off == core_ctrl) {
      regs[off] = (v & 3) == 3 ? (never_start ? (v | 0x80000000) : (v & ~0x80000000u)) : v;
      return;
    }
    if (!pre() && off == 0x610b00) {
      regs[off] = (v & 1) && !never_start ? ((v & ~0x30000u) | 0x10000) : v;
      return;
    }
    if (!pre() && off > 0x610b00 && off < 0x610b00 + 33 * 0x10 && (off & 0xf) == 0) {
      regs[off] = v;
      const uint32_t w = (off - 0x610b00) / 0x10 - 1;
      if (v == 0x11 && !never_start) regs[0x610664 + w * 4] = 0x40000;
      return;
    }
    // PUT
    if (pre() && off == 0x640000) { core.put = v; process(core, push_address(0), false, 0); return; }
    if (!pre() && off == 0x680000) { core.put = v; process(core, push_address(0), false, 0); return; }
    if (!pre() && off >= 0x690000 && off < 0x690000 + 32 * 0x1000 && (off & 0xfff) == 0) {
      const uint32_t w = (off - 0x690000) / 0x1000;
      win[w].put = v;
      process(win[w], push_address(1 + w), true, w);
      return;
    }
    // DP lane power: the change takes effect at once
    if (off >= 0x61c034 && off < 0x61c034 + 8 * 0x800 && (off - 0x61c034) % 0x800 == 0) { regs[off] = v & ~0x80000000u; return; }
    // the supervisor's final handshake
    if (cls == DisplayClass::Gf119 && off == 0x6101d0 && v == 0x80000000 && super_stage) {
      ++super_acks;
      if (super_stage < 3) {
        ++super_stage;
        regs[0x6100ac] |= 1u << (super_stage - 1);
        for (uint32_t h = 0; h < 4; ++h) regs[0x6101d4 + h * 0x800] = pending_masks[h];
      } else {
        super_stage = 0;
        commit(core, false, 0);
        if (super_commit_notify) notify_at(super_notify_at, false);
      }
      return;
    }
    regs[off] = v;
  }

  uint32_t pending_masks[4] = {};

  uint64_t push_address(uint32_t chid) {
    uint32_t v = 0;
    if (cls == DisplayClass::Nv50) v = regs[0x610204];
    else if (cls == DisplayClass::Gf119) v = regs[0x610494];
    else v = regs[0x610b24 + chid * 0x10];
    return uint64_t{v & ~3u} << 8;
  }

  void process(Chan& c, uint64_t push, bool window, uint32_t w) {
    if (never_fetch) return;
    uint32_t guard = 0;
    while (c.get != c.put && ++guard < 100000) {
      const uint32_t header = mem.rd(push + c.get);
      if (header == 0x20000000) { c.get = 0; ++c.jumps; continue; }
      const uint32_t count = (header >> 18) & 0x7ff, method = header & 0xfffc;
      if (count == 0 || (header & 0xe0000000)) { ++errors; c.get = c.put; return; }
      for (uint32_t i = 0; i < count; ++i) apply(c, method + i * 4, mem.rd(push + c.get + 4 + i * 4), window, w);
      c.get += 4 + count * 4;
    }
  }

  // RAMHT lookup as the hardware does it, with the model's own hash and context decoding
  bool resolve(uint32_t chid, uint32_t handle, uint64_t* start, uint64_t* limit) {
    uint64_t inst = 0;
    if (pre()) inst = uint64_t{regs[0x610010] & ~0xfu} << 8;
    else inst = uint64_t{regs[0x610014]} << 16;
    uint32_t hash = 0;
    for (uint32_t h = handle; h; h >>= 9) hash ^= h & 511;
    hash ^= chid << 5;
    hash &= 511;
    for (uint32_t i = 0; i < 512; ++i) {
      const uint32_t slot = (hash + i) & 511;
      const uint32_t hd = mem.rd(inst + slot * 8), ctx = mem.rd(inst + slot * 8 + 4);
      if (ctx == 0) return false;
      uint32_t c_chid = 0, object = 0;
      if (cls == DisplayClass::Nv50) { c_chid = ctx >> 28; object = (ctx >> 10) & 0x3ffff; if ((ctx & 0xf) != c_chid) ++errors; }
      else if (cls == DisplayClass::Gf119) { c_chid = ctx >> 27; object = (ctx >> 9) & 0x3ffff; if (!(ctx & 1)) ++errors; }
      else { c_chid = ctx >> 25; object = (ctx >> 9) & 0xffff; if (!(ctx & 0x40)) ++errors; }
      if (hd != handle || c_chid != chid) continue;
      const uint64_t o = inst + object;
      if (pre()) {
        const uint32_t w0 = mem.rd(o), w1 = mem.rd(o + 4), w2 = mem.rd(o + 8), w3 = mem.rd(o + 12);
        const bool vram = cls == DisplayClass::Nv50 ? ((w0 >> 16) & 3) == 1 : (w0 & 0xf) == 9;
        if (!vram) { ++errors; return false; }
        *start = w2 | uint64_t{w3 & 0xff} << 32;
        *limit = w1 | uint64_t{w3 >> 24} << 32;
      } else {
        if ((mem.rd(o) & 3) != 1) { ++errors; return false; }
        *start = (mem.rd(o + 4) | uint64_t{mem.rd(o + 8)} << 32) << 8;
        *limit = ((mem.rd(o + 12) | uint64_t{mem.rd(o + 16)} << 32) << 8) | 0xff;
      }
      return true;
    }
    return false;
  }

  void notify_at(uint64_t address, bool c37) {
    if (never_notify) return;
    mem.wr(address, c37 ? 0x80000000u : 0x00000001u);
  }

  void commit(Chan& c, bool window, uint32_t w) {
    if (never_latch) return;
    c.arm = c.assy;
    ++c.updates;
    if (!window) {
      for (uint32_t h = 0; h < 4; ++h) {
        uint32_t off_m = 0, iso_m = 0;
        if (cls == DisplayClass::Nv50) { off_m = 0x860 + h * 0x400; iso_m = 0x874 + h * 0x400; }
        else if (cls == DisplayClass::Gf119) { off_m = 0x460 + h * 0x300; iso_m = 0x474 + h * 0x300; }
        else break;
        uint64_t start = 0, limit = 0;
        if (resolve(0, value(c.arm, iso_m), &start, &limit)) scanout[h] = start + (uint64_t{value(c.arm, off_m)} << 8);
      }
    } else {
      uint64_t start = 0, limit = 0;
      if (resolve(1 + w, value(c.arm, 0x240), &start, &limit)) window_scanout[w] = start + (uint64_t{value(c.arm, 0x260)} << 8);
    }
  }

  void apply(Chan& c, uint32_t m, uint32_t d, bool window, uint32_t w) {
    c.methods.push_back({m, d});
    const bool update = (pre() && m == 0x0080) || (!pre() && m == 0x0200);
    if (!update) { c.assy[m] = d; return; }
    // where the notifier goes, as armed at the UPDATE
    bool notify = false;
    uint32_t offset = 0, handle = 0;
    if (pre()) { notify = value(c.assy, 0x84) & 0x80000000; offset = value(c.assy, 0x84) & 0xffc; handle = value(c.assy, 0x88); }
    else if (!window) { notify = value(c.assy, 0x20c) & 0x1000; offset = value(c.assy, 0x20c) & 0xff0; handle = value(c.assy, 0x208); }
    else { handle = value(c.assy, 0x21c); notify = handle != 0; offset = value(c.assy, 0x220) & 0xff0; }
    uint64_t at = 0;
    if (notify) {
      uint64_t start = 0, limit = 0;
      if (!resolve(window ? 1 + w : 0, handle, &start, &limit) || start + offset + 16 > limit + 1) { ++errors; notify = false; }
      else at = start + offset;
    }
    // GF119: a change of the raster, the clock or an OR goes through the supervisor
    if (cls == DisplayClass::Gf119 && supervisor && !window) {
      bool modeset = false;
      for (uint32_t h = 0; h < 4; ++h) {
        pending_masks[h] = 0;
        const uint32_t b = h * 0x300;
        const bool raster = value(c.assy, 0x414 + b) != value(c.arm, 0x414 + b);
        const bool clock = value(c.assy, 0x450 + b) != value(c.arm, 0x450 + b);
        if (raster || clock) { pending_masks[h] = 0x1000 | (clock ? 0x10000 : 0); modeset = true; }
      }
      for (uint32_t s = 0; s < 4; ++s) modeset |= value(c.assy, 0x200 + s * 0x20) != value(c.arm, 0x200 + s * 0x20);
      if (modeset) {
        super_stage = 1;
        regs[0x6100ac] |= 1;
        for (uint32_t h = 0; h < 4; ++h) regs[0x6101d4 + h * 0x800] = pending_masks[h];
        super_commit_notify = notify;
        super_notify_at = at;
        return;
      }
    }
    commit(c, window, w);
    if (notify) notify_at(at, !pre());
  }

  // The firmware's picture on `head` (and on Volta+ through `window`)
  void firmware_surface(uint32_t head, uint64_t offset, uint32_t width, uint32_t height, uint32_t pitch, uint32_t window = 0) {
    if (pre()) {
      const uint32_t b = cls == DisplayClass::Nv50 ? head * 0x400 + 0x400 : head * 0x300;   // 507d: head methods at 0x800 + head * 0x400
      const uint32_t off_m = cls == DisplayClass::Nv50 ? 0x860 : 0x460, size_m = cls == DisplayClass::Nv50 ? 0x868 : 0x468;
      const uint32_t base = cls == DisplayClass::Nv50 ? head * 0x400 : b;
      const uint32_t layout = cls == DisplayClass::Nv50 ? 1u << 20 : 1u << 24;
      for (auto* m : {&core.arm, &core.assy}) {
        (*m)[off_m + base] = static_cast<uint32_t>(offset >> 8);
        (*m)[size_m + base] = height << 16 | width;
        (*m)[size_m + 4 + base] = layout | pitch;
        (*m)[size_m + 8 + base] = 0xcfu << 8;
        (*m)[size_m + 12 + base] = 0x00000002;    // the VBIOS's own handle
      }
    } else {
      for (auto* m : {&core.arm, &core.assy}) (*m)[0x1000 + window * 0x80] = head;
      for (auto* m : {&win[window].arm, &win[window].assy}) {
        (*m)[0x224] = height << 16 | width;
        (*m)[0x228] = 1u << 4;
        (*m)[0x22c] = 0xcf;
        (*m)[0x230] = pitch / 64;
        (*m)[0x240] = 0x00000002;
        (*m)[0x260] = static_cast<uint32_t>(offset >> 8);
        (*m)[0x298] = height << 16 | width;
        (*m)[0x2a4] = height << 16 | width;
      }
    }
  }

  bool wrote(uint32_t off, uint32_t v) const {
    for (auto& w : writes) if (w.first == off && w.second == v) return true;
    return false;
  }
  int write_index(uint32_t off) const {
    for (size_t i = 0; i < writes.size(); ++i) if (writes[i].first == off) return static_cast<int>(i);
    return -1;
  }
  static bool has_method(const Chan& c, uint32_t m, uint32_t d) {
    for (auto& x : c.methods) if (x.first == m && x.second == d) return true;
    return false;
  }
  // Volta+: the window channels' other windows read as unowned
  void unown_windows(uint32_t except) {
    for (uint32_t w = 0; w < 8; ++w)
      if (w != except) { core.arm[0x1000 + w * 0x80] = 0xf; core.assy[0x1000 + w * 0x80] = 0xf; }
  }
};

Chip chip_of(Family family, uint32_t chipset, bool native = true) {
  Chip c;
  c.family = family;
  c.chipset = chipset;
  c.display_native = native;
  c.needs_gsp = family >= Family::Ampere;
  return c;
}

constexpr DisplayTimeouts kQuick{40, 1};

// A firmware framebuffer of 1024x768 at VRAM 0 with a recognisable picture
struct Firmware {
  std::vector<uint8_t> pixels = std::vector<uint8_t>(4096 * 768, 0);
  NvFirmwareFramebuffer fb() {
    NvFirmwareFramebuffer f;
    f.pixels = ByteView(pixels.data(), pixels.size());
    f.width = 1024;
    f.height = 768;
    f.pitch_bytes = 4096;
    return f;
  }
  Firmware() { for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<uint8_t>(i * 7); }
};

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------------
static void test_classes() {
  CHECK(display_class(chip_of(Family::Tesla, 0x50)) == DisplayClass::Nv50);
  CHECK(display_class(chip_of(Family::Fermi, 0xc0)) == DisplayClass::Nv50);
  CHECK(display_class(chip_of(Family::Fermi, 0xd9)) == DisplayClass::Gf119);
  CHECK(display_class(chip_of(Family::Kepler, 0xe4)) == DisplayClass::Gf119);
  CHECK(display_class(chip_of(Family::Pascal, 0x132)) == DisplayClass::Gf119);
  CHECK(display_class(chip_of(Family::Volta, 0x140)) == DisplayClass::Gv100);
  CHECK(display_class(chip_of(Family::Turing, 0x162)) == DisplayClass::Tu102);
  CHECK(display_class(chip_of(Family::Ampere, 0x172)) == DisplayClass::Gsp);
  CHECK(display_class(chip_of(Family::Nv40, 0x40)) == DisplayClass::None);

  // Ampere: refused before a register is touched
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  Firmware fw;
  NvNativeDisplay ampere(model, mem, chip_of(Family::Ampere, 0x172), kQuick);
  ByteView view;
  CHECK(!ampere.engage_display(fw.fb(), &view));
  CHECK(strstr(ampere.display_status(), "GSP") != nullptr);
  CHECK(model.writes.empty());
  // a chip whose display the chip table says is not native
  NvNativeDisplay off(model, mem, chip_of(Family::Kepler, 0xe4, false), kQuick);
  CHECK(!off.engage_display(fw.fb(), &view) && model.writes.empty());
  NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4, false), kQuick);
  CHECK(engine.probe() == DisplayStatus::NotNative);
}

static void test_gf119_takeover() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  model.firmware_surface(1, 0, 1024, 768, 4096);
  model.core.arm[0x414 + 0x300] = model.core.assy[0x414 + 0x300] = 806u << 16 | 1344;   // a raster the takeover must not touch
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(display.display_engaged() && display.engaged_heads() == 0x2);
  CHECK(view.size() == 4096u * 768);
  CHECK(view.equals(ConstByteView(fw.pixels.data(), fw.pixels.size())));   // the picture came along
  const uint64_t fb = display.framebuffer_offset();
  CHECK(fb >= kFirstAllocation && (fb & 0xffff) == 0);
  CHECK(model.errors == 0);
  CHECK(model.scanout[1] == fb);
  // ownership: the VBIOS bit written back, then the handshake bit cleared and waited for
  CHECK(model.wrote(0x6100ac, 0x100));
  CHECK(model.write_index(0x6100ac) < model.write_index(0x6194e8));
  CHECK((model.regs[0x6194e8] & 3) == 0);
  // capabilities copied before the handshake; instance memory; interrupts off
  CHECK(model.write_index(0x6101b4) >= 0 && model.write_index(0x6101b4) < model.write_index(0x6100ac));
  const uint32_t inst = model.regs[0x610010];
  CHECK((inst & 0xf) == 9 && ((uint64_t{inst & ~0xfu} << 8) & 0xffff) == 0);
  CHECK(model.wrote(0x6100b0, 0) && model.wrote(0x610090, 0));
  // the core channel: push buffer in VRAM, then PUT enabled, PUT 0, started
  CHECK((model.regs[0x610494] & 3) == 1);
  CHECK(model.wrote(0x610498, 0x00010000) && model.wrote(0x61049c, 1));
  CHECK(model.regs[0x610490] == 0x01000013);
  // the method stream: notifier armed, the surface of head 1, UPDATE, notifier off
  const auto& m = model.core.methods;
  CHECK(Model::has_method(model.core, 0x0088, 0xf0000000));
  CHECK(Model::has_method(model.core, 0x0084, 0x80000000));
  CHECK(Model::has_method(model.core, 0x0460 + 0x300, static_cast<uint32_t>(fb >> 8)));
  CHECK(Model::has_method(model.core, 0x0468 + 0x300, 768u << 16 | 1024));
  CHECK(Model::has_method(model.core, 0x046c + 0x300, 1u << 24 | 4096));
  CHECK(Model::has_method(model.core, 0x0470 + 0x300, 0xcfu << 8));
  CHECK(Model::has_method(model.core, 0x0474 + 0x300, 0xf0000001));
  CHECK(m.size() >= 2 && m[m.size() - 2].first == 0x0080 && m.back().first == 0x0084 && m.back().second == 0);
  for (auto& x : m) CHECK(x.first < 0x0400 + 0x300 || x.first >= 0x0460 + 0x300);   // nothing of the raster or OR sent
  CHECK(model.core.arm[0x414 + 0x300] == (806u << 16 | 1344));
  CHECK(model.core.updates == 1);
  // what is drawn goes to the screen; release copies it back and restores the firmware's surface
  view[0] = 0xAB;
  view[4096 * 767 + 5] = 0xCD;
  display.disengage_display();
  CHECK(!display.display_engaged());
  CHECK(fw.pixels[0] == 0xAB && fw.pixels[4096 * 767 + 5] == 0xCD);
  CHECK(model.core.arm[0x460 + 0x300] == 0 && model.scanout[1] == 0);
  CHECK(model.core.arm[0x474 + 0x300] == 0xf0000001);
  CHECK(mem.released.size() == 1 && mem.released[0] == fb);
  CHECK(model.errors == 0);
  // engaging again works on the claimed engine
  CHECK(display.engage_display(fw.fb(), &view) && model.scanout[1] != 0);
}

static void test_gf119_clone() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  model.firmware_surface(0, 0x100000, 1024, 768, 4096);
  model.firmware_surface(2, 0x100000, 1024, 768, 4096);
  model.firmware_surface(3, 0x600000, 800, 600, 3328);   // another picture: left alone
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(Family::Maxwell, 0x117), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(display.engaged_heads() == 0x5);
  CHECK(model.scanout[0] == display.framebuffer_offset() && model.scanout[2] == display.framebuffer_offset());
  CHECK(model.core.arm[0x460 + 3 * 0x300] == 0x6000);
  display.disengage_display();
  CHECK(model.scanout[0] == 0x100000 && model.scanout[2] == 0x100000);
  // a firmware mode no head shows
  NvFirmwareFramebuffer other = fw.fb();
  other.height = 600;
  CHECK(!display.engage_display(other, &view));
  CHECK(strstr(display.display_status(), "no head") != nullptr);
}

static void test_nv50_takeover() {
  FakeMemory mem;
  Model model(DisplayClass::Nv50, mem);
  model.firmware_surface(0, 0, 1024, 768, 4096);
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(Family::Tesla, 0x84), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(model.errors == 0);
  CHECK(model.scanout[0] == display.framebuffer_offset());
  CHECK(model.wrote(0x610024, 0x100));
  CHECK(model.regs[0x610200] == 0x01000013);
  CHECK(model.wrote(0x610208, 0x00010000) && model.wrote(0x61020c, 0));
  CHECK(Model::has_method(model.core, 0x0860, static_cast<uint32_t>(display.framebuffer_offset() >> 8)));
  CHECK(Model::has_method(model.core, 0x086c, 1u << 20 | 4096));
  CHECK(Model::has_method(model.core, 0x0874, 0xf0000001));
  CHECK(model.regs[0x61002c] == 0 && model.regs[0x610028] == 0);
  display.disengage_display();
  CHECK(model.scanout[0] == 0);
}

static void test_volta_turing(DisplayClass cls, Family family, uint32_t chipset) {
  FakeMemory mem;
  Model model(cls, mem);
  model.unown_windows(2);
  model.firmware_surface(1, 0, 1024, 768, 4096, 2);
  Firmware fw;
  NvNativeDisplay display(model, mem, chip_of(family, chipset), kQuick);
  ByteView view;
  CHECK(display.engage_display(fw.fb(), &view));
  CHECK(model.errors == 0);
  const uint64_t fb = display.framebuffer_offset();
  CHECK(model.window_scanout[2] == fb);
  // ownership and instance memory
  CHECK((model.regs[0x6254e8] & 2) == 0);
  CHECK(model.regs[0x610010] == 0x9 && (uint64_t{model.regs[0x610014]} << 16) >= kFirstAllocation);
  CHECK(model.regs[0x640008] == (cls == DisplayClass::Tu102 ? 0x21u : 0x1234u));
  // the core channel and window 2's channel (control slot 3)
  CHECK(model.wrote(0x610b28, 1) && model.wrote(0x610b2c, 0x40));
  CHECK((model.regs[0x610b00] & 0x30000) == 0x10000);
  CHECK(model.regs[0x610b00 + 3 * 0x10] == 0x11);
  CHECK((model.regs[0x610b24 + 3 * 0x10] & 3) == 1);
  // the window's methods: notifier, surface, UPDATE
  const Model::Chan& w = model.win[2];
  CHECK(Model::has_method(w, 0x021c, 0xf0000000));
  CHECK(Model::has_method(w, 0x0260, static_cast<uint32_t>(fb >> 8)));
  CHECK(Model::has_method(w, 0x0240, 0xf0000001));
  CHECK(Model::has_method(w, 0x0230, 4096 / 64));
  CHECK(Model::has_method(w, 0x0224, 768u << 16 | 1024));
  CHECK(w.methods.back().first == 0x0200 && w.methods.back().second == 1);
  CHECK(model.core.methods.empty());     // the core channel had nothing to say
  display.disengage_display();
  CHECK(model.window_scanout[2] == 0);
  CHECK(mem.released.size() == 1);
}

static void test_timeouts() {
  Firmware fw;
  ByteView view;
  {
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.vbios_never_releases = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "VBIOS") != nullptr);
    CHECK(mem.allocations == mem.releases);      // the framebuffer went back
  }
  {
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_start = true;
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(engine.probe() == DisplayStatus::Ok);
    CHECK(engine.claim() == DisplayStatus::ChannelTimeout);
  }
  {
    FakeMemory mem;
    Model model(DisplayClass::Gv100, mem);
    model.unown_windows(0);
    model.firmware_surface(0, 0, 1024, 768, 4096, 0);
    model.never_start = true;
    NvDisplayEngine engine(model, mem, chip_of(Family::Volta, 0x140), kQuick);
    CHECK(engine.probe() == DisplayStatus::Ok);
    CHECK(engine.claim(0) == DisplayStatus::ChannelTimeout);
  }
  {   // the engine never fetches: PUT moves, GET does not
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_fetch = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "push buffer") != nullptr);
    CHECK(model.scanout[0] == ~0ull && model.core.arm[0x460] == 0);
  }
  {   // fetched and armed, but the notifier never comes: the old surface is sent back
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_notify = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "complete") != nullptr);
    CHECK(model.core.arm[0x460] == 0 && model.scanout[0] == 0);
    CHECK(mem.released.size() == 1);
  }
  {   // the engine takes the methods and notifies, but the armed state does not change
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.never_latch = true;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "take") != nullptr);
  }
}

static void test_refusals() {
  Firmware fw;
  ByteView view;
  {
    FakeMemory mem;
    mem.fail = true;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view));
    CHECK(strstr(display.display_status(), "video memory") != nullptr);
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(engine.probe() == DisplayStatus::Ok && engine.claim() == DisplayStatus::NoMemory);
  }
  {   // a block-linear firmware surface is not taken over
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    model.core.arm[0x46c] = 4096;
    NvNativeDisplay display(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(!display.engage_display(fw.fb(), &view) && model.writes.empty());
  }
  {   // a GPU that reads all ones
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.regs[0x022448] = 0xffffffff;
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    CHECK(engine.probe() == DisplayStatus::NoHeads);
  }
  {   // bad surfaces and operations before claim
    FakeMemory mem;
    Model model(DisplayClass::Gf119, mem);
    model.firmware_surface(0, 0, 1024, 768, 4096);
    NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
    ScanoutSurface s;
    CHECK(engine.probe() == DisplayStatus::Ok && engine.armed_surface(0, &s));
    CHECK(engine.set_surface(0, s) == DisplayStatus::NotClaimed);
    CHECK(engine.claim() == DisplayStatus::Ok);
    ScanoutSurface bad = s;
    bad.offset = 0x123;
    CHECK(engine.set_surface(0, bad) == DisplayStatus::UnsupportedSurface);
    bad = s;
    bad.pitch_bytes = 4000;
    CHECK(engine.set_surface(0, bad) == DisplayStatus::UnsupportedSurface);
    CHECK(engine.set_surface(7, s) == DisplayStatus::BadArgument);
    // the push buffer wraps around many times
    s.context_dma = NvDisplayEngine::vram_handle();
    for (uint32_t i = 0; i < 200; ++i) {
      s.offset = (i & 1) ? 0x400000 : 0x800000;
      CHECK(engine.set_surface(0, s) == DisplayStatus::Ok);
    }
    CHECK(model.core.jumps >= 2 && model.scanout[0] == 0x400000 && model.errors == 0);
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// Milestone B
// ---------------------------------------------------------------------------------------------------------------------------------------
static DisplayTiming mode_1080p() {
  DisplayTiming t;
  t.clock_khz = 148500;
  t.hdisplay = 1920; t.hsync_start = 2008; t.hsync_end = 2052; t.htotal = 2200;
  t.vdisplay = 1080; t.vsync_start = 1084; t.vsync_end = 1089; t.vtotal = 1125;
  return t;
}

static void test_timings() {
  HeadRaster r;
  CHECK(compute_head_raster(mode_1080p(), &r));
  CHECK(r.h_total == 2200 && r.h_sync_end == 43 && r.h_blank_end == 191 && r.h_blank_start == 2111);
  CHECK(r.v_total == 1125 && r.v_sync_end == 4 && r.v_blank_end == 40 && r.v_blank_start == 1120);
  CHECK(r.v_blank2_start == 1 && r.v_blank2_end == 0 && r.blank_us == 637);
  CHECK(r.raster_size() == (1125u << 16 | 2200) && r.blank_start() == (1120u << 16 | 2111));

  DisplayTiming t;
  t.clock_khz = 74250;
  t.hdisplay = 1280; t.hsync_start = 1390; t.hsync_end = 1430; t.htotal = 1650;
  t.vdisplay = 720; t.vsync_start = 725; t.vsync_end = 730; t.vtotal = 750;
  CHECK(compute_head_raster(t, &r));
  CHECK(r.h_sync_end == 39 && r.h_blank_end == 259 && r.h_blank_start == 1539);
  CHECK(r.v_sync_end == 4 && r.v_blank_end == 24 && r.v_blank_start == 744 && r.blank_us == 622);

  DisplayTiming i = mode_1080p();      // 1080i
  i.clock_khz = 74250;
  i.vsync_end = 1094;
  i.interlaced = true;
  CHECK(compute_head_raster(i, &r));
  CHECK(r.v_sync_end == 4 && r.v_blank_end == 19 && r.v_blank_start == 559 && r.blank_us == 592);
  CHECK(r.v_blank2_end == 581 && r.v_blank2_start == 1121 && r.v_total == 1125 && r.interlaced);

  DisplayTiming bad = mode_1080p();
  bad.hsync_end = bad.hsync_start;
  CHECK(!compute_head_raster(bad, &r));
  bad = mode_1080p();
  bad.clock_khz = 0;
  CHECK(!compute_head_raster(bad, &r));
  bad = mode_1080p();
  bad.vtotal = 1000;
  CHECK(!compute_head_raster(bad, &r));
}

static void test_routes() {
  DcbOutput tmds;
  tmds.type = DcbOutputType::Tmds;
  tmds.heads = 0x3;
  tmds.or_mask = 0x4;
  tmds.sorconf_link = 3;
  OutputRoute r;
  CHECK(route_output(tmds, 0xf, 0xf, 1, 297000, &r) && r.sor == 2 && r.head == 1 && r.protocol == 0x5 && r.link == 3);
  CHECK(route_output(tmds, 0xf, 0xf, 3, 108000, &r) && r.head == 0 && r.protocol == 0x1 && r.link == 1);
  DcbOutput dp;
  dp.type = DcbOutputType::Dp;
  dp.heads = 0xf;
  dp.or_mask = 0x2;
  dp.sorconf_link = 2;
  CHECK(route_output(dp, 0xf, 0xf, 2, 148500, &r) && r.sor == 1 && r.head == 2 && r.protocol == 0x9 && r.link == 2 && r.dp);
  CHECK(!route_output(dp, 0x1, 0xf, 0, 148500, &r));     // its SOR is not there
  dp.heads = 0x10;
  CHECK(!route_output(dp, 0xf, 0xf, 0, 148500, &r));     // nor a head it may use
  DcbOutput analog;
  analog.type = DcbOutputType::Analog;
  analog.heads = 1;
  analog.or_mask = 1;
  CHECK(!route_output(analog, 0xf, 0xf, 0, 148500, &r));
}

namespace {
struct Clock final : INvPixelClock {
  std::vector<std::pair<uint32_t, uint32_t>> calls;
  bool set_vpll(uint32_t head, uint32_t khz) override { calls.push_back({head, khz}); return true; }
};
struct Gpio final : INvGpio {
  std::vector<std::pair<uint8_t, bool>> calls;
  bool set_function(uint8_t function, bool on) override { calls.push_back({function, on}); return true; }
};
// A DisplayPort sink whose clock recovery fails on some lanes at some link rates.
struct Sink final : IDpcdAux {
  std::vector<uint8_t> dpcd = std::vector<uint8_t>(0x800, 0);
  std::map<uint8_t, uint8_t> broken;     // link rate code -> lanes that never recover
  Sink() {
    dpcd[0x000] = 0x12;
    dpcd[0x001] = 0x14;
    dpcd[0x002] = 0x84;
    dpcd[0x003] = 0x01;
    dpcd[0x006] = 0x01;
  }
  void model() {
    const uint8_t rate = dpcd[0x100], lanes = dpcd[0x101] & 0x1f, pattern = dpcd[0x102] & 3;
    bool all = true;
    for (uint32_t l = 0; l < 4; ++l) {
      uint8_t nibble = 0;
      if (l < lanes) {
        const bool ok = pattern != 0 && !(broken[rate] & (1u << l));
        nibble = ok ? (pattern >= 2 ? 7 : 1) : 0;
        if (!ok || pattern < 2) all = false;
      }
      const uint32_t at = 0x202 + l / 2, shift = l % 2 * 4;
      dpcd[at] = static_cast<uint8_t>((dpcd[at] & ~(0xf << shift)) | (nibble << shift));
    }
    dpcd[0x204] = all ? 1 : 0;
    dpcd[0x206] = dpcd[0x207] = 0;
  }
  bool dpcd_read(uint32_t address, ByteView out) override {
    if (address + out.size() > dpcd.size()) return false;
    if (address == 0x200 || address == 0x202) model();
    for (size_t i = 0; i < out.size(); ++i) out[i] = dpcd[address + i];
    return true;
  }
  bool dpcd_write(uint32_t address, ConstByteView data) override {
    if (address + data.size() > dpcd.size()) return false;
    for (size_t i = 0; i < data.size(); ++i) dpcd[address + i] = data[i];
    return true;
  }
};
}  // namespace

static void test_modeset_tmds() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe4), kQuick);
  CHECK(engine.probe() == DisplayStatus::Ok);
  Clock clock;
  NvModeSetter setter(engine, &clock, nullptr, nullptr);
  NvModeSetter::Request req;
  req.output.type = DcbOutputType::Tmds;
  req.output.heads = 0x3;
  req.output.or_mask = 0x2;
  req.output.sorconf_link = 1;
  req.head = 1;
  req.timing = mode_1080p();
  req.surface.offset = 0x1000000;
  req.surface.width = 1920;
  req.surface.height = 1080;
  req.surface.pitch_bytes = 7680;
  req.surface.format = 0xcf;
  req.surface.pitch_linear = true;
  CHECK(setter.set_mode(req) == DisplayStatus::Ok);
  CHECK(model.errors == 0);
  CHECK(setter.supervisor_stages() == 7 && model.super_acks == 3);
  CHECK(clock.calls.size() == 1 && clock.calls[0].first == 1 && clock.calls[0].second == 148500);
  const uint32_t h = 0x300;
  CHECK(model.core.arm[0x0200 + 0x20] == (0x2u | 0x1u << 8));             // SOR 1 owned by head 1, single TMDS A
  CHECK(model.core.arm[0x0404 + h] == 5u << 6);
  CHECK(model.core.arm[0x0408 + h] == (0x31ec6000u | 1u << 25));
  CHECK(model.core.arm[0x0414 + h] == (1125u << 16 | 2200) && model.core.arm[0x0418 + h] == (4u << 16 | 43));
  CHECK(model.core.arm[0x041c + h] == (40u << 16 | 191) && model.core.arm[0x0420 + h] == (1120u << 16 | 2111) && model.core.arm[0x0424 + h] == 1);
  CHECK(model.core.arm[0x0450 + h] == 148500000u && model.core.arm[0x0454 + h] == 0x00200000 && model.core.arm[0x0458 + h] == 148500000u);
  CHECK(model.core.arm[0x04b8 + h] == (1080u << 16 | 1920) && model.core.arm[0x04c8 + h] == (1080u << 16 | 1920));
  CHECK(model.scanout[1] == 0x1000000);
  CHECK((model.regs[0x612300 + 0x800] & 0x007c0707) == 0x0au << 18);
  CHECK(model.regs[0x6101d4 + 0x800] == 0 && (model.regs[0x6100ac] & 7) == 0);
  // an unsupported depth, a surface of the wrong size
  req.depth_bpp = 16;
  CHECK(setter.set_mode(req) == DisplayStatus::BadArgument);
  req.depth_bpp = 24;
  req.surface.width = 1280;
  CHECK(setter.set_mode(req) == DisplayStatus::UnsupportedSurface);
  // Volta and NV50 mode setting is not done
  Model volta(DisplayClass::Gv100, mem);
  NvDisplayEngine ve(volta, mem, chip_of(Family::Volta, 0x140), kQuick);
  CHECK(ve.probe() == DisplayStatus::Ok);
  NvModeSetter vs(ve, &clock, nullptr, nullptr);
  CHECK(vs.set_mode(req) == DisplayStatus::NotImplemented);
}

static void test_dp_training_fallback() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  NvDisplayEngine engine(model, mem, chip_of(Family::Pascal, 0x134), kQuick);
  CHECK(engine.probe() == DisplayStatus::Ok);
  Sink sink;
  sink.broken[0x14] = 0xf;      // HBR2 never trains
  sink.broken[0x0a] = 0xc;      // HBR: lanes 2 and 3 are bad
  const Dpcd::Capabilities caps = [&] {
    Dpcd::Capabilities c;
    CHECK(Dpcd::read_capabilities(sink, false, &c));
    return c;
  }();
  CHECK(caps.valid && caps.max_lanes == 4 && caps.max_link_rate_code == 0x14);
  DcbOutput dp;
  dp.type = DcbOutputType::Dp;
  NvSorDp sor(model, 0, 1, placeholder_dp_drive_table(), kQuick);
  // 1080p60 at 24 bpp: 445 500 kB/s
  const DpTrainResult r = train_dp_link(sink, sor, caps, dp, 148500ull * 3);
  CHECK(r.trained && r.bandwidth_code == 0x0a && r.lanes == 2);
  CHECK(r.attempts == 5);
  const uint8_t want_bw[] = {0x14, 0x14, 0x14, 0x0a, 0x0a}, want_lanes[] = {4, 2, 1, 4, 2};
  for (uint32_t i = 0; i < 5; ++i) CHECK(r.tried_bandwidth[i] == want_bw[i] && r.tried_lanes[i] == want_lanes[i]);
  CHECK((model.regs[0x61c10c] & 0x001f4000) == (0x3u << 16 | 0x4000));
  CHECK(((model.regs[0x612300] >> 18) & 0x1f) == 0x0a);
  CHECK((model.regs[0x61c130] & 0xf) == (1u << 2 | 1u << 1));     // lanes 0 and 1 are bytes 2 and 1
  CHECK(model.regs[0x61c110] == 0x10101010);                       // pattern off at the end
  CHECK(sink.dpcd[0x100] == 0x0a && (sink.dpcd[0x101] & 0x1f) == 2);
  // the DCB entry caps the rate: HBR at most, so HBR2 is never tried
  dp.dp_max_link_rate = 0x0a;
  const DpTrainResult capped = train_dp_link(sink, sor, caps, dp, 148500ull * 3);
  CHECK(capped.trained && capped.tried_bandwidth[0] == 0x0a && capped.attempts == 2);
  // nothing carries 4K60 at 24 bpp once HBR2 is broken
  dp.dp_max_link_rate = 0;
  const DpTrainResult big = train_dp_link(sink, sor, caps, dp, 594000ull * 3);
  CHECK(!big.trained && big.attempts == 1);
  sink.broken.clear();
  sink.broken[0x14] = 0xf;
  sink.broken[0x0a] = 0xf;
  sink.broken[0x06] = 0xf;
  const DpTrainResult none = train_dp_link(sink, sor, caps, dp, 148500ull * 3);
  CHECK(!none.trained && none.last == DpLinkTrainer::Result::ClockRecoveryTries);
}

static void test_modeset_edp() {
  FakeMemory mem;
  Model model(DisplayClass::Gf119, mem);
  NvDisplayEngine engine(model, mem, chip_of(Family::Kepler, 0xe7), kQuick);
  CHECK(engine.probe() == DisplayStatus::Ok);
  Clock clock;
  Gpio gpio;
  Sink sink;
  sink.broken[0x14] = 0xc;      // HBR2 x4 fails, HBR2 x2 trains
  NvModeSetter setter(engine, &clock, &gpio, &sink);
  NvModeSetter::Request req;
  req.output.type = DcbOutputType::Dp;
  req.output.heads = 0x1;
  req.output.or_mask = 0x1;
  req.output.sorconf_link = 1;
  req.edp = true;
  req.timing = mode_1080p();
  req.surface.offset = 0x1000000;
  req.surface.width = 1920;
  req.surface.height = 1080;
  req.surface.pitch_bytes = 7680;
  req.surface.format = 0xe6;
  req.surface.pitch_linear = true;
  CHECK(setter.set_mode(req) == DisplayStatus::Ok);
  CHECK(gpio.calls.size() == 1 && gpio.calls[0].first == 0x01 && gpio.calls[0].second);
  CHECK(setter.dp_result().trained && setter.dp_result().bandwidth_code == 0x14 && setter.dp_result().lanes == 2);
  const DpTransferUnit& tu = setter.transfer_unit();
  CHECK(tu.valid && tu.tu >= 32 && tu.tu <= 64 && tu.watermark > 6 && tu.hblank_symbols > 0);
  CHECK((model.regs[0x616610] & 0x0800003f) == (0x08000000 | (tu.watermark & 0x3f)));
  CHECK(((model.regs[0x61c10c] & 0x1fc) >> 2) == tu.tu);
  CHECK(model.core.arm[0x0200] == (0x1u | 0x8u << 8));             // SOR 0, head 0, DP link A
  CHECK(model.errors == 0);
  // without the GPIO the panel cannot be powered
  NvModeSetter no_gpio(engine, &clock, nullptr, &sink);
  CHECK(no_gpio.set_mode(req) == DisplayStatus::NoOutput);
  // a sink that trains nowhere
  sink.broken[0x14] = sink.broken[0x0a] = sink.broken[0x06] = 0xf;
  CHECK(setter.set_mode(req) == DisplayStatus::LinkTraining);
  // the transfer unit of a mode the link cannot carry
  HeadRaster r;
  CHECK(compute_head_raster(mode_1080p(), &r));
  CHECK(!compute_dp_transfer_unit(r, 0x06, 1, true, 24).valid);
  CHECK(compute_dp_transfer_unit(r, 0x14, 4, true, 24).valid);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_classes();
  test_gf119_takeover();
  test_gf119_clone();
  test_nv50_takeover();
  test_volta_turing(DisplayClass::Gv100, Family::Volta, 0x140);
  test_volta_turing(DisplayClass::Tu102, Family::Turing, 0x164);
  test_timeouts();
  test_refusals();
  test_timings();
  test_routes();
  test_modeset_tmds();
  test_dp_training_fallback();
  test_modeset_edp();
  if (g_failed) {
    printf("NvDisplayTest: %d FAILED\n", g_failed);
    return 1;
  }
  printf("NvDisplayTest: all passed\n");
  return 0;
}
