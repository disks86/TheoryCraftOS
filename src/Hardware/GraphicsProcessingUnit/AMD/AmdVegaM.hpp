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


#pragma once
#include <stdint.h>
#include "Allocators/IAllocator.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdAsicReset.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDce11.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGfx8Compute.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdGmc8.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIh3.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdIrq.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSdma3.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomBios.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdAux.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDdc.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdDpSource.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomConnectors.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/Dpcd.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AtomI2c.hpp"
#include "Hardware/GraphicsProcessingUnit/AMD/AmdSmu7Ucode.hpp"
#include "Hardware/Mmio.hpp"
#include "Hardware/RegisterMapper.hpp"
#include "Hardware/VideoCodec/AMD/AmdVcn1.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace hardware {

// Brings the parts of a Radeon RX Vega M up together, in the order of the Linux amdgpu driver's hw_init of the VI generation: the memory controller and the GART (AmdGmc8),
// the interrupt handler ring (AmdIh3), the engines' microcode through the SMC (AmdSmu7Ucode), the graphics engine with its compute queues (AmdGfx8, AmdGfx8Compute) and the
// SDMA engines (AmdSdma3); afterwards the display surface can be taken over (amd_dce11::Display). Each piece is the one tested on its own; this is the glue that gives them
// memory, firmware files and each other. It is opt-in (TCOS_AMD_VEGAM) because none of it has run on a real card: the pieces were written against simulated hardware, the
// start-up of a card that was left by the firmware but never driven by this OS may differ, and a wrong register on a real GPU can hang the machine.
//
// Memory: one block of video memory the CPU reaches through the memory BAR, at a fixed place in the visible part (kRegionOffset), clear of the firmware's framebuffer and of
// the video engine's memory (which sits at the end of the visible part, see AmdUvd6Plan): the dummy and scratch pages, the GART's page table, the microcode images and the SMC's
// scratch area, the graphics ring and its clear-state buffer, the compute queues, the SDMA rings, the interrupt ring and the OS's framebuffer. Everything is in video memory:
// the interrupt ring is told it is not snooped.
//
// A step that fails stops the ones before it, in reverse, and leaves the card as the firmware had it as far as the pieces' own stop() does that; status() says which step.
// How the CPU's pointers turn into the addresses the card is given for system memory (the page table entries of the GART). The OS keeps its memory mapped at its own
// addresses, so the default is the pointer's value; a platform that maps differently (and a test that needs addresses inside the card's 40 bits) supplies this.
class ISystemAddresses {
 public:
  virtual ~ISystemAddresses() = default;
  virtual uint64_t physical_address(const void* p) const = 0;
};

class AmdVegaM final : public amd_dce11::INativeDisplay, private AmdIrq::ISink {
 public:
  // Where the block starts in video memory, and the most the whole block may reach: the visible part of video memory is 256 MiB without Resizable BAR, and the video engine's
  // memory is in its last half.
  static constexpr uint64_t kRegionOffset = 48ull << 20;
  static constexpr uint64_t kRegionLimit = 128ull << 20;
  static constexpr uint64_t kPageBytes = 4096;

