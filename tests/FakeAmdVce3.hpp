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


// A simulated Vega M for the VCE tests: the register file with the ring controller (the commands the driver writes to the ring are run: indirect buffers go to a hook,
// fences are written back), the stand-in for the engine's firmware (it parses the task blocks the way the firmware does and encodes with the CPU H.264 encoder), and the
// part of the card the engine glue touches (the VCE's own boot behaviour, the SMC's mailbox, the harvest fuses).
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <functional>
#include <map>
#include <memory>
#include <vector>
#include "Hardware/VideoCodec/AMD/AmdVce3Encoder.hpp"
#include "Hardware/VideoCodec/Software/H264.hpp"
#include "Hardware/VideoCodec/Software/H264Encoder.hpp"
#include "FakeAmdVcn.hpp"

namespace fakevce {

using namespace hardware;
using namespace hardware::amd_vce3;

namespace reg {
constexpr uint32_t kRptr[3] = {0x8063, 0x805e, 0x80d7}, kWptr[3] = {0x8064, 0x805f, 0x80d8};
constexpr uint32_t kGrbmGfxIndex = 0xc200;
}  // namespace reg
constexpr uint32_t kRingDwords = 1024;

struct Window {
  uint64_t gpu = 0;
  uint8_t* host = nullptr;
  uint64_t size = 0;
};

struct FakeMapper final : IRegisterMapper {
  uint8_t* host = nullptr;
  uint64_t physical_base = 0, size = 0;
  bool refuse = false;
  bool map_registers(uint64_t physical, uint64_t length, MmioRegion* region) override {
    if (refuse) return false;
    if (physical < physical_base || length > size || physical - physical_base > size - length) return false;
    *region = MmioRegion(reinterpret_cast<uint64_t>(host + (physical - physical_base)), length);
    return true;
  }
};

// The registers of the ring pointers and the controller that runs the commands (as in AmdVce3RingTest), with a hook that gets each indirect buffer.
struct Controller : IVcnRegisters {
  std::vector<Window> windows;
  uint64_t ring_gpu = 0;
  uint32_t regs[2][4096] = {};
  uint32_t grbm = 0xE0000000;
  bool stall = false;
  uint32_t protocol_errors = 0;
  std::function<void(const std::vector<uint32_t>&)> on_ib;
  uint32_t selected() const { return (grbm >> 4) & 1; }
  uint32_t read(uint32_t r) override {
    if (r == reg::kGrbmGfxIndex) return grbm;
    return r >= 0x8000 && r < 0x9000 ? regs[selected()][r - 0x8000] : 0;
  }
  void write(uint32_t r, uint32_t v) override {
    if (r == reg::kGrbmGfxIndex) { grbm = v; return; }
    if (r >= 0x8000 && r < 0x9000) regs[selected()][r - 0x8000] = v;
    if (r == reg::kWptr[0]) run();
  }
  uint8_t* translate(uint64_t gpu, uint64_t bytes) const {
    for (const Window& w : windows)
      if (gpu >= w.gpu && gpu - w.gpu <= w.size && bytes <= w.size - (gpu - w.gpu)) return w.host + (gpu - w.gpu);
    return nullptr;
  }
  uint32_t get32(uint64_t gpu) const {
    const uint8_t* p = translate(gpu, 4);
    return p ? p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t{p[3]} << 24) : 0xDEADBEEF;
  }
  void put32(uint64_t gpu, uint32_t v) const {
    uint8_t* p = translate(gpu, 4);
    if (!p) return;
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
  }
  void set_pointers(uint32_t position) { for (int i = 0; i < 2; ++i) regs[i][reg::kRptr[0] - 0x8000] = regs[i][reg::kWptr[0] - 0x8000] = position; }

 private:
  void run() {
    uint32_t& rptr = regs[selected()][reg::kRptr[0] - 0x8000];
    const uint32_t wptr = regs[selected()][reg::kWptr[0] - 0x8000];
    auto next = [&]() { const uint32_t v = get32(ring_gpu + uint64_t{rptr} * 4); rptr = (rptr + 1) % kRingDwords; return v; };
    while (rptr != wptr) {
      const uint32_t cmd = next();
      switch (cmd) {
        case 0: case 1: case 4: break;
        case 0x102: {
          const uint32_t vmid = next(), lo = next(), hi = next(), length = next();
          if (vmid != 0) ++protocol_errors;
          std::vector<uint32_t> words;
          const uint64_t address = lo | (uint64_t{hi} << 32);
          if (!translate(address, uint64_t{length} * 4)) { ++protocol_errors; break; }
          for (uint32_t i = 0; i < length; ++i) words.push_back(get32(address + uint64_t{i} * 4));
          if (on_ib) on_ib(words);
          break;
        }
        case 3: {
          const uint32_t lo = next(), hi = next(), seq = next();
          if (!stall) put32(lo | (uint64_t{hi} << 32), seq);
          break;
        }
        default: ++protocol_errors; rptr = wptr; return;
      }
    }
  }
};

