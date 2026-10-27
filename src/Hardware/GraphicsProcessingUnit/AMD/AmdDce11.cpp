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


// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license): dce_v11_0.c (dce_v11_0_page_flip, dce_v11_0_crtc_do_set_base, crtc_offsets) and gmc_v8_0.c
// (the system aperture registers).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"

namespace hardware {
namespace amd_dce11 {

namespace {
// The register numbers and field masks this file uses, generated from dce_11_2_d.h, dce_11_2_sh_mask.h and gmc_8_1_d.h by scripts/gen_vcn1_regs.py --vi.
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11Registers.inc"

#define REG_SET_FIELD(orig, reg, field, val) (((orig) & ~reg##__##field##_MASK) | (reg##__##field##_MASK & ((val) << reg##__##field##__SHIFT)))
// (the field of a register value; written with the generated names spelled out)
#define FIELD(value, mask, shift) (((value) & (mask)) >> (shift))

// CRTCn_REGISTER_OFFSET (vid.h): the register blocks of the six controllers.
constexpr uint32_t kCrtcOffsets[kMaxCrtcs] = {0x1b9c - 0x1b9c, 0x1d9c - 0x1b9c, 0x1f9c - 0x1b9c, 0x419c - 0x1b9c, 0x439c - 0x1b9c, 0x459c - 0x1b9c};
// GRPH_CONTROL: the 32-bit ARGB8888 format of dce_v11_0_crtc_do_set_base (GRPH_DEPTH 2, GRPH_FORMAT 0) and linear addressing (array mode 0).
constexpr uint32_t kDepth32 = 2, kFormatArgb8888 = 0, kArrayLinear = 0;
// HPDn_REGISTER_OFFSET (vid.h)
constexpr uint32_t kHpdOffsets[Interrupts::kMaxHpd] = {0x1898 - 0x1898, 0x18a0 - 0x1898, 0x18a8 - 0x1898, 0x18b0 - 0x1898, 0x18b8 - 0x1898, 0x18c0 - 0x1898};

// interrupt_status_offsets of dce_v11_0.c: the register and bits that say which controller's vertical blank and line and which hot plug pin interrupted.
struct InterruptStatus {
  uint32_t reg, vblank, vline, hpd;
};
const InterruptStatus kInterruptStatus[kMaxCrtcs] = {
    {mmDISP_INTERRUPT_STATUS, DISP_INTERRUPT_STATUS__LB_D1_VBLANK_INTERRUPT_MASK, DISP_INTERRUPT_STATUS__LB_D1_VLINE_INTERRUPT_MASK, DISP_INTERRUPT_STATUS__DC_HPD1_INTERRUPT_MASK},
    {mmDISP_INTERRUPT_STATUS_CONTINUE, DISP_INTERRUPT_STATUS_CONTINUE__LB_D2_VBLANK_INTERRUPT_MASK, DISP_INTERRUPT_STATUS_CONTINUE__LB_D2_VLINE_INTERRUPT_MASK,
     DISP_INTERRUPT_STATUS_CONTINUE__DC_HPD2_INTERRUPT_MASK},
    {mmDISP_INTERRUPT_STATUS_CONTINUE2, DISP_INTERRUPT_STATUS_CONTINUE2__LB_D3_VBLANK_INTERRUPT_MASK, DISP_INTERRUPT_STATUS_CONTINUE2__LB_D3_VLINE_INTERRUPT_MASK,
     DISP_INTERRUPT_STATUS_CONTINUE2__DC_HPD3_INTERRUPT_MASK},
    {mmDISP_INTERRUPT_STATUS_CONTINUE3, DISP_INTERRUPT_STATUS_CONTINUE3__LB_D4_VBLANK_INTERRUPT_MASK, DISP_INTERRUPT_STATUS_CONTINUE3__LB_D4_VLINE_INTERRUPT_MASK,
     DISP_INTERRUPT_STATUS_CONTINUE3__DC_HPD4_INTERRUPT_MASK},
    {mmDISP_INTERRUPT_STATUS_CONTINUE4, DISP_INTERRUPT_STATUS_CONTINUE4__LB_D5_VBLANK_INTERRUPT_MASK, DISP_INTERRUPT_STATUS_CONTINUE4__LB_D5_VLINE_INTERRUPT_MASK,
     DISP_INTERRUPT_STATUS_CONTINUE4__DC_HPD5_INTERRUPT_MASK},
    {mmDISP_INTERRUPT_STATUS_CONTINUE5, DISP_INTERRUPT_STATUS_CONTINUE5__LB_D6_VBLANK_INTERRUPT_MASK, DISP_INTERRUPT_STATUS_CONTINUE5__LB_D6_VLINE_INTERRUPT_MASK,
     DISP_INTERRUPT_STATUS_CONTINUE5__DC_HPD6_INTERRUPT_MASK},
};
}  // namespace

uint32_t Plane::offset() const { return kCrtcOffsets[crtc_]; }

bool Plane::probe(uint64_t surface, uint32_t pitch_pixels) {
  active_ = false;
  // (address 0 is a valid place for a framebuffer: video memory starts at the card's own address 0 when the memory controller's base is 0)
  if ((surface & (kSurfaceAlignment - 1)) != 0 || pitch_pixels == 0) return false;
  for (uint32_t i = 0; i < kMaxCrtcs; ++i) {
    const uint32_t off = kCrtcOffsets[i];
    const uint32_t crtc_control = r_.read(mmCRTC_CONTROL + off);
    const uint32_t grph_enable = r_.read(mmGRPH_ENABLE + off);
    if (!(crtc_control & CRTC_CONTROL__CRTC_MASTER_EN_MASK) || !(grph_enable & GRPH_ENABLE__GRPH_ENABLE_MASK)) continue;
    const uint32_t control = r_.read(mmGRPH_CONTROL + off);
    if (FIELD(control, GRPH_CONTROL__GRPH_DEPTH_MASK, GRPH_CONTROL__GRPH_DEPTH__SHIFT) != kDepth32 ||
        FIELD(control, GRPH_CONTROL__GRPH_FORMAT_MASK, GRPH_CONTROL__GRPH_FORMAT__SHIFT) != kFormatArgb8888 ||
        FIELD(control, GRPH_CONTROL__GRPH_ARRAY_MODE_MASK, GRPH_CONTROL__GRPH_ARRAY_MODE__SHIFT) != kArrayLinear)
      continue;
    if (r_.read(mmGRPH_SWAP_CNTL + off) != 0) continue;   // no byte swapping, no crossbar
    if (r_.read(mmGRPH_PITCH + off) != pitch_pixels) continue;
    const uint64_t address = (uint64_t{r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off)} << 32) | (r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS + off) & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
    if (address != surface) continue;
    crtc_ = i;
    original_ = surface;
    pitch_ = pitch_pixels;
    active_ = true;
    break;
  }
  if (!active_) return false;
  // The system aperture (gmc_v8_0_mc_program): pages, inclusive.
  aperture_low_ = uint64_t{r_.read(mmMC_VM_SYSTEM_APERTURE_LOW_ADDR)} << 12;
  aperture_high_ = (uint64_t{r_.read(mmMC_VM_SYSTEM_APERTURE_HIGH_ADDR)} << 12) | 0xFFF;
  if (aperture_high_ <= aperture_low_) active_ = false;
  return active_;
}

bool Plane::in_aperture(uint64_t address, uint64_t bytes) const {
  return active_ && bytes != 0 && address >= aperture_low_ && address <= aperture_high_ && bytes - 1 <= aperture_high_ - address;
}

uint64_t Plane::surface() const {
  if (!active_) return 0;
  const uint32_t off = kCrtcOffsets[crtc_];
  return (uint64_t{r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off)} << 32) | (r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS + off) & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
}

