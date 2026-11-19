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

// The sequences follow nouveau (MIT license): nvkm/engine/disp/nv50.c, gf119.c, gv100.c, tu102.c, ga102.c (the *_disp_init and *_core_init/fini
// functions), nvkm/core/ramht.c, nvkm/engine/dma/user*.c and dispnv50/core*.c / wndwc37e.c (the update and surface methods). All numbers are
// in NvDisplayHw.hpp.
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplay.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvDisplayHw.hpp"

namespace hardware {
namespace nv {

using namespace disp_hw;

namespace {

// The one place the display code turns bytes of GPU-visible memory into 32-bit words. Memory the GPU writes (notifiers) or reads (push
// buffers, RAMHT, context DMAs) has to be read and written with single aligned 32-bit accesses that the compiler neither merges nor drops:
// the offset is checked against the view and to be a multiple of 4, the view itself starts aligned (INvMemory gives at least 256-byte
// alignment), so the cast is to an aligned uint32_t inside the view; volatile keeps each access.
bool store32(ByteView memory, uint64_t offset, uint32_t value) {
  ByteView word;
  if ((offset & 3) || !memory.try_subview(offset, 4, &word) || (reinterpret_cast<uint64_t>(word.data()) & 3)) return false;
  *reinterpret_cast<volatile uint32_t*>(word.data()) = value;   // little endian, as the GPU reads it (x86 and the hosts tested are LE)
  return true;
}
bool load32(ByteView memory, uint64_t offset, uint32_t* value) {
  ByteView word;
  if ((offset & 3) || !memory.try_subview(offset, 4, &word) || (reinterpret_cast<uint64_t>(word.data()) & 3)) return false;
  *value = *reinterpret_cast<volatile const uint32_t*>(word.data());
  return true;
}

constexpr uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

}  // namespace

DisplayClass display_class(const Chip& chip) {
  switch (chip.family) {
    case Family::Tesla: return DisplayClass::Nv50;
    // GF100..GF110 kept the Tesla display (nouveau: gt215_disp_new); GF117 and GF119 brought the 907d one.
    case Family::Fermi: return (chip.chipset == 0xd7 || chip.chipset == 0xd9) ? DisplayClass::Gf119 : DisplayClass::Nv50;
    case Family::Kepler:
    case Family::Maxwell:
    case Family::Pascal: return DisplayClass::Gf119;
    case Family::Volta: return DisplayClass::Gv100;
    case Family::Turing: return DisplayClass::Tu102;
    // GA102..GA107: nouveau drives their display without GSP (ga102_disp). GA100 has none.
    case Family::Ampere: return chip.chipset == 0x170 ? DisplayClass::Gsp : DisplayClass::Ga102;
    case Family::Hopper:
    case Family::Ada:
    case Family::Blackwell: return DisplayClass::Gsp;
    default: return DisplayClass::None;
  }
}

const char* display_class_name(DisplayClass display_class) {
  switch (display_class) {
    case DisplayClass::Nv50: return "NV50 (507d)";
    case DisplayClass::Gf119: return "GF119 (907d)";
    case DisplayClass::Gv100: return "GV100 (c37d)";
    case DisplayClass::Tu102: return "TU102 (c57d)";
    case DisplayClass::Ga102: return "GA102 (c67d)";
    case DisplayClass::Gsp: return "GSP-managed";
    default: return "none";
  }
}

const char* display_status_text(DisplayStatus status) {
  switch (status) {
    case DisplayStatus::Ok: return "ok";
    case DisplayStatus::NotNative: return "the display engine of this chip is not programmed natively";
    case DisplayStatus::NeedsGsp: return "the display of Ada and later chips runs through the GSP firmware";
    case DisplayStatus::NoHeads: return "the display reports no heads";
    case DisplayStatus::NoMemory: return "no video memory for the display engine";
    case DisplayStatus::OwnershipTimeout: return "the VBIOS did not hand the display over";
    case DisplayStatus::ChannelTimeout: return "a display channel did not come up";
    case DisplayStatus::PushTimeout: return "the display engine did not fetch its push buffer";
    case DisplayStatus::NotifierTimeout: return "the display engine did not complete the update";
    case DisplayStatus::NotLatched: return "the display engine did not take the new state";
    case DisplayStatus::NoFirmwareSurface: return "no head shows the firmware's framebuffer";
    case DisplayStatus::UnsupportedSurface: return "the surface is not linear 32-bit or out of range";
    case DisplayStatus::BadArgument: return "bad argument";
    case DisplayStatus::NotClaimed: return "the display has not been taken over";
    case DisplayStatus::NotImplemented: return "not implemented for this display generation";
    case DisplayStatus::LinkTraining: return "DisplayPort link training failed";
    case DisplayStatus::NoOutput: return "the output cannot be routed to a head";
  }
  return "?";
}

bool ScanoutSurface::linear_32bpp() const { return pitch_linear && (format == kFormatA8R8G8B8 || format == kFormatX8R8G8B8); }

// ---------------------------------------------------------------------------------------------------------------------------------------
// MethodList
// ---------------------------------------------------------------------------------------------------------------------------------------
bool MethodList::add(uint32_t method, uint32_t value) {
  if (count_ >= kCapacity || (method & 3) || method > 0xfffc) {
    overflow_ = true;
    return false;
  }
  method_[count_] = method;
  value_[count_] = value;
  ++count_;
  return true;
}

uint32_t MethodList::push_words() const {
  uint32_t words = 0;
  for (uint32_t i = 0; i < count_; ++i) {
    const bool continues = i > 0 && method_[i] == method_[i - 1] + 4;
    words += continues ? 1 : 2;
  }
  return words;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// NvDisplayEngine
// ---------------------------------------------------------------------------------------------------------------------------------------
NvDisplayEngine::NvDisplayEngine(INvRegisters& registers, INvMemory& memory, const Chip& chip, DisplayTimeouts timeouts)
    : r_(registers), memory_(memory), chip_(chip), timeouts_(timeouts) {}

NvDisplayEngine::~NvDisplayEngine() {
  shutdown();
  if (instance_.valid()) memory_.release(instance_);
  if (notifier_.valid()) memory_.release(notifier_);
}

uint32_t NvDisplayEngine::vram_handle() { return kHandleVram; }

DisplayStatus NvDisplayEngine::fail(DisplayStatus status, const char* why) {
  reason_ = why ? why : display_status_text(status);
  return status;
}

bool NvDisplayEngine::wait_register(uint32_t offset, uint32_t mask, uint32_t value) {
  for (uint32_t i = 0; i < timeouts_.attempts; ++i) {
    if ((r_.read32(offset) & mask) == value) return true;
    r_.delay_us(timeouts_.poll_us);
  }
  return false;
}

DisplayStatus NvDisplayEngine::probe() {
  class_ = nv::display_class(chip_);
  probed_ = false;
  if (class_ == DisplayClass::Gsp) return fail(DisplayStatus::NeedsGsp, nullptr);
  // (Chip::display_native covers Tesla..Turing, the chips whose boot display is taken over; GA102's display is driven only from scratch.)
  if (class_ == DisplayClass::None || (class_ != DisplayClass::Ga102 && !chip_.display_native)) return fail(DisplayStatus::NotNative, nullptr);
  switch (class_) {
    case DisplayClass::Nv50: {
      heads_ = kNv50Heads;
      head_mask_ = (1u << kNv50Heads) - 1;
      const uint32_t present = r_.read32(kNv50SorPresent);
      if (present == 0xffffffffu) return fail(DisplayStatus::NoHeads, "the display engine does not answer");
      sor_mask_ = (present >> 24) & 0xf;
      if (sor_mask_ == 0) sor_mask_ = 0x3;     // NV50 itself has two SORs and may not report them
      break;
    }
    case DisplayClass::Gf119: {
      const uint32_t count = r_.read32(kGf119HeadCount), present = r_.read32(kGf119Present);
      if (count == 0xffffffffu || present == 0xffffffffu) return fail(DisplayStatus::NoHeads, "the display engine does not answer");
      heads_ = count & 0xf;
      head_mask_ = present & 0xf;
      sor_mask_ = (present >> 8) & 0xff;
      break;
    }
    case DisplayClass::Gv100:
    case DisplayClass::Tu102:
    case DisplayClass::Ga102: {
      const uint32_t counts = r_.read32(kGv100Counts), present = r_.read32(kGv100Present);
      if (counts == 0xffffffffu || present == 0xffffffffu) return fail(DisplayStatus::NoHeads, "the display engine does not answer");
      heads_ = counts & 0xf;
      head_mask_ = present & 0xff;
      sor_mask_ = (present >> 8) & 0xff;
      windows_ = (counts >> 20) & 0x3f;
      window_mask_ = r_.read32(kGv100WindowsPresent);
      break;
    }
    default: break;
  }
  if (heads_ > kMaxHeads) heads_ = kMaxHeads;
  head_mask_ &= (1u << kMaxHeads) - 1;
  if (heads_ == 0 || head_mask_ == 0) return fail(DisplayStatus::NoHeads, nullptr);
  probed_ = true;
  reason_ = "probed";
  return DisplayStatus::Ok;
}

uint32_t NvDisplayEngine::armed(uint32_t method) const {
  return r_.read32((class_ == DisplayClass::Gf119 ? kGf119Armed : kGv100Armed) + method);
}

uint32_t NvDisplayEngine::window_armed(uint32_t window, uint32_t method) const {
  return r_.read32(kGv100WindowArmed + window * kWindowUserStride + method);
}

bool NvDisplayEngine::armed_surface(uint32_t head, ScanoutSurface* out, uint32_t* window) {
  if (!probed_ || !out || head >= kMaxHeads || !(head_mask_ & (1u << head))) return false;
  ScanoutSurface s;
  switch (class_) {
    case DisplayClass::Nv50: {
      const uint32_t h = head * kNv50HeadStateStride + kNv50ArmedDelta;
      s.offset = uint64_t{r_.read32(kNv50StateOffset + h)} << 8;
      const uint32_t size = r_.read32(kNv50StateSize + h), storage = r_.read32(kNv50StateStorage + h);
      s.width = size & 0xffff;
      s.height = size >> 16;
      s.pitch_linear = storage & m507d::kStorageLayoutPitch;
      s.pitch_bytes = s.pitch_linear ? (storage & 0x3ff00) : 0;
      s.format = static_cast<uint8_t>(r_.read32(kNv50StateParams + h) >> 8);
      s.context_dma = r_.read32(kNv50StateIso + h);
      s.point_in = r_.read32(kNv50StatePointIn + h);
      break;
    }
    case DisplayClass::Gf119: {
      const uint32_t h = head * m907d::kHeadStride;
      s.offset = uint64_t{armed(m907d::kHeadOffset + h)} << 8;
      const uint32_t size = armed(m907d::kHeadSize + h), storage = armed(m907d::kHeadStorage + h);
      s.width = size & 0xffff;
      s.height = size >> 16;
      s.pitch_linear = storage & m907d::kStorageLayoutPitch;
      s.pitch_bytes = s.pitch_linear ? (storage & 0x3ff00) : 0;
      s.format = static_cast<uint8_t>(armed(m907d::kHeadParams + h) >> 8);
      s.context_dma = armed(m907d::kHeadIso + h);
      s.point_in = armed(m907d::kHeadPointIn + h);
      break;
    }
    case DisplayClass::Gv100:
    case DisplayClass::Tu102:
    case DisplayClass::Ga102: {
      bool found = false;
      for (uint32_t w = 0; w < kMaxWindows && !found; ++w) {
        if (!(window_mask_ & (1u << w))) continue;
        if ((armed(mc37d::kWindowControl + w * mc37d::kWindowStride) & mc37d::kWindowOwnerMask) != head) continue;
        ScanoutSurface t;
        const uint32_t size = window_armed(w, mc37e::kSize);
        t.width = size & 0xffff;
        t.height = size >> 16;
        if (t.width == 0 || t.height == 0) continue;
        t.offset = uint64_t{window_armed(w, mc37e::kOffset)} << 8;
        t.pitch_linear = window_armed(w, mc37e::kStorage) & mc37e::kStorageLayoutPitch;
        t.pitch_bytes = t.pitch_linear ? (window_armed(w, mc37e::kPlanarStorage) & mc37e::kPlanarPitchMask) * mc37e::kPlanarPitchUnit : 0;
        t.format = static_cast<uint8_t>(window_armed(w, mc37e::kParams) & mc37e::kFormatMask);
        t.context_dma = window_armed(w, mc37e::kIso);
        t.point_in = window_armed(w, mc37e::kPointIn);
        t.size_in = window_armed(w, mc37e::kSizeIn);
        t.size_out = window_armed(w, mc37e::kSizeOut);
        s = t;
        if (window) *window = w;
        found = true;
      }
      if (!found) return false;
      break;
    }
    default: return false;
  }
  *out = s;
  return s.valid();
}

// The "inform EVO of the display capabilities" copies of *_disp_init.
void NvDisplayEngine::write_capabilities() {
  switch (class_) {
    case DisplayClass::Nv50:
      r_.write32(kNv50CapsMirror, r_.read32(kNv50Caps));
      for (uint32_t h = 0; h < heads_; ++h)
        for (uint32_t j = 0; j < 4; ++j) r_.write32(kNv50HeadCapsMirror + h * 0x10 + j * 4, r_.read32(kNv50HeadCaps + h * 0x800 + j * 4));
      for (uint32_t i = 0; i < 3; ++i) r_.write32(kNv50DacCapsMirror + i * 4, r_.read32(kNv50DacCaps + i * 0x800));
      for (uint32_t i = 0; i < kMaxSors; ++i)
        if (sor_mask_ & (1u << i)) r_.write32(kNv50SorCapsMirror + i * 4, r_.read32(kNv50SorCaps + i * kSorStride));
      break;
    case DisplayClass::Gf119: {
      for (uint32_t h = 0; h < kMaxHeads; ++h) {
        if (!(head_mask_ & (1u << h))) continue;
        for (uint32_t j = 0; j < 3; ++j) r_.write32(kGf119HeadCapsMirror + h * 0x800 + j * 4, r_.read32(kGf119HeadCaps + h * 0x800 + j * 4));
      }
      const uint32_t dacs = (r_.read32(kGf119Present) >> 4) & 0xf;
      for (uint32_t i = 0; i < 4; ++i)
        if (dacs & (1u << i)) r_.write32(kGf119DacCapsMirror + i * 0x800, r_.read32(kGf119DacCaps + i * 0x800));
      for (uint32_t i = 0; i < kMaxSors; ++i)
        if (sor_mask_ & (1u << i)) r_.write32(kGf119SorCapsMirror + i * 0x800, r_.read32(kGf119SorCaps + i * kSorStride));
      break;
    }
    case DisplayClass::Gv100:
    case DisplayClass::Tu102:
    case DisplayClass::Ga102: {
      const bool turing = class_ == DisplayClass::Tu102 || class_ == DisplayClass::Ga102;   // (ga102_disp.init = tu102_disp_init)
      r_.write32(kGv100LockPinsMirror, turing ? kTu102LockPinsValue : r_.read32(kGv100LockPins));
      for (uint32_t i = 0; i < kMaxSors; ++i) {
        if (!(sor_mask_ & (1u << i))) continue;
        const uint32_t caps = r_.read32(kGf119SorCaps + i * kSorStride);
        r_.mask(kGv100CapsEnable, 0x100u << i, 0x100u << i);
        r_.write32(kGv100SorCapsMirror + i * 8, caps);
      }
      for (uint32_t h = 0; h < kMaxHeads; ++h) {
        if (!(head_mask_ & (1u << h))) continue;
        r_.write32(kGv100HeadRgMirror + h * 0x20, r_.read32(kGv100HeadRgCaps + h * 0x800));
        const uint32_t words = turing ? kTu102PostcompWords : kGv100PostcompWords;
        for (uint32_t j = 0; j < words; ++j) {
          const uint32_t from = (turing ? kTu102HeadPostcomp : kGv100HeadPostcomp) + h * 0x800 + j * 4;
          r_.write32((turing ? kTu102HeadPostcompMirror : kGv100HeadPostcompMirror) + h * 0x20 + j * 4, r_.read32(from));
        }
      }
      for (uint32_t w = 0; w < kMaxWindows; ++w) {
        if (!(window_mask_ & (1u << w))) continue;
        r_.mask(kGv100WindowCapsEnable, 1u << w, 1u << w);
        for (uint32_t j = 0; j < 6; ++j) {
          if (turing) r_.mask(kTu102WindowCapsMirror + w * 0x20 + j * 4, kTu102WindowCapsMask, r_.read32(kTu102WindowCaps + w * 0x800 + j * 4) & kTu102WindowCapsMask);
          else r_.write32(kGv100WindowCapsMirror + w * 0x20 + j * 4, r_.read32(kGv100WindowCaps + w * 0x800 + j * 4));
        }
        if (turing) r_.mask(kTu102WindowCapsValid, kTu102WindowCapsValidBit, kTu102WindowCapsValidBit);
      }
      for (uint32_t i = 0; i < (turing ? 3u : 4u); ++i) r_.write32(kGv100IhubMirror + i * 4, r_.read32(kGv100Ihub + i * 4));
      r_.mask(kGv100Misc, 1, 1);
      break;
    }
    default: break;
  }
}

DisplayStatus NvDisplayEngine::take_from_vbios() {
  if (pre_volta()) {
    const uint32_t owner = class_ == DisplayClass::Nv50 ? kNv50VbiosOwner : kGf119VbiosOwner;
    if (!(r_.read32(owner) & kVbiosOwnerBit)) return DisplayStatus::Ok;
    r_.write32(owner, kVbiosOwnerBit);
    r_.mask(kVbiosHandshake, kHandshakeRequest, 0);
    if (!wait_register(kVbiosHandshake, kHandshakeBusy, 0)) return fail(DisplayStatus::OwnershipTimeout, nullptr);
    return DisplayStatus::Ok;
  }
  if (!(r_.read32(kGv100Handshake) & kHandshakeBusy)) return DisplayStatus::Ok;
  r_.mask(kGv100Handshake, kHandshakeRequest, 0);
  if (!wait_register(kGv100Handshake, kHandshakeBusy, 0)) return fail(DisplayStatus::OwnershipTimeout, nullptr);
  return DisplayStatus::Ok;
}

bool NvDisplayEngine::push_target(const GpuMemory& memory, uint32_t* target) const {
  switch (memory.target) {
    case MemoryTarget::Vram: *target = kPushTargetVram; return true;
    case MemoryTarget::SystemNoncoherent: *target = kPushTargetNoncoherent; return true;
    case MemoryTarget::SystemCoherent: *target = kPushTargetCoherent; return true;
  }
  return false;
}

// The RAMHT (nvkm_ramht_hash / nvkm_ramht_insert): the handle folded into the table's bits, the channel id mixed in, linear probing.
// 4 KiB (9 bits) before Volta, 8 KiB (10 bits) from Volta on.
bool NvDisplayEngine::ramht_insert(uint32_t chid, uint32_t handle, uint32_t object_offset) {
  const uint32_t entries = ramht_bytes() / 8;
  const uint32_t bits = pre_volta() ? kRamhtBits : kGv100RamhtBits;
  uint32_t hash = 0;
  for (uint32_t h = handle; h; h >>= bits) hash ^= h & (entries - 1);
  hash ^= chid << (bits - 4);
  hash &= entries - 1;
  uint32_t context = 0;
  switch (class_) {
    case DisplayClass::Nv50: context = (object_offset << kNv50RamhtShift) | (chid << 28) | chid; break;
    case DisplayClass::Gf119: context = (object_offset << kGf119RamhtShift) | (chid << 27) | 1; break;
    default: context = (object_offset << kGv100RamhtShift) | (chid << 25) | 0x40; break;
  }
  for (uint32_t i = 0; i < entries; ++i) {
    const uint32_t slot = (hash + i) & (entries - 1);
    uint32_t used = 0;
    if (!load32(instance_.cpu, slot * 8 + 4, &used)) return false;
    if (used != 0) continue;
    return store32(instance_.cpu, slot * 8, handle) && store32(instance_.cpu, slot * 8 + 4, context);
  }
  return false;
}

// A context DMA object in the instance memory (nv50_dmaobj_bind / gf119_dmaobj_bind / gv100_dmaobj_bind), for VRAM [start, limit].
bool NvDisplayEngine::add_context_dma(uint32_t chid, uint32_t handle, uint64_t start, uint64_t limit) {
  const uint64_t object = align_up(instance_used_, kCtxDmaAlign);
  if (object + kCtxDmaAlign > instance_.cpu.size() || object >= kInstanceBytes) return false;
  instance_used_ = static_cast<uint32_t>(object + kCtxDmaAlign);
  uint32_t words[6] = {};
  switch (class_) {
    case DisplayClass::Nv50:
    case DisplayClass::Gf119:
      words[0] = class_ == DisplayClass::Nv50 ? kNv50CtxDmaVram : kGf119CtxDmaVram;
      words[1] = static_cast<uint32_t>(limit);
      words[2] = static_cast<uint32_t>(start);
      words[3] = static_cast<uint32_t>(limit >> 32) << 24 | static_cast<uint32_t>(start >> 32);
      words[4] = 0;
      words[5] = class_ == DisplayClass::Nv50 ? kNv50CtxDmaFlags5 : 0;
      break;
    default:
      words[0] = kGv100CtxDmaVram;
      words[1] = static_cast<uint32_t>(start >> 8);
      words[2] = static_cast<uint32_t>(start >> 40);
      words[3] = static_cast<uint32_t>(limit >> 8);
      words[4] = static_cast<uint32_t>(limit >> 40);
      break;
  }
  for (uint32_t i = 0; i < 6; ++i)
    if (!store32(instance_.cpu, object + i * 4, words[i])) return false;
  return ramht_insert(chid, handle, static_cast<uint32_t>(object));
}

DisplayStatus NvDisplayEngine::setup_instance() {
  if (!instance_.valid()) instance_ = memory_.allocate(MemoryTarget::Vram, kInstanceBytes, kInstanceBytes);
  if (!notifier_.valid()) notifier_ = memory_.allocate(MemoryTarget::Vram, kNotifierBytes, kNotifierBytes);
  if (!instance_.valid() || !notifier_.valid() || instance_.cpu.size() < kInstanceBytes || notifier_.cpu.size() < kNotifierBytes ||
      (instance_.gpu_address & (kInstanceBytes - 1)) || (instance_.gpu_address >> 40) != 0)
    return fail(DisplayStatus::NoMemory, "no video memory for the display's instance memory or notifier");
  instance_.cpu.fill(0);
  notifier_.cpu.fill(0);
  instance_used_ = ramht_bytes();
  if (pre_volta()) {
    r_.write32(kInstance, static_cast<uint32_t>(instance_.gpu_address >> 8) | kInstanceEnableNv50);
  } else {
    r_.write32(kInstance, kGv100InstanceEnable | kPushTargetVram);
    r_.write32(kGv100InstanceAddress, static_cast<uint32_t>(instance_.gpu_address >> 16));
  }
  memory_.flush(instance_);
  return DisplayStatus::Ok;
}

DisplayStatus NvDisplayEngine::start_core() {
  if (!core_.push.valid()) core_.push = memory_.allocate(MemoryTarget::Vram, kPushBytes, kPushBytes);
  if (!core_.push.valid()) core_.push = memory_.allocate(MemoryTarget::SystemCoherent, kPushBytes, kPushBytes);
  uint32_t target = 0;
  if (!core_.push.valid() || core_.push.cpu.size() < kPushBytes || !push_target(core_.push, &target) || (core_.push.gpu_address >> 40) != 0)
    return fail(DisplayStatus::NoMemory, "no memory for the core channel's push buffer");
  core_.push.cpu.fill(0);
  memory_.flush(core_.push);
  const uint32_t push = target | static_cast<uint32_t>(core_.push.gpu_address >> 8);
  core_.chid = 0;
  core_.cursor = 0;
  if (pre_volta()) {
    const bool nv50 = class_ == DisplayClass::Nv50;
    const uint32_t ctrl = nv50 ? kNv50CoreCtrl : kGf119CoreCtrl;
    if (nv50) {   // "attempt to unstick channel from some unknown state"
      if ((r_.read32(ctrl) & 0x009f0000) == 0x00020000) r_.mask(ctrl, 0x00800000, 0x00800000);
      if ((r_.read32(ctrl) & 0x003f0000) == 0x00030000) r_.mask(ctrl, 0x00600000, 0x00600000);
    }
    r_.write32(nv50 ? kNv50CorePush : kGf119CorePush, push);
    r_.write32(nv50 ? kNv50CorePush1 : kGf119CorePush1, kCorePush1Value);
    r_.write32(nv50 ? kNv50CorePush2 : kGf119CorePush2, nv50 ? kNv50CorePush2Value : kGf119CorePush2Value);
    r_.mask(ctrl, kCtrlPutEnable, kCtrlPutEnable);
    r_.write32(kCoreUserPre + kUserPut, 0);
    r_.write32(ctrl, kCtrlStartCore);
    if (!wait_register(ctrl, kCtrlBusy, 0)) return fail(DisplayStatus::ChannelTimeout, "the core channel did not start");
    core_.user = kCoreUserPre;
  } else {
    r_.write32(kGv100PushLow, push);
    r_.write32(kGv100PushHigh, 0);
    r_.write32(kGv100Push2, kGv100Push2Value);
    r_.write32(kGv100Push3, kGv100Push3Value);
    r_.mask(kGv100CoreCtrl, kCtrlPutEnable, kCtrlPutEnable);
    r_.write32(kCoreUserGv100 + kUserPut, 0);
    r_.mask(kGv100CoreCtrl, 1, 1);
    if (!wait_register(kGv100CoreCtrl, kGv100CtrlStatusMask, kGv100CtrlStatusUp)) return fail(DisplayStatus::ChannelTimeout, "the core channel did not start");
    core_.user = kCoreUserGv100;
  }
  core_.up = true;
  return DisplayStatus::Ok;
}

DisplayStatus NvDisplayEngine::start_window(uint32_t window) {
  if (window >= kMaxWindows || !(window_mask_ & (1u << window))) return fail(DisplayStatus::BadArgument, "no such window");
  if (!window_.push.valid()) window_.push = memory_.allocate(MemoryTarget::Vram, kPushBytes, kPushBytes);
  if (!window_.push.valid()) window_.push = memory_.allocate(MemoryTarget::SystemCoherent, kPushBytes, kPushBytes);
  uint32_t target = 0;
  if (!window_.push.valid() || window_.push.cpu.size() < kPushBytes || !push_target(window_.push, &target) || (window_.push.gpu_address >> 40) != 0)
    return fail(DisplayStatus::NoMemory, "no memory for the window channel's push buffer");
  window_.push.cpu.fill(0);
  memory_.flush(window_.push);
  const uint32_t poff = (1 + window) * kGv100ChannelStride;
  window_.chid = 1 + window;
  window_.window = window;
  window_.cursor = 0;
  window_.user = kWindowUserGv100 + window * kWindowUserStride;
  r_.write32(kGv100PushLow + poff, target | static_cast<uint32_t>(window_.push.gpu_address >> 8));
  r_.write32(kGv100PushHigh + poff, 0);
  r_.write32(kGv100Push2 + poff, kGv100Push2Value);
  r_.write32(kGv100Push3 + poff, kGv100Push3Value);
  r_.write32(window_.user + kUserPut, 0);
  r_.write32(kGv100CoreCtrl + poff, kGv100WindowStart);
  if (!wait_register(kGv100WindowIdle + window * 4, kGv100WindowIdleMask, kGv100WindowIdleValue))
    return fail(DisplayStatus::ChannelTimeout, "the window channel did not start");
  window_.up = true;
  return DisplayStatus::Ok;
}

// The window's immediate channel (gv100_disp_wimm: a DMA channel like the window's, without context DMAs: it names no memory).
DisplayStatus NvDisplayEngine::start_window_immediate() {
  if (wimm_.up) return DisplayStatus::Ok;
  if (pre_volta() || !window_.up) return fail(DisplayStatus::NotClaimed, "no window channel");
  if (!wimm_.push.valid()) wimm_.push = memory_.allocate(MemoryTarget::Vram, kPushBytes, kPushBytes);
  uint32_t target = 0;
  if (!wimm_.push.valid() || wimm_.push.cpu.size() < kPushBytes || !push_target(wimm_.push, &target) || (wimm_.push.gpu_address >> 40) != 0) {
    if (wimm_.push.valid()) memory_.release(wimm_.push);
    wimm_.push = GpuMemory();
    return fail(DisplayStatus::NoMemory, "no memory for the window immediate channel's push buffer");
  }
  wimm_.push.cpu.fill(0);
  memory_.flush(wimm_.push);
  const uint32_t w = window_.window;
  wimm_.chid = kGv100WimmChid + w;
  wimm_.window = w;
  wimm_.cursor = 0;
  wimm_.user = kWindowUserGv100 + (wimm_.chid - 1) * kWindowUserStride;
  const uint32_t poff = wimm_.chid * kGv100ChannelStride;
  r_.write32(kGv100PushLow + poff, target | static_cast<uint32_t>(wimm_.push.gpu_address >> 8));
  r_.write32(kGv100PushHigh + poff, 0);
  r_.write32(kGv100Push2 + poff, kGv100Push2Value);
  r_.write32(kGv100Push3 + poff, kGv100Push3Value);
  r_.write32(wimm_.user + kUserPut, 0);
  r_.write32(kGv100CoreCtrl + poff, kGv100WindowStart);
  if (!wait_register(kGv100WindowIdle + (wimm_.chid - 1) * 4, kGv100WindowIdleMask, kGv100WindowIdleValue)) {
    wimm_.up = true;      // (stopped and released by stop_channel)
    stop_channel(wimm_);
    return fail(DisplayStatus::ChannelTimeout, "the window immediate channel did not start");
  }
  wimm_.up = true;
  return DisplayStatus::Ok;
}

DisplayStatus NvDisplayEngine::claim(uint32_t window) {
  if (!probed_) return fail(DisplayStatus::NotClaimed, "claim before probe");
  if (claimed_) return DisplayStatus::Ok;
  if (!pre_volta() && (window >= kMaxWindows || !(window_mask_ & (1u << window)))) return fail(DisplayStatus::BadArgument, "no such window");
  write_capabilities();
  DisplayStatus s = take_from_vbios();
  if (s != DisplayStatus::Ok) return s;
  s = setup_instance();
  if (s != DisplayStatus::Ok) return s;
  // The context DMAs: the notifier page and all of VRAM, for the core channel (and the window channel on Volta+).
  const uint64_t vram = memory_.vram_bytes() ? memory_.vram_bytes() : (uint64_t{1} << 40);
  const uint64_t ntfy_start = notifier_.gpu_address, ntfy_limit = notifier_.gpu_address + notifier_.bytes - 1;
  bool ok = add_context_dma(0, kHandleNotifier, ntfy_start, ntfy_limit) && add_context_dma(0, kHandleVram, 0, vram - 1);
  if (ok && !pre_volta()) ok = add_context_dma(1 + window, kHandleNotifier, ntfy_start, ntfy_limit) && add_context_dma(1 + window, kHandleVram, 0, vram - 1);
  if (!ok) return fail(DisplayStatus::NoMemory, "the display's instance memory is full");
  memory_.flush(instance_);
  // No interrupts: completion is polled (and the supervisor too, NvDisplayMode).
  if (class_ == DisplayClass::Nv50) {
    r_.write32(kNv50IntrEnable1, 0);
    r_.write32(kNv50IntrEnable0, 0);
  } else if (class_ == DisplayClass::Gf119) {
    r_.write32(kGf119IntrEnable0, 0);
    r_.write32(kGf119IntrEnableErr, 0);
    r_.write32(kGf119IntrEnableSuper, 0);
    for (uint32_t h = 0; h < kMaxHeads; ++h)
      if (head_mask_ & (1u << h)) r_.mask(kGf119UnderflowReport + h * 0x800, 0x111, 0x010);
  }
  s = start_core();
  if (s != DisplayStatus::Ok) return s;
  if (!pre_volta()) {
    s = start_window(window);
    if (s != DisplayStatus::Ok) {
      stop_channel(core_);
      return s;
    }
  }
  claimed_ = true;
  reason_ = "claimed";
  return DisplayStatus::Ok;
}

void NvDisplayEngine::stop_channel(Channel& channel) {
  if (channel.up) {
    uint32_t ctrl = 0;
    if (class_ == DisplayClass::Nv50) ctrl = kNv50CoreCtrl;
    else if (class_ == DisplayClass::Gf119) ctrl = kGf119CoreCtrl;
    else ctrl = kGv100CoreCtrl + channel.chid * kGv100ChannelStride;
    r_.mask(ctrl, kCtrlPutEnable, 0);
    r_.mask(ctrl, kCtrlEnableBits, 0);
    if (pre_volta()) (void)wait_register(ctrl, kCtrlStopBusyMask, 0);   // a channel that does not stop is left; its memory goes regardless
    channel.up = false;
  }
  if (channel.push.valid()) memory_.release(channel.push);
  channel.push = GpuMemory();
}

void NvDisplayEngine::shutdown() {
  stop_channel(wimm_);
  stop_channel(window_);
  stop_channel(core_);
  claimed_ = false;
}

// Writes `methods` into the push buffer and moves PUT; waits for GET to catch up. A batch that does not fit before the end of the buffer
// starts at its beginning (a jump word, as nv50_dmac_wind).
DisplayStatus NvDisplayEngine::submit(Channel& channel, const MethodList& methods) {
  if (!channel.up) return fail(DisplayStatus::NotClaimed, nullptr);
  if (methods.overflowed()) return fail(DisplayStatus::BadArgument, "too many methods in one batch");
  const uint32_t total = static_cast<uint32_t>(channel.push.cpu.size() / 4);
  const uint32_t needed = methods.push_words();
  if (needed + 1 >= total) return fail(DisplayStatus::BadArgument, "batch larger than the push buffer");
  if (channel.cursor + needed + 1 > total) {
    if (!store32(channel.push.cpu, uint64_t{channel.cursor} * 4, kPushJumpToStart)) return fail(DisplayStatus::BadArgument, nullptr);
    memory_.flush(channel.push);
    channel.cursor = 0;
    r_.write32(channel.user + kUserPut, 0);
    if (!wait_register(channel.user + kUserGet, 0xffffffffu, 0)) return fail(DisplayStatus::PushTimeout, nullptr);
  }
  uint32_t cursor = channel.cursor;
  for (uint32_t i = 0; i < methods.size();) {
    uint32_t run = 1;
    while (i + run < methods.size() && run < kPushCountMax && methods.method(i + run) == methods.method(i) + run * 4) ++run;
    bool ok = store32(channel.push.cpu, uint64_t{cursor++} * 4, (run << kPushCountShift) | methods.method(i));
    for (uint32_t j = 0; j < run && ok; ++j) ok = store32(channel.push.cpu, uint64_t{cursor++} * 4, methods.value(i + j));
    if (!ok) return fail(DisplayStatus::BadArgument, nullptr);
    i += run;
  }
  memory_.flush(channel.push);
  channel.cursor = cursor;
  r_.write32(channel.user + kUserPut, cursor * 4);
  if (!wait_register(channel.user + kUserGet, 0xffffffffu, cursor * 4)) return fail(DisplayStatus::PushTimeout, nullptr);
  return DisplayStatus::Ok;
}

bool NvDisplayEngine::wait_notifier(uint32_t offset, bool c37) {
  for (uint32_t i = 0; i < timeouts_.attempts; ++i) {
    uint32_t word = 0;
    if (!load32(notifier_.cpu, offset, &word)) return false;
    if (c37 ? ((word >> kNotifierStatusShift) & kNotifierStatusMask) == kNotifierFinished : (word & kNotifierDoneEvo) != 0) return true;
    if (poller_) poller_->poll();
    r_.delay_us(timeouts_.poll_us);
  }
  return false;
}

// The notifier armed, the methods, UPDATE (core507d_update / core907d / corec37d_update / wndwc37e_update), and the wait for the notifier.
DisplayStatus NvDisplayEngine::update(Channel& channel, const MethodList& methods, bool window) {
  if (!channel.up) return fail(DisplayStatus::NotClaimed, nullptr);
  const uint32_t offset = window ? kWindowNotifierOffset : kCoreNotifierOffset;
  for (uint32_t i = 0; i < 4; ++i) (void)store32(notifier_.cpu, offset + i * 4, 0);
  memory_.flush(notifier_);
  MethodList list;
  bool ok = true;
  if (pre_volta()) {
    ok = list.add(m907d::kContextDmaNotifier, kHandleNotifier) && list.add(m907d::kNotifierControl, m907d::kNotifierEnable | offset);
  } else if (!window) {
    ok = list.add(mc37d::kContextDmaNotifier, kHandleNotifier) && list.add(mc37d::kNotifierControl, mc37d::kNotifierEnable | offset);
  } else {
    ok = list.add(mc37e::kContextDmaNotifier, kHandleNotifier) && list.add(mc37e::kNotifierControl, offset);
  }
  for (uint32_t i = 0; i < methods.size() && ok; ++i) ok = list.add(methods.method(i), methods.value(i));
  if (pre_volta()) {
    ok = ok && list.add(m907d::kUpdate, 0) && list.add(m907d::kNotifierControl, 0);
  } else if (!window) {
    ok = ok && list.add(mc37d::kInterlockFlags, 0) && list.add(mc37d::kWindowInterlockFlags, 0) && list.add(mc37d::kUpdate, mc37d::kUpdateValue) &&
         list.add(mc37d::kNotifierControl, 0);
  } else {
    ok = ok && list.add(mc37e::kUpdate, mc37e::kUpdateValue);
  }
  if (!ok || methods.overflowed()) return fail(DisplayStatus::BadArgument, "too many methods in one update");
  const DisplayStatus s = submit(channel, list);
  if (s != DisplayStatus::Ok) return s;
  if (!wait_notifier(offset, !pre_volta())) return fail(DisplayStatus::NotifierTimeout, nullptr);
  return DisplayStatus::Ok;
}

DisplayStatus NvDisplayEngine::core_update(const MethodList& methods) {
  if (!claimed_) return fail(DisplayStatus::NotClaimed, nullptr);
  return update(core_, methods, false);
}

DisplayStatus NvDisplayEngine::core_send(const MethodList& methods) {
  if (!claimed_) return fail(DisplayStatus::NotClaimed, nullptr);
  return submit(core_, methods);
}

DisplayStatus NvDisplayEngine::window_update(const MethodList& methods) {
  if (!claimed_ || pre_volta() || !window_.up) return fail(DisplayStatus::NotClaimed, nullptr);
  return update(window_, methods, true);
}

DisplayStatus NvDisplayEngine::window_immediate_update(const MethodList& methods) {
  if (!claimed_ || pre_volta()) return fail(DisplayStatus::NotClaimed, nullptr);
  DisplayStatus s = start_window_immediate();
  if (s != DisplayStatus::Ok) return s;
  MethodList list;
  bool ok = true;
  for (uint32_t i = 0; i < methods.size() && ok; ++i) ok = list.add(methods.method(i), methods.value(i));
  if (!ok || methods.overflowed() || !list.add(mc37b::kUpdate, mc37b::kUpdateValue)) return fail(DisplayStatus::BadArgument, "too many methods in one update");
  return submit(wimm_, list);
}

void NvDisplayEngine::surface_methods(uint32_t head, const ScanoutSurface& s, MethodList* out) const {
  const uint32_t size = s.height << 16 | (s.width & 0xffff);
  switch (class_) {
    case DisplayClass::Nv50: {
      const uint32_t h = head * m507d::kHeadStride;
      (void)(out->add(m507d::kHeadOffset + h, static_cast<uint32_t>(s.offset >> 8)) && out->add(m507d::kHeadSize + h, size) &&
             out->add(m507d::kHeadStorage + h, m507d::kStorageLayoutPitch | (s.pitch_bytes & 0x3ff00)) &&
             out->add(m507d::kHeadParams + h, uint32_t{s.format} << 8) && out->add(m507d::kHeadIso + h, s.context_dma) &&
             out->add(m507d::kHeadPointIn + h, s.point_in));
      break;
    }
    case DisplayClass::Gf119: {
      const uint32_t h = head * m907d::kHeadStride;
      (void)(out->add(m907d::kHeadOffset + h, static_cast<uint32_t>(s.offset >> 8)) && out->add(m907d::kHeadSize + h, size) &&
             out->add(m907d::kHeadStorage + h, m907d::kStorageLayoutPitch | (s.pitch_bytes & 0x3ff00)) &&
             out->add(m907d::kHeadParams + h, uint32_t{s.format} << 8) && out->add(m907d::kHeadIso + h, s.context_dma) &&
             out->add(m907d::kHeadPointIn + h, s.point_in));
      break;
    }
    default:
      (void)(out->add(mc37e::kSize, size) && out->add(mc37e::kStorage, mc37e::kStorageLayoutPitch) && out->add(mc37e::kParams, s.format) &&
             out->add(mc37e::kPlanarStorage, (s.pitch_bytes / mc37e::kPlanarPitchUnit) & mc37e::kPlanarPitchMask) &&
             out->add(mc37e::kIso, s.context_dma) && out->add(mc37e::kOffset, static_cast<uint32_t>(s.offset >> 8)) &&
             out->add(mc37e::kPointIn, s.point_in) && out->add(mc37e::kSizeIn, s.size_in ? s.size_in : size) &&
             out->add(mc37e::kSizeOut, s.size_out ? s.size_out : size));
      break;
  }
}

DisplayStatus NvDisplayEngine::set_surface(uint32_t head, const ScanoutSurface& surface) {
  if (!claimed_) return fail(DisplayStatus::NotClaimed, nullptr);
  if (head >= kMaxHeads || !(head_mask_ & (1u << head))) return fail(DisplayStatus::BadArgument, "no such head");
  const uint32_t pitch_unit = pre_volta() ? 256 : mc37e::kPlanarPitchUnit;
  const uint32_t pitch_max = pre_volta() ? 0x3ff00 : mc37e::kPlanarPitchMask * mc37e::kPlanarPitchUnit;
  if (!surface.valid() || !surface.linear_32bpp() || (surface.offset & 0xff) || (surface.offset >> 40) || (surface.pitch_bytes % pitch_unit) ||
      surface.pitch_bytes > pitch_max || surface.width > 0x7fff || surface.height > 0x7fff || uint64_t{surface.width} * 4 > surface.pitch_bytes)
    return fail(DisplayStatus::UnsupportedSurface, nullptr);
  ScanoutSurface before;
  uint32_t window = 0;
  const bool had_before = armed_surface(head, &before, &window);
  if (!pre_volta() && (!had_before || window != window_.window || !window_.up))
    return fail(DisplayStatus::NotImplemented, "the head's window is not the one this driver brought up");
  Channel& channel = pre_volta() ? core_ : window_;
  MethodList methods;
  surface_methods(head, surface, &methods);
  DisplayStatus s = update(channel, methods, !pre_volta());
  if (s == DisplayStatus::PushTimeout) return s;    // the engine is not listening: sending the old surface again would not be heard either
  ScanoutSurface now;
  if (s == DisplayStatus::Ok && armed_surface(head, &now) && now.offset == surface.offset && now.pitch_bytes == surface.pitch_bytes &&
      now.width == surface.width && now.height == surface.height && now.context_dma == surface.context_dma)
    return DisplayStatus::Ok;
  if (s == DisplayStatus::Ok) s = fail(DisplayStatus::NotLatched, nullptr);
  const char* why = reason_;
  if (had_before && before.linear_32bpp()) {
    before.context_dma = kHandleVram;   // the firmware's handle means nothing in this driver's RAMHT (its offsets are taken as VRAM offsets)
    MethodList back;
    surface_methods(head, before, &back);
    (void)update(channel, back, !pre_volta());
  }
  reason_ = why;
  return s;
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// NvNativeDisplay
// ---------------------------------------------------------------------------------------------------------------------------------------
bool NvNativeDisplay::engage_display(const NvFirmwareFramebuffer& firmware, ByteView* memory) {
  if (!memory) return fail("bad argument");
  if (engaged_) {
    *memory = memory_view_;
    return true;
  }
  if (firmware.width == 0 || firmware.height == 0 || firmware.pitch_bytes == 0 || uint64_t{firmware.width} * 4 > firmware.pitch_bytes)
    return fail("the firmware's mode is not a 32-bit one");
  const uint64_t picture = uint64_t{firmware.pitch_bytes} * firmware.height;
  if (!firmware.pixels.empty() && firmware.pixels.size() < picture) return fail("the firmware's framebuffer is smaller than its mode");
  DisplayStatus s = engine_.probe();
  if (s != DisplayStatus::Ok) return fail(engine_.reason());
  if (engine_.display_class() == DisplayClass::Ga102)   // (the boot display of an Ampere card: its GOP mode stays, no GSP is booted here)
    return fail("the takeover is not done on GA102: its boot display stays the firmware's (no GSP is run; mode setting from scratch only)");

  // The heads that show the firmware's picture (several when it is cloned), all from the same surface.
  uint32_t heads = 0, window = 0;
  ScanoutSurface original;
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h) {
    ScanoutSurface surface;
    uint32_t w = 0;
    if (!engine_.armed_surface(h, &surface, &w)) continue;
    if (surface.width != firmware.width || surface.height != firmware.height || surface.pitch_bytes != firmware.pitch_bytes || !surface.linear_32bpp()) continue;
    if (heads != 0 && surface.offset != original.offset) continue;
    if (heads == 0) {
      original = surface;
      window = w;
    }
    heads |= 1u << h;
    if (engine_.display_class() == DisplayClass::Gv100 || engine_.display_class() == DisplayClass::Tu102) break;   // one window channel
  }
  if (heads == 0) return fail(display_status_text(DisplayStatus::NoFirmwareSurface));

  if (!framebuffer_.valid()) {
    const uint64_t bytes = (picture + 0xffff) & ~uint64_t{0xffff};
    framebuffer_ = memory_.allocate(MemoryTarget::Vram, bytes, 0x10000);
    if (!framebuffer_.valid()) return fail("no video memory for the framebuffer");
    if (framebuffer_.cpu.size() < picture || (framebuffer_.gpu_address & 0xff)) {
      release_framebuffer();
      return fail("the framebuffer in video memory is not visible to the CPU");
    }
  }
  ByteView view = framebuffer_.cpu.first(picture);
  if (!firmware.pixels.empty()) (void)view.copy_from(ConstByteView(firmware.pixels.first(picture)));
  memory_.flush(framebuffer_);

  s = engine_.claim(window);
  if (s != DisplayStatus::Ok) {
    release_framebuffer();
    return fail(engine_.reason());
  }
  ScanoutSurface target = original;
  target.offset = framebuffer_.gpu_address;
  target.context_dma = NvDisplayEngine::vram_handle();
  ScanoutSurface back = original;
  back.context_dma = NvDisplayEngine::vram_handle();
  uint32_t switched = 0;
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h) {
    if (!(heads & (1u << h))) continue;
    s = engine_.set_surface(h, target);
    if (s != DisplayStatus::Ok) break;
    switched |= 1u << h;
  }
  if (s != DisplayStatus::Ok) {
    const char* why = engine_.reason();
    for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h)
      if (switched & (1u << h)) (void)engine_.set_surface(h, back);
    for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h) {
      ScanoutSurface now;
      if ((heads & (1u << h)) && engine_.armed_surface(h, &now) && now.offset == framebuffer_.gpu_address) framebuffer_in_doubt_ = true;
    }
    release_framebuffer();
    return fail(why);
  }
  heads_ = heads;
  original_ = original;
  firmware_ = firmware;
  memory_view_ = view;
  engaged_ = true;
  status_ = "engaged";
  *memory = view;
  return true;
}

void NvNativeDisplay::disengage_display() {
  if (!engaged_) return;
  if (!firmware_.pixels.empty()) (void)firmware_.pixels.copy_from(ConstByteView(memory_view_));
  ScanoutSurface back = original_;
  back.context_dma = NvDisplayEngine::vram_handle();
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h)
    if (heads_ & (1u << h)) (void)engine_.set_surface(h, back);
  for (uint32_t h = 0; h < NvDisplayEngine::kMaxHeads; ++h) {
    ScanoutSurface now;
    if ((heads_ & (1u << h)) && (!engine_.armed_surface(h, &now) || now.offset != original_.offset)) framebuffer_in_doubt_ = true;
  }
  engaged_ = false;
  heads_ = 0;
  memory_view_ = ByteView();
  status_ = framebuffer_in_doubt_ ? "the firmware's framebuffer could not be put back" : "not engaged";
  release_framebuffer();
}

void NvNativeDisplay::release_framebuffer() {
  if (!framebuffer_.valid() || framebuffer_in_doubt_) return;   // one a head may still be fetching stays allocated for good
  memory_.release(framebuffer_);
  framebuffer_ = GpuMemory();
}

}  // namespace nv
}  // namespace hardware