// The stand-in for the firmware.
struct Firmware {
  Controller* ctrl = nullptr;
  allocators::IAllocator* alloc = nullptr;
  std::unique_ptr<H264Encoder> encoder;
  uint32_t handle = 0;
  bool created = false, destroyed = false, opened_session = false;
  uint32_t width = 0, height = 0, ref_pitch = 0;
  uint32_t rc_method = 99, qp_i = 0, qp_p = 0, gop = 0, poc_lsb_minus4 = 99;
  int sessions = 0, creates = 0, configs = 0, encodes = 0, destroys = 0, problems = 0;
  bool no_bitstream = false;
  bool wrong_picture = false;   // the engine encodes something other than the picture it was given
  bool corrupt = false;   // the bitstream the engine writes is damaged
  struct Encoded { uint32_t type, frame_num, poc, ref_type, ref_frame, ref_luma, cur_luma, bytes; bool headers; uint32_t cpb_hi, cpb_lo; };
  std::vector<Encoded> pictures;
  std::vector<uint32_t> blocks_seen;   // the command ids in order

  void problem(const char* what) { ++problems; printf("  fake firmware: %s\n", what); }

  void on_ib(const std::vector<uint32_t>& w) {
    size_t at = 0;
    uint64_t feedback_address = 0;
    uint32_t bs_size = 0, cpb_hi = 0, cpb_lo = 0;
    uint64_t bs_address = 0;
    bool encode_pending = false;
    std::vector<uint32_t> encode;
    uint32_t ring_index = 0;
    while (at < w.size()) {
      const uint32_t bytes = w[at];
      if (bytes == 0) { ++at; continue; }   // padding
      if (bytes % 4 != 0 || bytes < 8 || at + bytes / 4 > w.size()) { problem("block size is not a whole number of dwords inside the buffer"); return; }
      const uint32_t id = w[at + 1];
      const uint32_t n = bytes / 4 - 2;   // payload dwords
      const uint32_t* p = &w[at + 2];
      blocks_seen.push_back(id);
      switch (id) {
        case 0x00000001: ++sessions; if (n != 1) problem("session block length"); else handle = p[0]; opened_session = true; break;
        case 0x00000002: if (n != 6) problem("task info length"); else { ring_index = p[5]; } break;
        case 0x01000001:
          ++creates;
          if (!opened_session) problem("create before session");
          if (n != 14) { problem("create length"); break; }
          created = true;
          width = p[4]; height = p[5]; ref_pitch = p[6];
          if (p[1] != 66 || p[7] != p[6]) problem("create profile or pitches");
          break;
        case 0x02000001: ++destroys; destroyed = true; break;
        case 0x04000005: ++configs; if (n != 26) problem("rate control length"); else { rc_method = p[0]; gop = p[4]; qp_i = p[5]; qp_p = p[6]; } break;
        case 0x04000001: if (n != 1) problem("config extension length"); break;
        case 0x04000007: if (n != 24) problem("motion estimation length"); break;
        case 0x04000008: if (n != 17) problem("rdo length"); break;
        case 0x04000002: if (n != 27) problem("pic control length"); else poc_lsb_minus4 = p[15]; break;
        case 0x05000001: if (n != 2) problem("context buffer length"); else { cpb_hi = p[0]; cpb_lo = p[1]; } break;
        case 0x05000004: if (n != 3) problem("bitstream buffer length"); else { bs_address = (uint64_t{p[0]} << 32) | p[1]; bs_size = p[2]; } break;
        case 0x05000005: if (n != 3) problem("feedback length"); else feedback_address = (uint64_t{p[0]} << 32) | p[1]; break;
        case 0x03000001: if (n != 96) problem("encode length"); else { encode_pending = true; encode.assign(p, p + n); } break;
        default: problem("unknown command id"); return;
      }
      at += bytes / 4;
    }
    if (!encode_pending) return;
    if (!created || p_handle_mismatch()) { problem("encode before create, or another session"); return; }
    run_encode(encode, bs_address - uint64_t{ring_index} * 0 + uint64_t{ring_index} * bs_size, bs_size, feedback_address, cpb_hi, cpb_lo);
  }
  bool p_handle_mismatch() const { return handle == 0; }

