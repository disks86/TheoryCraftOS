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

// Follows the Linux amdgpu driver (Copyright Advanced Micro Devices, Inc.; MIT license) where it has a counterpart: amdgpu_device.c
// (amdgpu_device_need_post, amdgpu_device_asic_init), gmc_v8_0.c (gmc_v8_0_mc_program), dce_v11_0.c (dce_v11_0_hw_init, the crtc and encoder
// helpers' prepare / mode_set / commit / disable), amdgpu_atombios_encoders.c (amdgpu_atombios_encoder_init_dig, _setup_dig) and
// amdgpu_atombios_dp.c (amdgpu_atombios_dp_get_panel_mode, the link training). The DCE 12 path: amdgpu_device.c (amdgpu_device_asic_init for GC 9.0 - 9.4.0),
// amdgpu_atomfirmware.c (amdgpu_atomfirmware_scratch_regs_init) and the display core's dce110_hwseq.c (dce110_apply_ctx_to_hw, dce110_enable_stream_timing,
// dce110_program_front_end_for_pipe), link_dpms.c's enable order, dce_link_encoder.c and dce_clock_source.c (see AmdDce12.hpp for the register parts).
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSecondaryGpu.hpp"
#include "Hardware/Acpi/AcpiTables.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdAux.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDdc.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDpSource.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGpuGenerations.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdVbios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {
namespace amd_secondary {

namespace {
constexpr uint32_t kTmdsSingleLinkKhz = 165000;
constexpr uint32_t kFirmwareInfoTable = 4;                      // GetIndexIntoMasterTable(DATA, FirmwareInfo)
constexpr uint32_t kFwiDefaultSclk = 8, kFwiDefaultMclk = 12;   // ATOM_FIRMWARE_INFO: ulDefaultEngineClock, ulDefaultMemoryClock (10 kHz)
// atom_firmware_info_v3_1 (atomfirmware.h): bootup_sclk_in10khz, bootup_mclk_in10khz at the same offsets; bios_scratch_reg_startaddr after the capability and the parser entry
constexpr uint32_t kFwiV3ScratchStart = 0x18, kFwiV3MinBytes = 0x1C;
constexpr uint32_t kMaxRegisterDword = static_cast<uint32_t>(amd_dce12::kRegisterBytes / 4);
constexpr uint8_t kAtomCrtcInvalid = 0xFF, kEncoderModeCrt = 15;             // ATOM_CRTC_INVALID, ATOM_ENCODER_MODE_CRT (the mode of SIGNAL_TYPE_NONE)
constexpr uint8_t kHpdSourceUnknown = 7;                                    // HPD_SOURCEID_UNKNOWN (grph_object_defs.h)

uint64_t align_up(uint64_t value, uint64_t alignment) { return (value + alignment - 1) / alignment * alignment; }

bool same_size(const CrtcMode& a, const CrtcMode& b) { return a.h_display == b.h_display && a.v_display == b.v_display; }

ModeSetOutput output_of(const AtomConnectors::Connector& connector, uint8_t crtc, uint8_t encoder_mode, uint32_t bpc, uint8_t pll) {
  ModeSetOutput o;
  o.crtc = crtc;
  o.pll = pll;
  o.encoder_id = AtomConnectors::encoder_id(connector);
  o.dig_encoder = AtomConnectors::dig_encoder(connector);
  o.link_b = AtomConnectors::link_b(connector);
  o.connector_object_id = connector.object_id;
  o.hpd = connector.hpd_pin;
  o.encoder_mode = encoder_mode;
  o.dfp = encoder_mode != AmdModeSet::kModeLvds;
  o.bits_per_color = bpc;
  return o;
}

// amdgpu_atombios_dp_get_panel_mode for an external connector: a DP bridge chip whose DP_EDP_CONFIGURATION_CAP has the alternate scrambler reset
// bit is in the internal DP2 mode, the NUTMEG and TRAVIS bridges without it DP1, everything else an external DP.
uint8_t panel_mode_of(const AtomConnectors::Connector& connector, IDpcdAux& aux) {
  const uint8_t bridge = connector.bridge_object ? static_cast<uint8_t>(connector.bridge_object) : AtomConnectors::kEncoderNone;
  if (bridge == AtomConnectors::kEncoderNone) return AmdModeSet::kPanelModeExternalDp;
  uint8_t tmp = 0;
  if (!aux.dpcd_read(Dpcd::kEdpConfigurationCap, ByteView(&tmp, 1))) return AmdModeSet::kPanelModeExternalDp;
  if (tmp & 1) return AmdModeSet::kPanelModeInternalDp2;
  if (bridge == AtomConnectors::kEncoderNutmeg || bridge == AtomConnectors::kEncoderTravis) return AmdModeSet::kPanelModeInternalDp1;
  return AmdModeSet::kPanelModeExternalDp;
}

// The one place the memory BAR's window becomes a view of bytes: the window was mapped for this driver alone (the mapper refuses a range the
// OS owns), it is identity mapped, and it outlives the view (the driver never unmaps it while the display exists).
[[maybe_unused]] ByteView bar_view(const MmioRegion& window) { return ByteView(reinterpret_cast<uint8_t*>(window.address()), window.length()); }
}  // namespace

uint64_t UptimeClock::now_us() const { return uptime_us(); }

// The card as the AtomBIOS interpreter sees it (amdgpu's cail_* callbacks): registers by dword; a discrete VI card has no memory controller or PLL
// access through the interpreter.
class Display::Host final : public IAtomHost {
 public:
  Host(IVcnRegisters& registers, const IClock& clock) : registers_(registers), clock_(clock) {}
  uint32_t read_register(uint32_t reg) override { return registers_.read(reg); }
  void write_register(uint32_t reg, uint32_t value) override { registers_.write(reg, value); }
  uint32_t read_mc(uint32_t) override { return 0; }     // (cail_mc_read: "invalid MC read")
  void write_mc(uint32_t, uint32_t) override {}
  uint32_t read_pll(uint32_t) override { return 0; }    // (cail_pll_read)
  void write_pll(uint32_t, uint32_t) override {}
  void delay_us(uint32_t microseconds) override { wait(microseconds); }
  void delay_ms(uint32_t milliseconds) override { wait(uint64_t{milliseconds} * 1000); }
  uint64_t now_us() override { return clock_.now_us(); }