  struct Inputs {
    IVcnRegisters* registers = nullptr;           // the register BAR (BAR 5), by dword
    const IClock* clock = nullptr;
    filesystem::IFileSystem* file_system = nullptr;   // where /firmware is
    allocators::IAllocator* allocator = nullptr;
    IRegisterMapper* mapper = nullptr;            // maps the block of the memory BAR for the CPU
    uint16_t device_id = 0;
    uint64_t aperture_physical = 0;               // the memory BAR (BAR 0): where video memory shows for the CPU
    uint64_t aperture_bytes = 0;
    MmioRegion doorbells;                         // the doorbell BAR (BAR 2), at least the first 8 KiB
    uint64_t max_framebuffer_bytes = 0;           // the largest picture the display could show (0: no display part)
    uint32_t hpd_pins = 0x3F;                     // the hot plug detect pins to watch (bit n: pin n); which pin a connector is on is in the video BIOS, so by default all of them
    IPciBus* pci = nullptr;                       // where the card is, for the full reset (AmdAsicReset): its configuration space
    PciAddress pci_address{};
    ConstByteView vbios;                          // the card's video BIOS image, for POSTing it after a card reset (AtomBios; AmdVbios finds it); copied by bring_up. Empty: no POST
    bool allow_asic_reset = false;                // recover() falls back to a full reset of the card when an engine will not come back from its soft reset (read the warning at reset_card())
    bool auto_recover = false;                    // service_interrupts() resets an engine that a fault or a timeout left wedged (at most kMaxRecoveries times in all)
    bool msi = false;                             // the card's MSI is on (PciMsi): the interrupt ring is told to re-arm it as the driver does (adev->irq.msi_enabled)
    const ISystemAddresses* system_addresses = nullptr;   // for upload() and download(); null: a pointer is its own address
  };
  // The sizes of the pieces of the block, for a given microcode set, GART table and framebuffer.
  struct Layout {
    bool ok = false;
    uint64_t dummy_page = 0, scratch_page = 0, gart_table = 0, smu = 0, gfx = 0, compute = 0, sdma = 0, ih = 0, ib = 0, framebuffer = 0;   // offsets in the block
    uint64_t smu_bytes = 0, gart_table_bytes = 0, framebuffer_bytes = 0, total = 0;
  };
  static constexpr uint64_t kGfxBytes = AmdGfx8::kRingBytes + 4096 + 4096;   // the ring, the write-back page, the clear-state buffer
  static constexpr uint64_t kIhBytes = (AmdIh3::kRingBytes + AmdIh3::kWritebackBytes + kPageBytes - 1) / kPageBytes * kPageBytes;
  // The staging area for the command buffers the submit functions below take (one at a time), in dwords.
  static constexpr uint32_t kIbDwords = 2048;
  static constexpr uint64_t kIbBytes = kIbDwords * 4;
  static Layout plan(uint64_t smu_bytes, uint64_t gart_table_bytes, uint32_t compute_queues, uint64_t framebuffer_bytes);

  enum class Step : uint8_t { None, MemoryMap, Firmware, Memory, MemoryController, InterruptHandler, Microcode, GraphicsEngine, Sdma, Done };
  static const char* step_name(Step step);

  explicit AmdVegaM(allocators::IAllocator& allocator) : status_(allocator), vbios_(allocator) {}
  ~AmdVegaM() { shutdown(); }
  AmdVegaM(const AmdVegaM&) = delete;
  AmdVegaM& operator=(const AmdVegaM&) = delete;

  // Runs every step. True if all came up. The Config of each piece is its default (the Vega M's own values where the driver takes them from the video BIOS are guesses: see
  // AmdGfx8::Config).
  bool bring_up(const Inputs& inputs);
  bool up() const { return step_ == Step::Done; }
  Step step() const { return step_; }          // the last step that completed (Done: all); the one that failed is the next
  const char* status() const { return status_.c_str(); }
  // Stops what came up, in reverse.
  void shutdown();

  // The display part: takes the CRTC that shows the firmware's framebuffer over for a framebuffer of ours in the block. False if the block has no room for it or the display is
  // not what is expected (see amd_dce11::Display).
  bool engage_display(const amd_dce11::FirmwareFramebuffer& firmware, ByteView* memory) override;
  bool display_engaged() const override { return display_ && display_->engaged(); }
  void disengage_display() override;
  // The picture in system memory to the display's framebuffer through the SDMA engines (upload()).
  bool upload_picture(ConstByteView pixels, uint64_t offset) override { return display_ && display_->engaged() && upload(pixels, offset); }

  // Command buffers: the commands are copied into the block's staging area, padded the way the engine's driver pads (AmdGfx8::pad_ib, AmdSdma3::pad_ib), run as an indirect
  // buffer, and waited for by a fence. One at a time (the call returns when the engine is done). `commands` are PM4 packets for the graphics ring and the compute queues, SDMA
  // packets for the DMA engines; at most kIbDwords with the padding. False if the card is not up, the buffer does not fit, or the engine does not finish (see AmdGfx8::submit_ib).
  bool submit_graphics(ArrayView<const uint32_t> commands, uint32_t timeout_us = 100000);
  bool submit_compute(uint32_t queue, ArrayView<const uint32_t> commands, uint32_t timeout_us = 100000);
  bool submit_sdma(uint32_t instance, ArrayView<const uint32_t> commands, uint32_t timeout_us = 100000);