  // The encode block's fields by their place (counted from Mesa's radeon_vce_52.c): 1-based `n` is the n-th payload dword.
  void run_encode(const std::vector<uint32_t>& e, uint64_t bs_address, uint32_t bs_size, uint64_t feedback_address, uint32_t cpb_hi, uint32_t cpb_lo) {
    auto f = [&](uint32_t n) { return e[n - 1]; };
    ++encodes;
    const uint64_t luma = (uint64_t{f(8)} << 32) | f(9), chroma = (uint64_t{f(10)} << 32) | f(11);
    const uint32_t frame_y_pitch = f(12), luma_pitch = f(13), chroma_pitch = f(14);
    const uint32_t type = f(17), idr = f(18);
    if (frame_y_pitch < height || luma_pitch != ref_pitch || chroma_pitch != ref_pitch || f(15) != 0x00010000) problem("encode pitches");
    if ((type == 3) != (idr == 1) || f(1) != (f(80) == 0 ? 0x11u : 0u)) problem("encode picture type, idr flag or headers");
    Encoded rec{type, f(80), f(81), f(55), f(56), f(58), f(72), 0, f(1) == 0x11, cpb_hi, cpb_lo};
    if (type == 0) {
      // A P picture: its first L0 entry names the previous picture, and the picture goes to another place in the buffer than its reference.
      if (rec.ref_luma == 0xffffffff || rec.ref_luma == rec.cur_luma) problem("P picture reference");
      // The reference is the previous picture, so the reference list needs no modification (dwords 26 and 27).
      if (f(26) != 0 || f(27) != 0) problem("P picture asks for a reference list modification");
    } else if (f(58) != 0xffffffff) {
      problem("IDR picture with a reference");
    }
    // The input: limited-range NV12 in the engine's memory, expanded to a full-range I420 picture for the CPU encoder.
    VideoFrameBuffer i420(*alloc);
    if (!i420.allocate({width, height, PixelFormat::I420}) || !ctrl->translate(luma, uint64_t{luma_pitch} * height) || !ctrl->translate(chroma, uint64_t{chroma_pitch} * height / 2)) {
      problem("input pictures outside the engine's memory");
      return;
    }
    const uint8_t* ly = ctrl->translate(luma, uint64_t{luma_pitch} * height);
    const uint8_t* lc = ctrl->translate(chroma, uint64_t{chroma_pitch} * height / 2);
    ByteView y = i420.plane(0), u = i420.plane(1), v = i420.plane(2);
    auto clamp = [](int x) { return static_cast<uint8_t>(x < 0 ? 0 : x > 255 ? 255 : x); };
    for (uint32_t r = 0; r < height; ++r)
      for (uint32_t c = 0; c < width; ++c) y[uint64_t{r} * width + c] = clamp(((ly[uint64_t{r} * luma_pitch + c] - 16) * 255 + 109) / 219);
    for (uint32_t r = 0; r < height / 2; ++r)
      for (uint32_t c = 0; c < width / 2; ++c) {
        u[uint64_t{r} * (width / 2) + c] = clamp(128 + ((lc[uint64_t{r} * chroma_pitch + c * 2] - 128) * 255) / 224);
        v[uint64_t{r} * (width / 2) + c] = clamp(128 + ((lc[uint64_t{r} * chroma_pitch + c * 2 + 1] - 128) * 255) / 224);
      }
    if (wrong_picture) memset(y.data(), 128, y.size());
    if (!encoder) {
      encoder.reset(new H264Encoder(*alloc));
      if (!encoder->configure(width, height, 30, 1, 90, 1000000)) { problem("CPU encoder"); return; }
    }
    ByteBuffer out(*alloc);
    bool key = false;
    if (!encoder->encode(i420.frame(0, true), type == 3, &out, &key)) { problem("CPU encode"); return; }
    uint8_t* bs = ctrl->translate(bs_address, out.size());
    uint8_t* fb = ctrl->translate(feedback_address, 40);
    if (!bs || !fb || out.size() > bs_size) { problem("bitstream or feedback outside the memory, or too small"); return; }
    memcpy(bs, out.view().data(), out.size());
    if (corrupt)
      for (uint64_t i = 0; i < out.size(); ++i) bs[i] ^= 0x5A;
    rec.bytes = static_cast<uint32_t>(out.size());
    pictures.push_back(rec);
    if (no_bitstream) return;
    ctrl->put32(feedback_address + 0, 1);                              // encStatus
    ctrl->put32(feedback_address + 4, 1);                              // encHasBitstream
    ctrl->put32(feedback_address + 12, 0);                             // encBitstreamOffset
    ctrl->put32(feedback_address + 16, rec.bytes);                     // encBitstreamSize
    ctrl->put32(feedback_address + 36, 0);                             // (the entry the size is reduced by)
  }
};