 private:
  void wait(uint64_t microseconds) {
    const uint64_t begin = clock_.now_us();
    while (clock_.now_us() - begin < microseconds) {}
  }
  IVcnRegisters& registers_;
  const IClock& clock_;
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// Modes
// ---------------------------------------------------------------------------------------------------------------------------------------
uint32_t refresh_millihz(const CrtcMode& m) {
  const uint64_t pixels = uint64_t{m.h_total} * m.v_total;
  if (!pixels) return 0;
  return static_cast<uint32_t>(uint64_t{m.pixel_clock_khz} * 1000000 / pixels);
}

uint32_t edid_modes(ConstByteView edid, const ModeCheck* check, ArrayView<Mode> out) {
  uint32_t count = 0;
  for (uint32_t slot = 0; slot < 4 && count < out.size(); ++slot) {
    CrtcMode m;
    if (!edid_detailed_timing(edid, slot, &m)) continue;
    bool seen = false;
    for (uint32_t i = 0; i < count; ++i) seen = seen || same_size(out[i].timing, m);
    if (seen) continue;
    const uint64_t pitch = align_up(m.h_display, kPitchAlignPixels);
    if (pitch * 4 * m.v_display > kSurfaceBytes) continue;
    if (check) {
      ModeCheck c = *check;
      c.clock_khz = m.pixel_clock_khz;
      c.hdisplay = m.h_display;
      c.vdisplay = m.v_display;
      if (mode_valid(c) != ModeStatus::Ok) continue;
    }
    Mode& o = out[count++];
    o.timing = m;
    o.refresh_millihz = refresh_millihz(m);
    o.preferred = slot == 0;
  }
  return count;
}

Optional<uint32_t> choose_mode(ArrayView<const Mode> modes) {
  Optional<uint32_t> best;
  for (uint32_t i = 0; i < modes.size(); ++i)
    if (modes[i].preferred) return Optional<uint32_t>(i);
  for (uint32_t i = 0; i < modes.size(); ++i) {
    if (!best) {
      best = Optional<uint32_t>(i);
      continue;
    }
    const Mode& a = modes[i];
    const Mode& b = modes[*best];
    const uint64_t pa = uint64_t{a.timing.h_display} * a.timing.v_display, pb = uint64_t{b.timing.h_display} * b.timing.v_display;
    if (pa > pb || (pa == pb && a.refresh_millihz > b.refresh_millihz)) best = Optional<uint32_t>(i);
  }
  return best;
}

bool drivable(AtomConnectors::Type type) {
  switch (type) {
    case AtomConnectors::Type::DisplayPort:
    case AtomConnectors::Type::HdmiA:
    case AtomConnectors::Type::HdmiB:
    case AtomConnectors::Type::DviD:
    case AtomConnectors::Type::DviI: return true;
    default: return false;      // the internal panels belong to the integrated GPU; analogue and TV outputs are not DIG outputs
  }
}

bool encoder_mode_of(AtomConnectors::Type type, uint8_t* mode) {
  switch (type) {
    case AtomConnectors::Type::DisplayPort:
    case AtomConnectors::Type::EmbeddedDisplayPort: *mode = AmdModeSet::kModeDp; return true;
    case AtomConnectors::Type::HdmiA:
    case AtomConnectors::Type::HdmiB: *mode = AmdModeSet::kModeHdmi; return true;
    case AtomConnectors::Type::DviD:
    case AtomConnectors::Type::DviI: *mode = AmdModeSet::kModeDvi; return true;
    case AtomConnectors::Type::Lvds: *mode = AmdModeSet::kModeLvds; return true;
    default: return false;
  }
}

const char* status_name(Status status) {
  switch (status) {
    case Status::Ok: return "running";
    case Status::NoAccess: return "no access to the card";
    case Status::NoBios: return "no AtomBIOS video BIOS";
    case Status::PostFailed: return "the POST (ASIC_Init) failed";
    case Status::NoMemoryMap: return "the memory controller does not describe video memory";
    case Status::TooLittleMemory: return "too little video memory or memory BAR";
    case Status::MemoryController: return "the memory controller's apertures were refused";
    case Status::NoConnectors: return "the video BIOS lists no connectors";
    case Status::NoMonitor: return "no monitor on an external connector";
    case Status::NoMode: return "no mode of the monitor fits the output";
    case Status::NoPipe: return "every display pipe is fused off";
    case Status::ModeSet: return "the mode set failed";
    case Status::NotRunning: return "not running";
    case Status::BadIndex: return "no such mode";
  }
  return "?";
}

Optional<Engine> engine_of(uint16_t device_id, bool* pipe_harvesting) {
  if (pipe_harvesting) *pipe_harvesting = false;
  const AmdGpuInfo info = amd_gpu_identify(device_id);
  if (!info.discrete) return Optional<Engine>();
  if (info.architecture == AmdGpuArchitecture::Polaris) return Optional<Engine>(Engine::Dce11);
  if (info.architecture != AmdGpuArchitecture::Vega) return Optional<Engine>();
  // Vega 10 (0x6860..0x687F), Vega 12 (0x69A0..0x69AF), Vega 20 (0x66A0..0x66AF: dce121, pipe harvesting)
  if (pipe_harvesting) *pipe_harvesting = device_id >= 0x66A0 && device_id <= 0x66AF;
  return Optional<Engine>(Engine::Dce12);
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// The display
// ---------------------------------------------------------------------------------------------------------------------------------------
Display::Display(const Inputs& inputs) : in_(inputs) {}

Display::~Display() { shutdown(); }

Status Display::fail(Status status, const char* why) {
  shutdown();
  last_ = status;
  status_ = why;
  return status;
}

uint32_t Display::default_clock_10khz(uint32_t offset) {
  uint16_t start = 0;
  if (!bios_ || !bios_->data_table_header(kFirmwareInfoTable, nullptr, nullptr, nullptr, &start)) return 0;
  ConstByteView word;
  if (!vbios_->view().try_subview(uint64_t{start} + offset, 4, &word)) return 0;
  return word[0] | uint32_t{word[1]} << 8 | uint32_t{word[2]} << 16 | uint32_t{word[3]} << 24;
}

Status Display::start() {
  shutdown();
  if (!in_.registers || !in_.clock || !in_.allocator || in_.aperture.size() < kApertureBytes) return fail(Status::NoAccess, status_name(Status::NoAccess));
  allocators::IAllocator& allocator = *in_.allocator;
  IVcnRegisters& r = *in_.registers;
  vbios_ = UniquePointer<ByteBuffer>::make(allocator, allocator);
  host_ = UniquePointer<Host>::make(allocator, r, *in_.clock);
  if (!vbios_ || !host_ || !vbios_->append(in_.vbios)) return fail(Status::NoAccess, "out of memory");
  bios_ = UniquePointer<AtomBios>::make(allocator, *host_, vbios_->view(), allocator);
  if (!bios_ || !bios_->valid()) return fail(Status::NoBios, status_name(Status::NoBios));

  const bool dce12 = in_.engine == Engine::Dce12;
  // amdgpu_device_need_post (CIK and later): the video BIOS marks the end of its ASIC_Init in BIOS_SCRATCH_7 (on an ATOM firmware card at the address its
  // firmware_info 3.1 gives)
  uint32_t scratch7 = AmdVbios::kBiosScratch7;
  if (dce12) {
    uint32_t start = 0;
    if (!scratch_start(&start)) return fail(Status::NoBios, "not an ATOM firmware video BIOS (no firmware_info 3.x with the BIOS scratch registers)");
    scratch7 = start + 7;
  }
  posted_ = false;
  if (!(r.read(scratch7) & AmdVbios::kAsicInitComplete)) {
    const AtomBios::Status post = bios_->asic_init();     // (amdgpu_atom_asic_init on DCE 12 too: amdgpu_device_asic_init for GC 9.0 - 9.4.0)
    if (post != AtomBios::Status::Ok) return fail(Status::PostFailed, AtomBios::status_name(post));
    posted_ = true;
  }

  if (dce12) {
    // gmc_v9_0 / mmhub_v1_0: where the POST put video memory, the apertures, the HDP and the display hub (amd_dce12::program_memory)
    map12_ = amd_dce12::read_memory_map(r);
    if (!map12_.read) return fail(Status::NoMemoryMap, status_name(Status::NoMemoryMap));
    map_.read = true;
    map_.video_memory_bytes = map12_.video_memory_bytes;
    map_.gpu_base = map12_.gpu_base;
    map_.gpu_top = map12_.gpu_top + (16ull << 20);
    if (map_.video_memory_bytes < kApertureBytes) return fail(Status::TooLittleMemory, status_name(Status::TooLittleMemory));
    if (!amd_dce12::program_memory(r, map12_, map12_.gpu_base + kScratchOffset)) return fail(Status::MemoryController, status_name(Status::MemoryController));
    // read_pipe_fuses (Vega 20): the first pipe that is not fused off
    crtc_ = 0;
    if (in_.pipe_harvesting) {
      const uint32_t fuses = amd_dce12::pipe_fuses(r);
      while (crtc_ < amd_dce12::kMaxPipes && (fuses >> crtc_ & 1)) ++crtc_;
      if (crtc_ >= amd_dce12::kMaxPipes) return fail(Status::NoPipe, status_name(Status::NoPipe));
    }
  } else {
    // the memory controller: where the POST put video memory, the system aperture over it (gmc_v8_0_mc_program)
    map_ = amd_read_gmc8_memory_map(r);
    if (!map_.read) return fail(Status::NoMemoryMap, status_name(Status::NoMemoryMap));
    if (map_.video_memory_bytes < kApertureBytes) return fail(Status::TooLittleMemory, status_name(Status::TooLittleMemory));
    AmdGmc8GartPlan plan;
    plan.vram_start = map_.gpu_base;
    plan.vram_end = map_.gpu_base + map_.video_memory_bytes - 1;
    AmdGmc8 gmc(r, *in_.clock);
    if (gmc.program_apertures(plan, map_.gpu_base + kScratchOffset, true) == AmdGmc8::Result::BadArgument) return fail(Status::MemoryController, status_name(Status::MemoryController));
  }

  // the connectors, the display block, the monitors
  connector_count_ = AtomConnectors::parse(vbios_->view(), ArrayView<AtomConnectors::Connector>(connectors_, AtomConnectors::kMaxConnectors));
  if (connector_count_ == 0) return fail(Status::NoConnectors, status_name(Status::NoConnectors));
  if (dce12) init_display_block12();
  else init_display_block();
  for (uint32_t i = 0; i < connector_count_; ++i) read_monitor(i);
  if (!pick_output()) return fail(last_, status_);

  // the surface and the mode
  const AtomConnectors::Connector& c = connectors_[driven_];
  const CrtcMode& m = modes_[current_].timing;
  bpc_ = monitor_bpc(c.type, true, dp_sink_, monitor_, m.pixel_clock_khz, false);
  if (dce12 && !AtomConnectors::uses_aux(c.type)) bpc_ = 8;     // (no deep colour on TMDS: the DTO resync is not ported)
  if (!plane_surface(m)) return fail(Status::ModeSet, "the surface does not fit");
  stage_ = dce12 ? program12(c, m) : program(c, m);
  if (stage_ != ModeStage::Done) return fail(Status::ModeSet, status_name(Status::ModeSet));
  running_ = true;
  last_ = Status::Ok;
  status_ = status_name(Status::Ok);
  return Status::Ok;
}

// dce_v11_0_hw_init's display part (the VGA render state is off already: AmdGmc8::program_apertures): the power gating initialised, every DIG's PHY
// (amdgpu_atombios_encoder_init_dig: ATOM_TRANSMITTER_ACTION_INIT), the hot plug pins on (dce_v11_0_hpd_init, here with their interrupts off).
bool Display::init_display_block() {
  AmdModeSet set(*host_, *bios_);
  uint8_t frev = 0, crev = 0;
  bool ok = true;
  if (bios_->cmd_table_header(AmdModeSet::kEnableDispPowerGating, &frev, &crev)) ok = set.powergate_init() && ok;
  uint32_t done = 0;    // bit n: DIG n initialised
  for (uint32_t i = 0; i < connector_count_; ++i) {
    const AtomConnectors::Connector& c = connectors_[i];
    uint8_t mode = 0;
    const uint8_t dig = AtomConnectors::dig_encoder(c);
    if (dig == AtomConnectors::kNoDig || dig > 6 || (done >> dig & 1) || !encoder_mode_of(c.type, &mode)) continue;
    done |= 1u << dig;
    ok = set.transmitter(output_of(c, crtc_, mode, 8, AmdModeSet::kPllInvalid), CrtcMode(), AmdModeSet::kTransmitterInit) && ok;
  }
  amd_dce11::Interrupts irq(*in_.registers);
  for (uint32_t i = 0; i < connector_count_; ++i) {
    const uint8_t pin = connectors_[i].hpd_pin;
    if (pin >= amd_dce11::Interrupts::kMaxHpd) continue;
    irq.hpd_init(pin);
    irq.hpd_set_interrupt(pin, false);
  }
  hpd_on_ = true;
  return ok;
}

// The EDID of an external connector: over its DDC lines, or I2C over the AUX channel for DisplayPort.
bool Display::read_monitor(uint32_t index) {
  edid_sizes_[index] = 0;
  const AtomConnectors::Connector& c = connectors_[index];
  if (!drivable(c.type) || !c.has_ddc) return false;
  const AtomI2c::Bus bus = in_.engine == Engine::Dce12 ? AtomI2c::Bus() : AtomI2c::lookup(vbios_->view(), c.ddc_id);
  if (in_.engine != Engine::Dce12 && !bus.valid) return false;
  ByteBuffer edid(*in_.allocator);
  bool ok;
  if (in_.engine == Engine::Dce12) {
    ok = read_ddc12(c, &edid);
  } else if (AtomConnectors::uses_aux(c.type)) {
    AmdAux aux(*host_, *bios_, c.ddc_id, c.hpd_pin);
    ok = aux.read_edid(&edid);
  } else {
    ok = AmdDdc(*in_.registers, *in_.clock, bus).read_edid(&edid);
  }
  if (!ok || edid.size() < 128) return false;
  const ConstByteView bytes = edid.view().at_most(kEdidBytes);
  if (!ArrayView<uint8_t>(edid_bytes_[index]).first(bytes.size()).copy_from(bytes)) return false;
  edid_sizes_[index] = static_cast<uint32_t>(bytes.size());
  return true;
}

// The first external connector with a monitor whose EDID has a mode the output carries.
bool Display::pick_output() {
  bool monitor = false;
  for (uint32_t i = 0; i < connector_count_; ++i) {
    if (edid_sizes_[i] == 0) continue;
    const AtomConnectors::Connector& c = connectors_[i];
    MonitorInfo info;
    if (!parse_monitor_info(edid(i), &info)) continue;
    monitor = true;
    ModeCheck check;
    check.type = c.type;
    check.connector_object_id = c.object_id;
    check.use_digital = true;
    check.monitor = info;
    dp_sink_ = false;
    dpcd_ = Dpcd::Capabilities();
    if (AtomConnectors::uses_aux(c.type)) {
      AmdAux aux(*host_, *bios_, c.ddc_id, c.hpd_pin);
      dp_sink_ = Dpcd::read_capabilities(aux, false, &dpcd_);
      check.dp_sink = dp_sink_;
      check.dpcd = dpcd_;
      check.dp12_capable = true;    // (HBR2 assumed of the encoder: a link that cannot be trained fails at the training)
    }
    mode_count_ = edid_modes(edid(i), &check, ArrayView<Mode>(modes_, kMaxModes));
    const Optional<uint32_t> chosen = choose_mode(ArrayView<const Mode>(modes_, mode_count_));
    if (!chosen) continue;
    driven_ = static_cast<int32_t>(i);
    current_ = *chosen;
    monitor_ = info;
    return true;
  }
  mode_count_ = 0;
  last_ = monitor ? Status::NoMode : Status::NoMonitor;
  status_ = status_name(last_);
  return false;
}

// The surface for `mode` at kSurfaceOffset: linear ARGB8888, rows kPitchAlignPixels aligned, cleared to black.
bool Display::plane_surface(const CrtcMode& mode) {
  const uint64_t pitch = align_up(mode.h_display, kPitchAlignPixels);
  const uint64_t bytes = pitch * 4 * mode.v_display;
  if (mode.h_display == 0 || mode.v_display == 0 || bytes > kSurfaceBytes) return false;
  surface_ = amd_dce11::ScanoutSurface();
  surface_.address = map_.gpu_base + kSurfaceOffset;
  surface_.width = surface_.viewport_width = mode.h_display;
  surface_.height = surface_.viewport_height = mode.v_display;
  surface_.pitch_pixels = static_cast<uint32_t>(pitch);
  pixels_ = in_.aperture.subview(kSurfaceOffset, bytes);
  pixels_.fill(0);
  flush();
  return true;
}

void Display::update_watermarks() {
  const uint32_t sclk = default_clock_10khz(kFwiDefaultSclk), mclk = default_clock_10khz(kFwiDefaultMclk);
  if (sclk == 0 || mclk == 0 || sclk > 0xFFFFFFFFu / 10 || mclk > 0xFFFFFFFFu / 10) return;
  amd_dce11::WatermarkClocks clocks;
  clocks.sclk_high_khz = clocks.sclk_low_khz = sclk * 10;
  clocks.mclk_high_khz = clocks.mclk_low_khz = mclk * 10;
  amd_dce11::Watermarks(*in_.registers, *in_.clock).update(heads_, clocks);
}

// dce_v11_0_crtc_prepare / _mode_set / _commit with the encoder's prepare / mode_set / commit, as AmdVegaM::set_mode does them for the boot card
// (without the audio).
ModeStage Display::program(const AtomConnectors::Connector& c, const CrtcMode& mode) {
  link_ = LinkTraining();
  uint8_t encoder_mode = 0;
  const uint8_t dig = AtomConnectors::dig_encoder(c);
  if (!encoder_mode_of(c.type, &encoder_mode) || dig > 5 || mode.pixel_clock_khz == 0) return ModeStage::Unsupported;
  const bool dp = encoder_mode == AmdModeSet::kModeDp;
  uint8_t pll = AmdModeSet::pick_pll(encoder_mode, AtomConnectors::encoder_id(c), AtomConnectors::link_b(c));
  if (pll == AmdModeSet::kPllInvalid) return ModeStage::Unsupported;
  pll_ = pll;
  ModeSetOutput o = output_of(c, crtc_, encoder_mode, bpc_, pll);
  o.dual_link = encoder_mode == AmdModeSet::kModeDvi && mode.pixel_clock_khz > kTmdsSingleLinkKhz;
  AmdModeSet set(*host_, *bios_);
  const auto optional = [&](uint32_t table) { uint8_t frev = 0, crev = 0; return !bios_->cmd_table_header(table, &frev, &crev); };
  amd_dce11::Scanout display(*in_.registers);
  mode_set_ = true;      // (from here on a failure leaves something to switch off: shutdown() runs the disable sequence)
  // dce_v11_0_crtc_prepare: power gating off, the CRTC locked, dpms off (blanked if it was on, disabled)
  if ((!optional(AmdModeSet::kEnableDispPowerGating) && !set.powergate_crtc(crtc_, false)) || (!optional(AmdModeSet::kUpdateDoubleBuffer) && !set.lock_crtc(crtc_, true)))
    return ModeStage::Prepare;
  bool off = true;
  if (crtc_enabled_) {
    display.vga_enable(crtc_, true);
    off = set.blank_crtc(crtc_, true);
    display.vga_enable(crtc_, false);
  }
  if (!set.enable_crtc(crtc_, false) || !off) return ModeStage::Prepare;
  crtc_enabled_ = false;
  // the DisplayPort link configuration (made when the driver fixes the mode up)
  UniquePointer<AmdAux> aux;
  if (dp) {
    if (!c.has_ddc) return ModeStage::SinkCapabilities;
    aux = UniquePointer<AmdAux>::make(*in_.allocator, *host_, *bios_, c.ddc_id, c.hpd_pin);
    if (!aux || !Dpcd::read_capabilities(*aux, false, &link_.caps)) return ModeStage::SinkCapabilities;
    link_.config = DpLinkConfig::choose(link_.caps, mode.pixel_clock_khz, bpc_ * 3);
    if (link_.config.lanes == 0) return ModeStage::SinkCapabilities;
    o.lanes = link_.config.lanes;
    o.link_rate_khz = link_.config.rate_khz;
    link_.panel_mode = panel_mode_of(c, *aux);
  }
  // dce_v11_0_encoder_prepare: the CRTC's source, the FMT block
  if (!set.select_crtc_source(o) || !display.program_fmt(crtc_, bpc_, false)) return ModeStage::CrtcSource;
  // dce_v11_0_crtc_mode_set: the pixel clock, the timing, the base (the surface), the bandwidth, the overscan and the scaler
  if (!set.set_pixel_clock(o, mode)) return ModeStage::PixelClock;
  if (!set.set_dtd_timing(o, mode)) return ModeStage::Timing;
  if (!display.set(crtc_, surface_)) return ModeStage::Scanout;
  heads_[crtc_] = {true, mode.h_display, mode.h_total, mode.pixel_clock_khz, mode.interlaced};
  update_watermarks();
  if (!optional(AmdModeSet::kSetCrtcOverscan) && !set.overscan_setup(o)) return ModeStage::Overscan;
  if (!optional(AmdModeSet::kEnableScaler) && !set.scaler_setup(o)) return ModeStage::Scaler;
  // dce_v11_0_encoder_mode_set: the encoder off (setup_dig(ATOM_DISABLE): DP video off, the transmitter disabled)
  bool encoder_off = true;
  if (dp) encoder_off = set.encoder(o, mode, AmdModeSet::kEncoderDpVideoOff);
  if (!set.transmitter(o, mode, AmdModeSet::kTransmitterDisable) || !encoder_off) return ModeStage::EncoderOff;
  display.set_interleave(crtc_, mode.interlaced);
  // dce_v11_0_crtc_commit: enabled, unblanked, unlocked
  if (!set.enable_crtc(crtc_, true)) return ModeStage::CrtcEnable;
  crtc_enabled_ = true;
  display.vga_enable(crtc_, true);
  const bool unblanked = set.blank_crtc(crtc_, false);
  display.vga_enable(crtc_, false);
  if (!unblanked || (!optional(AmdModeSet::kUpdateDoubleBuffer) && !set.lock_crtc(crtc_, false))) return ModeStage::Unblank;
  // dce_v11_0_encoder_commit: setup_dig(ATOM_ENABLE)
  if (!set.encoder(o, mode, AmdModeSet::kEncoderSetup)) return ModeStage::EncoderSetup;
  if (!set.encoder(o, mode, AmdModeSet::kEncoderSetupPanelMode, dp ? link_.panel_mode : AmdModeSet::kPanelModeExternalDp)) return ModeStage::PanelMode;
  if (!set.transmitter(o, mode, AmdModeSet::kTransmitterEnable)) return ModeStage::TransmitterEnable;
  if (dp) {
    DpSourceConfig source;
    source.dig_encoder = dig;
    source.link_b = AtomConnectors::link_b(c);
    source.lanes = link_.config.lanes;
    source.rate_khz = link_.config.rate_khz;
    source.pixel_clock_khz = mode.pixel_clock_khz;
    source.connector_object_id = c.object_id;
    source.hpd = c.hpd_pin;
    source.bits_per_color = bpc_;
    source.pll = pll;
    AmdDpSource dp_source(*host_, *bios_, source);
    link_.outcome = DpLinkTrainer::train(*aux, dp_source, link_.caps, link_.config.bandwidth_code(), link_.config.lanes, link_.config.rate_index,
                                         link_.panel_mode == AmdModeSet::kPanelModeInternalDp2);
    if (!link_.outcome.trained()) return ModeStage::LinkTraining;
    if (!set.encoder(o, mode, AmdModeSet::kEncoderDpVideoOn)) return ModeStage::VideoOn;
  }
  return ModeStage::Done;
}

// dce_v11_0_encoder_disable and dce_v11_0_crtc_disable: the encoder off (DP video off, the sink to D3, the transmitter disabled), dpms off, the
// graphics plane off, the CRTC power gated, its PLL off.
void Display::disable(const AtomConnectors::Connector& c) {
  uint8_t encoder_mode = 0;
  if (!encoder_mode_of(c.type, &encoder_mode)) return;
  AmdModeSet set(*host_, *bios_);
  const CrtcMode none;
  const ModeSetOutput o = output_of(c, crtc_, encoder_mode, bpc_, pll_);
  if (encoder_mode == AmdModeSet::kModeDp) {
    (void)set.encoder(o, none, AmdModeSet::kEncoderDpVideoOff);
    AmdAux aux(*host_, *bios_, c.ddc_id, c.hpd_pin);
    Dpcd::Capabilities caps;
    if (c.has_ddc && Dpcd::read_capabilities(aux, false, &caps) && caps.revision >= 0x11) {
      const uint8_t d3 = Dpcd::kPowerD3;
      (void)aux.dpcd_write(Dpcd::kSetPower, ConstByteView(&d3, 1));
      host_->delay_us(1000);
    }
  }
  (void)set.transmitter(o, none, AmdModeSet::kTransmitterDisable);
  amd_dce11::Scanout display(*in_.registers);
  if (crtc_enabled_) {
    display.vga_enable(crtc_, true);
    (void)set.blank_crtc(crtc_, true);
    display.vga_enable(crtc_, false);
  }
  (void)set.enable_crtc(crtc_, false);
  crtc_enabled_ = false;
  heads_[crtc_] = amd_dce11::WatermarkHead();
  update_watermarks();
  (void)display.disable(crtc_);
  uint8_t frev = 0, crev = 0;
  if (bios_->cmd_table_header(AmdModeSet::kEnableDispPowerGating, &frev, &crev)) (void)set.powergate_crtc(crtc_, true);
  const bool combo = pll_ >= AmdModeSet::kComboPhyPll0 && pll_ < AmdModeSet::kComboPhyPll0 + 6;
  if (pll_ <= AmdModeSet::kPpll0 || combo) {      // (the DP DTO is left alone, as in dce_v11_0_crtc_disable)
    ModeSetOutput clock_off;
    clock_off.crtc = combo ? AmdModeSet::kCrtcInvalid : crtc_;
    clock_off.pll = pll_;
    clock_off.encoder_mode = 0;
    (void)set.set_pixel_clock(clock_off, CrtcMode());
  }
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// DCE 12 (Vega 10, 12, 20)
// ---------------------------------------------------------------------------------------------------------------------------------------

// amdgpu_atomfirmware_scratch_regs_init: firmware_info 3.x's bios_scratch_reg_startaddr (a register of the BAR; 0 or beyond it is refused).
bool Display::scratch_start(uint32_t* scratch) {
  uint16_t start = 0, size = 0;
  uint8_t frev = 0, crev = 0;
  if (!bios_->data_table_header(kFirmwareInfoTable, &size, &frev, &crev, &start) || frev != 3 || size < kFwiV3MinBytes) return false;
  ConstByteView word;
  if (!vbios_->view().try_subview(uint64_t{start} + kFwiV3ScratchStart, 4, &word)) return false;
  const uint32_t value = word[0] | uint32_t{word[1]} << 8 | uint32_t{word[2]} << 16 | uint32_t{word[3]} << 24;
  if (value == 0 || value >= kMaxRegisterDword - 8) return false;
  *scratch = value;
  return true;
}

// The transmitter parameters of a connector as dce110_link_encoder builds them: the PHY (the DIG's: UNIPHY A..G), its hot plug select (HPD n + 1, 0 none), the connector.
amd_dce12::CommandTables::Transmitter Display::transmitter12(const AtomConnectors::Connector& c, uint8_t action, uint8_t encoder_mode) const {
  amd_dce12::CommandTables::Transmitter t;
  t.phy = AtomConnectors::dig_encoder(c);
  t.action = action;
  t.mode_or_lane_set = encoder_mode;          // ATOM_TRANSMITTER_DIGMODE_V6_DP 0, _DVI 2, _HDMI 3: the encoder modes' numbers
  t.hpd_select = c.hpd_pin < amd_dce12::kMaxHpd ? static_cast<uint8_t>(c.hpd_pin + 1) : 0;
  t.connector_object_id = c.object_id;
  return t;
}

// dce110_link_encoder_hw_init for each DIG (the transmitter INIT with no front end, four lanes, the DIG mode of SIGNAL_TYPE_NONE - DVI - or DP for eDP), its back end's hot plug
// select (hpd_initialize), and the hot plug pins on.
bool Display::init_display_block12() {
  amd_dce12::CommandTables tables(*bios_);
  amd_dce12::Pipe pipe(*in_.registers, *in_.clock);
  bool ok = true;
  uint32_t done = 0;
  for (uint32_t i = 0; i < connector_count_; ++i) {
    const AtomConnectors::Connector& c = connectors_[i];
    const uint8_t dig = AtomConnectors::dig_encoder(c);
    if (dig >= amd_dce12::kMaxDigs || (done >> dig & 1)) continue;
    done |= 1u << dig;
    amd_dce12::CommandTables::Transmitter t =
        transmitter12(c, AmdModeSet::kTransmitterInit, c.type == AtomConnectors::Type::EmbeddedDisplayPort ? AmdModeSet::kModeDp : AmdModeSet::kModeDvi);
    t.lanes = 4;
    ok = tables.transmitter(t) && ok;
    ok = pipe.set_hpd_select(dig, c.hpd_pin < amd_dce12::kMaxHpd ? c.hpd_pin : kHpdSourceUnknown) && ok;
  }
  for (uint32_t i = 0; i < connector_count_; ++i) (void)pipe.hpd_enable(connectors_[i].hpd_pin, true);
  hpd_on_ = true;
  return ok;
}

// The EDID over the connector's DDC pads (the gpio_pin_lut entry of its I2C record names the line) or, for DisplayPort, I2C over AUX.
bool Display::read_ddc12(const AtomConnectors::Connector& c, ByteBuffer* edid) {
  if (AtomConnectors::uses_aux(c.type)) {
    AmdAux aux(*host_, *bios_, c.ddc_id, c.hpd_pin);
    return aux.read_edid(edid);
  }
  const AtomI2c::PinV21 pin = AtomI2c::lookup_v21(vbios_->view(), c.ddc_id);
  if (!pin.found || pin.shift != 0) return false;           // (the clock pad is bit 0 of its A register: DC_GPIO_DDCnCLK_A)
  const uint32_t line = amd_dce12::ddc_line_of(pin.a_register);
  if (line >= amd_dce12::kMaxDdc) return false;
  AtomI2c::Bus bus = amd_dce12::ddc_bus(line);
  bus.hw_capable = (c.ddc_id & 0x80) != 0;                  // I2C_HW_CAP: the pad pair is switched to DDC mode first
  return AmdDdc(*in_.registers, *in_.clock, bus).read_edid(edid);
}

// The mode set in the display core's order for one stream on CRTC crtc_ (dce110_apply_ctx_to_hw, dce110_enable_stream_timing, dce110_apply_single_controller_ctx_to_hw,
// dce110_program_front_end_for_pipe, link_set_dpms_on), with the encoder table for the stream and the link training (see AmdDce12.hpp).
ModeStage Display::program12(const AtomConnectors::Connector& c, const CrtcMode& mode) {
  link_ = LinkTraining();
  uint8_t encoder_mode = 0;
  const uint8_t dig = AtomConnectors::dig_encoder(c);
  if (!encoder_mode_of(c.type, &encoder_mode) || dig > 5 || mode.pixel_clock_khz == 0 || mode.interlaced || mode.double_scan || mode.pixel_clock_khz > 0xFFFFFFFFu / 10)
    return ModeStage::Unsupported;
  const bool dp = encoder_mode == AmdModeSet::kModeDp;
  // the clock source (dce112 find_matching_pll / the DP clock source): DP DTO, else the combo PHY PLL of the transmitter (A..F: 20..25)
  const uint8_t pll = AmdModeSet::pick_pll(encoder_mode, AtomConnectors::encoder_id(c), AtomConnectors::link_b(c));
  if (pll == AmdModeSet::kPllInvalid) return ModeStage::Unsupported;
  pll_ = pll;
  const bool dual_link = encoder_mode == AmdModeSet::kModeDvi && mode.pixel_clock_khz > kTmdsSingleLinkKhz;
  amd_dce12::CommandTables tables(*bios_);
  amd_dce12::Pipe pipe(*in_.registers, *in_.clock);
  mode_set_ = true;      // (from here on a failure leaves something to switch off)
  // dce110_apply_ctx_to_hw: the safe display marks; dce110_enable_stream_timing: blanked first ("otherwise CRTC will be hung in bad state")
  (void)pipe.safe_watermarks(crtc_);
  (void)pipe.set_blank(crtc_, true);
  // the DisplayPort link configuration (decided before the stream is enabled)
  UniquePointer<AmdAux> aux;
  if (dp) {
    if (!c.has_ddc) return ModeStage::SinkCapabilities;
    aux = UniquePointer<AmdAux>::make(*in_.allocator, *host_, *bios_, c.ddc_id, c.hpd_pin);
    if (!aux || !Dpcd::read_capabilities(*aux, false, &link_.caps)) return ModeStage::SinkCapabilities;
    link_.config = DpLinkConfig::choose(link_.caps, mode.pixel_clock_khz, bpc_ * 3);
    if (link_.config.lanes == 0 || link_.config.rate_index != DpLinkConfig::kNoRateIndex) return ModeStage::SinkCapabilities;
    link_.panel_mode = panel_mode_of(c, *aux);
  }
  // dce112_program_pix_clk (SetPixelClock 1.7), the timing (SetCRTC_UsingDTDTiming 3) with the advanced request, dce120_timing_generator_enable_crtc
  if (!tables.set_pixel_clock(crtc_, pll, AtomConnectors::encoder_id(c), encoder_mode, mode.pixel_clock_khz * 10, dual_link ? amd_dce12::CommandTables::kPixelClockDualLink : 0))
    return ModeStage::PixelClock;
  if (!tables.set_crtc_timing(crtc_, mode) || !pipe.advanced_request(crtc_, mode)) return ModeStage::Timing;
  (void)pipe.prepare_enable(crtc_);
  if (!tables.enable_crtc(crtc_, true)) return ModeStage::CrtcEnable;
  crtc_enabled_ = true;
  // the output's FMT block; the front end: DMIF buffers, the VGA mode off, the plane
  (void)pipe.program_fmt(crtc_, bpc_);
  if (!pipe.allocate_dmif(crtc_, mode) || !pipe.disable_vga(crtc_) || !pipe.set_surface(crtc_, surface_)) return ModeStage::Scanout;
  // link_set_dpms_on: the DIG front end on the CRTC and the back end on the front end, the stream (encoder table), the PHY, the link
  if (!pipe.connect(dig, crtc_, true)) return ModeStage::CrtcSource;
  const uint32_t lanes = dp ? link_.config.lanes : (dual_link ? 8u : 4u);
  if (!tables.encoder_stream_setup(dig, encoder_mode, static_cast<uint8_t>(lanes), mode.pixel_clock_khz, AmdModeSet::panel_bpc(bpc_), dp ? link_.config.rate_khz : 0))
    return ModeStage::EncoderSetup;
  if (dp && !tables.encoder_panel_mode(dig, link_.panel_mode)) return ModeStage::PanelMode;
  amd_dce12::CommandTables::Transmitter t = transmitter12(c, AmdModeSet::kTransmitterEnable, encoder_mode);
  t.lanes = static_cast<uint8_t>(lanes);
  t.symbol_clock_khz = dp ? link_.config.rate_khz : mode.pixel_clock_khz;     // (enable_dp_output: the link rate; enable_tmds_output: the pixel clock)
  if (!tables.transmitter(t)) return ModeStage::TransmitterEnable;
  if (dp) {
    amd_dce12::CommandTables::Transmitter drive = transmitter12(c, AmdModeSet::kTransmitterSetupVsEmph, encoder_mode);
    drive.lanes = static_cast<uint8_t>(lanes);
    drive.symbol_clock_khz = link_.config.rate_khz;
    amd_dce12::DpSource source(*host_, tables, dig, drive);
    link_.outcome = DpLinkTrainer::train(*aux, source, link_.caps, link_.config.bandwidth_code(), link_.config.lanes, link_.config.rate_index,
                                         link_.panel_mode == AmdModeSet::kPanelModeInternalDp2);
    if (!link_.outcome.trained()) return ModeStage::LinkTraining;
    if (!tables.encoder_command(dig, AmdModeSet::kEncoderDpVideoOn)) return ModeStage::VideoOn;
  }
  // unblanked (dce110_unblank_stream / program_surface_visibility)
  if (!pipe.set_blank(crtc_, false)) return ModeStage::Unblank;
  return ModeStage::Done;
}

// The stream taken down (dce110_disable_stream, the link off, dce110_reset_hw_ctx_wrap): DP video off and the sink to D3, the transmitter off, the front end let go, blanked,
// the CRTC off, the plane off and its DMIF buffers freed, the combo PHY PLL powered down (dce110_clock_source_power_down; the DP DTO is left alone).
void Display::disable12(const AtomConnectors::Connector& c) {
  uint8_t encoder_mode = 0;
  const uint8_t dig = AtomConnectors::dig_encoder(c);
  if (!encoder_mode_of(c.type, &encoder_mode) || dig >= amd_dce12::kMaxDigs) return;
  amd_dce12::CommandTables tables(*bios_);
  amd_dce12::Pipe pipe(*in_.registers, *in_.clock);
  if (encoder_mode == AmdModeSet::kModeDp) {
    (void)tables.encoder_command(dig, AmdModeSet::kEncoderDpVideoOff);
    AmdAux aux(*host_, *bios_, c.ddc_id, c.hpd_pin);
    Dpcd::Capabilities caps;
    if (c.has_ddc && Dpcd::read_capabilities(aux, false, &caps) && caps.revision >= 0x11) {
      const uint8_t d3 = Dpcd::kPowerD3;
      (void)aux.dpcd_write(Dpcd::kSetPower, ConstByteView(&d3, 1));
      host_->delay_us(1000);
    }
  }
  (void)tables.transmitter(transmitter12(c, AmdModeSet::kTransmitterDisable, encoder_mode));
  (void)pipe.connect(dig, crtc_, false);
  (void)pipe.set_blank(crtc_, true);
  if (crtc_enabled_) (void)tables.enable_crtc(crtc_, false);
  crtc_enabled_ = false;
  (void)pipe.disable_surface(crtc_);
  (void)pipe.free_dmif(crtc_);
  const bool combo = pll_ >= AmdModeSet::kComboPhyPll0 && pll_ < AmdModeSet::kComboPhyPll0 + 6;
  if (combo) (void)tables.set_pixel_clock(kAtomCrtcInvalid, pll_, 0, kEncoderModeCrt, 0, amd_dce12::CommandTables::kPixelClockForcePll);
  mode_set_ = false;
}

Status Display::set_mode(uint32_t index) {
  if (!running_ || driven_ < 0) return Status::NotRunning;
  if (index >= mode_count_) return Status::BadIndex;
  const AtomConnectors::Connector& c = connectors_[driven_];
  const uint32_t old = current_;
  const auto apply = [&](uint32_t i) {
    const CrtcMode& m = modes_[i].timing;
    bpc_ = monitor_bpc(c.type, true, dp_sink_, monitor_, m.pixel_clock_khz, false);
    if (in_.engine == Engine::Dce12 && !AtomConnectors::uses_aux(c.type)) bpc_ = 8;
    if (in_.engine == Engine::Dce12 && mode_set_) disable12(c);     // (DC takes the stream down before it sets the new one)
    if (!plane_surface(m)) return ModeStage::Scanout;
    return in_.engine == Engine::Dce12 ? program12(c, m) : program(c, m);
  };
  stage_ = apply(index);
  if (stage_ == ModeStage::Done) {
    current_ = index;
    return Status::Ok;
  }
  const ModeStage failed = stage_;
  if (apply(old) != ModeStage::Done) {
    (void)fail(Status::ModeSet, "the mode set failed and the old mode could not be set again");
    stage_ = failed;
    return Status::ModeSet;
  }
  stage_ = failed;
  return Status::ModeSet;
}

void Display::flush() {
  if (!in_.registers || !map_.read) return;
  if (in_.engine == Engine::Dce12) amd_dce12::flush_hdp(*in_.registers);
  else amd_gmc8_flush_hdp(*in_.registers);
}

void Display::shutdown() {
  if (mode_set_ && driven_ >= 0 && host_ && bios_) {
    if (in_.engine == Engine::Dce12) disable12(connectors_[driven_]);
    else disable(connectors_[driven_]);
  }
  mode_set_ = false;
  if (hpd_on_ && in_.registers && in_.clock && in_.engine == Engine::Dce12) {
    amd_dce12::Pipe pipe(*in_.registers, *in_.clock);
    for (uint32_t i = 0; i < connector_count_; ++i) (void)pipe.hpd_enable(connectors_[i].hpd_pin, false);
  } else if (hpd_on_ && in_.registers) {
    amd_dce11::Interrupts irq(*in_.registers);
    for (uint32_t i = 0; i < connector_count_; ++i)
      if (connectors_[i].hpd_pin < amd_dce11::Interrupts::kMaxHpd) irq.hpd_fini(connectors_[i].hpd_pin);
  }
  hpd_on_ = false;
  running_ = false;
  pixels_ = ByteView();
  surface_ = amd_dce11::ScanoutSurface();
  driven_ = -1;
  mode_count_ = 0;
  current_ = 0;
  crtc_ = 0;
  bios_ = UniquePointer<AtomBios>();
  host_ = UniquePointer<Host>();
  vbios_ = UniquePointer<ByteBuffer>();
  last_ = Status::NotRunning;
  status_ = status_name(Status::NotRunning);
}

}  // namespace amd_secondary

// ---------------------------------------------------------------------------------------------------------------------------------------
// AmdSecondaryGpu
// ---------------------------------------------------------------------------------------------------------------------------------------
bool AmdSecondaryGpu::supports(const PciDeviceInfo& device) {
  if (device.vendor_id != kPciVendor || device.class_code != kPciClassDisplay) return false;
  return static_cast<bool>(amd_secondary::engine_of(device.device_id));
}

AmdSecondaryGpu::AmdSecondaryGpu(const GpuContext& context) : context_(context) {
  const AmdGpuInfo info = amd_gpu_identify(context.device.device_id);
  const ArrayView<char> out(name_);
  uint32_t n = 0;
  for (const char* p = info.name; p && *p && n + 1 < out.size(); ++p) out[n++] = *p;
  out[n] = 0;
}

AmdSecondaryGpu::~AmdSecondaryGpu() { shutdown(); }

bool AmdSecondaryGpu::fail(const char* why) {
  status_ = why;
  display_ = UniquePointer<amd_secondary::Display>();
  registers_ = UniquePointer<MmioVcnRegisters>();
  ordered_ = 0;
  set_framebuffer(ByteView(), 0, 0, 0, true);
  return false;
}

bool AmdSecondaryGpu::start_display(const amd_secondary::Display::Inputs& inputs) {
  if (!context_.allocator) return fail("no allocator");
  display_ = UniquePointer<amd_secondary::Display>::make(*context_.allocator, inputs);
  if (!display_) return fail("out of memory");
  if (display_->start() != amd_secondary::Status::Ok) return fail(display_->status());
  set_video_memory_total(display_->memory_map().video_memory_bytes);
  order();
  show();
  status_ = "running";
  return true;
}

bool AmdSecondaryGpu::initialize_with(const amd_secondary::Display::Inputs& inputs) {
#ifndef TCOS_AMD_NATIVE
  (void)inputs;
  return fail("built without TCOS_AMD_NATIVE: the display of a secondary AMD GPU is not driven");
#else
  return start_display(inputs);
#endif
}

bool AmdSecondaryGpu::initialize() {
#ifndef TCOS_AMD_NATIVE
  // (Nothing is read or written: the pool's probe does the read-only part.)
  return fail("built without TCOS_AMD_NATIVE: the display of a secondary AMD GPU is not driven");
#else
  if (!context_.allocator || !context_.registers || !context_.pci) return fail("no allocator, register mapper or PCI bus");
  bool harvesting = false;
  const Optional<amd_secondary::Engine> engine = amd_secondary::engine_of(context_.device.device_id, &harvesting);
  if (!engine) return fail("not a Polaris-family (DCE 11.2) or Vega 10/12/20 (DCE 12) GPU");
  const bool dce12 = *engine == amd_secondary::Engine::Dce12;
  allocators::IAllocator& allocator = *context_.allocator;
  const IPciBus& pci = *context_.pci;
  const PciAddress address = context_.device.address;
  constexpr uint16_t kPciCommand = 0x04;
  constexpr uint32_t kMemorySpace = 1u << 1;
  const uint32_t command = pci.read32(address, kPciCommand) & 0xFFFF;     // (not the status bits above, which clear when written)
  if (!(command & kMemorySpace)) pci.write32(address, kPciCommand, command | kMemorySpace);
  const uint64_t bar5 = pci.memory_bar(address, 5), bar0 = pci.memory_bar(address, 0);
  MmioRegion register_window, aperture_window;
  if (!bar5 || !bar0 || !context_.registers->map_registers(bar5, dce12 ? amd_dce12::kRegisterBytes : kAmdGmc8RegisterBytes, &register_window)) return fail("BAR 5 not mapped");
  if (!context_.registers->map_registers(bar0, amd_secondary::kApertureBytes, &aperture_window) || aperture_window.length() < amd_secondary::kApertureBytes)
    return fail("BAR 0 not mapped");
  registers_ = UniquePointer<MmioVcnRegisters>::make(allocator, register_window);
  if (!registers_) return fail("out of memory");
  if (registers_->read(dce12 ? amd_dce12::kNbioBase[2] + 0xC3 : AmdVbios::kBiosScratch7) == 0xFFFFFFFFu)    // (RCC_CONFIG_MEMSIZE on DCE 12)
    return fail("the GPU does not answer (powered down?)");
  // The video BIOS: the firmware's copy in the ACPI VFCT table (what laptops with a discrete card use), the copy in video memory (only on a card
  // that was POSTed), the card's ROM through the SMC. (The ATRM method needs the AML interpreter and the expansion ROM a writable PCI bus: not here.)
  ByteBuffer vbios(allocator);
  AmdVbios::Sources sources;
  AcpiTables tables(context_.st);
  if (context_.st && tables.initialize()) sources.vfct = tables.find("VFCT");
  sources.mapper = context_.registers;
  sources.registers = dce12 ? nullptr : registers_.get();     // (the copy in video memory and the ROM read use the Volcanic Islands registers: not for a Vega)
  sources.bar0_physical = bar0;
  sources.bar0_bytes = amd_secondary::kApertureBytes;
  sources.allocator = &allocator;
  sources.function = address;
  sources.vendor_id = context_.device.vendor_id;
  sources.device_id = context_.device.device_id;
  if (AmdVbios::find(sources, &vbios) == AmdVbios::Source::None) return fail("no video BIOS found (VFCT, video memory, ROM)");
  amd_secondary::Display::Inputs in;
  in.registers = registers_.get();
  in.clock = &clock_;
  in.allocator = &allocator;
  in.vbios = vbios.view();     // (copied by start())
  in.aperture = amd_secondary::bar_view(aperture_window);
  in.engine = *engine;
  in.pipe_harvesting = harvesting;
  return start_display(in);
#endif
}

void AmdSecondaryGpu::shutdown() {
  if (display_) display_->shutdown();
  display_ = UniquePointer<amd_secondary::Display>();
  registers_ = UniquePointer<MmioVcnRegisters>();
  ordered_ = 0;
  set_framebuffer(ByteView(), 0, 0, 0, true);
}

void AmdSecondaryGpu::show() {
  if (display_ && display_->running()) set_framebuffer(display_->pixels(), display_->width(), display_->height(), display_->pitch_bytes(), true);
  else set_framebuffer(ByteView(), 0, 0, 0, true);
}

uint32_t AmdSecondaryGpu::mode_count() const { return display_ && display_->running() ? display_->mode_count() : 0; }

bool AmdSecondaryGpu::mode_info(uint32_t index, DisplayMode* out) const {
  if (!out || index >= mode_count()) return false;
  const amd_secondary::Mode* m = display_->mode(index);
  out->width = m->timing.h_display;
  out->height = m->timing.v_display;
  return true;
}

uint32_t AmdSecondaryGpu::current_mode() const { return display_ ? display_->current_mode() : 0; }

bool AmdSecondaryGpu::set_mode(uint32_t index) {
  if (!display_ || !display_->running()) return false;
  const bool ok = display_->set_mode(index) == amd_secondary::Status::Ok;
  show();
  return ok;
}

void AmdSecondaryGpu::present() {
  if (display_ && display_->running()) display_->flush();
}

bool AmdSecondaryGpu::pci_location(PciAddress* out) const {
  if (!out) return false;
  *out = context_.device.address;
  return true;
}

// ------------------------------------------------------------------ the monitors

void AmdSecondaryGpu::order() {
  ordered_ = 0;
  if (!display_) return;
  const ArrayView<const AtomConnectors::Connector> list = display_->connectors();
  const ArrayView<uint8_t> out(order_);
  const int32_t shown = display_->connector();
  if (shown >= 0) out[ordered_++] = static_cast<uint8_t>(shown);
  for (uint32_t pass = 0; pass < 2; ++pass)
    for (uint32_t i = 0; i < list.size() && ordered_ < out.size(); ++i) {
      if (static_cast<int32_t>(i) == shown) continue;
      const bool has_monitor = !display_->edid(i).empty();
      if (has_monitor == (pass == 0)) out[ordered_++] = static_cast<uint8_t>(i);
    }
}

bool AmdSecondaryGpu::driven(uint32_t connector) const {
  return display_ && display_->running() && connector < ordered_ && static_cast<int32_t>(order_[connector]) == display_->connector();
}

bool AmdSecondaryGpu::connector_info(uint32_t connector, ConnectorInfo* out) const {
  if (!out || !display_ || connector >= ordered_) return false;
  const uint32_t index = order_[connector];
  const AtomConnectors::Connector& c = display_->connectors()[index];
  *out = ConnectorInfo();
  out->connected = !display_->edid(index).empty();
  switch (c.type) {
    case AtomConnectors::Type::HdmiA:
    case AtomConnectors::Type::HdmiB: out->type = ConnectorType::Hdmi; break;
    case AtomConnectors::Type::DisplayPort: out->type = ConnectorType::DisplayPort; break;
    case AtomConnectors::Type::DviD:
    case AtomConnectors::Type::DviI: out->type = ConnectorType::Dvi; break;
    case AtomConnectors::Type::Vga: out->type = ConnectorType::Vga; break;
    case AtomConnectors::Type::Lvds:
    case AtomConnectors::Type::EmbeddedDisplayPort: out->type = ConnectorType::Internal; break;
    case AtomConnectors::Type::Composite:
    case AtomConnectors::Type::SVideo:
    case AtomConnectors::Type::DIN: out->type = ConnectorType::Composite; break;
  }
  (void)AtomConnectors::name(c, ArrayView<char>(out->name));
  if (driven(connector)) {
    for (uint32_t i = 0; i < display_->mode_count(); ++i)
      if (display_->mode(i)->preferred) {
        out->preferred_width = display_->mode(i)->timing.h_display;
        out->preferred_height = display_->mode(i)->timing.v_display;
      }
  }
  return true;
}

uint32_t AmdSecondaryGpu::read_edid(uint32_t connector, ByteView out) {
  if (!display_ || connector >= ordered_) return 0;
  const ConstByteView edid = display_->edid(order_[connector]).at_most(out.size());
  if (!out.first(edid.size()).copy_from(edid)) return 0;
  return static_cast<uint32_t>(edid.size());
}

bool AmdSecondaryGpu::mode_available(uint32_t connector, uint32_t width, uint32_t height) const {
  if (!driven(connector)) return false;
  for (uint32_t i = 0; i < display_->mode_count(); ++i)
    if (display_->mode(i)->timing.h_display == width && display_->mode(i)->timing.v_display == height) return true;
  return false;
}

uint32_t AmdSecondaryGpu::device_modes(uint32_t connector, MonitorMode* out, uint32_t capacity) const {
  if (!driven(connector) || !out) return 0;
  const ArrayView<MonitorMode> list(out, capacity);
  uint32_t n = 0;
  for (uint32_t i = 0; i < display_->mode_count() && n < list.size(); ++i) {
    const amd_secondary::Mode& m = *display_->mode(i);
    MonitorMode& o = list[n++];
    o = MonitorMode();
    o.width = m.timing.h_display;
    o.height = m.timing.v_display;
    o.refresh_millihz = m.refresh_millihz;
    o.interlaced = m.timing.interlaced;
    o.preferred = m.preferred;
  }
  return n;
}

bool AmdSecondaryGpu::set_display_mode(uint32_t connector, uint32_t width, uint32_t height) {
  if (!driven(connector)) return false;
  for (uint32_t i = 0; i < display_->mode_count(); ++i)
    if (display_->mode(i)->timing.h_display == width && display_->mode(i)->timing.v_display == height) return set_mode(i);
  return false;
}

}  // namespace hardware
