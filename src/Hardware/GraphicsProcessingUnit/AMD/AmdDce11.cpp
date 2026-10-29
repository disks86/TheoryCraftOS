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

// The registers of the graphics plane that the surface setup writes (dce_11_2_d.h, offset by the CRTC's block), and the VGA control registers (bit 0 is the VGA mode enable).
namespace {
constexpr uint32_t kGrphEnable = 0x1a00, kGrphControl = 0x1a01, kGrphLut10BitBypass = 0x1a02, kGrphSwapCntl = 0x1a03, kGrphPrimaryAddress = 0x1a04, kGrphSecondaryAddress = 0x1a05,
                   kGrphPitch = 0x1a06, kGrphPrimaryAddressHigh = 0x1a07, kGrphSecondaryAddressHigh = 0x1a08, kGrphSurfaceOffsetX = 0x1a09, kGrphSurfaceOffsetY = 0x1a0a,
                   kGrphXStart = 0x1a0b, kGrphYStart = 0x1a0c, kGrphXEnd = 0x1a0d, kGrphYEnd = 0x1a0e, kGrphFlipControl = 0x1a12, kLbDesktopHeight = 0x1ac3,
                   kViewportStart = 0x1b5c, kViewportSize = 0x1b5d, kCrtcMasterUpdateMode = 0x1bbe;
constexpr uint32_t kVgaControl[kMaxCrtcs] = {0xcc, 0xce, 0xf8, 0xf9, 0xfa, 0xfb};
constexpr uint32_t kFlipHRetraceEnable = 0x1, kLut10BitBypassEnable = 0x100;
// GRPH_CONTROL fields: depth 1:0, num banks 3:2, bank width 7:6, format 10:8, bank height 12:11, tile split 15:13, macro tile aspect 19:18, array mode 23:20, pipe config 28:24, micro tile mode 30:29
constexpr uint32_t kShiftNumBanks = 2, kShiftBankWidth = 6, kShiftFormat = 8, kShiftBankHeight = 11, kShiftTileSplit = 13, kShiftMacroTileAspect = 18, kShiftArrayMode = 20, kShiftPipeConfig = 24,
                   kShiftMicroTileMode = 29;
// GRPH_SWAP_CNTL crossbars: red 5:4, green 7:6, blue 9:8, alpha 11:10 (2 selects the blue / red input)
constexpr uint32_t kSwapRedCrossbarBlue = 2u << 4, kSwapBlueCrossbarRed = 2u << 8;
// the colour pipeline of dce_v11_0_crtc_load_lut
constexpr uint32_t kInputCscControl = 0x1a35, kPrescaleGrphControl = 0x1a2d, kInputGammaControl = 0x1a10, kDcLutControl = 0x1a80, kDcLutBlackBlue = 0x1a81, kDcLutBlackGreen = 0x1a82,
                   kDcLutBlackRed = 0x1a83, kDcLutWhiteBlue = 0x1a84, kDcLutWhiteGreen = 0x1a85, kDcLutWhiteRed = 0x1a86, kDcLutRwMode = 0x1a78, kDcLutWriteEnMask = 0x1a7e,
                   kDcLutRwIndex = 0x1a79, kDcLut30Color = 0x1a7c, kDegammaControl = 0x1a58, kGamutRemapControl = 0x1a59, kRegammaControl = 0x1aa0, kOutputCscControl = 0x1a3c,
                   kDenormControl = 0x1a50, kAlphaControl = 0x1abc;
constexpr uint32_t kInputCscGrphModeMask = 0x3, kPrescaleBypass = 0x10, kInputGammaModeMask = 0x1, kDegammaModeMask = 0x3 | 0x300 | 0x3000, kGamutModeMask = 0x3, kRegammaModeMask = 0x7,
                   kOutputCscModeMask = 0x7, kCursorAlphaBlend = 0x2;

struct FormatBits {
  uint32_t depth, format, swap;
  bool bypass_lut;
};
bool format_bits(ScanoutFormat f, FormatBits* out) {
  switch (f) {
    case ScanoutFormat::Indexed8: *out = {0, 0, 0, false}; return true;
    case ScanoutFormat::Argb4444: *out = {1, 2, 0, false}; return true;
    case ScanoutFormat::Argb1555: *out = {1, 0, 0, false}; return true;
    case ScanoutFormat::Bgra5551: *out = {1, 5, 0, false}; return true;
    case ScanoutFormat::Rgb565: *out = {1, 1, 0, false}; return true;
    case ScanoutFormat::Argb8888: *out = {2, 0, 0, false}; return true;
    case ScanoutFormat::Argb2101010: *out = {2, 1, 0, true}; return true;     // (more than 8 bits per colour bypasses the table to keep the precision)
    case ScanoutFormat::Bgra1010102: *out = {2, 4, 0, true}; return true;
    case ScanoutFormat::Abgr8888: *out = {2, 0, kSwapRedCrossbarBlue | kSwapBlueCrossbarRed, false}; return true;
  }
  return false;
}
}  // namespace