// The whole card: the ring controller plus the engine's boot behaviour (the ECPU reports its microcode loaded in VCE_STATUS when its clock is on, the windows and the memory
// base are programmed as the driver programs them and the block holds the microcode), the SMC's mailbox and the harvest fuses.
struct Card final : Controller {
  static constexpr uint32_t kStatus = 0x8001, kVcpuCntl = 0x8005, kOffset0 = 0x8009, kSize0 = 0x800a, kSize1 = 0x800c, kSize2 = 0x800e, kSoftReset = 0x8048;
  static constexpr uint32_t kLmiCtrl = 0x85a6, kLmiVmCtrl = 0x85a8, kBar0 = 0x8616, kBar1 = 0x8617, kBar2 = 0x8618;
  static constexpr uint32_t kSmcIndex = 0x80, kSmcData = 0x81, kSmcMessage = 0x94, kSmcResponse = 0x95, kSmcArgument = 0xa4;
  static constexpr uint32_t ixCcHarvestFuses = 0xc00c0028;
  std::map<uint32_t, uint32_t> global;
  uint32_t fuses = 0;                 // the VCE_DISABLE field of CC_HARVEST_FUSES
  uint32_t smc_index = 0;
  // The block the engine looks at, and what its first word must be for the microcode to be accepted.
  uint64_t block_gpu = 0;
  const uint8_t* block_host = nullptr;
  uint32_t expected_first_word = 0;
  bool dead = false;                  // the ECPU never boots
  bool smc_running = true;
  uint32_t smc_result = 1;            // PPSMC_Result_OK
  std::vector<uint32_t> smc_messages;
  uint32_t boots = 0;

  Card() {
    regs[0][kSoftReset - 0x8000] = regs[1][kSoftReset - 0x8000] = 1;
    global[kSmcResponse] = 1;   // the mailbox is idle
  }
  static bool is_vce(uint32_t r) { return r >= 0x8000 && r < 0x9000; }
  uint32_t read(uint32_t r) override {
    if (is_vce(r) || r == 0xc200) return Controller::read(r);
    if (r == kSmcData) return smc_index == ixCcHarvestFuses ? fuses << 1 : 0;
    auto it = global.find(r);
    return it == global.end() ? 0 : it->second;
  }
  void write(uint32_t r, uint32_t v) override {
    if (is_vce(r)) {
      const uint32_t old = Controller::read(r);
      Controller::write(r, v);
      if (r == kSoftReset && (old & 1) && !(v & 1)) boot(selected());
      return;
    }
    if (r == 0xc200) { Controller::write(r, v); return; }
    if (r == kSmcIndex) smc_index = v;
    if (r == kSmcResponse) { global[r] = v; return; }
    if (r == kSmcMessage) {
      smc_messages.push_back(v);
      global[r] = v;
      if (smc_running) global[kSmcResponse] = smc_result;
      return;
    }
    global[r] = v;
  }
  void boot(uint32_t i) {
    ++boots;
    const uint32_t* b = regs[i];
    auto at = [&](uint32_t r) { return b[r - 0x8000]; };
    const uint64_t bar = block_gpu >> 8;
    uint32_t first = 0;
    if (block_host) memcpy(&first, block_host, 4);
    const bool bars = at(kBar0) == bar && at(kBar1) == bar && at(kBar2) == bar;
    const bool lmi = at(kLmiCtrl) == 0x00398000 && at(kLmiVmCtrl) == 0;
    const bool windows = at(kSize0) != 0 && at(kSize1) != 0 && at(kSize2) != 0 && at(kOffset0) == 256;
    if (!dead && (at(kVcpuCntl) & 1) && bars && lmi && windows && first == expected_first_word) regs[i][kStatus - 0x8000] |= 2;
  }
};

}  // namespace fakevce