  // Copies between system memory and the block's framebuffer area (layout().framebuffer_bytes, the only part of the block meant for pictures and other data), through the
  // GART: the system pages are mapped into a window of kTransferPages GART pages (as amdgpu_ttm_gart_bind does, snooped system pages), and the SDMA engine `instance` copies
  // between the window and video memory, a window at a time. `offset` is in the framebuffer area. One call at a time; the call returns when the data has been copied. The
  // memory is addressed through Inputs::system_addresses (by default a buffer's address is the address the card is given: the OS runs with its memory mapped at its own addresses).
  // False if the card is not up, the range leaves the area, an address is beyond the card's 40 bits, or the engine does not finish (then the window stays mapped: the engine may
  // still be using the pages, and the caller must not free the memory).
  static constexpr uint32_t kTransferPages = 512;   // the first pages of the GART: 2 MiB a window
  bool upload(ConstByteView source, uint64_t offset, uint32_t instance = 0, uint32_t timeout_us = 100000);
  bool download(ByteView destination, uint64_t offset, uint32_t instance = 0, uint32_t timeout_us = 100000);

  // Interrupts: reads what the card posted to the interrupt ring and dispatches it by source (AmdIrq). A fault the engines report (a privileged register or instruction in a
  // command buffer, an SDMA illegal instruction) wedges the ring it came from, as the driver's fault handlers hand the ring to its scheduler's timeout handling; fences and
  // other events are counted. The card's interrupt line is not connected, so the caller polls this (the desktop loop does). Returns how many vectors were handled.
  uint32_t service_interrupts(uint32_t max_vectors = 64);
  const AmdIrq* irq() const { return irq_.get(); }
  // Everything the interrupt ring delivers is also passed to `observer` (not owned; null to stop), after the card has dealt with it: how the video engines, which run on their own,
  // learn of their fences. Called from service_interrupts().
  void set_event_observer(AmdIrq::ISink* observer) { observer_ = observer; }
  // Hot plug. The pins the card watches (Inputs::hpd_pins) report a display being plugged in or pulled out through the interrupt ring (AmdIrq); service_interrupts() turns that into
  // events for whoever wants to react, queued here (the oldest are dropped when more than kMaxHotplugEvents wait, and counted). `connected_pins()` is the state: bit n set when pin n
  // senses a display, as of bring-up and the events since. Which connector a pin is on is in the video BIOS's object table (AtomConnectors), read at bring-up when Inputs::vbios is set:
  // an event carries the connector, so that it can say "HDMI-A-1" and not "output 2"; without a video BIOS, or for a pin the table does not list, `has_connector` is false.
  struct HotplugEvent {
    uint32_t pin = 0;
    bool connected = false;
    bool has_connector = false;
    AtomConnectors::Connector connector;
    bool throttled = false;   // the pin interrupted so often that it was switched off (AmdIrq::kHotplugStormLimit): no more events from it
  };
  static constexpr uint32_t kMaxHotplugEvents = 16;
  bool take_hotplug(HotplugEvent* out);
  uint32_t connected_pins() const { return connected_pins_; }
  // The monitor's EDID, into *out: over the connector's DDC lines (AmdDdc: the GPIO pins of the bus named by the connector's I2C record in the video BIOS, driven by software), or for a
  // DisplayPort or eDP connector over the AUX channel (AmdAux: I2C over AUX, the video BIOS's ProcessAuxChannelTransaction). False if the
  // card is not up, the connector has no DDC line in the table or no GPIO entry, nothing answers, or the EDID never checked. (An eDP panel that is not powered does not answer: the panel power is not done.)
  // It takes the bus for some tens of milliseconds, so it is for the desktop loop, not an interrupt path.
  bool read_edid(const AtomConnectors::Connector& connector, ByteBuffer* out);
  // The DisplayPort configuration data of the sink on a DisplayPort or eDP connector, read over the AUX channel (the same way, with the same eDP panel power-up, as read_edid): the receiver's
  // capabilities, and for `status` (optional) the link status. False if the connector is not a DisplayPort one, nothing answers, or what answers is no DPCD. Like read_edid it takes the AUX
  // channel for some milliseconds, so it is for the desktop loop (or the handler of a hot plug), not an interrupt path.
  bool read_dpcd(const AtomConnectors::Connector& connector, Dpcd::Capabilities* capabilities, Dpcd::LinkStatus* status = nullptr);
  // DisplayPort link training of a DisplayPort or eDP connector's link through the video BIOS (amdgpu_atombios_dp_link_train): the sink's capabilities are read (the eDP panel powered up
  // first), the lane count and rate chosen for the mode (DpLinkConfig::choose: `pixel_clock_khz` at `bpc` bits per colour), and the link trained by DpLinkTrainer with the connector's DIG
  // encoder and UNIPHY transmitter driven by AmdDpSource. `out` gets what was chosen and the outcome whether or not it worked. False if the connector is not a DisplayPort one with an
  // AUX channel and a DIG encoder of this card (0 to 5), nothing answers, or the training fails. It takes the AUX channel and the video BIOS for as long as the sink needs (tens of
  // milliseconds), so it is for the desktop loop. Not done: setting up the PLL and the encoder for the mode first (the training runs on the clocks the firmware left), and the stream.
  struct LinkTraining {
    Dpcd::Capabilities caps;
    DpLinkConfig config;
    DpLinkTrainer::Outcome outcome;
  };
  bool train_link(const AtomConnectors::Connector& connector, uint32_t pixel_clock_khz, uint32_t bpc, LinkTraining* out);
  // The short pulse of a DisplayPort sink (a status change that is not a plug: the link was lost, or the sink asks for a test) is taken by the interrupt handler as a pin to look at
  // (`pending_short_pulses()`: bit n for pin n). The look is the AUX channel's, which takes some milliseconds, so it is not done there: service_short_pulses() does it, from the desktop loop.
  // For each pending pin, lowest first, up to `capacity` of them, it reads the sink's link configuration (DP_LANE_COUNT_SET) and status (the lane training status, the alignment, the service
  // interrupt vector) and reports what a driver's short pulse handler decides on: the link is lost when it was set up for lanes and does not pass drm_dp_channel_eq_ok for them (retrain),
  // an automated test request is made when the sink asks for one. The returned events are in `out`; pins past `capacity` stay pending. Not done: the retraining.
  struct SinkEvent {
    uint32_t pin = 0;
    bool has_connector = false;
    AtomConnectors::Connector connector;
    bool checked = false;             // the sink's status was read (the pin is a DisplayPort or eDP connector of the video BIOS's table with an AUX channel)
    bool read_failed = false;         // checked, but the AUX channel did not answer (the sink is gone or asleep)
    uint32_t lanes = 0;               // DP_LANE_COUNT_SET: 0 if the link has not been trained
    Dpcd::LinkStatus status;
    bool link_lost = false;           // lanes were set up and the status says they are not in sync
    bool test_request = false;        // DP_AUTOMATED_TEST_REQUEST in the service interrupt vector
    bool content_protection = false;  // DP_CP_IRQ
    bool sink_specific = false;       // DP_SINK_SPECIFIC_IRQ
    // What was done about it. The handled service interrupts (Dpcd::kIrqHandled) are acknowledged first, as the driver does; an automated test request is read (DP_TEST_REQUEST) and
    // answered (DP_TEST_RESPONSE): the EDID read test with the checksum of the last block of the sink's EDID, every other test with a NAK.
    uint8_t acked = 0;                // the bits written back to DP_DEVICE_SERVICE_IRQ_VECTOR
    uint8_t test_kind = 0;            // DP_TEST_REQUEST
    uint8_t test_response = 0;        // DP_TEST_RESPONSE as written (Dpcd::kTestAck | kTestEdidChecksumWrite, or kTestNak); 0 if none was given
    bool handling_failed = false;     // a read or write of the above did not go through
  };
  uint32_t pending_short_pulses() const { return rx_pending_; }
  uint32_t service_short_pulses(SinkEvent* out, uint32_t capacity);
  ArrayView<const AtomConnectors::Connector> connectors() const { return ArrayView<const AtomConnectors::Connector>(connectors_, connector_count_); }
  uint32_t hotplug_events_dropped() const { return hotplug_dropped_; }
  // The audio endpoints of the display engine (amd_dce11::AudioPins, dce_v11_0_audio_get_connected_pins): bit n of the mask is set when endpoint n has a port (its default configuration's
  // port connectivity is not "none"), as read at bring-up and by refresh_audio_pins(). Which output an endpoint serves is not fixed (the driver hands an encoder the first connected one), so
  // this says how many displays could have audio, not which. `audio_outputs` is the other side of it: the connectors that carry audio (HDMI, DisplayPort) and have a display on their hot plug
  // pin now, from the video BIOS's table (up to `capacity`, in the table's order; the count).
  static constexpr uint32_t kAudioPins = amd_dce11::AudioPins::kMaxPins;
  uint32_t audio_pins_connected() const { return audio_pins_; }
  uint32_t refresh_audio_pins();
  uint32_t audio_outputs(AtomConnectors::Connector* out, uint32_t capacity) const;
  // The endpoint the driver would give the connector's audio: the connector's encoder (from the video BIOS's display path) is a DIG with an audio format block (AtomConnectors::afmt_instance:
  // HDMI and DisplayPort, not the internal panel), and dce_v11_0_audio_get_pin hands it the first connected endpoint. False if the connector has no such encoder, or no endpoint is connected.
  // (The same endpoint for every connector: the driver's choice, which does not tell two displays apart.)
  bool audio_pin_for(const AtomConnectors::Connector& connector, uint32_t* pin) const;
  // dce_v11_0_afmt_audio_select_pin: the endpoint audio_pin_for gives the connector goes into its audio format block's AFMT_AUDIO_SRC_CONTROL (amd_dce11::AudioFormat), after the endpoints
  // are read again (refresh_audio_pins) as the driver does each time. `select_audio_pin` is for one connector; false if the connector has no audio block (HDMI and DisplayPort on a DIG
  // of 0 to 5), no endpoint is connected, or the card is not up. `select_audio_pins` does it for every audio connector with a display on its pin now; the count. Like the driver, it only
  // writes the select field: enabling the endpoint (the hot plug control's AUDIO_ENABLED), the ELD and the audio clock are not done.
  bool select_audio_pin(const AtomConnectors::Connector& connector);
  uint32_t select_audio_pins();
  // The mode the audio is set up for (dce_v11_0_afmt_setmode's drm_display_mode and the controller and depth of the encoder's crtc): the pixel clock for the audio clock and the N and CTS, the
  // timing for the AVI InfoFrame, the controller whose timing the audio clock follows, and the bits per colour component (8, or 10 and 12 for deep colour). The default is 1920x1080 at 60 Hz.
  struct AudioMode : HdmiMode {
    uint32_t crtc = 0;
    uint32_t bpc = 8;
    AudioMode() {
      clock_khz = 148500;
      hdisplay = 1920;
      vdisplay = 1080;
      htotal = 2200;
      vtotal = 1125;
    }
  };
  // dce_v11_0_afmt_setmode, with the audio parts of it all: the endpoint of the connector (audio_pin_for) is disabled; the audio clock, the packets in the blanking and the audio clock
  // regeneration are set up for the mode (amd_dce11::AudioFormat::setup_packets); the endpoint gets the monitor's speaker allocation, all eight channels are enabled, the connector's audio format
  // block selects the endpoint (select_audio_pin), the endpoint gets its Short Audio Descriptors and its lip sync latency (the mode's: progressive or interlaced); the AVI InfoFrame of the mode is
  // sent and the audio samples with it (AudioFormat::start); and the endpoint is enabled (HOT_PLUG_CONTROL.AUDIO_ENABLED). The hardware makes the ELD that the sound driver reads from the
  // endpoint's registers. `edid` is the whole EDID of the monitor (a missing or CTA-less one gives stereo speakers and no descriptors, as the driver does). The variant without an EDID reads it
  // over the connector's DDC or AUX channel (read_edid) and is false if that fails. False also when the connector has no audio block, no endpoint is connected, or the mode's controller does not
  // exist: nothing is written then. Not done: an audio source - the audio samples come from the sound driver through the Azalia controller, which is not set up here.
  bool enable_audio(const AtomConnectors::Connector& connector, ConstByteView edid, const AudioMode& mode = AudioMode());
  bool enable_audio(const AtomConnectors::Connector& connector, const AudioMode& mode = AudioMode());
  // dce_v11_0_audio_enable(pin, false) for the endpoint the connector's block has selected. False if the connector has no audio block.
  bool disable_audio(const AtomConnectors::Connector& connector);