bool Scanout::set(uint32_t crtc, const ScanoutSurface& s) {
  FormatBits fb;
  const ScanoutTiling& t = s.tiling;
  if (crtc >= kMaxCrtcs || (s.address & (kSurfaceAlignment - 1)) != 0 || s.width == 0 || s.height == 0 || s.pitch_pixels < s.width || s.viewport_width == 0 || s.viewport_height == 0 ||
      !format_bits(s.format, &fb) || (t.mode != ScanoutTiling::Mode::Linear && t.mode != ScanoutTiling::Mode::Tiled1D && t.mode != ScanoutTiling::Mode::Tiled2D) || t.pipe_config > 0x1F ||
      t.num_banks > 3 || t.bank_width > 3 || t.bank_height > 3 || t.macro_tile_aspect > 3 || t.tile_split > 7)
    return false;
  uint32_t control = fb.depth | (fb.format << kShiftFormat);
  if (t.mode == ScanoutTiling::Mode::Tiled2D) {
    control |= (uint32_t{t.num_banks} << kShiftNumBanks) | (uint32_t{4} << kShiftArrayMode) | (uint32_t{t.tile_split} << kShiftTileSplit) | (uint32_t{t.bank_width} << kShiftBankWidth) |
               (uint32_t{t.bank_height} << kShiftBankHeight) | (uint32_t{t.macro_tile_aspect} << kShiftMacroTileAspect) | (0u << kShiftMicroTileMode);       // ADDR_SURF_MICRO_TILING_DISPLAY
  } else if (t.mode == ScanoutTiling::Mode::Tiled1D) {
    control |= uint32_t{2} << kShiftArrayMode;
  }
  control |= uint32_t{t.pipe_config} << kShiftPipeConfig;
  const uint32_t off = kCrtcOffsets[crtc];
  r_.write(kVgaControl[crtc], r_.read(kVgaControl[crtc]) & ~1u);                                  // dce_v11_0_vga_enable(false)
  // the surface address is taken at vertical blank, not horizontal
  r_.write(kGrphFlipControl + off, r_.read(kGrphFlipControl + off) & ~kFlipHRetraceEnable);
  r_.write(kGrphPrimaryAddressHigh + off, static_cast<uint32_t>(s.address >> 32));
  r_.write(kGrphSecondaryAddressHigh + off, static_cast<uint32_t>(s.address >> 32));
  r_.write(kGrphPrimaryAddress + off, static_cast<uint32_t>(s.address) & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
  r_.write(kGrphSecondaryAddress + off, static_cast<uint32_t>(s.address) & GRPH_PRIMARY_SURFACE_ADDRESS__GRPH_PRIMARY_SURFACE_ADDRESS_MASK);
  r_.write(kGrphControl + off, control);
  r_.write(kGrphSwapCntl + off, fb.swap);                                                        // no endian swap (little endian)
  const uint32_t bypass = r_.read(kGrphLut10BitBypass + off);
  r_.write(kGrphLut10BitBypass + off, fb.bypass_lut ? (bypass | kLut10BitBypassEnable) : (bypass & ~kLut10BitBypassEnable));
  r_.write(kGrphSurfaceOffsetX + off, 0);
  r_.write(kGrphSurfaceOffsetY + off, 0);
  r_.write(kGrphXStart + off, 0);
  r_.write(kGrphYStart + off, 0);
  r_.write(kGrphXEnd + off, s.width);
  r_.write(kGrphYEnd + off, s.height);
  r_.write(kGrphPitch + off, s.pitch_pixels);
  r_.write(kGrphEnable + off, 1);                                                                // dce_v11_0_grph_enable(true)
  r_.write(kLbDesktopHeight + off, s.height);
  r_.write(kViewportStart + off, ((s.x & ~3u) << 16) | (s.y & ~1u));
  r_.write(kViewportSize + off, (s.viewport_width << 16) | ((s.viewport_height + 1) & ~1u));
  r_.write(kCrtcMasterUpdateMode + off, 0);                                                      // the flip may happen anywhere in the vertical blank interval
  return true;
}

bool Scanout::load_lut(uint32_t crtc, const LutEntry* table) {
  if (crtc >= kMaxCrtcs) return false;
  const uint32_t off = kCrtcOffsets[crtc];
  auto modify = [&](uint32_t reg, uint32_t clear, uint32_t set) { r_.write(reg + off, (r_.read(reg + off) & ~clear) | set); };
  modify(kInputCscControl, kInputCscGrphModeMask, 0);
  modify(kPrescaleGrphControl, kPrescaleBypass, kPrescaleBypass);
  modify(kInputGammaControl, kInputGammaModeMask, 0);
  r_.write(kDcLutControl + off, 0);
  r_.write(kDcLutBlackBlue + off, 0);
  r_.write(kDcLutBlackGreen + off, 0);
  r_.write(kDcLutBlackRed + off, 0);
  r_.write(kDcLutWhiteBlue + off, 0xffff);
  r_.write(kDcLutWhiteGreen + off, 0xffff);
  r_.write(kDcLutWhiteRed + off, 0xffff);
  r_.write(kDcLutRwMode + off, 0);
  r_.write(kDcLutWriteEnMask + off, 0x7);
  r_.write(kDcLutRwIndex + off, 0);
  for (uint32_t i = 0; i < 256; ++i) {
    const uint32_t r = table ? table[i].red : i << 8 | i, g = table ? table[i].green : i << 8 | i, b = table ? table[i].blue : i << 8 | i;
    r_.write(kDcLut30Color + off, ((r & 0xffc0) << 14) | ((g & 0xffc0) << 4) | (b >> 6));
  }
  modify(kDegammaControl, kDegammaModeMask, 0);
  modify(kGamutRemapControl, kGamutModeMask, 0);
  modify(kRegammaControl, kRegammaModeMask, 0);
  modify(kOutputCscControl, kOutputCscModeMask, 0);
  r_.write(kDenormControl + off, 0);
  modify(kAlphaControl, 0, kCursorAlphaBlend);
  return true;
}

bool Scanout::disable(uint32_t crtc) {
  if (crtc >= kMaxCrtcs) return false;
  r_.write(kGrphEnable + kCrtcOffsets[crtc], 0);
  return true;
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

void Interrupts::set_vline(uint32_t crtc, bool enable) {
  if (crtc >= kMaxCrtcs) return;
  uint32_t mask = r_.read(mmLB_INTERRUPT_MASK + kCrtcOffsets[crtc]);
  mask = REG_SET_FIELD(mask, LB_INTERRUPT_MASK, VLINE_INTERRUPT_MASK, enable ? 1 : 0);
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

void Interrupts::hpd_rx_set_interrupt(uint32_t pin, bool enable) {
  if (pin >= kMaxHpd) return;
  uint32_t tmp = r_.read(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_RX_INT_EN, enable ? 1 : 0);
  r_.write(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin], tmp);
}

bool Interrupts::hpd_rx_asserted(uint32_t pin) {
  return pin < kMaxHpd && (r_.read(mmDC_HPD_INT_STATUS + kHpdOffsets[pin]) & DC_HPD_INT_STATUS__DC_HPD_RX_INT_STATUS_MASK) != 0;
}

void Interrupts::ack_hpd_rx(uint32_t pin) {
  if (pin >= kMaxHpd) return;
  uint32_t tmp = r_.read(mmDC_HPD_INT_CONTROL + kHpdOffsets[pin]);
  tmp = REG_SET_FIELD(tmp, DC_HPD_INT_CONTROL, DC_HPD_RX_INT_ACK, 1);
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
  hpd_rx_set_interrupt(pin, false);
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

namespace {
constexpr uint32_t kAudioPinOffsets[AudioPins::kMaxPins] = {0x17a8 - 0x17a8, 0x17ac - 0x17a8, 0x17b0 - 0x17a8, 0x17b4 - 0x17a8, 0x17b8 - 0x17a8, 0x17bc - 0x17a8, 0x17c0 - 0x17a8, 0x17c4 - 0x17a8};
}  // namespace

uint32_t AudioPins::endpoint_read(uint32_t pin, uint32_t reg) {
  if (pin >= kMaxPins) return 0;
  r_.write(mmAZALIA_F0_CODEC_ENDPOINT_INDEX + kAudioPinOffsets[pin], reg);
  return r_.read(mmAZALIA_F0_CODEC_ENDPOINT_DATA + kAudioPinOffsets[pin]);
}

uint32_t AudioPins::port_connectivity(uint32_t pin) {
  const uint32_t tmp = endpoint_read(pin, ixAZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_CONFIGURATION_DEFAULT);
  return FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_CONFIGURATION_DEFAULT__PORT_CONNECTIVITY_MASK, AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_CONFIGURATION_DEFAULT__PORT_CONNECTIVITY__SHIFT);
}

bool AudioPins::endpoint_write(uint32_t pin, uint32_t reg, uint32_t value) {
  if (pin >= kMaxPins) return false;
  r_.write(mmAZALIA_F0_CODEC_ENDPOINT_INDEX + kAudioPinOffsets[pin], reg);
  r_.write(mmAZALIA_F0_CODEC_ENDPOINT_DATA + kAudioPinOffsets[pin], value);
  return true;
}

bool AudioPins::set_enabled(uint32_t pin, bool enable) {
  return endpoint_write(pin, ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL, enable ? AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK : 0);
}

bool AudioPins::enabled(uint32_t pin) {
  return (endpoint_read(pin, ixAZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL) & AZALIA_F0_CODEC_PIN_CONTROL_HOT_PLUG_CONTROL__AUDIO_ENABLED_MASK) != 0;
}

bool AudioPins::write_speaker_allocation(uint32_t pin, const EdidAudio::Info& info) {
  if (pin >= kMaxPins) return false;
  uint32_t tmp = endpoint_read(pin, ixAZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER);
  tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER, DP_CONNECTION, 0);
  tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER, HDMI_CONNECTION, 1);
  tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER, SPEAKER_ALLOCATION, info.speaker_count ? info.speaker_allocation : kStereoSpeakers);
  return endpoint_write(pin, ixAZALIA_F0_CODEC_PIN_CONTROL_CHANNEL_SPEAKER, tmp);
}