// dce_v11_0_page_flip, asynchronous: the flip is taken at once, not at the next vertical blank.
bool Plane::flip(uint64_t address, uint32_t timeout_us) {
  const uint32_t off = offset();
  const uint32_t saved_flip_control = r_.read(mmGRPH_FLIP_CONTROL + off);
  // flip immediate for async, default is vsync
  uint32_t tmp = saved_flip_control;
  tmp = REG_SET_FIELD(tmp, GRPH_FLIP_CONTROL, GRPH_SURFACE_UPDATE_IMMEDIATE_EN, 1);
  r_.write(mmGRPH_FLIP_CONTROL + off, tmp);
  // update pitch
  r_.write(mmGRPH_PITCH + off, pitch_);
  // update the scanout addresses
  r_.write(mmGRPH_PRIMARY_SURFACE_ADDRESS_HIGH + off, static_cast<uint32_t>(address >> 32));
  // writing to the low address triggers the update
  r_.write(mmGRPH_PRIMARY_SURFACE_ADDRESS + off, static_cast<uint32_t>(address));
  // post the write
  r_.read(mmGRPH_PRIMARY_SURFACE_ADDRESS + off);
  // the update is taken when it is no longer pending
  bool taken = false;
  const uint64_t begin = clock_.now_us();
  do {
    if (!(r_.read(mmGRPH_UPDATE + off) & GRPH_UPDATE__GRPH_SURFACE_UPDATE_PENDING_MASK) && surface() == address) {
      taken = true;
      break;
    }
  } while (clock_.now_us() - begin < timeout_us);
  r_.write(mmGRPH_FLIP_CONTROL + off, saved_flip_control);   // the firmware's flip policy is kept
  return taken;
}