  // Recovery. An engine that timed out on a fence or reported a fault is wedged: nothing more is queued on it. recover() resets the ones that are - the graphics engine with its
  // compute queues (AmdGfx8::recover) if the graphics ring or a compute queue is wedged, and the SDMA engines (AmdSdma3::recover) if one is - and brings them back with empty
  // rings. A fault of one compute queue resets all of them, as the driver's soft reset of the IP does. `failed` is set if an engine did not come back (its ring test failed): it is
  // then stopped; recover() tries it again (an engine that is not running counts as needing it), and if that does not help either the card needs a full restart (shutdown and
  // bring_up). Nothing is done for engines that are running and not wedged.
  struct Recovery {
    bool graphics = false, sdma = false, failed = false;
    bool card_reset = false;   // the soft reset did not help and the whole card was reset (reset_card); `failed` then says whether it is up again
  };
  Recovery recover();
  // The full reset of the card (AmdAsicReset: the PCI configuration reset the driver falls back to): everything is stopped, the card is reset, and bring_up is run again with the same
  // inputs. True if the card is up again. WARNING: the driver then POSTs the card (the video BIOS's init tables, through the AtomBIOS interpreter) to train the memory and set the
  // clocks up, and this OS has no interpreter for them. A real card that has been reset therefore comes back quiet (no engine, no DMA) but not usable, and bring_up is expected to
  // fail until the machine is restarted; the video memory, including the firmware's framebuffer, is gone with it. Needs Inputs::pci. At most kMaxAsicResets in all.
  bool reset_card();
  // The POST (amdgpu_device_asic_init): runs the video BIOS's ASIC_Init command table with the AtomBIOS interpreter, which trains the memory and sets the clocks up. Done by
  // reset_card() after the reset when Inputs::vbios holds the image; false (and status() says why) if the image is not an AtomBIOS image or its table fails. Can also be called by hand
  // on a card the firmware did not POST (with the same inputs bring_up takes).
  bool post(const Inputs& inputs);
  uint32_t asic_resets() const { return asic_resets_; }
  static constexpr uint32_t kMaxAsicResets = 2;
  bool wedged() const;   // any engine is, or was left stopped by a recovery that did not work
  uint32_t recoveries() const { return recoveries_; }
  static constexpr uint32_t kMaxRecoveries = 8;   // what service_interrupts() will do by itself in one life of the card (a card that keeps hanging is left alone)
  // The vertical blank interrupt of the controller that shows the OS's framebuffer (it comes every frame, so it is off until asked for): counted per controller in irq()->vblank_count().
  // False if the display is not engaged. It is switched off again when the display is given back or the card stopped.
  bool set_vblank_interrupt(bool enable);
  // The vertical line interrupt of the same controller (LB_INTERRUPT_MASK.VLINE_INTERRUPT_MASK, as dce_v11_0_set_crtc_vline_interrupt_state does; the line it comes at is whatever the
  // controller has: the driver programs none either), counted in irq()->vline_count(). Same rules as the vblank interrupt: false if the display is not engaged, off again when the
  // display is given back or the card stopped, and it follows the controller if the display is engaged again on another.
  bool set_vline_interrupt(bool enable);

