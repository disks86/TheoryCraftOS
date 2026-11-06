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

// Host test for the AMD XDNA NPU hardware layer: a pretend NPU answers the SMU, PSP and mailbox protocols of the Linux amdxdna driver, and a pretend
// firmware answers management messages. Malformed device behaviour (bad pointers, sizes, ids, truncated and bit-flipped headers) must fail cleanly.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <vector>
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaDevice.hpp"
#include "Hardware/NeuralProcessingUnit/AMD/AmdXdnaNpu.hpp"

using namespace hardware;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// Plain memory for the firmware buffers, so the pretend PSP can look at what it was given.
struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  bool fail = false;
  void* allocate(uint64_t size) override { return allocate_aligned(size, 16); }
  void* allocate_aligned(uint64_t size, uint64_t alignment) override {
    if (fail) return nullptr;
    void* p = aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    if (p) ++live;
    return p;
  }
  void deallocate(void* p) override {
    if (p) { --live; free(p); }
  }
};

// A clock that moves on a millisecond every time it is read: one-second waits end after about a thousand polls.
struct TestClock final : IClock {
  mutable uint64_t now = 0;
  uint64_t step = 1000;
  const char* name() const override { return "test clock"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  uint64_t now_us() const override { return now += step; }
};

struct SimBar final : INpuRegisters {
  std::map<uint64_t, uint32_t> regs;
  std::function<void(uint64_t, uint32_t)> on_write;
  std::function<void(uint64_t)> on_read;
  uint32_t read32(uint64_t offset) override {
    if (on_read) on_read(offset);
    auto it = regs.find(offset);
    return it == regs.end() ? 0 : it->second;
  }
  void write32(uint64_t offset, uint32_t value) override {
    regs[offset] = value;
    if (on_write) on_write(offset, value);
  }
  void write64(uint64_t offset, uint64_t value) override {
    write32(offset, static_cast<uint32_t>(value));
    write32(offset + 4, static_cast<uint32_t>(value >> 32));
  }
};

constexpr uint32_t kTomb = AmdXdnaMailbox::kTombstone;

struct PspCall {
  uint32_t cmd, arg0, arg1, size;
};

// A pretend NPU: SMU and PSP mailboxes, the firmware's management channel description, and the firmware's side of the rings.
struct SimNpu {
  AmdXdnaIdentity id;
  const AmdXdnaLayout& layout;
  SimBar bar[6];
  AmdXdnaBars bars;
  TestClock clock;

  // SMU
  std::vector<std::pair<uint32_t, uint32_t>> smu_log;  // (command, argument)
  bool smu_silent = false;
  uint32_t smu_result = 1, smu_fail_cmd = 0;
  // PSP
  std::vector<PspCall> psp_log;
  bool psp_never_ready = false, psp_hang = false;
  uint32_t psp_error = 0, psp_fail_cmd = 0;
  std::vector<uint8_t> firmware_seen, cert_seen;
  bool firmware_aligned = true;
  // firmware
  bool fw_running = false, fw_never_alive = false, wait_mode_stuck = false;
  uint32_t magic = AmdXdnaHw::kMgmtMagic, prot_major = 6, prot_minor = 18;
  uint32_t mbox_regs = 0, x2i_off = 0x20000, i2x_off = 0x21000, info_off = 0x30000, x2i_size = 0x400, i2x_size = 0x400;
  int32_t ring_shift = 0;  // moves the published i2x ring by this much (to push it outside the BAR)
  // firmware side of the rings (offsets inside each ring)
  uint32_t dev_x2i_head = 0, dev_i2x_tail = 0;
  bool respond = true, i2x_fill_to_end = false;
  std::vector<std::pair<uint32_t, std::vector<uint32_t>>> received;  // (opcode, payload words)
  std::function<bool(uint32_t opcode, const std::vector<uint32_t>& req, std::vector<uint32_t>& resp)> responder;
  std::function<void(std::vector<uint32_t>& message)> corrupt;  // header + payload words, before they go into the ring
  std::deque<std::vector<uint32_t>> held;                        // responses not yet delivered when `respond` is off
  uint32_t status_for = 0, status_value = 0;                     // answer this opcode with this status
  uint32_t ring_reads_outside = 0;
  uint32_t x2i_tombstones = 0, i2x_tombstones = 0, i2x_exact_end = 0;  // how often each ring wrapped, and how

  // AIE2 address helpers: BAR offsets of the rings and registers
  uint8_t ring_bar() const { return layout.sram_bar; }
  uint8_t reg_bar() const { return layout.mbox_bar; }
  bool aie4() const { return id.generation == AmdXdnaGeneration::Aie4; }
  uint32_t x2i_head_reg() const { return mbox_regs + 0x0; }
  uint32_t x2i_tail_reg() const { return mbox_regs + 0x4; }
  uint32_t i2x_head_reg() const { return mbox_regs + 0x10; }   // the interrupt register follows it at +0x14
  uint32_t i2x_tail_reg() const { return mbox_regs + 0x18; }

  SimNpu(uint16_t device, uint8_t revision) : id(amd_xdna_identify(device, revision)), layout(amd_xdna_layout(id.model)) {
    mbox_regs = 0x8000;
    if (aie4()) { x2i_off = 0x1000; i2x_off = 0x2000; info_off = 0; }
    for (uint8_t i = 0; i < 6; ++i)
      if (layout.bar_window[i]) bars.bar[i] = {&bar[i], layout.bar_window[i]};
    const AmdXdnaRegister status = layout.psp[static_cast<uint8_t>(AmdXdnaPspReg::Status)];
    if (status.present()) bar[status.bar].regs[status.offset] = AmdXdnaHw::kPspStatusReady;
    for (uint8_t i = 0; i < 6; ++i) {
      bar[i].on_write = [this, i](uint64_t off, uint32_t v) { written(i, off, v); };
    }
    bar[ring_bar()].on_read = [this](uint64_t off) {
      const bool in_x2i = off >= x2i_off && off < x2i_off + x2i_size;
      const bool in_i2x = off >= i2x_off && off < i2x_off + i2x_size;
      const bool in_info = off >= info_off && off < info_off + 64;
      const bool alive = !aie4() && off == layout.fw_alive_offset;
      if (!in_x2i && !in_i2x && !in_info && !alive) ++ring_reads_outside;
    };
    responder = [this](uint32_t opcode, const std::vector<uint32_t>& req, std::vector<uint32_t>& resp) { return default_answer(opcode, req, resp); };
  }

  uint32_t& reg(AmdXdnaRegister r) { return bar[r.bar].regs[r.offset]; }
  bool is(AmdXdnaRegister r, uint8_t b, uint64_t off) const { return r.present() && r.bar == b && r.offset == off; }
  AmdXdnaRegister smu(AmdXdnaSmuReg r) const { return layout.smu[static_cast<uint8_t>(r)]; }
  AmdXdnaRegister psp(AmdXdnaPspReg r) const { return layout.psp[static_cast<uint8_t>(r)]; }

  void written(uint8_t b, uint64_t off, uint32_t v) {
    if (is(smu(AmdXdnaSmuReg::Intr), b, off) && v == 1) smu_kick();
    if (is(psp(AmdXdnaPspReg::Intr), b, off) && v == layout.psp_notify && v != 0) psp_kick();
    if (fw_running && b == reg_bar() && off == x2i_tail_reg()) firmware_consume();
  }

  void smu_kick() {
    const uint32_t cmd = reg(smu(AmdXdnaSmuReg::Cmd)), arg = reg(smu(AmdXdnaSmuReg::Arg));
    smu_log.push_back({cmd, arg});
    if (smu_silent) return;
    if (cmd == 5 || cmd == 6) reg(smu(AmdXdnaSmuReg::Out)) = arg;  // the clock actually set
    reg(smu(AmdXdnaSmuReg::Resp)) = cmd == smu_fail_cmd ? 0xFF : smu_result;
  }

