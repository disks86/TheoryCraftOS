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

// The display engine (DCE 12.0 / 12.1) of the discrete Vega GPUs - Vega 10 (Radeon RX Vega 56/64, Vega Frontier, Pro WX 9100), Vega 12 (Radeon Pro Vega 16/20 of the
// MacBook Pro, Pro V320) and Vega 20 (Radeon VII, Pro VII) - and the parts of the memory controller and the host data path the display needs, set up from scratch for
// AmdSecondaryGpu: the counterpart of AmdDce11 and AmdGmc8 for the SOC15 register layout, where every register is a segment base of its IP block (vega10_ip_offset.h; Vega 12
// uses Vega 10's table and Vega 20's is the same for the blocks used here) plus its number.
//
// Linux drives these GPUs with its display core (DC: dce120_resource.c, dce120_timing_generator.c, dce120_hwseq.c and the DCE 11 code they reuse - dce_mem_input.c,
// dce_transform.c, dce_opp.c, dce_link_encoder.c, dce110_hwseq.c), not with a legacy dce_v12_0.c (there is none), and the register sequences here are DC's:
//
//   - Memory: video memory where the POST put it (mmhub_v1_0_get_fb_location: MC_VM_FB_LOCATION_BASE/TOP in 16 MiB units; the size from the NBIO's RCC_CONFIG_MEMSIZE,
//     nbio_v6_1/v7_4_get_memsize), then what gmc_v9_0_hw_init and mmhub_v1_0_gart_enable do without a GART: the VGA aperture locked out, the AGP aperture off
//     (amdgpu_gmc_set_agp_default), the system aperture over video memory with the scratch page as its default address (mmhub_v1_0_init_system_aperture_regs), the L1 TLB
//     in its system access mode (mmhub_v1_0_init_tlb_regs), the HDP set up (hdp_v4_0_init_registers), the BIF framebuffer access on (nbio_v6_1_mc_access_enable), and the
//     display hub's own AGP aperture off for a card with only local memory (dce120_update_dchub, FRAME_BUFFER_MODE_LOCAL_ONLY; its FB location is the POST's).
//   - The pipe (Pipe): the graphics plane of DCP n (dce_mi_program_surface_config with GFX9 linear tiling, dce_mi_program_surface_flip_and_addr), the scaler in bypass with the
//     viewport and no overscan (dce_transform_set_scaler for 1:1), the line buffer's depth (dce_transform_set_pixel_storage_depth for 30 bpp: the denormalisation), the DMIF
//     buffers (dce_mi_allocate_dmif / _free_dmif), DC's "safe" watermarks (dce110_set_safe_displaymarks with dce120_mi_program_display_marks: DC programs these before its
//     bandwidth calculation, which is not ported), the CRTC's blank (dce120_tg_set_blank), the update mode before EnableCRTC (dce120_timing_generator_enable_crtc), the advanced
//     request (dce120_timing_generator_enable_advanced_request), the VGA mode off (dce120_timing_generator_disable_vga), the FMT block's truncation (dce110_opp_program_fmt's
//     bit depth reduction), the DIG front end on the CRTC and the back end on the front end (dig_connect_to_otg, dce110_link_encoder_connect_dig_be_to_fe), the back end's hot
//     plug pin (hpd_initialize) and the hot plug pins on.
//   - The video BIOS's command tables (CommandTables): ATOM firmware (atomfirmware.h, command table format 2.1), with the parameters bios_parser2 / command_table2.c builds:
//     SetPixelClock 1.7, SetCRTC_UsingDTDTiming 3, EnableCRTC 1, DIG1TransmitterControl 1.6, DIG1EncoderControl 1.5 (whose stream setup, panel mode and link training
//     commands DC does with the DIG registers instead; the table's actions are the ones atomfirmware.h defines for 1.5). Power gating (EnableDispPowerGating) is not called:
//     dce120_enable_display_power_gating is disabled "for bringup" and returns false.
//
// Register numbers and masks are generated from AMD's headers (AmdDce12Registers.inc: dce_12_0, mmhub_1_0, nbio_6_1, hdp_4_0). Vega 20's NBIO is 7.4; the registers used
// here (RCC_CONFIG_MEMSIZE, BIF_FB_EN, the HDP flush) have the same numbers in nbio_7_4_offset.h. Vega 20 may have pipes fused off (CC_DC_PIPE_DIS, read_pipe_fuses).
//
// Not done: DC's bandwidth calculation (dce_calcs.c: the watermarks stay at the safe values), the display clock (SetDCEClock), the stream encoder's registers (DC's
// dp/hdmi_set_stream_attribute: the encoder table's stream setup is used), the AUX engine (the video BIOS's ProcessAuxChannelTransaction table is used, AmdAux), audio,
// infoframes, the colour pipeline beyond its reset values (input CSC, gamma, output CSC), GFX9 tiled surfaces, several pipes. Not run on a real card.
#pragma once
#include <stdint.h>
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdModeSet.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/DpLinkTrain.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"

