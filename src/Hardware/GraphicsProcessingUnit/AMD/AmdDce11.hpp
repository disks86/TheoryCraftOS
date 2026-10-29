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


// The primary plane of the display engine (DCE 11.2) of a Radeon RX Vega M, for one thing: pointing it at another framebuffer. This is the dGPU counterpart of
// amd_dcn1::Plane (AmdDcn1.hpp) for the older display block of the Polaris family: the firmware has lit the screen (the card's video BIOS ran at POST and the UEFI graphics
// output is on it), and everything about how the picture is made and shown (the timings, the PLLs, the encoders and the link) is left exactly as the firmware set it. Only
// the graphics surface's address is written, the way amdgpu's dce_v11_0_page_flip does it (the high half, then the low half, which triggers the update), and the engine's
// answer (GRPH_UPDATE: is the update still pending) is read back.
//
// Setting a mode of its own would need the video BIOS's command tables (the PLL, the DIG encoder and the transmitter are programmed through AtomBIOS in the driver): there is
// no interpreter for them here, so there is no mode setting on this card, only this. It does not touch a CRTC it does not recognise: probe() looks for the one that shows the
// firmware's framebuffer (same GPU address, same pitch, 32-bit ARGB8888, linear, no byte swapping) and refuses anything else. The new address has to lie inside the system
// aperture the memory controller was given (AmdGmc8), where the display reads physical video memory without translation. If the engine does not take the address, the old one
// is put back.
//
// Register numbers are generated from AMD's own headers (AmdDce11Registers.inc); the sequence is ported from the Linux amdgpu driver (dce_v11_0.c, from kernel 6.12: the legacy
// display code was later removed in favour of the display core). Not run on a real card: the host tests drive it against a simulated register file.
#pragma once
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/EdidAudio.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/HdmiInfoframe.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Allocators/IAllocator.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {
namespace amd_dce11 {

constexpr uint32_t kMaxCrtcs = 6;                  // the Polaris 10 and the VEGAM; the Polaris 11 and 12 have 5
constexpr uint64_t kSurfaceAlignment = 256;        // GRPH_PRIMARY_SURFACE_ADDRESS holds bits 31:8
constexpr uint32_t kDefaultTimeoutUs = 100000;     // far more than a frame

class Plane {
 public:
  Plane(IVcnRegisters& registers, const IClock& clock) : r_(registers), clock_(clock) {}

  // Looks for the CRTC whose graphics surface is `surface` (the GPU's address of the firmware's framebuffer) with `pitch_pixels` between rows. False if none matches, or
  // the one that does is not on, linear, ARGB8888 and unswapped.
  bool probe(uint64_t surface, uint32_t pitch_pixels);
  bool active() const { return active_; }
  uint32_t crtc() const { return crtc_; }
  uint64_t original_surface() const { return original_; }
  // The system aperture as the registers hold it: byte addresses, both ends inclusive (low is the first byte, high the last of the last page).
  uint64_t aperture_low() const { return aperture_low_; }
  uint64_t aperture_high() const { return aperture_high_; }
  bool in_aperture(uint64_t address, uint64_t bytes) const;
  // The address the graphics surface fetches now (the register, not what was asked).
  uint64_t surface() const;

  // Points the surface at `address` (aligned, `bytes` of picture inside the aperture) with an immediate flip. Waits until the engine no longer has an update pending; if it
  // never takes it, the previous address is put back and false returned.
  bool set_surface(uint64_t address, uint64_t bytes, uint32_t timeout_us = kDefaultTimeoutUs);
  // Back to the firmware's framebuffer.
  bool restore(uint32_t timeout_us = kDefaultTimeoutUs);

 private:
  uint32_t offset() const;
  bool flip(uint64_t address, uint32_t timeout_us);

  IVcnRegisters& r_;
  const IClock& clock_;
  bool active_ = false;
  uint32_t crtc_ = 0;
  uint64_t original_ = 0;
  uint32_t pitch_ = 0;
  uint64_t aperture_low_ = 0, aperture_high_ = 0;
};

// The pixel formats of dce_v11_0_crtc_do_set_base (DRM names, little endian; the alpha of the X formats is ignored by the display).
enum class ScanoutFormat : uint8_t { Argb8888, Abgr8888, Rgb565, Argb1555, Argb4444, Bgra5551, Argb2101010, Bgra1010102, Indexed8 };

// The surface layout of a tiled picture (the tiling flags of the buffer): GRPH_ARRAY_MODE and for the 2D mode the bank and tile parameters; the pipe configuration is for both.
struct ScanoutTiling {
  enum class Mode : uint8_t { Linear = 0, Tiled1D = 2, Tiled2D = 4 };       // ARRAY_LINEAR_GENERAL, ARRAY_1D_TILED_THIN1, ARRAY_2D_TILED_THIN1
  Mode mode = Mode::Linear;
  uint8_t pipe_config = 0;         // 5 bits
  uint8_t num_banks = 0;           // 2 bits (2D only, as the fields below)
  uint8_t bank_width = 0;          // 2 bits
  uint8_t bank_height = 0;         // 2 bits
  uint8_t macro_tile_aspect = 0;   // 2 bits
  uint8_t tile_split = 0;          // 3 bits
};

// A picture for a CRTC's graphics plane to scan out.
struct ScanoutSurface {
  uint64_t address = 0;            // the GPU's address of the first pixel (256 byte aligned)
  uint32_t width = 0, height = 0;  // in pixels
  uint32_t pitch_pixels = 0;       // pixels from one row to the next (at least width)
  uint32_t viewport_width = 0;     // the mode's horizontal display
  uint32_t viewport_height = 0;    // the mode's vertical display
  uint32_t x = 0, y = 0;           // the top left of the viewport in the picture (x rounded down to a multiple of 4, y to even)
  ScanoutFormat format = ScanoutFormat::Argb8888;
  ScanoutTiling tiling;
};

// One look-up table entry: 16 bit red, green and blue (only the top 10 bits are used).
struct LutEntry {
  uint16_t red = 0, green = 0, blue = 0;
};

// dce_v11_0_crtc_do_set_base for a surface at (0, 0): the graphics plane of a CRTC pointed at a picture, with the VGA mode off, the surface update at vertical blank, the pixel format and
// tiling in the control register, the look-up table bypassed for 10 bit formats, the window and viewport and the plane enabled; and dce_v11_0_crtc_load_lut. Not done: the bandwidth update
// (dce_v11_0_bandwidth_update), the cursor.
class Scanout {
 public:
  explicit Scanout(IVcnRegisters& registers) : r_(registers) {}
  // False if the CRTC does not exist or the surface is misaligned, empty, has a pitch below its width or a tiling field out of range.
  bool set(uint32_t crtc, const ScanoutSurface& surface);
  // The graphics plane off (dce_v11_0_grph_enable(false)).
  bool disable(uint32_t crtc);
  // dce_v11_0_crtc_load_lut: the colour pipeline set to pass the plane through the table (no input CSC, no prescale, no degamma, gamut remap or regamma, no output CSC, cursor alpha
  // blending on) and the 256 entries written, 10 bits per colour (entry 0 first). With a null table the identity ramp.
  bool load_lut(uint32_t crtc, const LutEntry* table256);
  // dce_v11_0_vga_enable: the CRTC's VGA mode on or off (the driver turns it on around the unblank and off after).
  bool vga_enable(uint32_t crtc, bool enable);
  // dce_v11_0_set_interleave: LB_DATA_FORMAT.INTERLEAVE_EN for an interlaced mode.
  bool set_interleave(uint32_t crtc, bool interlaced);
  // dce_v11_0_program_fmt: the FMT block's truncation or dither for `bpc` bits per colour (6, 8, 10; other depths are not touched, as in the driver). `dither`: spatial dither with the random
  // generators instead of truncation. False for a CRTC that does not exist.
  bool program_fmt(uint32_t crtc, uint32_t bpc, bool dither);

 private:
  IVcnRegisters& r_;
};

// One display controller as the watermark calculation sees it (the CRTC's mode, drm's crtc_* values).
struct WatermarkHead {
  bool enabled = false;
  uint32_t hdisplay = 0, htotal = 0, clock_khz = 0;
  bool interlaced = false;
};

// The engine and memory clocks the watermarks are computed for, in kHz: at the highest and the lowest power state (the same while there is no dynamic power management: the current ones).
struct WatermarkClocks {
  uint32_t sclk_high_khz = 0, sclk_low_khz = 0, mclk_high_khz = 0, mclk_low_khz = 0;
  bool apu = false;
};

// dce_v11_0_bandwidth_update: the line buffer split between the display controllers (dce_v11_0_line_buffer_adjust: memory configuration by display width, DMIF buffers, waiting for the
// allocation) and the urgency watermarks of each (dce_v11_0_program_watermarks, with the driver's 20.12 fixed point arithmetic): wm A for the high clocks, wm B for the low, the high
// watermark the line time. Pictures are taken as 4 bytes per pixel, unscaled (vertical scale ratio 1, one vertical tap), as the driver does without a scaler.
class Watermarks {
 public:
  Watermarks(IVcnRegisters& registers, const IClock& clock) : r_(registers), clock_(clock) {}
  // `heads`: kMaxCrtcs entries. Returns false only if a DMIF allocation never completed (the watermarks are programmed all the same).
  bool update(const WatermarkHead* heads, const WatermarkClocks& clocks);
  // The values for the last update, as the driver saves them for power management.
  struct Result {
    uint32_t line_time = 0, wm_high = 0, wm_low = 0, lb_vblank_lead_lines = 0;
  };
  const Result& result(uint32_t crtc) const { return results_[crtc]; }
  // The number of DRAM channels from MC_SHARED_CHMAP.NOOFCHAN (cik_get_number_of_dram_channels)
  static uint32_t dram_channels(uint32_t chmap);
  // The pieces of the calculation, in the driver's units (MB/s, ns); exposed for testing.
  struct Params {
    uint32_t dram_channels = 1, yclk = 0, sclk = 0, disp_clk = 0, src_width = 0, active_time = 0, blank_time = 0, vsc_full = 1u << 12, num_heads = 1, bytes_per_pixel = 4, lb_size = 0, vtaps = 1;
    bool interlaced = false;
  };
  static uint32_t latency_watermark(const Params& wm);
  static uint32_t available_bandwidth(const Params& wm);
  static uint32_t average_bandwidth(const Params& wm);
  static uint32_t dram_bandwidth(const Params& wm);

 private:
  uint32_t line_buffer_adjust(uint32_t crtc, const WatermarkHead& head, const WatermarkClocks& clocks, bool* complete);
  void program(uint32_t crtc, const WatermarkHead& head, const WatermarkClocks& clocks, uint32_t lb_size, uint32_t num_heads);

  IVcnRegisters& r_;
  const IClock& clock_;
  Result results_[kMaxCrtcs];
};

// The firmware's framebuffer as the UEFI graphics output describes the current mode.
struct FirmwareFramebuffer {
  uint64_t physical = 0;     // the CPU's address of its first byte (the OS runs identity mapped)
  uint64_t bytes = 0;        // the whole allocation
  uint32_t width = 0, height = 0;
  uint32_t pitch_pixels = 0;
};

// What AmdGpu needs of a display engine that can show the OS's own framebuffer in place of the firmware's (the Vega M's, see AmdVegaM): take the screen over for the firmware's
// current mode, hand the memory to draw in back, and give the screen back before the firmware changes the mode.
class INativeDisplay {
 public:
  virtual bool engage_display(const FirmwareFramebuffer& firmware, ByteView* memory) = 0;
  virtual bool display_engaged() const = 0;
  virtual void disengage_display() = 0;
  // Copies `pixels` (a run of whole rows of the picture, in system memory) to `offset` bytes into the picture in video memory by the card's DMA engines. False if the card
  // cannot (no engine, the copy failed): the caller then copies by the CPU.
  virtual bool upload_picture(ConstByteView pixels, uint64_t offset) { (void)pixels; (void)offset; return false; }

 protected:
  INativeDisplay() = default;
  ~INativeDisplay() = default;
};

// The OS's own framebuffer in video memory and the display pointed at it, the counterpart of amd_dcn1::Scanout for a card with its own memory: the picture is copied into a
// window of video memory (`region`, mapped for the CPU through the memory BAR, `region_gpu` its GPU address), the surface address is switched, and `memory` is what to draw
// in. The firmware's framebuffer has to be in video memory the memory BAR shows (the card drives the firmware's display): its GPU address is `gpu_base` plus its distance
// from `cpu_base`, the start of the memory BAR. Anything that does not fit (another GPU's display, a mode the plane is not in, no room, the engine not taking the address)
// leaves the firmware's framebuffer on the screen. The window is uncached memory (a mapped BAR), so there is nothing to flush.
class Display {
 public:
  Display(IVcnRegisters& registers, const IClock& clock, uint64_t cpu_base, uint64_t gpu_base, const MmioRegion& region, uint64_t region_gpu)
      : plane_(registers, clock), cpu_base_(cpu_base), gpu_base_(gpu_base), region_(region), region_gpu_(region_gpu) {}
  ~Display() { disengage(); }
  Display(const Display&) = delete;
  Display& operator=(const Display&) = delete;

  bool engage(const FirmwareFramebuffer& firmware, ByteView* memory);
  bool engaged() const { return engaged_; }
  // Gives the screen back to the firmware's framebuffer, with what was drawn since copied back into it.
  void disengage();
  const Plane& plane() const { return plane_; }

 private:
  Plane plane_;
  uint64_t cpu_base_, gpu_base_;
  MmioRegion region_;
  uint64_t region_gpu_;
  bool engaged_ = false;
  FirmwareFramebuffer firmware_;
  ByteView memory_;
};

// The display engine's interrupt sources, as dce_v11_0.c controls them: the page flip of a controller's graphics surface (dce_v11_0_set_pageflip_irq_state), its vertical blank
// (dce_v11_0_set_crtc_vblank_interrupt_state), and the hot plug detect pins (dce_v11_0_hpd_init / _hpd_fini / _set_hpd_irq_state / _hpd_set_polarity), with the acknowledgements the
// driver's handlers make (dce_v11_0_crtc_vblank_int_ack, the pageflip handler's clear, dce_v11_0_hpd_int_ack). The pins and controllers are numbered from 0; which pin a connector
// uses is in the video BIOS, which is not read here, so the pins are handled all alike.
class Interrupts {
 public:
  static constexpr uint32_t kMaxHpd = 6;
  // dce_v11_0_hpd_init's filter: AMDGPU_HPD_CONNECT_INT_DELAY_IN_MS and AMDGPU_HPD_DISCONNECT_INT_DELAY_IN_MS (amdgpu_mode.h)
  static constexpr uint32_t kHpdConnectDelayMs = 50, kHpdDisconnectDelayMs = 10;

  explicit Interrupts(IVcnRegisters& registers) : r_(registers) {}

  void set_pageflip(uint32_t crtc, bool enable);   // GRPH_INTERRUPT_CONTROL.GRPH_PFLIP_INT_MASK
  void set_vblank(uint32_t crtc, bool enable);     // LB_INTERRUPT_MASK.VBLANK_INTERRUPT_MASK
  void set_vline(uint32_t crtc, bool enable);      // LB_INTERRUPT_MASK.VLINE_INTERRUPT_MASK (dce_v11_0_set_crtc_vline_interrupt_state)
  // A pin on: DC_HPD_EN, the toggle filter, the pending interrupt acknowledged, the polarity set to report the next change, then DC_HPD_INT_EN. Off: DC_HPD_EN and DC_HPD_INT_EN cleared.
  void hpd_init(uint32_t pin);
  void hpd_fini(uint32_t pin);
  void hpd_set_interrupt(uint32_t pin, bool enable);   // DC_HPD_INT_EN alone (dce_v11_0_set_hpd_irq_state)
  bool hpd_sense(uint32_t pin);                        // dce_v11_0_hpd_sense: something is connected
  // The short pulse of a DisplayPort sink (a status change that is not a plug: link loss, a request to retest): DC_HPD_INT_CONTROL.DC_HPD_RX_INT_EN, the pending state in
  // DC_HPD_INT_STATUS.DC_HPD_RX_INT_STATUS and DC_HPD_INT_CONTROL.DC_HPD_RX_INT_ACK. (The kernel's own amdgpu display code, dce_v11_0, does not use it; the registers are the
  // DC code's, see the documentation.) hpd_init does not switch it on, hpd_fini switches it off.
  void hpd_rx_set_interrupt(uint32_t pin, bool enable);
  bool hpd_rx_asserted(uint32_t pin);
  void ack_hpd_rx(uint32_t pin);
  void hpd_set_polarity(uint32_t pin);                 // interrupt on the next change from what is sensed now

  // What a handler does with a vector: whether the interrupt was asserted (DISP_INTERRUPT_STATUS*), and the acknowledgement. False if the controller or pin does not exist.
  bool vblank_asserted(uint32_t crtc);
  bool hpd_asserted(uint32_t pin);
  void ack_vblank(uint32_t crtc);
  void ack_vline(uint32_t crtc);
  bool vline_asserted(uint32_t crtc);
  bool ack_pageflip(uint32_t crtc);   // clears GRPH_PFLIP_INT_OCCURRED if it is set; true if it was
  void ack_hpd(uint32_t pin);

 private:
  IVcnRegisters& r_;
};

// The audio endpoints of the display engine (dce_v11_0_audio_endpt_rreg and dce_v11_0_audio_get_connected_pins): an indexed register pair per endpoint, and in it the pin's default
// configuration, whose port connectivity field says whether the endpoint has a physical connection (1 means none). The Vega M (CHIP_VEGAM, like Polaris 10) has 8 endpoints. Which output
// an endpoint serves is not fixed: the driver gives an encoder the first connected one it finds (dce_v11_0_audio_get_pin).
class AudioPins {
 public:
  static constexpr uint32_t kMaxPins = 8;
  static constexpr uint32_t kPortConnectivityNone = 1;
  static constexpr uint32_t kStereoSpeakers = 5;     // the speaker allocation the driver writes for a monitor that gives none
  explicit AudioPins(IVcnRegisters& registers) : r_(registers) {}
  uint32_t endpoint_read(uint32_t pin, uint32_t reg);   // 0 for a pin that does not exist
  uint32_t port_connectivity(uint32_t pin);              // the field of the default configuration (0 connector, 1 none, 2 fixed function, 3 both)
  bool connected(uint32_t pin);                          // port connectivity is not "none"
  uint32_t connected_mask(uint32_t pin_count = kMaxPins); // bit n: pin n

  // What the driver programs into an endpoint when a display is set up (dce_v11_0_afmt_setmode and the functions it calls). The hardware makes the ELD that the sound driver reads out of
  // these: there is no ELD buffer to fill. All of them are false for a pin that does not exist.
  // dce_v11_0_audio_endpt_wreg: the indexed register `reg` of the endpoint
  bool endpoint_write(uint32_t pin, uint32_t reg, uint32_t value);
  // dce_v11_0_audio_enable: HOT_PLUG_CONTROL is written as AUDIO_ENABLED or as zero (the whole register, as the driver does)
  bool set_enabled(uint32_t pin, bool enable);
  bool enabled(uint32_t pin);
  // dce_v11_0_audio_write_speaker_allocation: CHANNEL_SPEAKER with the DisplayPort connection bit cleared, the HDMI one set (always, as the driver does), and the speaker allocation: the monitor's
  // (EdidAudio::Info::speaker_allocation) when it has a speaker allocation data block, else kStereoSpeakers (5, stereo: the driver's default).
  bool write_speaker_allocation(uint32_t pin, const EdidAudio::Info& info);
  // dce_v11_0_audio_write_sad_regs: the descriptor registers for PCM, AC-3, MPEG-1, MP3, MPEG-2, AAC LC, DTS, ATRAC, E-AC-3, DTS-HD, MLP and WMA Pro, each from the monitor's Short Audio
  // Descriptors of that format (the one with the most channels; for PCM the sample rates of all of them also go to the stereo field). Nothing is written (false) for a monitor without any.
  bool write_sads(uint32_t pin, const EdidAudio::Info& info);
  // dce_v11_0_audio_write_latency_fields: the lip sync latencies of the mode (progressive or interlaced) if the monitor gives them, else zero.
  bool write_latency(uint32_t pin, const EdidAudio::Info& info, bool interlaced);

 private:
  IVcnRegisters& r_;
};

// The audio format blocks of the display engine (dce_v11_0_afmt_init, dce_v11_0_afmt_audio_select_pin): DCE 11 has one per DIG encoder, at the DIG's register block, and each takes its
// audio from one of the endpoints (AudioPins) - AFMT_AUDIO_SRC_CONTROL.AFMT_AUDIO_SRC_SELECT is that endpoint's number. The Vega M (CHIP_VEGAM, like Polaris 10) has 6 DIGs, so blocks 0 to 5:
// the transmitter UNIPHY3 (DIG 6) has no audio block there.
class AudioFormat {
 public:
  static constexpr uint32_t kBlocks = 6;
  explicit AudioFormat(IVcnRegisters& registers) : r_(registers) {}
  // The endpoint (0..AudioPins::kMaxPins - 1) block `block` takes its audio from: the field alone is written, the rest of the register is kept. False for a block or endpoint that does not exist.
  bool select_pin(uint32_t block, uint32_t pin);
  // The endpoint the block has selected, or kNoPin for a block that does not exist.
  static constexpr uint32_t kNoPin = 0xFFFFFFFFu;
  uint32_t selected_pin(uint32_t block);

  // dce_v11_0_afmt_setmode, the part before the endpoint's speaker allocation: the audio clock (dce_v11_0_audio_set_dto: DTO 0 takes the display controller `crtc`'s timing, phase 24 MHz over
  // the pixel clock `clock_khz`), the packets in the blanking (null packets, general control for deep colour by `bpc`: 8 and 6, 16 or none off, 10 and 12 on; audio info frames; the audio packet
  // delay, the 60958 channel status update, the audio clock regeneration packet with the N and CTS of Hdmi::acr, and the channel numbers of the channel status). False for a block or controller
  // that does not exist (nothing is written).
  bool setup_packets(uint32_t block, uint32_t crtc, uint32_t clock_khz, uint32_t bpc);
  // the part after it: all eight audio channels enabled (AFMT_AUDIO_PACKET_CONTROL2)
  bool enable_channels(uint32_t block);
  // and the last part, after the endpoint's selection and descriptors: the AVI InfoFrame (`avi`, Hdmi::avi_infoframe's kAviBytes bytes) into AFMT_AVI_INFO0..3 and sent every frame on line 2,
  // the audio samples sent, the ramp test registers the driver sets. False for a block that does not exist or a short frame.
  bool start(uint32_t block, ConstByteView avi);

 private:
  IVcnRegisters& r_;
};

// The OS's picture kept in system memory (cached, so reading and blending are fast; the mapped window of video memory is uncached and slow to read) and sent to the display's
// framebuffer in video memory a run of rows at a time: by the card's DMA engines when the display offers them, else by the CPU. `video` is the picture in video memory as the
// display gave it (Display::engage's `memory`).
class Presenter {
 public:
  explicit Presenter(allocators::IAllocator& allocator) : back_(allocator) {}
  // The back buffer is filled from what the display shows now. False if there is no memory or the sizes do not match.
  bool start(INativeDisplay& display, ByteView video, uint32_t pitch_bytes, uint32_t height);
  // What to draw in.
  ByteView buffer() { return back_.writable_view(); }
  // Sends rows [first_row, first_row + rows) of the back buffer to video memory. False if the rows are outside the picture.
  bool present(uint32_t first_row, uint32_t rows);
  bool present_all() { return present(0, height_); }
  uint32_t dma_presents() const { return dma_presents_; }
  uint32_t cpu_presents() const { return cpu_presents_; }

 private:
  ByteBuffer back_;
  INativeDisplay* display_ = nullptr;
  ByteView video_;
  uint32_t pitch_ = 0, height_ = 0, dma_presents_ = 0, cpu_presents_ = 0;
};

}  // namespace amd_dce11
}  // namespace hardware