  void psp_kick() {
    const AmdXdnaRegister cmdr = psp(AmdXdnaPspReg::Cmd), arg2r = psp(AmdXdnaPspReg::Arg2);
    const uint32_t c = reg(cmdr);
    PspCall call{};
    if (cmdr.bar == arg2r.bar && cmdr.offset == arg2r.offset) {  // AIE4: ARG2 replaced the command register: command in the top byte
      call.cmd = c >> 24;
      call.size = c & 0xFFFFFF;
    } else {
      call.cmd = c;
      call.size = reg(arg2r);
    }
    call.arg0 = reg(psp(AmdXdnaPspReg::Arg0));
    call.arg1 = reg(psp(AmdXdnaPspReg::Arg1));
    psp_log.push_back(call);
    if (psp_hang) return;
    uint32_t result = (call.cmd == psp_fail_cmd) ? psp_error : 0;
    if (call.cmd == 1 || call.cmd == 4) {  // validate: look at the buffer
      const uint64_t addr = (uint64_t{call.arg1} << 32) | call.arg0;
      const uint32_t align = call.cmd == 1 ? 0x10000 : 0x8000;
      if (addr % align != 0 || call.size % align != 0 || call.size == 0) firmware_aligned = false;
      const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
      (call.cmd == 1 ? firmware_seen : cert_seen).assign(p, p + call.size);
    }
    if (call.cmd == 2 && result == 0 && !fw_never_alive) publish_channel();
    if (call.cmd == 3) fw_running = false;
    reg(psp(AmdXdnaPspReg::Resp)) = result;
    reg(psp(AmdXdnaPspReg::Status)) |= AmdXdnaHw::kPspStatusReady;
  }

  void sram(uint32_t off, uint32_t v) { bar[ring_bar()].regs[off] = v; }

  void publish_channel() {
    fw_running = true;
    dev_x2i_head = dev_i2x_tail = 0;
    bar[reg_bar()].regs[x2i_head_reg()] = 0;
    bar[reg_bar()].regs[x2i_tail_reg()] = 0;
    bar[reg_bar()].regs[i2x_head_reg()] = 0;
    bar[reg_bar()].regs[i2x_tail_reg()] = 0;
    const uint32_t i2x_published = static_cast<uint32_t>(static_cast<int64_t>(i2x_off) + ring_shift);
    if (aie4()) {
      const uint32_t words[12] = {1, prot_major, prot_minor, x2i_tail_reg(), x2i_head_reg(), x2i_off, x2i_size, i2x_tail_reg(), i2x_head_reg(), i2x_published, i2x_size, 3};
      for (uint32_t i = 0; i < 12; ++i) sram(info_off + i * 4, words[i]);
      return;
    }
    const uint32_t m = layout.mbox_dev_addr, s = layout.sram_dev_addr;
    const uint32_t words[12] = {m + x2i_tail_reg(), m + x2i_head_reg(), s + x2i_off, x2i_size, m + i2x_tail_reg(), m + i2x_head_reg(), s + i2x_published,
                                i2x_size, magic, 2, prot_major, prot_minor};
    for (uint32_t i = 0; i < 12; ++i) sram(info_off + i * 4, words[i]);
    sram(layout.fw_alive_offset, s + info_off);
  }

  uint32_t ring(uint32_t start, uint32_t off) { return bar[ring_bar()].regs[start + off]; }
  void ring_put(uint32_t start, uint32_t off, uint32_t v) { bar[ring_bar()].regs[start + off] = v; }

  void firmware_consume() {
    const uint32_t tail = bar[reg_bar()].regs[x2i_tail_reg()];
    for (int guard = 0; dev_x2i_head != tail && guard < 1000; ++guard) {
      uint32_t total = ring(x2i_off, dev_x2i_head);
      if (total == kTomb) { dev_x2i_head = 0; ++x2i_tombstones; continue; }
      const uint32_t id = ring(x2i_off, dev_x2i_head + 8), opcode = ring(x2i_off, dev_x2i_head + 12);
      std::vector<uint32_t> req;
      for (uint32_t i = 0; i < total / 4; ++i) req.push_back(ring(x2i_off, dev_x2i_head + 16 + i * 4));
      dev_x2i_head += 16 + total;
      received.push_back({opcode, req});
      std::vector<uint32_t> resp;
      if (!responder(opcode, req, resp)) continue;
      if (opcode == status_for && !resp.empty()) resp[0] = status_value;
      std::vector<uint32_t> message = {static_cast<uint32_t>(resp.size() * 4), static_cast<uint32_t>(resp.size() * 4) | (1u << 16), id, opcode};
      message.insert(message.end(), resp.begin(), resp.end());
      if (corrupt) corrupt(message);
      held.push_back(message);
    }
    bar[reg_bar()].regs[x2i_head_reg()] = dev_x2i_head;
    if (respond) deliver();
  }

  // The firmware writes held responses into the i2x ring (wrapping with a tombstone like the driver's writer).
  void deliver() {
    while (!held.empty()) {
      std::vector<uint32_t> message = held.front();
      held.pop_front();
      const uint32_t length = static_cast<uint32_t>(message.size() * 4);
      const uint32_t limit = i2x_fill_to_end ? i2x_size : i2x_size - 4;
      if (dev_i2x_tail + length > limit) {
        ring_put(i2x_off, dev_i2x_tail, kTomb);
        dev_i2x_tail = 0;
        ++i2x_tombstones;
      }
      for (uint32_t i = 0; i < message.size(); ++i) ring_put(i2x_off, dev_i2x_tail + i * 4, message[i]);
      dev_i2x_tail += length;
      if (dev_i2x_tail == i2x_size) {
        ++i2x_exact_end;
        bar[reg_bar()].regs[i2x_tail_reg()] = dev_i2x_tail;
        dev_i2x_tail = 0;  // the next write starts at 0; the host sees the head reach the end first
        continue;
      }
      bar[reg_bar()].regs[i2x_tail_reg()] = dev_i2x_tail;
    }
  }

  // The firmware's answers to the management messages (status first).
  bool default_answer(uint32_t opcode, const std::vector<uint32_t>& req, std::vector<uint32_t>& resp) {
    switch (opcode) {
      case 0x101:  // suspend: the firmware enters its wait mode
        if (psp(AmdXdnaPspReg::WaitMode).present()) reg(psp(AmdXdnaPspReg::WaitMode)) = wait_mode_stuck ? 0 : 1;
        resp = {0};
        return true;
      case 0x102: case 0x103: case 0x10A: case 0x113: case 0x3: case 0x106: case 0x115: resp = {0}; return true;
      case 0x10B: resp = {0, req.empty() ? 0u : req[0] * 10, 0}; return true;
      case 0x108: resp = {0, 1, 2, 3, 4}; return true;
      case 0xF: resp = {0, (2u << 16) | 1u}; return true;  // AIE version major 1? little-endian u16 pair: major = low half
      case 0xE: {
        // size 0x2000, version 2.0, 8 columns x 6 rows: 4 core rows from 2, 1 mem row at 1, 1 shim row at 0
        const uint16_t h[20] = {2, 0, 8, 6, 4, 1, 1, 2, 1, 0, 2, 6, 6, 16, 64, 16, 128, 128, 128, 0};
        resp = {0, 0x2000};
        for (uint32_t i = 0; i < 20; i += 2) resp.push_back(h[i] | (uint32_t{h[i + 1]} << 16));
        return true;
      }
      case 0x2: {
        resp = {0, 7, 5u | (1u << 16)};  // context 7, msix 5, one queue pair
        for (uint32_t q = 0; q < 16; ++q) resp.push_back(0x3000000 + q * 4);
        return true;
      }
      case 0x117: resp = {0, 3, 0xABCD}; return true;
      case 0x301: resp = {0, prot_major, prot_minor}; return true;
      default: resp = {0}; return true;
    }
  }