namespace hardware {
namespace amd_dce12 {

constexpr uint32_t kMaxPipes = 6, kMaxDigs = 7, kMaxHpd = 6, kMaxDdc = 6;
constexpr uint64_t kSurfaceAlignment = 256;          // GRPH_PRIMARY_SURFACE_ADDRESS holds bits 31:8
constexpr uint32_t kDefaultTimeoutUs = 100000;
constexpr uint64_t kRegisterBytes = 512 * 1024;      // the register BAR (BAR 5) of a SOC15 GPU

// SOC15 segment bases (vega10_ip_offset.h, the same in vega20_ip_offset.h): register = base[BASE_IDX] + number.
constexpr uint32_t kDceBase[3] = {0x00000012, 0x000000C0, 0x000034C0};
constexpr uint32_t kMmhubBase[1] = {0x0001A000};
constexpr uint32_t kNbioBase[4] = {0x00000000, 0x00000014, 0x00000D20, 0x00010400};
constexpr uint32_t kHdpBase[1] = {0x00000F20};

// What the memory controller says about video memory once the POST has run.
struct MemoryMap {
  bool read = false;
  uint64_t video_memory_bytes = 0;   // RCC_CONFIG_MEMSIZE (MiB) as bytes
  uint64_t gpu_base = 0;             // MC_VM_FB_LOCATION_BASE << 24: the GPU's (MC) address of the first byte
  uint64_t gpu_top = 0;              // MC_VM_FB_LOCATION_TOP << 24: the start of the last 16 MiB the controller covers
  uint64_t fb_offset = 0;            // MC_VM_FB_OFFSET << 24: the physical offset of video memory (amdgpu_gmc_vram_mc2pa)
};
MemoryMap read_memory_map(IVcnRegisters& registers);

// The memory set-up the display needs (gmc_v9_0_hw_init / mmhub_v1_0_gart_enable without the GART, see above). `scratch_gpu_address`: the page unmapped system aperture
// accesses go to (inside video memory, 4 KiB aligned). False (nothing written) for a map that was not read or a scratch page outside it.
[[nodiscard]] bool program_memory(IVcnRegisters& registers, const MemoryMap& map, uint64_t scratch_gpu_address);
// The host data path flushed (the HDP memory coherency flush register of the NBIO, which hdp_v4_0_flush_hdp reaches through its remapped copy), and read back to post it.
void flush_hdp(IVcnRegisters& registers);
// read_pipe_fuses (Vega 20): bit n set means pipe n is fused off.
uint32_t pipe_fuses(IVcnRegisters& registers);

// The GPIO pads of DDC line `line` (0..5: DC_GPIO_DDC1..6) as a software I2C bus for AmdDdc: the clock and data bits of the line's MASK, A, EN and Y registers. Invalid for
// another line.
AtomI2c::Bus ddc_bus(uint32_t line);
// The DDC line whose A register is `a_register` (an absolute register number, as the video BIOS's GPIO pin table gives it), or kMaxDdc.
uint32_t ddc_line_of(uint32_t a_register);
// DC_GPIO_HPD_A, absolute: the register the GPIO pin table names for the hot plug pins
uint32_t hpd_gpio_register();

// The registers of one display pipe (DCP, SCL, LB, CRTC, FMT, DMIF) and the DIG and HPD blocks.
class Pipe {
 public:
  Pipe(IVcnRegisters& registers, const IClock& clock) : r_(registers), clock_(clock) {}