bool Plane::set_surface(uint64_t address, uint64_t bytes, uint32_t timeout_us) {
  if (!active_ || (address & (kSurfaceAlignment - 1)) != 0 || !in_aperture(address, bytes)) return false;
  const uint64_t previous = surface();
  if (flip(address, timeout_us)) return true;
  flip(previous, timeout_us);   // put the old one back
  return false;
}

bool Plane::restore(uint32_t timeout_us) {
  if (!active_) return false;
  return surface() == original_ || flip(original_, timeout_us);
}

namespace {
// The one place a mapped window becomes a view of bytes. The window is memory the mapper handed out for this driver alone (it refuses a range the OS owns), at an
// address that is the same for the CPU as the OS's identity map says, and it outlives the view: the mapping is never torn down.
ByteView view_of(const MmioRegion& window, uint64_t length) { return ByteView(reinterpret_cast<uint8_t*>(window.address()), length); }
// The firmware's framebuffer, which the firmware identity-maps (the UEFI graphics output gave its address).
ByteView firmware_view(const FirmwareFramebuffer& fb, uint64_t length) { return ByteView(reinterpret_cast<uint8_t*>(fb.physical), length); }
}  // namespace

bool Display::engage(const FirmwareFramebuffer& fb, ByteView* memory) {
  disengage();
  if (fb.width == 0 || fb.height == 0 || fb.pitch_pixels < fb.width || fb.physical == 0 || fb.pitch_pixels > UINT32_MAX / 4) return false;
  const uint64_t picture = uint64_t{fb.pitch_pixels} * 4 * fb.height;
  if (picture > fb.bytes || !region_.valid() || region_.length() < picture || (region_gpu_ & (kSurfaceAlignment - 1)) != 0) return false;
  if (fb.physical < cpu_base_ || fb.physical - cpu_base_ > UINT64_MAX - gpu_base_) return false;
  // The firmware's framebuffer as the GPU addresses it must be what the CRTC is fetching now: this is what ties the registers to the memory map.
  const uint64_t firmware_gpu = gpu_base_ + (fb.physical - cpu_base_);
  if (!plane_.probe(firmware_gpu, fb.pitch_pixels) || !plane_.in_aperture(region_gpu_, picture)) return false;
  const ByteView ours = view_of(region_, region_.length());
  if (!ours.first(picture).copy_from(firmware_view(fb, picture))) return false;
  if (!plane_.set_surface(region_gpu_, picture)) return false;
  firmware_ = fb;
  memory_ = ours.first(picture);
  *memory = memory_;
  engaged_ = true;
  return true;
}

void Display::disengage() {
  if (!engaged_) return;
  engaged_ = false;
  // What was drawn since goes back into the firmware's framebuffer, so the picture does not jump when the display goes back to it.
  firmware_view(firmware_, memory_.size()).copy_from(memory_);
  plane_.restore();
  memory_ = ByteView();
}

void Interrupts::set_pageflip(uint32_t crtc, bool enable) {
  if (crtc >= kMaxCrtcs) return;
  const uint32_t reg = r_.read(mmGRPH_INTERRUPT_CONTROL + kCrtcOffsets[crtc]);
  r_.write(mmGRPH_INTERRUPT_CONTROL + kCrtcOffsets[crtc], enable ? (reg | GRPH_INTERRUPT_CONTROL__GRPH_PFLIP_INT_MASK_MASK) : (reg & ~GRPH_INTERRUPT_CONTROL__GRPH_PFLIP_INT_MASK_MASK));
}

void Interrupts::set_vblank(uint32_t crtc, bool enable) {
  if (crtc >= kMaxCrtcs) return;
  uint32_t mask = r_.read(mmLB_INTERRUPT_MASK + kCrtcOffsets[crtc]);
  mask = REG_SET_FIELD(mask, LB_INTERRUPT_MASK, VBLANK_INTERRUPT_MASK, enable ? 1 : 0);
  r_.write(mmLB_INTERRUPT_MASK + kCrtcOffsets[crtc], mask);
}

bool Interrupts::hpd_sense(uint32_t pin) {
  return pin < kMaxHpd && (r_.read(mmDC_HPD_INT_STATUS + kHpdOffsets[pin]) & DC_HPD_INT_STATUS__DC_HPD_SENSE_MASK) != 0;
}

void Interrupts::hpd_set_polarity(uint32_t pin) {
  if (pin >= kMaxHpd) return;
  const bool connected = hpd_sense(pin);
  uint32_t tmp = r_.read(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_INT_POLARITY, connected ? 0 : 1);
  r_.write(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin], tmp);
}

void Interrupts::ack_hpd(uint32_t pin) {
  if (pin >= kMaxHpd) return;
  uint32_t tmp = r_.read(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_INT_ACK, 1);
  r_.write(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin], tmp);
}