  std::vector<uint32_t> opcodes() const {
    std::vector<uint32_t> out;
    for (const auto& r : received) out.push_back(r.first);
    return out;
  }
};

static const uint16_t kAie2Devices[][2] = {{0x1502, 0x00}, {0x17F0, 0x10}, {0x17F0, 0x11}, {0x17F0, 0x20}};

static std::vector<uint8_t> make_firmware(uint32_t size, uint8_t seed) {
  std::vector<uint8_t> fw(size);
  for (uint32_t i = 0; i < size; ++i) fw[i] = static_cast<uint8_t>(seed + i * 7);
  return fw;
}

static void test_identify() {
  struct Want { uint16_t dev; uint8_t rev; AmdXdnaModel model; AmdXdnaGeneration gen; const char* dir; bool vf; };
  const Want want[] = {
      {0x1502, 0x00, AmdXdnaModel::Npu1, AmdXdnaGeneration::Xdna, "amdnpu/1502_00/", false},
      {0x17F0, 0x10, AmdXdnaModel::Npu4, AmdXdnaGeneration::Xdna2, "amdnpu/17f0_10/", false},
      {0x17F0, 0x11, AmdXdnaModel::Npu5, AmdXdnaGeneration::Xdna2, "amdnpu/17f0_11/", false},
      {0x17F0, 0x20, AmdXdnaModel::Npu6, AmdXdnaGeneration::Xdna2, "amdnpu/17f0_10/", false},
      {0x17F2, 0x10, AmdXdnaModel::Npu3Pf, AmdXdnaGeneration::Aie4, "amdnpu/17f2_10/", false},
      {0x1B0B, 0x10, AmdXdnaModel::Npu3Pf, AmdXdnaGeneration::Aie4, "amdnpu/1b0b_10/", false},
      {0x17F3, 0x10, AmdXdnaModel::Npu3Vf, AmdXdnaGeneration::Aie4, nullptr, true},
      {0x1B0C, 0x10, AmdXdnaModel::Npu3Vf, AmdXdnaGeneration::Aie4, nullptr, true},
  };
  for (const Want& w : want) {
    const AmdXdnaIdentity id = amd_xdna_identify(w.dev, w.rev);
    CHECK(id.valid() && id.model == w.model && id.generation == w.gen && id.virtual_function == w.vf && id.npu && id.vbnv && id.platform);
    CHECK((w.dir == nullptr) == (id.firmware_dir == nullptr) && (!w.dir || strcmp(w.dir, id.firmware_dir) == 0));
    CHECK(amd_xdna_generation(w.dev) == w.gen);
    const AmdXdnaLayout& l = amd_xdna_layout(id.model);
    CHECK(l.valid() && l.model == w.model);
    // Every fixed register lies inside the window mapped for its BAR.
    for (const AmdXdnaRegister& r : l.psp)
      if (r.present()) CHECK(r.bar < 6 && l.bar_window[r.bar] >= r.offset + 4);
    for (const AmdXdnaRegister& r : l.smu)
      if (r.present()) CHECK(r.bar < 6 && l.bar_window[r.bar] >= r.offset + 4);
    CHECK(l.bar_window[l.sram_bar] >= (w.gen == AmdXdnaGeneration::Aie4 ? 64u : l.fw_alive_offset + 4u));
    CHECK((l.cert_firmware != nullptr) == (w.model == AmdXdnaModel::Npu3Pf));
  }
  // Every (device, revision) pair the driver lists, and nothing else.
  uint32_t known = 0;
  for (uint32_t dev = 0; dev <= 0xFFFF; ++dev)
    for (uint32_t rev = 0; rev <= 0xFF; ++rev) known += amd_xdna_identify(static_cast<uint16_t>(dev), static_cast<uint8_t>(rev)).valid();
  CHECK(known == 8);
  CHECK(!amd_xdna_identify(0x1502, 0x01).valid() && !amd_xdna_identify(0x17F0, 0x00).valid() && !amd_xdna_layout(AmdXdnaModel::Unknown).valid());
  CHECK(strcmp(amd_xdna_identify(0x1502, 0).vbnv, "RyzenAI-npu1") == 0 && strcmp(amd_xdna_identify(0x17F0, 0x20).npu, "npu6") == 0);
  CHECK(strcmp(amd_xdna_layout(AmdXdnaModel::Npu1).firmware_names[0], "npu_7.sbin") == 0 && strcmp(amd_xdna_layout(AmdXdnaModel::Npu4).firmware_names[1], "npu.sbin") == 0);
  CHECK(strcmp(amd_xdna_layout(AmdXdnaModel::Npu3Pf).firmware_names[0], "npu.dev.sbin") == 0 && !amd_xdna_layout(AmdXdnaModel::Npu3Vf).psp[0].present());
  // Register offsets against the regs files (aperture address minus BAR base).
  const AmdXdnaLayout& n1 = amd_xdna_layout(AmdXdnaModel::Npu1);
  CHECK(n1.psp[0].bar == 0 && n1.psp[0].offset == 0x100A0 && n1.psp[3].offset == 0x100BC && n1.smu[0].offset == 0x100AC && n1.mbox_bar == 4 && n1.first_col == 1);
  CHECK(n1.fw_alive_offset == 0x3F000 && n1.mbox_channel_offset == 0x20000 && n1.dpm.size() == 8 && n1.dpm[7].hclk_mhz == 1600 && !n1.dpm_by_level);
  const AmdXdnaLayout& n4 = amd_xdna_layout(AmdXdnaModel::Npu4);
  CHECK(n4.psp[0].bar == 4 && n4.psp[0].offset == 0xAEC && n4.psp[1].bar == 0 && n4.psp[1].offset == 0x10078 && n4.smu[0].bar == 5 && n4.smu[0].offset == 0x900);
  CHECK(n4.smu[2].offset == 0 && n4.fw_alive_offset == 0x1E000 && n4.dpm[7].npuclk_mhz == 1267 && n4.dpm_by_level && n4.hwctx_limit == 16);
  const AmdXdnaLayout& n3 = amd_xdna_layout(AmdXdnaModel::Npu3Pf);
  CHECK(n3.psp[3].offset == n3.psp[0].offset && n3.psp_arg2_mask == 0xFFFFFFFFu && n3.psp_notify == 0xD007BE11u && !n3.psp[7].present());
  // Revision names reported by the firmware, as the driver's table spells them.
  CHECK(strcmp(amd_xdna_revision_name(1), "NPU Strix") == 0 && strcmp(amd_xdna_revision_name(5), "NPU Strix Halo") == 0 && !amd_xdna_revision_name(0) &&
        !amd_xdna_revision_name(10));
  // The NPU class keeps its old answers and learns the AIE4 ids.
  CHECK(AmdXdnaNpu::xdna_generation(0x1502) == 1 && AmdXdnaNpu::xdna_generation(0x17F0) == 2 && AmdXdnaNpu::xdna_generation(0x17F2) == 3 &&
        AmdXdnaNpu::matches(0x1B0C) && !AmdXdnaNpu::matches(0x1234));
  CHECK(strcmp(AmdXdnaNpu::platform(0x17F0, 0x11), "Krackan Point (Ryzen AI 300)") == 0 && !AmdXdnaNpu::platform(0x17F0, 0x99));
}

static void test_protocol() {
  uint64_t f = 0;
  const AmdXdnaLayout& n1 = amd_xdna_layout(AmdXdnaModel::Npu1);
  CHECK(amd_xdna_check_protocol(n1, 5, 7, &f) && f == 0);
  CHECK(amd_xdna_check_protocol(n1, 5, 8, &f) && f == amd_xdna_feature_bit(AmdXdnaFeature::NpuCommand));
  CHECK(!amd_xdna_check_protocol(n1, 5, 6, &f) && !amd_xdna_check_protocol(n1, 6, 0, &f) && f == 0);
  const AmdXdnaLayout& n4 = amd_xdna_layout(AmdXdnaModel::Npu4);
  CHECK(!amd_xdna_check_protocol(n4, 6, 11, &f));
  CHECK(amd_xdna_check_protocol(n4, 6, 12, &f) && f == (amd_xdna_feature_bit(AmdXdnaFeature::Preempt) | amd_xdna_feature_bit(AmdXdnaFeature::TemporalOnly)));
  CHECK(amd_xdna_check_protocol(n4, 6, 18, &f) && (f & amd_xdna_feature_bit(AmdXdnaFeature::AddHostBuffer)) && !(f & amd_xdna_feature_bit(AmdXdnaFeature::GetDevRevision)));
  CHECK(amd_xdna_check_protocol(n4, 7, 0, &f) && f == 0x7F);
  CHECK(amd_xdna_check_protocol(amd_xdna_layout(AmdXdnaModel::Npu3Vf), 5, 10, &f) && !amd_xdna_check_protocol(amd_xdna_layout(AmdXdnaModel::Npu3Vf), 5, 9, &f));
}

static void test_probe() {
  SimNpu sim(0x17F0, 0x10);
  AmdXdnaHw hw(sim.layout, sim.bars, &sim.clock);
  AmdXdnaProbe p = hw.probe();
  CHECK(p.valid && p.psp_ready && !p.firmware_alive && p.smu_response == 0);
  sim.bar[2].regs[sim.layout.fw_alive_offset] = 0x3630000;
  p = hw.probe();
  CHECK(p.firmware_alive && p.fw_alive_value == 0x3630000);
  CHECK(sim.smu_log.empty() && sim.psp_log.empty());
  // A device that is not decoding reads all-ones everywhere.
  SimNpu off(0x1502, 0);
  for (uint8_t b = 0; b < 6; ++b) off.bar[b].on_read = nullptr;
  for (uint8_t b = 0; b < 6; ++b)
    for (uint64_t o : {uint64_t{0x100A0}, uint64_t{0x100B0}, uint64_t{0x10034}, uint64_t{0x3F000}}) off.bar[b].regs[o] = 0xFFFFFFFFu;
  AmdXdnaHw hw_off(off.layout, off.bars, &off.clock);
  CHECK(!hw_off.probe().valid);
  // No BARs mapped: nothing to read, not valid.
  AmdXdnaBars none;
  AmdXdnaHw hw_none(sim.layout, none, &sim.clock);
  CHECK(!hw_none.probe().valid);
}

static void test_smu() {
  for (const auto& d : kAie2Devices) {
    SimNpu sim(d[0], static_cast<uint8_t>(d[1]));
    AmdXdnaHw hw(sim.layout, sim.bars, &sim.clock);
    CHECK(hw.smu_init() == AmdXdnaError::None);
    CHECK(sim.smu_log.size() == 2 && sim.smu_log[0].first == 4 && sim.smu_log[1].first == 3);  // power off, then on
    CHECK(hw.set_dpm_level(hw.max_dpm_level()) == AmdXdnaError::None && hw.dpm_level() == 7);
    if (sim.layout.dpm_by_level) {
      CHECK(sim.smu_log.size() == 4 && sim.smu_log[2] == std::make_pair(8u, 7u) && sim.smu_log[3] == std::make_pair(7u, 7u));  // hard, soft
      CHECK(hw.hclk_mhz() == 1800 && hw.npuclk_mhz() == 1267);
    } else {
      CHECK(sim.smu_log.size() == 4 && sim.smu_log[2] == std::make_pair(5u, 847u) && sim.smu_log[3] == std::make_pair(6u, 1600u));
      CHECK(hw.hclk_mhz() == 1600 && hw.npuclk_mhz() == 847);
    }
    CHECK(hw.set_dpm_level(8) == AmdXdnaError::BadArgument);
    // Error response.
    sim.smu_fail_cmd = 3;
    CHECK(hw.smu_init() == AmdXdnaError::SmuFailed && hw.last_smu_response() == 0xFF);
    sim.smu_fail_cmd = 0;
    sim.smu_result = 2;
    CHECK(hw.smu_power_off() == AmdXdnaError::SmuFailed);
    sim.smu_result = 1;
    // Timeout: the SMU never answers.
    sim.smu_silent = true;
    const uint64_t t0 = sim.clock.now;
    CHECK(hw.smu_init() == AmdXdnaError::Timeout);
    CHECK(sim.clock.now - t0 >= AmdXdnaHw::kTimeoutUs && sim.clock.now - t0 < 3 * AmdXdnaHw::kTimeoutUs);
  }
  // Without a clock the wait is counted in polls and still ends.
  SimNpu sim(0x1502, 0);
  sim.smu_silent = true;
  AmdXdnaHw hw(sim.layout, sim.bars, nullptr);
  CHECK(hw.smu_exec(AmdXdnaSmuCommand::PowerOn, 0, nullptr) == AmdXdnaError::Timeout);
  // A model without an SMU (the VF) refuses.
  SimNpu vf(0x17F3, 0x10);
  AmdXdnaHw hv(vf.layout, vf.bars, &vf.clock);
  CHECK(hv.smu_init() == AmdXdnaError::Registers);
}

static void test_psp() {
  TestAllocator alloc;
  for (const auto& d : kAie2Devices) {
    SimNpu sim(d[0], static_cast<uint8_t>(d[1]));
    {
      AmdXdnaHw hw(sim.layout, sim.bars, &sim.clock);
      CHECK(hw.psp_start() == AmdXdnaError::BadState);  // nothing loaded
      const std::vector<uint8_t> fw = make_firmware(0x12345, 3);
      CHECK(hw.load_firmware(alloc, ConstByteView(fw.data(), fw.size()), ConstByteView()) == AmdXdnaError::None);
      CHECK(hw.firmware_buffer_size() == 0x20000 && hw.firmware_bus_address() % 0x10000 == 0);
      CHECK(hw.psp_start() == AmdXdnaError::None);
      CHECK(sim.psp_log.size() == 2 && sim.psp_log[0].cmd == 1 && sim.psp_log[0].size == 0x20000 && sim.psp_log[1].cmd == 2 && sim.psp_log[1].arg0 == 1);
      CHECK(sim.firmware_aligned && sim.firmware_seen.size() == 0x20000 && memcmp(sim.firmware_seen.data(), fw.data(), fw.size()) == 0);
      bool zero_pad = true;
      for (size_t i = fw.size(); i < sim.firmware_seen.size(); ++i) zero_pad = zero_pad && sim.firmware_seen[i] == 0;
      CHECK(zero_pad);
      CHECK(hw.psp_stop() == AmdXdnaError::None && sim.psp_log.back().cmd == 3);
      // An error code from the PSP.
      sim.psp_fail_cmd = 1;
      sim.psp_error = 0xFFFF0007u;
      CHECK(hw.psp_start() == AmdXdnaError::PspFailed && hw.last_psp_response() == 0xFFFF0007u);
      sim.psp_fail_cmd = 0;
      // Busy after the kick and never ready again.
      sim.psp_hang = true;
      CHECK(hw.psp_start() == AmdXdnaError::Timeout);
      sim.psp_hang = false;
      // Not ready before the command: nothing is written.
      sim.reg(sim.psp(AmdXdnaPspReg::Status)) = 0;
      const size_t calls = sim.psp_log.size();
      CHECK(hw.psp_start() == AmdXdnaError::Timeout && sim.psp_log.size() == calls);
      // Bad firmware sizes.
      CHECK(hw.load_firmware(alloc, ConstByteView(), ConstByteView()) == AmdXdnaError::BadFirmware);
      std::vector<uint8_t> big(AmdXdnaHw::kPspSizeLimit - AmdXdnaHw::kFirmwareAlign + 1);
      CHECK(hw.load_firmware(alloc, ConstByteView(big.data(), big.size()), ConstByteView()) == AmdXdnaError::BadFirmware);
      std::vector<uint8_t> largest(AmdXdnaHw::kPspSizeLimit - AmdXdnaHw::kFirmwareAlign);
      CHECK(hw.load_firmware(alloc, ConstByteView(largest.data(), largest.size()), ConstByteView()) == AmdXdnaError::None);
      alloc.fail = true;
      CHECK(hw.load_firmware(alloc, ConstByteView(fw.data(), fw.size()), ConstByteView()) == AmdXdnaError::NoMemory);
      alloc.fail = false;
      CHECK(hw.psp_wait_mode() == AmdXdnaError::Timeout);  // PWAITMODE never set here
    }
    CHECK(alloc.live == 0);
  }
  // AIE4: the certificate image is required, validated with its own command and alignment; ARG2 carries the command.
  SimNpu sim(0x17F2, 0x10);
  {
    AmdXdnaHw hw(sim.layout, sim.bars, &sim.clock);
    const std::vector<uint8_t> fw = make_firmware(0x30000, 1), cert = make_firmware(0x1234, 9);
    CHECK(hw.load_firmware(alloc, ConstByteView(fw.data(), fw.size()), ConstByteView()) == AmdXdnaError::BadFirmware);
    CHECK(hw.load_firmware(alloc, ConstByteView(fw.data(), fw.size()), ConstByteView(cert.data(), cert.size())) == AmdXdnaError::None);
    CHECK(hw.psp_start() == AmdXdnaError::None);
    CHECK(sim.psp_log.size() == 3 && sim.psp_log[0].cmd == 1 && sim.psp_log[0].size == 0x30000 && sim.psp_log[1].cmd == 4 && sim.psp_log[1].size == 0x8000 &&
          sim.psp_log[2].cmd == 2);
    CHECK(sim.firmware_aligned && memcmp(sim.cert_seen.data(), cert.data(), cert.size()) == 0);
    CHECK(hw.psp_wait_mode() == AmdXdnaError::None);  // no PWAITMODE on npu3
  }
  CHECK(alloc.live == 0);
  SimNpu vf(0x1B0C, 0x10);
  AmdXdnaHw hv(vf.layout, vf.bars, &vf.clock);
  const uint8_t one[4] = {1, 2, 3, 4};
  CHECK(hv.load_firmware(alloc, ConstByteView(one), ConstByteView()) == AmdXdnaError::BadState);
}

// A started AIE2 device on a pretend NPU.
struct Rig {
  SimNpu sim;
  TestAllocator alloc;
  AmdXdnaDevice dev;
  explicit Rig(uint16_t d = 0x17F0, uint8_t r = 0x10) : sim(d, r), dev(sim.layout, sim.bars, &sim.clock) {}
  AmdXdnaError start() {
    const std::vector<uint8_t> fw = make_firmware(0x8000, 5), cert = make_firmware(0x100, 6);
    const AmdXdnaError e = dev.load_firmware(alloc, ConstByteView(fw.data(), fw.size()), ConstByteView(cert.data(), cert.size()));
    return e == AmdXdnaError::None ? dev.start() : e;
  }
};

static void test_start_stop() {
  for (const auto& d : kAie2Devices) {
    Rig rig(d[0], static_cast<uint8_t>(d[1]));
    rig.sim.prot_major = d[0] == 0x1502 ? 5 : 6;
    rig.sim.prot_minor = d[0] == 0x1502 ? 8 : 12;
    CHECK(rig.dev.start() == AmdXdnaError::BadState && rig.dev.failed_stage() == AmdXdnaStage::Firmware);  // no firmware yet
    CHECK(rig.start() == AmdXdnaError::None && rig.dev.running() && rig.dev.failed_stage() == AmdXdnaStage::None);
    CHECK(rig.sim.ring_reads_outside == 0);
    // The alive word was cleared after the description was read.
    CHECK(rig.sim.bar[rig.sim.layout.sram_bar].regs[rig.sim.layout.fw_alive_offset] == 0);
    const std::vector<uint32_t> ops = rig.sim.opcodes();
    std::vector<uint32_t> want;
    std::vector<uint32_t> types;
    for (const auto& r : rig.sim.received)
      if (r.first == 0x10A) types.push_back(r.second[0]);
    if (d[0] == 0x1502) {
      want = {0x10A, 0x10A, 0x103, 0x101, 0x102, 0x10A, 0x108, 0xF, 0xE};  // no UPDATE_PROPERTY on 5.8
      CHECK(types == std::vector<uint32_t>({2, 4, 1}));
    } else {
      // 6.12: preemption supported, so the "frame boundary preemption" init row is skipped; no UPDATE_PROPERTY before 6.15.
      want = {0x10A, 0x10A, 0x103, 0x101, 0x102, 0x10A, 0x10A, 0x10A, 0x10A, 0x108, 0xF, 0xE};
      CHECK(types == std::vector<uint32_t>({5, 10, 1, 2, 3, 4}));
    }
    CHECK(ops == want);
    CHECK(rig.dev.firmware_version().major == 1 && rig.dev.firmware_version().build == 4 && rig.dev.aie_major() == 1 && rig.dev.aie_minor() == 2);
    CHECK(rig.dev.total_columns() == 8 && rig.dev.tile_info().core_row_start == 2 && rig.dev.tile_info().shim_events == 128 && rig.dev.tile_info().col_size == 0x2000);
    CHECK(rig.dev.hw().dpm_level() == 7 && rig.sim.psp_log.size() == 2);
    CHECK(rig.dev.start() == AmdXdnaError::BadState);
    // Stop: clock gating defaults, suspend, release the PSP, lowest DPM level, power off.
    rig.sim.received.clear();
    const size_t smu_before = rig.sim.smu_log.size();
    CHECK(rig.dev.stop() == AmdXdnaError::None && !rig.dev.running());
    CHECK(rig.sim.opcodes().back() == 0x101 && rig.sim.psp_log.back().cmd == 3 && rig.sim.smu_log.back().first == 4 && rig.sim.smu_log.size() > smu_before);
    CHECK(rig.dev.stop() == AmdXdnaError::BadState);
    // And again.
    rig.sim.received.clear();
    CHECK(rig.dev.start() == AmdXdnaError::None && rig.dev.running());
  }
  // 6.18 brings UPDATE_PROPERTY (time quantum); 7.x keeps everything.
  {
    Rig rig;
    rig.sim.prot_minor = 18;
    CHECK(rig.start() == AmdXdnaError::None && rig.dev.has_feature(AmdXdnaFeature::UpdateProperty) && !rig.dev.has_feature(AmdXdnaFeature::GetDevRevision));
    bool quota = false;
    for (const auto& r : rig.sim.received)
      if (r.first == 0x113) quota = r.second.size() == 5 && r.second[0] == 0 && (r.second[1] & 0xFF) == 0xFF && r.second[3] == 30000;
    CHECK(quota);
  }
  // Failures at each stage leave the device powered off.
  {
    Rig rig;
    rig.sim.smu_fail_cmd = 3;
    CHECK(rig.start() == AmdXdnaError::SmuFailed && rig.dev.failed_stage() == AmdXdnaStage::Smu && !rig.dev.running() && rig.sim.smu_log.back().first == 3 &&
          rig.sim.psp_log.empty());
  }
  {
    Rig rig;
    rig.sim.psp_fail_cmd = 2;
    rig.sim.psp_error = 0xFFFF0002u;
    CHECK(rig.start() == AmdXdnaError::PspFailed && rig.dev.failed_stage() == AmdXdnaStage::Psp && rig.sim.smu_log.back().first == 4);
  }
  {
    Rig rig;
    rig.sim.fw_never_alive = true;
    CHECK(rig.start() == AmdXdnaError::NotAlive && rig.dev.failed_stage() == AmdXdnaStage::Alive && rig.sim.psp_log.back().cmd == 3);
  }
  {
    Rig rig;
    rig.sim.magic = 0x12345678;
    CHECK(rig.start() == AmdXdnaError::BadChannel && rig.dev.failed_stage() == AmdXdnaStage::Channel);
    CHECK(rig.sim.bar[2].regs[rig.sim.layout.fw_alive_offset] == 0);
  }
  {
    Rig rig;
    rig.sim.prot_major = 9;
    CHECK(rig.start() == AmdXdnaError::Protocol && rig.dev.failed_stage() == AmdXdnaStage::Protocol);
  }
  {
    Rig rig;  // a ring published outside the SRAM BAR
    rig.sim.ring_shift = 0x40000;
    CHECK(rig.start() == AmdXdnaError::BadChannel && rig.dev.failed_stage() == AmdXdnaStage::Channel);
  }
  {
    Rig rig;  // the two rings overlap
    rig.sim.i2x_off = rig.sim.x2i_off + 0x100;
    CHECK(rig.start() == AmdXdnaError::BadChannel);
  }
  {
    Rig rig;  // a too small ring
    rig.sim.x2i_size = 32;
    CHECK(rig.start() == AmdXdnaError::BadChannel);
  }
  {
    Rig rig;  // the alive word points below the SRAM BAR's device address
    rig.sim.info_off = 0;
    rig.sim.publish_channel();
    rig.sim.fw_never_alive = true;
    rig.sim.sram(rig.sim.layout.fw_alive_offset, 0x1000);
    CHECK(rig.start() == AmdXdnaError::BadChannel);
  }
  {
    Rig rig;
    rig.sim.status_for = 0x103;  // the firmware refuses the PASID
    rig.sim.status_value = 0x4000003;
    CHECK(rig.start() == AmdXdnaError::DeviceStatus && rig.dev.failed_stage() == AmdXdnaStage::Pasid);
    CHECK(rig.sim.psp_log.back().cmd == 3 && rig.sim.smu_log.back().first == 4);
  }
  {
    Rig rig;
    rig.sim.wait_mode_stuck = true;  // suspend answered but the wait mode never shows
    CHECK(rig.start() == AmdXdnaError::Timeout && rig.dev.failed_stage() == AmdXdnaStage::Reset);
  }
  {
    Rig rig;
    rig.sim.respond = false;  // the firmware takes requests and never answers
    CHECK(rig.start() == AmdXdnaError::Timeout && rig.dev.failed_stage() == AmdXdnaStage::RuntimeConfig && rig.dev.mailbox().broken());
  }
  // AIE4: physical function (SMU, PSP with certificate, mailbox_info) and virtual function (mailbox only).
  {
    Rig rig(0x1B0B, 0x10);
    rig.sim.prot_major = 5;
    rig.sim.prot_minor = 10;
    CHECK(rig.start() == AmdXdnaError::None && rig.dev.running() && !rig.dev.aie2() && rig.dev.mailbox().running());
    CHECK(rig.sim.psp_log.size() == 3 && rig.sim.smu_log.size() == 2 && rig.dev.mailbox().config().msix == 3);
    CHECK(rig.dev.mailbox().config().x2i.start == 0x1000 && !rig.dev.mailbox().config().has_irq_reg);
    uint8_t req[4] = {1, 0, 0, 0}, resp[4] = {};
    CHECK(rig.dev.mailbox().call(0x42, ConstByteView(req), ByteView(resp)) == AmdXdnaError::None);
    CHECK(rig.dev.stop() == AmdXdnaError::None && rig.sim.psp_log.back().cmd == 3);
  }
  {
    Rig rig(0x17F3, 0x10);
    rig.sim.prot_major = 5;
    rig.sim.prot_minor = 10;
    rig.sim.publish_channel();  // a VF finds the firmware the PF started
    CHECK(rig.dev.load_firmware(rig.alloc, ConstByteView(), ConstByteView()) == AmdXdnaError::BadState);
    CHECK(rig.dev.start() == AmdXdnaError::None && rig.sim.psp_log.empty() && rig.sim.smu_log.empty());
    CHECK(rig.dev.stop() == AmdXdnaError::None);
  }
  {
    Rig rig(0x17F2, 0x10);
    rig.sim.prot_major = 4;  // older than npu3's table
    CHECK(rig.start() == AmdXdnaError::Protocol && rig.dev.failed_stage() == AmdXdnaStage::Protocol && rig.sim.smu_log.back().first == 4);
  }
}

static void test_messages() {
  Rig rig;
  rig.sim.prot_major = 7;
  CHECK(rig.start() == AmdXdnaError::None);
  AmdXdnaMessages& m = rig.dev.messages();
  rig.sim.received.clear();
  AmdXdnaContextRequest req;
  req.start_col = 2;
  req.num_col = 4;
  req.pasid = 0x33;
  req.priority = AmdXdnaPriority::Normal;
  AmdXdnaContext ctx;
  CHECK(m.create_context(req, &ctx) == AmdXdnaError::None);
  CHECK(ctx.id == 7 && ctx.msix == 5 && ctx.queue_pairs == 1 && ctx.x2i[0].head_addr == 0x3000000 && ctx.i2x[0].buf_size == 0x300001C && ctx.i2x[1].buf_size == 0x300003C);
  const std::vector<uint32_t>& sent = rig.sim.received[0].second;
  CHECK(rig.sim.received[0].first == 0x2 && sent.size() == 7 && sent[0] == 1 && sent[1] == (2u | (4u << 8)) && sent[2] == (1u | (0x33u << 16)) && sent[6] == 3);
  CHECK(m.destroy_context(7) == AmdXdnaError::None && rig.sim.received.back().second == std::vector<uint32_t>({7}));
  CHECK(m.create_context(AmdXdnaContextRequest{}, &ctx) == AmdXdnaError::BadArgument);
  // A context answer without queue pairs is malformed; the context is destroyed again.
  rig.sim.responder = [&rig](uint32_t op, const std::vector<uint32_t>& r, std::vector<uint32_t>& resp) {
    if (op == 0x2) {
      resp = {0, 9, 5u | (3u << 16)};  // three pairs claimed
      resp.resize(19, 0);
      return true;
    }
    return rig.sim.default_answer(op, r, resp);
  };
  rig.sim.received.clear();
  CHECK(m.create_context(req, &ctx) == AmdXdnaError::BadResponse && rig.sim.received.back().first == 0x3 && rig.sim.received.back().second[0] == 9);
  rig.sim.responder = [&rig](uint32_t op, const std::vector<uint32_t>& r, std::vector<uint32_t>& resp) { return rig.sim.default_answer(op, r, resp); };
  // Host buffers go in chunks: MAP, then ADD for the rest.
  rig.sim.received.clear();
  CHECK(m.map_host_buffer(7, 0x100000000ull, 3 * 0x4000000ull, 0x4000000ull, false) == AmdXdnaError::None);
  CHECK(rig.sim.opcodes() == std::vector<uint32_t>({0x106, 0x115, 0x115}));
  CHECK(rig.sim.received[1].second == std::vector<uint32_t>({7, 0x04000000, 1, 0x04000000, 0}));
  CHECK(m.map_host_buffer(7, 0, 0x5000, 0x4000, false) == AmdXdnaError::BadArgument && m.map_host_buffer(7, 0, 0, 0x4000, false) == AmdXdnaError::BadArgument &&
        m.map_host_buffer(7, ~0ull - 0x3FFF, 0x8000, 0x4000, false) == AmdXdnaError::BadArgument);
  rig.sim.received.clear();
  CHECK(m.map_host_buffer(7, 0x8000, 0x4000, 0x4000, true) == AmdXdnaError::None && rig.sim.opcodes() == std::vector<uint32_t>({0x115}));
  uint64_t value = 0;
  CHECK(m.get_runtime_config(6, &value) == AmdXdnaError::None && value == 60);
  uint32_t rev = 0, fuse = 0, major = 0, minor = 0;
  CHECK(m.get_dev_revision(&rev, &fuse) == AmdXdnaError::None && rev == 3 && fuse == 0xABCD && strcmp(amd_xdna_revision_name(rev), "NPU Krackan 1") == 0);
  CHECK(m.query_protocol_version(&major, &minor) == AmdXdnaError::None && major == 7);
  rig.sim.responder = [](uint32_t op, const std::vector<uint32_t>&, std::vector<uint32_t>& resp) {
    resp = op == 0x117 ? std::vector<uint32_t>{0, 42, 0} : std::vector<uint32_t>{0};
    return true;
  };
  CHECK(m.get_dev_revision(&rev, &fuse) == AmdXdnaError::BadResponse && fuse == 0);
  // A tile description that does not add up.
  rig.sim.responder = [](uint32_t op, const std::vector<uint32_t>&, std::vector<uint32_t>& resp) {
    resp = {0, 0x100, 0, (6u << 16) | 0u};  // 0 columns
    resp.resize(12, 0);
    return true;
  };
  AmdXdnaTileInfo tile;
  CHECK(m.query_aie_tile_info(&tile) == AmdXdnaError::BadResponse);
  rig.sim.responder = [&rig](uint32_t op, const std::vector<uint32_t>& r, std::vector<uint32_t>& resp) { return rig.sim.default_answer(op, r, resp); };
  rig.sim.status_for = 0x102;
  rig.sim.status_value = 0x2000005;
  CHECK(m.resume() == AmdXdnaError::DeviceStatus && m.last_status() == 0x2000005);
  rig.sim.status_for = 0;
  CHECK(m.resume() == AmdXdnaError::None && m.last_status() == 0);
  CHECK(rig.sim.ring_reads_outside == 0);
}

// Many messages of changing sizes through small rings: both directions wrap many times, with tombstones and with a message ending exactly at the end.
static void test_wraparound() {
  for (bool fill_to_end : {false, true}) {
    Rig rig;
    rig.sim.x2i_size = 128;
    rig.sim.i2x_size = 96;
    rig.sim.i2x_fill_to_end = fill_to_end;
    rig.sim.prot_major = 7;
    CHECK(rig.start() == AmdXdnaError::None);
    AmdXdnaMailbox& mb = rig.dev.mailbox();
    rig.sim.responder = [](uint32_t op, const std::vector<uint32_t>& req, std::vector<uint32_t>& resp) {
      resp.assign(op, 0);  // the opcode says how many words to answer
      for (uint32_t i = 1; i < op; ++i) resp[i] = req[0] + i;
      return true;
    };
    uint32_t ok = 0;
    for (uint32_t i = 0; i < 300; ++i) {
      const uint32_t req_words = 1 + i % 11;  // 20..60 bytes with the header, in a 124-byte ring (at most half of it per message)
      const uint32_t resp_words = 1 + ((i * 2654435761u) >> 7) % 9;  // 20..52-byte answers in a scrambled order
      std::vector<uint32_t> req(req_words, 0);
      req[0] = i + 100;
      std::vector<uint32_t> resp(resp_words, 0xEEEEEEEE);
      const AmdXdnaError e = mb.call(resp_words, ConstByteView(reinterpret_cast<const uint8_t*>(req.data()), req.size() * 4),
                                     ByteView(reinterpret_cast<uint8_t*>(resp.data()), resp.size() * 4));
      bool right = e == AmdXdnaError::None && resp[0] == 0;
      for (uint32_t k = 1; k < resp_words; ++k) right = right && resp[k] == req[0] + k;
      if (!right && ok == i) printf("wrap %d: call %u failed: %s (ring reads outside %u)\n", fill_to_end, i, amd_xdna_error_name(e), rig.sim.ring_reads_outside);
      ok += right;
    }
    CHECK(ok == 300 && !mb.broken() && rig.sim.ring_reads_outside == 0);
    printf("wraps (fill to end %d): requests %u, answers %u by tombstone, %u at the exact end\n", fill_to_end, rig.sim.x2i_tombstones, rig.sim.i2x_tombstones, rig.sim.i2x_exact_end);
    CHECK(rig.sim.x2i_tombstones > 20 && rig.sim.i2x_tombstones > 20 && (rig.sim.i2x_exact_end > 5) == fill_to_end);
  }
  // A request that can never fit is refused before anything is written.
  Rig rig;
  rig.sim.x2i_size = 80;  // 38-byte messages at most; bring-up sends up to 36
  rig.sim.prot_major = 7;
  CHECK(rig.start() == AmdXdnaError::None);
  uint8_t big[48] = {1}, resp[4] = {};
  CHECK(rig.dev.mailbox().call(1, ConstByteView(big), ByteView(resp)) == AmdXdnaError::BadMessage);
  uint8_t tomb[4] = {0xCE, 0xFA, 0xAD, 0xDE}, odd[6] = {1};
  CHECK(rig.dev.mailbox().call(1, ConstByteView(tomb), ByteView(resp)) == AmdXdnaError::BadMessage);
  CHECK(rig.dev.mailbox().call(1, ConstByteView(odd), ByteView(resp)) == AmdXdnaError::BadMessage);
  CHECK(rig.dev.mailbox().call(1, ConstByteView(), ByteView(resp)) == AmdXdnaError::BadMessage);
  // The device stops consuming requests: the sender waits for room and gives up.
  rig.sim.fw_running = false;
  uint8_t req[8] = {1};
  uint32_t id = 0;
  AmdXdnaError last = AmdXdnaError::None;
  for (int i = 0; i < 6 && last == AmdXdnaError::None; ++i) last = rig.dev.mailbox().send(1, ConstByteView(req), ByteView(resp), &id);
  CHECK(last == AmdXdnaError::Timeout && !rig.dev.mailbox().broken());
}

// Several requests outstanding at once, answered out of order; slots run out; stray answers are ignored.
static void test_async() {
  Rig rig;
  rig.sim.prot_major = 7;
  CHECK(rig.start() == AmdXdnaError::None);
  AmdXdnaMailbox& mb = rig.dev.mailbox();
  rig.sim.respond = false;
  rig.sim.responder = [](uint32_t, const std::vector<uint32_t>& req, std::vector<uint32_t>& resp) {
    resp = {0, req[0] * 2};
    return true;
  };
  uint32_t ids[AmdXdnaMailbox::kSlots];
  uint32_t answers[AmdXdnaMailbox::kSlots][2] = {};
  for (uint32_t i = 0; i < AmdXdnaMailbox::kSlots; ++i) {
    uint32_t v[1] = {i + 1};
    CHECK(mb.send(9, ConstByteView(reinterpret_cast<const uint8_t*>(v), 4), ByteView(reinterpret_cast<uint8_t*>(answers[i]), 8), &ids[i]) == AmdXdnaError::None);
  }
  uint32_t v[1] = {99}, spare[2];
  uint32_t extra = 0;
  CHECK(mb.send(9, ConstByteView(reinterpret_cast<const uint8_t*>(v), 4), ByteView(reinterpret_cast<uint8_t*>(spare), 8), &extra) == AmdXdnaError::NoSlot);
  std::reverse(rig.sim.held.begin(), rig.sim.held.end());
  rig.sim.deliver();
  CHECK(mb.poll() == AmdXdnaError::None);
  bool all = true;
  for (uint32_t i = 0; i < AmdXdnaMailbox::kSlots; ++i) all = all && mb.answered(ids[i]) && mb.finish(ids[i]) == AmdXdnaError::None && answers[i][1] == 2 * (i + 1);
  CHECK(all);
  CHECK(mb.finish(ids[0]) == AmdXdnaError::BadState);
  // An answer for a slot nobody waits on.
  std::vector<uint32_t> stray = {8, 8 | (1u << 16), AmdXdnaMailbox::kIdMagic | 3, 9, 0, 0};
  rig.sim.held.push_back(stray);
  rig.sim.deliver();
  CHECK(mb.poll() == AmdXdnaError::None && mb.stray_responses() == 1 && !mb.broken());
  rig.sim.respond = true;
  CHECK(rig.dev.messages().resume() == AmdXdnaError::BadResponse);  // the responder answers 8 bytes; resume expects 4: the size must match exactly
  CHECK(!mb.broken());
  rig.sim.responder = [&rig](uint32_t op, const std::vector<uint32_t>& r, std::vector<uint32_t>& resp) { return rig.sim.default_answer(op, r, resp); };
  CHECK(rig.dev.messages().resume() == AmdXdnaError::None);
}

// Malformed device behaviour: every case fails cleanly, never reads outside the rings, and leaves a broken channel refusing further use.
static void test_malformed() {
  struct Case {
    const char* name;
    std::function<void(SimNpu&)> setup;
    AmdXdnaError expect;
    bool broken;
  };
  const Case cases[] = {
      {"zero size", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] = 0; }; }, AmdXdnaError::ChannelBroken, true},
      {"misaligned size", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] = 2; }; }, AmdXdnaError::ChannelBroken, true},
      {"huge size", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] = 0xFFFFFFF0u; }; }, AmdXdnaError::ChannelBroken, true},
      {"size past tail", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] = 0x40; }; }, AmdXdnaError::ChannelBroken, true},
      {"bad id magic", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[2] ^= 0x01000000; }; }, AmdXdnaError::ChannelBroken, true},
      {"id beyond slots", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[2] |= 0x00800000; }; }, AmdXdnaError::ChannelBroken, true},
      {"shorter answer", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] -= 4; m.pop_back(); }; }, AmdXdnaError::BadResponse, false},
      {"longer answer", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] += 4; m.push_back(0); }; }, AmdXdnaError::BadResponse, false},
      {"tombstone before tail", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m[0] = kTomb; }; }, AmdXdnaError::ChannelBroken, true},
      {"header cut short", [](SimNpu& s) { s.corrupt = [](std::vector<uint32_t>& m) { m.resize(2); }; }, AmdXdnaError::ChannelBroken, true},
  };
  for (const Case& c : cases) {
    Rig rig;
    rig.sim.prot_major = 7;
    CHECK(rig.start() == AmdXdnaError::None);
    c.setup(rig.sim);
    uint32_t major = 0, minor = 0;
    const AmdXdnaError e = rig.dev.messages().query_protocol_version(&major, &minor);
    if (e != c.expect || rig.dev.mailbox().broken() != c.broken) printf("case '%s': got %s\n", c.name, amd_xdna_error_name(e));
    CHECK(e == c.expect && rig.dev.mailbox().broken() == c.broken);
    CHECK(rig.sim.ring_reads_outside == 0);
    if (c.broken) CHECK(rig.dev.messages().resume() == AmdXdnaError::ChannelBroken);
  }
  // Tail register out of range or misaligned; head register of the request ring out of range.
  for (uint32_t bad_tail : {0x401u, 0x402u, 0x800u, 0xFFFFFFFCu}) {
    Rig rig;
    rig.sim.prot_major = 7;
    CHECK(rig.start() == AmdXdnaError::None);
    rig.sim.respond = false;
    uint8_t req[4] = {1}, resp[4];
    uint32_t id = 0;
    CHECK(rig.dev.mailbox().send(1, ConstByteView(req), ByteView(resp), &id) == AmdXdnaError::None);
    rig.sim.bar[rig.sim.reg_bar()].regs[rig.sim.i2x_tail_reg()] = bad_tail;
    CHECK(rig.dev.mailbox().poll() == AmdXdnaError::ChannelBroken && rig.dev.mailbox().broken() && rig.dev.mailbox().finish(id) == AmdXdnaError::ChannelBroken);
  }
  for (uint32_t bad_head : {0x3FDu, 0x400u, 0x10000u}) {
    Rig rig;
    rig.sim.prot_major = 7;
    CHECK(rig.start() == AmdXdnaError::None);
    rig.sim.fw_running = false;
    rig.sim.bar[rig.sim.reg_bar()].regs[rig.sim.x2i_head_reg()] = bad_head;
    uint8_t req[4] = {1}, resp[4];
    CHECK(rig.dev.mailbox().call(1, ConstByteView(req), ByteView(resp)) == AmdXdnaError::ChannelBroken && rig.dev.mailbox().broken());
  }
  // Every single-bit flip of every header word of an answer: whatever happens, it is one of the clean outcomes, nothing is read outside the rings, and a
  // channel that survived still works.
  uint32_t outcomes[4] = {};
  for (uint32_t word = 0; word < 4; ++word) {
    for (uint32_t bit = 0; bit < 32; ++bit) {
      Rig rig;
      rig.sim.prot_major = 7;
      CHECK(rig.start() == AmdXdnaError::None);
      rig.sim.corrupt = [word, bit](std::vector<uint32_t>& m) { m[word] ^= 1u << bit; };
      uint32_t major = 0, minor = 0;
      const AmdXdnaError e = rig.dev.messages().query_protocol_version(&major, &minor);
      const bool clean = e == AmdXdnaError::None || e == AmdXdnaError::BadResponse || e == AmdXdnaError::ChannelBroken || e == AmdXdnaError::Timeout;
      CHECK(clean && rig.sim.ring_reads_outside == 0);
      outcomes[e == AmdXdnaError::None ? 0 : e == AmdXdnaError::BadResponse ? 1 : e == AmdXdnaError::ChannelBroken ? 2 : 3]++;
      rig.sim.corrupt = nullptr;
      if (!rig.dev.mailbox().broken()) {
        const AmdXdnaError again = rig.dev.messages().query_protocol_version(&major, &minor);
        CHECK(again == AmdXdnaError::None || again == AmdXdnaError::Timeout);
      }
    }
  }
  CHECK(outcomes[0] > 0 && outcomes[1] > 0 && outcomes[2] > 0 && outcomes[3] > 0);
  printf("header bit flips: %u answered, %u bad response, %u channel broken, %u timed out\n", outcomes[0], outcomes[1], outcomes[2], outcomes[3]);  // opcode/version flips pass, size flips mismatch, id flips break
  // Bit flips in the published channel description never get a channel going outside the BARs.
  for (uint32_t word = 0; word < 12; ++word) {
    for (uint32_t bit = 0; bit < 32; bit += 3) {
      Rig rig;
      rig.sim.prot_major = 7;
      const AmdXdnaError e = rig.start();
      CHECK(e == AmdXdnaError::None);
      rig.dev.stop();
      rig.sim.publish_channel();
      const uint32_t off = rig.sim.info_off + word * 4;
      rig.sim.bar[2].regs[off] ^= 1u << bit;
      AmdXdnaChannelConfig cfg;
      AmdXdnaMailbox mb(rig.sim.bars, &rig.sim.clock);
      const AmdXdnaError r = rig.dev.read_channel(&cfg);
      if (r == AmdXdnaError::None) {
        const AmdXdnaError s = mb.start(cfg);
        if (s == AmdXdnaError::None) {
          CHECK(rig.sim.bars.contains(cfg.ring_bar, cfg.x2i.start, cfg.x2i.size) && rig.sim.bars.contains(cfg.ring_bar, cfg.i2x.start, cfg.i2x.size));
          mb.stop();
        } else {
          CHECK(s == AmdXdnaError::BadChannel);
        }
      } else {
        CHECK(r == AmdXdnaError::BadChannel);
      }
    }
  }
}

int main() {
  test_identify();
  test_protocol();
  test_probe();
  test_smu();
  test_psp();
  test_start_stop();
  test_messages();
  test_wraparound();
  test_async();
  test_malformed();
  if (g_failed) { printf("%d check(s) failed\n", g_failed); return 1; }
  printf("AmdXdnaHwTest passed\n");
  return 0;
}