  // dce120_tg_set_blank: blank data double buffered, then blanked (or the blank control cleared).
  bool set_blank(uint32_t crtc, bool blank);
  bool blanked(uint32_t crtc);
  // dce120_timing_generator_enable_crtc's register part (MASTER_UPDATE_MODE 0, UNDERFLOW_UPDATE_LOCK 0), done before EnableCRTC.
  bool prepare_enable(uint32_t crtc);
  // dce120_timing_generator_enable_advanced_request(enable): the legacy requestor off and the advanced start line at the vertical sync and back porch (at most 10 lines).
  bool advanced_request(uint32_t crtc, const CrtcMode& mode);
  // dce120_timing_generator_disable_vga
  bool disable_vga(uint32_t crtc);
  // The FMT block for `bpc` bits per colour: truncation to 6, 8 or 10 bits; nothing for other depths (the register is written zero).
  bool program_fmt(uint32_t crtc, uint32_t bpc);
  // The graphics plane at `surface` (linear ARGB8888, GFX9 swizzle mode 0) with its flip, the scaler bypassed with the viewport, the line buffer's depth. False for a pipe that
  // does not exist or a surface that is misaligned, empty, or has a pitch below its width or a size beyond the 14-bit viewport.
  bool set_surface(uint32_t crtc, const amd_dce11::ScanoutSurface& surface);
  // The plane off (GRPH_ENABLE 0).
  bool disable_surface(uint32_t crtc);
  // GRPH_UPDATE.GRPH_SURFACE_UPDATE_PENDING
  bool flip_pending(uint32_t crtc);
  // dce_mi_allocate_dmif: two DMIF buffers, the allocation waited for (false if it never completes), the pixel duration. dce_mi_free_dmif: none.
  bool allocate_dmif(uint32_t crtc, const CrtcMode& mode, uint32_t timeout_us = kDefaultTimeoutUs);
  bool free_dmif(uint32_t crtc, uint32_t timeout_us = kDefaultTimeoutUs);
  // dce110_set_safe_displaymarks through dce120_mi_program_display_marks: urgency, urgent level and stutter exit 0xFFFF, stutter entry 0, P-state change 0x7FFF for the
  // four watermark sets, stutter enabled (DC's DCE 12 default) ignoring the frame buffer compressor.
  bool safe_watermarks(uint32_t crtc);
  // dig_connect_to_otg and dce110_link_encoder_connect_dig_be_to_fe: front end `dig` takes CRTC `crtc`, back end `dig` takes front end `dig` (connect) or lets it go.
  bool connect(uint32_t dig, uint32_t crtc, bool connect);
  // hpd_initialize: the back end's hot plug select (0: none, n: HPD pin n - 1)
  bool set_hpd_select(uint32_t dig, uint32_t hpd_source);
  // A hot plug pin on (DC_HPD_EN) or off. False for a pin that does not exist.
  bool hpd_enable(uint32_t pin, bool enable);

  // The register numbers, for the tests
  static uint32_t grph_enable(uint32_t crtc);
  static uint32_t crtc_blank_control(uint32_t crtc);

 private:
  IVcnRegisters& r_;
  const IClock& clock_;
};

// The ATOM firmware command tables a DCE 12 mode set calls, with command_table2.c's parameters (atomfirmware.h layouts). Each is false if the table is not there, has
// another revision, or fails.
class CommandTables {
 public:
  // GetIndexIntoMasterTable(COMMAND, ...) in atom_master_list_of_command_functions_v2_1
  static constexpr uint32_t kDigEncoderControl = 4, kSetPixelClock = 12, kEnableCrtc = 35, kSetCrtcUsingDtdTiming = 49, kTransmitterControl = 76;
  static constexpr uint8_t kPixelClockForcePll = 0x01, kPixelClockDualLink = 0x08;     // PIXEL_CLOCK_V7_MISC_FORCE_PROG_PPLL, _DVI_DUALLINK_EN

