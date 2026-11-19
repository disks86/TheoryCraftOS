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

// The display engine model of the NVIDIA display host tests (NvDisplayTest, NvidiaSecondaryGpuTest): registers (ownership handshake, channel
// control, the user areas' PUT/GET, the readback of assembly and armed state, the supervisor) and memory (the model fetches the push buffers
// from the fake video memory and decodes methods and data as EVO does, looks context DMA handles up through the RAMHT in the instance memory
// and writes notifiers where the context DMA points); a pixel clock, a GPIO and a DisplayPort sink that record what they are asked. The
// register and method numbers are typed in here separately from NvDisplayHw.hpp; both come from the same (remembered) nouveau sources, so
// the model checks that the driver is consistent with itself and with nouveau's sequences, not that the numbers are right on hardware.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <map>
#include <vector>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayMode.hpp"

using namespace hardware;
using namespace hardware::nv;

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

[[maybe_unused]] Chip chip_of(Family family, uint32_t chipset, bool native = true) {
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