  // For tests and diagnostics
  const AmdGmc8GartPlan& gart_plan() const { return gart_plan_; }
  const Layout& layout() const { return layout_; }
  AmdGfx8* graphics() { return gfx_.get(); }
  AmdSdma3* sdma() { return sdma_.get(); }
  AmdIh3* interrupts() { return ih_.get(); }
  AmdGmc8* memory_controller() { return gmc_.get(); }

 private:
  bool fail(Step failed, const char* text);
  void on_event(const AmdIrq::Event& event) override;
  bool transfer(uint64_t system_address, uint64_t bytes, uint64_t offset, bool to_video, uint32_t instance, uint32_t timeout_us);

  Step step_ = Step::None;
  String status_;
  ByteBuffer vbios_;   // the copy of Inputs::vbios
  Inputs inputs_;
  AmdGmc8GartPlan gart_plan_;
  Layout layout_;
  MmioRegion block_;
  uint64_t block_gpu_ = 0;
  UniquePointer<AmdGmc8> gmc_;
  UniquePointer<AmdIh3> ih_;
  UniquePointer<AmdIrq> irq_;
  UniquePointer<amd_dce11::Interrupts> display_irq_;
  int vblank_crtc_ = -1, vline_crtc_ = -1;
  uint32_t recoveries_ = 0, asic_resets_ = 0;
  HotplugEvent hotplug_[kMaxHotplugEvents];
  AtomConnectors::Connector connectors_[AtomConnectors::kMaxConnectors];
  uint32_t connector_count_ = 0;
  void power_up_panel(const AtomConnectors::Connector& connector, IAtomHost& host, AtomBios& bios);   // an eDP panel's power, before its AUX channel is used
  uint32_t audio_pins_ = 0;
  uint32_t rx_pending_ = 0;
  uint32_t hotplug_head_ = 0, hotplug_count_ = 0, hotplug_dropped_ = 0, connected_pins_ = 0, throttle_reported_ = 0;
  AmdIrq::ISink* observer_ = nullptr;
  UniquePointer<AmdGfx8Compute> compute_;
  UniquePointer<AmdGfx8> gfx_;
  UniquePointer<AmdSdma3> sdma_;
  UniquePointer<amd_dce11::Display> display_;
  uint64_t cpu_base_ = 0, gpu_base_ = 0;
};

}  // namespace hardware