namespace {
// eld_reg_to_type of dce_v11_0.c: the descriptor register of each audio coding type (HDMI_AUDIO_CODING_TYPE_*) the endpoint has one for
struct DescriptorRegister {
  uint32_t reg;
  uint8_t format;
};
constexpr uint8_t kFormatPcm = 1;
const DescriptorRegister kDescriptorRegisters[] = {
    {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0, kFormatPcm}, {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR1, 2},   {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR2, 3},
    {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR3, 4},          {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR4, 5},   {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR5, 6},
    {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR6, 7},          {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR7, 8},   {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR9, 10},
    {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR10, 11},        {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR11, 12}, {ixAZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR13, 14},
};
}  // namespace

bool AudioPins::write_sads(uint32_t pin, const EdidAudio::Info& info) {
  if (pin >= kMaxPins || info.sad_count == 0) return false;
  for (const DescriptorRegister& d : kDescriptorRegisters) {
    uint32_t tmp = 0;
    uint32_t stereo_freqs = 0;
    int max_channels = -1;
    for (uint32_t j = 0; j < info.sad_count && j < EdidAudio::kMaxSads; ++j) {
      const Sad& sad = info.sads[j];
      if (sad.format != d.format) continue;
      if (sad.channels > max_channels) {
        tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0, MAX_CHANNELS, sad.channels);
        tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0, DESCRIPTOR_BYTE_2, sad.byte2);
        tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES, sad.freq);
        max_channels = sad.channels;
      }
      if (sad.format == kFormatPcm) stereo_freqs |= sad.freq;
      else break;     // (of the other formats the first descriptor is the one)
    }
    tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_AUDIO_DESCRIPTOR0, SUPPORTED_FREQUENCIES_STEREO, stereo_freqs);
    endpoint_write(pin, d.reg, tmp);
  }
  return true;
}