void Interrupts::hpd_set_interrupt(uint32_t pin, bool enable) {
  if (pin >= kMaxHpd) return;
  uint32_t tmp = r_.read(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_INT_EN, enable ? 1 : 0);
  r_.write(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin], tmp);
}

void Interrupts::hpd_init(uint32_t pin) {
  if (pin >= kMaxHpd) return;
  uint32_t tmp = r_.read(mmDC_HPD_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_CONTROL, DC_HPD_EN, 1);
  r_.write(mmDC_HPD_CONTROL + kHpdOffsets[pin], tmp);

  tmp = r_.read(mmDC_HPD_TOGGLE_FILT_CNTL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_CONNECT_INT_DELAY, kHpdConnectDelayMs);
  tmp = REG_SET_FIELD(tmp, DC_HPD_TOGGLE_FILT_CNTL, DC_HPD_DISCONNECT_INT_DELAY, kHpdDisconnectDelayMs);
  r_.write(mmDC_HPD_TOGGLE_FILT_CNTL + kHpdOffsets[pin], tmp);

  ack_hpd(pin);
  hpd_set_polarity(pin);
  hpd_set_interrupt(pin, true);   // (amdgpu_irq_get -> dce_v11_0_set_hpd_irq_state)
}

void Interrupts::hpd_fini(uint32_t pin) {
  if (pin >= kMaxHpd) return;
  uint32_t tmp = r_.read(mmDC_HPD_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_CONTROL, DC_HPD_EN, 0);
  r_.write(mmDC_HPD_CONTROL + kHpdOffsets[pin], tmp);
  hpd_set_interrupt(pin, false);   // (amdgpu_irq_put)
}

bool Interrupts::vblank_asserted(uint32_t crtc) { return crtc < kMaxCrtcs && (r_.read(kInterruptStatus[crtc].reg) & kInterruptStatus[crtc].vblank) != 0; }
bool Interrupts::vline_asserted(uint32_t crtc) { return crtc < kMaxCrtcs && (r_.read(kInterruptStatus[crtc].reg) & kInterruptStatus[crtc].vline) != 0; }
bool Interrupts::hpd_asserted(uint32_t pin) { return pin < kMaxHpd && (r_.read(kInterruptStatus[pin].reg) & kInterruptStatus[pin].hpd) != 0; }

void Interrupts::ack_vblank(uint32_t crtc) {
  if (crtc >= kMaxCrtcs) return;
  uint32_t tmp = r_.read(mmLB_VBLANK_STATUS + kCrtcOffsets[crtc]);
  tmp = REG_SET_FIELD(tmp, LB_VBLANK_STATUS, VBLANK_ACK, 1);
  r_.write(mmLB_VBLANK_STATUS + kCrtcOffsets[crtc], tmp);
}

void Interrupts::ack_vline(uint32_t crtc) {
  if (crtc >= kMaxCrtcs) return;
  uint32_t tmp = r_.read(mmLB_VLINE_STATUS + kCrtcOffsets[crtc]);
  tmp = REG_SET_FIELD(tmp, LB_VLINE_STATUS, VLINE_ACK, 1);
  r_.write(mmLB_VLINE_STATUS + kCrtcOffsets[crtc], tmp);
}

bool Interrupts::ack_pageflip(uint32_t crtc) {
  if (crtc >= kMaxCrtcs) return false;
  if (!(r_.read(mmGRPH_INTERRUPT_STATUS + kCrtcOffsets[crtc]) & GRPH_INTERRUPT_STATUS__GRPH_PFLIP_INT_OCCURRED_MASK)) return false;
  r_.write(mmGRPH_INTERRUPT_STATUS + kCrtcOffsets[crtc], GRPH_INTERRUPT_STATUS__GRPH_PFLIP_INT_CLEAR_MASK);
  return true;
}

bool Presenter::start(INativeDisplay& display, ByteView video, uint32_t pitch_bytes, uint32_t height) {
  const uint64_t bytes = uint64_t{pitch_bytes} * height;
  if (pitch_bytes == 0 || height == 0 || video.size() != bytes) return false;
  back_.clear();
  display_ = nullptr;
  if (!back_.put_zeros(bytes) || !back_.writable_view().copy_from(video)) return false;
  display_ = &display;
  video_ = video;
  pitch_ = pitch_bytes;
  height_ = height;
  return true;
}

bool Presenter::present(uint32_t first_row, uint32_t rows) {
  if (!display_ || rows == 0 || first_row >= height_ || rows > height_ - first_row) return false;
  const uint64_t offset = uint64_t{first_row} * pitch_, length = uint64_t{rows} * pitch_;
  const ConstByteView source = back_.view().subview(offset, length);
  if (display_->upload_picture(source, offset)) {
    ++dma_presents_;
    return true;
  }
  ++cpu_presents_;
  return video_.subview(offset, length).copy_from(source);
}

}  // namespace amd_dce11
}  // namespace hardware