  explicit CommandTables(AtomBios& bios) : bios_(bios) {}

  // set_pixel_clock_v7 (set_pixel_clock_parameter_v1_7): `crtc` ATOM_CRTC1.. or ATOM_CRTC_INVALID, `pll` ATOM_COMBOPHY_PLLx / ATOM_DP_DTO, the pixel clock in 100 Hz
  // (0 switches the PLL off), the deep colour ratio 0 (DC leaves it so).
  bool set_pixel_clock(uint8_t crtc, uint8_t pll, uint8_t encoder_object_id, uint8_t encoder_mode, uint32_t pixel_clock_100hz, uint8_t misc);
  // set_crtc_using_dtd_timing_v3 (set_crtc_using_dtd_timing_parameters, no borders; the encoder mode byte is left 0 as DC leaves it). Progressive modes only.
  bool set_crtc_timing(uint8_t crtc, const CrtcMode& mode);
  // enable_crtc_v1
  bool enable_crtc(uint8_t crtc, bool enable);
  // transmitter_control_v1_6 (dig_transmitter_control_parameters_v1_6): DC gives the DIG front end select as 0 (it connects the front end itself).
  struct Transmitter {
    uint8_t phy = 0;            // ATOM_PHY_ID_UNIPHYA..G: 0..6
    uint8_t action = 0;         // ATOM_TRANSMITTER_ACTION_*
    uint8_t mode_or_lane_set = 0;   // the DIG mode (ATOM_TRANSMITTER_DIGMODE_V6_*), or the DPCD lane set for SETUP_VSEMPH
    uint8_t lanes = 0;
    uint32_t symbol_clock_khz = 0;  // the pixel clock (TMDS) or the link rate (DisplayPort), in kHz
    uint8_t hpd_select = 0;     // 1..6, 0 none
    uint8_t connector_object_id = 0;
  };
  bool transmitter(const Transmitter& t);
  // encoder_control_digx_v1_5: the stream setup (dig_encoder_stream_setup_parameters_v1_5), the panel mode (dp_panel_mode_set_parameters_v1_5), the other commands
  // (dig_encoder_generic_cmd_parameters_v1_5).
  bool encoder_stream_setup(uint8_t dig, uint8_t encoder_mode, uint8_t lanes, uint32_t pixel_clock_khz, uint8_t panel_bpc, uint32_t link_rate_khz);
  bool encoder_panel_mode(uint8_t dig, uint8_t panel_mode);
  bool encoder_command(uint8_t dig, uint8_t action);

 private:
  bool revision(uint32_t table, uint8_t want_crev);
  bool run(uint32_t table, uint32_t* words, uint32_t count);
  AtomBios& bios_;
};

// The source side of DisplayPort link training on DCE 12 through the ATOM firmware tables: the encoder's training commands (1.5) and the transmitter's voltage swing and
// pre-emphasis (1.6, once per lane as dce110_link_encoder_dp_set_lane_settings calls it).
class DpSource final : public IDpSource {
 public:
  DpSource(IAtomHost& host, CommandTables& tables, uint8_t dig, const CommandTables::Transmitter& base) : host_(host), tables_(tables), dig_(dig), base_(base) {}
  bool start_training() override { return tables_.encoder_command(dig_, kCmdStart); }
  bool set_pattern(uint8_t pattern) override;
  bool complete_training() override { return tables_.encoder_command(dig_, kCmdComplete); }
  bool set_drive(uint8_t lane_set) override;
  void delay_us(uint32_t microseconds) override { host_.delay_us(microseconds); }

 private:
  static constexpr uint8_t kCmdStart = 0x08, kCmdComplete = 0x0B;    // ATOM_ENCODER_CMD_DP_LINK_TRAINING_START / _COMPLETE
  IAtomHost& host_;
  CommandTables& tables_;
  uint8_t dig_;
  CommandTables::Transmitter base_;
};

}  // namespace amd_dce12
}  // namespace hardware