bool AudioPins::write_latency(uint32_t pin, const EdidAudio::Info& info, bool interlaced) {
  const uint32_t which = interlaced ? 1 : 0;
  uint32_t tmp = 0;
  if (info.latency_present[which]) {
    tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC, VIDEO_LIPSYNC, info.video_latency[which]);
    tmp = REG_SET_FIELD(tmp, AZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC, AUDIO_LIPSYNC, info.audio_latency[which]);
  }
  return endpoint_write(pin, ixAZALIA_F0_CODEC_PIN_CONTROL_RESPONSE_LIPSYNC, tmp);
}

bool AudioPins::connected(uint32_t pin) { return pin < kMaxPins && port_connectivity(pin) != kPortConnectivityNone; }

uint32_t AudioPins::connected_mask(uint32_t pin_count) {
  uint32_t mask = 0;
  for (uint32_t pin = 0; pin < pin_count && pin < kMaxPins; ++pin)
    if (connected(pin)) mask |= 1u << pin;
  return mask;
}

namespace {
// DIGn_REGISTER_OFFSET (vid.h) for DIG 0 to 5: the register blocks of the DIG encoders, and with them of the audio format blocks (dig_offsets in dce_v11_0.c).
constexpr uint32_t kDigOffsets[AudioFormat::kBlocks] = {0x4a00 - 0x4a00, 0x4b00 - 0x4a00, 0x4c00 - 0x4a00, 0x4d00 - 0x4a00, 0x4e00 - 0x4a00, 0x4f00 - 0x4a00};
}  // namespace

bool AudioFormat::select_pin(uint32_t block, uint32_t pin) {
  if (block >= kBlocks || pin >= AudioPins::kMaxPins) return false;
  uint32_t tmp = r_.read(mmAFMT_AUDIO_SRC_CONTROL + kDigOffsets[block]);
  tmp = REG_SET_FIELD(tmp, AFMT_AUDIO_SRC_CONTROL, AFMT_AUDIO_SRC_SELECT, pin);
  r_.write(mmAFMT_AUDIO_SRC_CONTROL + kDigOffsets[block], tmp);
  return true;
}

namespace {
// dce_v11_0_afmt_setmode's constants
constexpr uint32_t kAudioCrcControl = 0x1000;
constexpr uint32_t kRamp0 = 0x00FFFFFF, kRamp1 = 0x007FFFFF, kRamp2 = 1, kRamp3 = 1;
constexpr uint32_t kDtoPhase = 24 * 1000;
constexpr uint32_t kInfoframeLine = 2, kAudioPacketsPerLine = 3;
}  // namespace

bool AudioFormat::setup_packets(uint32_t block, uint32_t crtc, uint32_t clock_khz, uint32_t bpc) {
  if (block >= kBlocks || crtc >= kMaxCrtcs) return false;
  const uint32_t off = kDigOffsets[block];
  uint32_t tmp;
  // dce_v11_0_audio_set_dto
  tmp = r_.read(mmDCCG_AUDIO_DTO_SOURCE);
  tmp = REG_SET_FIELD(tmp, DCCG_AUDIO_DTO_SOURCE, DCCG_AUDIO_DTO0_SOURCE_SEL, crtc);
  r_.write(mmDCCG_AUDIO_DTO_SOURCE, tmp);
  r_.write(mmDCCG_AUDIO_DTO0_PHASE, kDtoPhase);
  r_.write(mmDCCG_AUDIO_DTO0_MODULE, clock_khz);

  tmp = r_.read(mmHDMI_VBI_PACKET_CONTROL + off);
  tmp = REG_SET_FIELD(tmp, HDMI_VBI_PACKET_CONTROL, HDMI_NULL_SEND, 1);      // send null packets when required
  r_.write(mmHDMI_VBI_PACKET_CONTROL + off, tmp);

  r_.write(mmAFMT_AUDIO_CRC_CONTROL + off, kAudioCrcControl);

  tmp = r_.read(mmHDMI_CONTROL + off);
  switch (bpc) {
    case 10:
      tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_ENABLE, 1);
      tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 1);
      break;
    case 12:
      tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_ENABLE, 1);
      tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 2);
      break;
    default:      // 0, 6, 8, 16 and anything else: no deep colour
      tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_ENABLE, 0);
      tmp = REG_SET_FIELD(tmp, HDMI_CONTROL, HDMI_DEEP_COLOR_DEPTH, 0);
      break;
  }
  r_.write(mmHDMI_CONTROL + off, tmp);

  tmp = r_.read(mmHDMI_VBI_PACKET_CONTROL + off);
  tmp = REG_SET_FIELD(tmp, HDMI_VBI_PACKET_CONTROL, HDMI_NULL_SEND, 1);
  tmp = REG_SET_FIELD(tmp, HDMI_VBI_PACKET_CONTROL, HDMI_GC_SEND, 1);        // send general control packets
  tmp = REG_SET_FIELD(tmp, HDMI_VBI_PACKET_CONTROL, HDMI_GC_CONT, 1);        // every frame
  r_.write(mmHDMI_VBI_PACKET_CONTROL + off, tmp);

  tmp = r_.read(mmHDMI_INFOFRAME_CONTROL0 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_SEND, 1);   // enable audio info frames (frames won't be sent until audio is enabled)
  tmp = REG_SET_FIELD(tmp, HDMI_INFOFRAME_CONTROL0, HDMI_AUDIO_INFO_CONT, 1);   // required for audio info values to be updated
  r_.write(mmHDMI_INFOFRAME_CONTROL0 + off, tmp);

  tmp = r_.read(mmAFMT_INFOFRAME_CONTROL0 + off);
  tmp = REG_SET_FIELD(tmp, AFMT_INFOFRAME_CONTROL0, AFMT_AUDIO_INFO_UPDATE, 1);
  r_.write(mmAFMT_INFOFRAME_CONTROL0 + off, tmp);

  tmp = r_.read(mmHDMI_INFOFRAME_CONTROL1 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_INFOFRAME_CONTROL1, HDMI_AUDIO_INFO_LINE, kInfoframeLine);     // anything other than 0
  r_.write(mmHDMI_INFOFRAME_CONTROL1 + off, tmp);

  r_.write(mmHDMI_GC + off, 0);          // unset HDMI_GC_AVMUTE

  tmp = r_.read(mmHDMI_AUDIO_PACKET_CONTROL + off);
  tmp = REG_SET_FIELD(tmp, HDMI_AUDIO_PACKET_CONTROL, HDMI_AUDIO_DELAY_EN, 1);                  // the default audio delay
  tmp = REG_SET_FIELD(tmp, HDMI_AUDIO_PACKET_CONTROL, HDMI_AUDIO_PACKETS_PER_LINE, kAudioPacketsPerLine);
  r_.write(mmHDMI_AUDIO_PACKET_CONTROL + off, tmp);

  tmp = r_.read(mmAFMT_AUDIO_PACKET_CONTROL + off);
  tmp = REG_SET_FIELD(tmp, AFMT_AUDIO_PACKET_CONTROL, AFMT_60958_CS_UPDATE, 1);          // allow 60958 channel status fields to be updated
  r_.write(mmAFMT_AUDIO_PACKET_CONTROL + off, tmp);

  tmp = r_.read(mmHDMI_ACR_PACKET_CONTROL + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_PACKET_CONTROL, HDMI_ACR_SOURCE, bpc > 8 ? 0 : 1);   // the software CTS value, but not with deep colour
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_PACKET_CONTROL, HDMI_ACR_AUTO_SEND, 1);               // allow the hardware to send ACR packets when required
  r_.write(mmHDMI_ACR_PACKET_CONTROL + off, tmp);

  // dce_v11_0_afmt_update_ACR
  const Hdmi::Acr acr = Hdmi::acr(clock_khz);
  tmp = r_.read(mmHDMI_ACR_32_0 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_32_0, HDMI_ACR_CTS_32, acr.cts_32khz);
  r_.write(mmHDMI_ACR_32_0 + off, tmp);
  tmp = r_.read(mmHDMI_ACR_32_1 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_32_1, HDMI_ACR_N_32, acr.n_32khz);
  r_.write(mmHDMI_ACR_32_1 + off, tmp);
  tmp = r_.read(mmHDMI_ACR_44_0 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_44_0, HDMI_ACR_CTS_44, acr.cts_44_1khz);
  r_.write(mmHDMI_ACR_44_0 + off, tmp);
  tmp = r_.read(mmHDMI_ACR_44_1 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_44_1, HDMI_ACR_N_44, acr.n_44_1khz);
  r_.write(mmHDMI_ACR_44_1 + off, tmp);
  tmp = r_.read(mmHDMI_ACR_48_0 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_48_0, HDMI_ACR_CTS_48, acr.cts_48khz);
  r_.write(mmHDMI_ACR_48_0 + off, tmp);
  tmp = r_.read(mmHDMI_ACR_48_1 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_ACR_48_1, HDMI_ACR_N_48, acr.n_48khz);
  r_.write(mmHDMI_ACR_48_1 + off, tmp);

  // the channel status: channel numbers 1 to 8
  tmp = r_.read(mmAFMT_60958_0 + off);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_0, AFMT_60958_CS_CHANNEL_NUMBER_L, 1);
  r_.write(mmAFMT_60958_0 + off, tmp);
  tmp = r_.read(mmAFMT_60958_1 + off);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_1, AFMT_60958_CS_CHANNEL_NUMBER_R, 2);
  r_.write(mmAFMT_60958_1 + off, tmp);
  tmp = r_.read(mmAFMT_60958_2 + off);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_2, 3);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_3, 4);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_4, 5);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_5, 6);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_6, 7);
  tmp = REG_SET_FIELD(tmp, AFMT_60958_2, AFMT_60958_CS_CHANNEL_NUMBER_7, 8);
  r_.write(mmAFMT_60958_2 + off, tmp);
  return true;
}

bool AudioFormat::enable_channels(uint32_t block) {
  if (block >= kBlocks) return false;
  r_.write(mmAFMT_AUDIO_PACKET_CONTROL2 + kDigOffsets[block], 0xffu << AFMT_AUDIO_PACKET_CONTROL2__AFMT_AUDIO_CHANNEL_ENABLE__SHIFT);
  return true;
}

bool AudioFormat::start(uint32_t block, ConstByteView avi) {
  if (block >= kBlocks || avi.size() < Hdmi::kAviBytes) return false;
  const uint32_t off = kDigOffsets[block];
  // dce_v11_0_afmt_update_avi_infoframe: the frame without its header from the checksum on (avi[3], the data bytes after it), the version from the header
  const auto word = [&](uint32_t first) { return uint32_t{avi[first]} | uint32_t{avi[first + 1]} << 8 | uint32_t{avi[first + 2]} << 16 | uint32_t{avi[first + 3]} << 24; };
  r_.write(mmAFMT_AVI_INFO0 + off, word(3));
  r_.write(mmAFMT_AVI_INFO1 + off, word(7));
  r_.write(mmAFMT_AVI_INFO2 + off, word(11));
  r_.write(mmAFMT_AVI_INFO3 + off, uint32_t{avi[15]} | uint32_t{avi[16]} << 8 | uint32_t{avi[1]} << 24);

  uint32_t tmp = r_.read(mmHDMI_INFOFRAME_CONTROL0 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_INFOFRAME_CONTROL0, HDMI_AVI_INFO_SEND, 1);     // enable AVI info frames
  tmp = REG_SET_FIELD(tmp, HDMI_INFOFRAME_CONTROL0, HDMI_AVI_INFO_CONT, 1);     // required for audio info values to be updated
  r_.write(mmHDMI_INFOFRAME_CONTROL0 + off, tmp);
  tmp = r_.read(mmHDMI_INFOFRAME_CONTROL1 + off);
  tmp = REG_SET_FIELD(tmp, HDMI_INFOFRAME_CONTROL1, HDMI_AVI_INFO_LINE, kInfoframeLine);
  r_.write(mmHDMI_INFOFRAME_CONTROL1 + off, tmp);
  tmp = r_.read(mmAFMT_AUDIO_PACKET_CONTROL + off);
  tmp = REG_SET_FIELD(tmp, AFMT_AUDIO_PACKET_CONTROL, AFMT_AUDIO_SAMPLE_SEND, 1);   // send audio packets
  r_.write(mmAFMT_AUDIO_PACKET_CONTROL + off, tmp);
  r_.write(mmAFMT_RAMP_CONTROL0 + off, kRamp0);
  r_.write(mmAFMT_RAMP_CONTROL1 + off, kRamp1);
  r_.write(mmAFMT_RAMP_CONTROL2 + off, kRamp2);
  r_.write(mmAFMT_RAMP_CONTROL3 + off, kRamp3);
  return true;
}

uint32_t AudioFormat::selected_pin(uint32_t block) {
  if (block >= kBlocks) return kNoPin;
  return FIELD(r_.read(mmAFMT_AUDIO_SRC_CONTROL + kDigOffsets[block]), AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT_MASK, AFMT_AUDIO_SRC_CONTROL__AFMT_AUDIO_SRC_SELECT__SHIFT);
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
