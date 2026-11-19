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
#include "Allocators/IAllocator.hpp"
#include "Hardware/AudioDevice/AudioCaptureBase.hpp"
#include "Hardware/AudioDevice/AudioDeviceBase.hpp"
#include "Hardware/Bus/Pci/IPciBus.hpp"
#include "Hardware/Bus/Pci/PciFunctionPower.hpp"
#include "uefi.hpp"

namespace hardware {

// The controller's registers by byte offset from its base: memory-mapped on the real machine (PhysicalHdaRegisters), simulated in a host test.
class IHdaRegisters {
 public:
  virtual uint8_t read8(uint32_t reg) = 0;
  virtual uint16_t read16(uint32_t reg) = 0;
  virtual uint32_t read32(uint32_t reg) = 0;
  virtual void write8(uint32_t reg, uint8_t value) = 0;
  virtual void write16(uint32_t reg, uint16_t value) = 0;
  virtual void write32(uint32_t reg, uint32_t value) = 0;

 protected:
  IHdaRegisters() = default;
  ~IHdaRegisters() = default;
};

// The registers of a controller whose memory BAR is at the physical (identity mapped) address `base`: the one place the volatile accesses are made.
class PhysicalHdaRegisters final : public IHdaRegisters {
 public:
  void set_base(uint64_t base) { base_ = base; }
  uint8_t read8(uint32_t reg) override;
  uint16_t read16(uint32_t reg) override;
  uint32_t read32(uint32_t reg) override;
  void write8(uint32_t reg, uint8_t value) override;
  void write16(uint32_t reg, uint16_t value) override;
  void write32(uint32_t reg, uint32_t value) override;

 private:
  uint64_t base_ = 0;
};

// An Intel High Definition Audio controller (QEMU's `intel-hda`, and the audio
// of virtually every PC chipset since 2004) with the first codec that answers.
// The driver talks to the codec through the CORB/RIRB command rings, finds an
// output pin (line out, speaker or headphones) and the DAC behind it, unmutes the
// path and plays through output stream 0 from one cyclic buffer described by a
// buffer descriptor list. Recording works the same way backwards: an input pin
// (microphone or line in), the converter that can reach it and input stream 0. Polled: no interrupts. Any vendor's controller that
// follows the specification (PCI class 0x0403) is used.
//
// That includes the audio function of a graphics card, whose codec is an HDMI / DisplayPort one: its pins are digital outputs, and one of them carries sound once the display
// driver has enabled its endpoint for a connected display (AmdVegaM::enable_audio: the pin then reports presence detect and a valid ELD). The output pin is chosen among those:
// a line out, speaker or headphone pin first as always, then a digital pin with a display behind it, then any output pin. For a digital pin the converter's digital output is
// enabled, the channel count set and the audio InfoFrame sent through the codec's DIP buffer. retarget() chooses again when displays come and go.
//
// Runtime power (after Linux's HD-audio runtime PM): while no audio is queued and nothing has been written for a while the controller can be
// suspended (its streams and command rings stopped, the codec's function group in D3, the link held in reset, the PCI function in D3hot) and
// is resumed by the next write() with sound in it (D0, the header restored, the link and the rings started again, the codec rediscovered and
// the path, InfoFrame, stream and volume programmed again). The idle policy runs from service(), called every few hundred milliseconds
// (SystemAudioDevices::poll does). An observer (set_power_observer) is told when the function goes idle and busy: the GPU manager lets the
// GPU that carries an HDMI audio function go to D3cold only while that function is idle. Thread safe: power transitions are serialized by
// power_mutex_, and the playback path's lock (mutex_) guards the flag that says the registers may be touched, so the mixer's thread never
// reads a suspended controller. A controller whose capture side is running is never suspended (recording has no "open" to wake it with).
class IntelHdaAudioDevice final : public RingAudioDevice {
 public:
  static constexpr uint32_t kEntries = 8;
  static constexpr uint32_t kEntryBytes = 8192;
  static constexpr uint32_t kRingBytes = kEntries * kEntryBytes;
  static constexpr uint8_t kStreamTag = 1;
  static constexpr uint8_t kCaptureStreamTag = 2;

  IntelHdaAudioDevice(const IPciBus& pci, const PciDeviceInfo& device, allocators::IAllocator& allocator,
                      efi::SystemTable* st)
      : pci_(&pci), device_(device), allocator_(allocator), st_(st) {}

  // A controller that is not behind the PCI bus code (a host test's simulated one, or a bus-less machine): the registers by the interface, nothing detached from the firmware.
  IntelHdaAudioDevice(IHdaRegisters& registers, allocators::IAllocator& allocator)
      : pci_(nullptr), allocator_(allocator), st_(nullptr), external_(&registers) {}
  // The same, with the PCI function's configuration space for runtime power (D3hot and the saved header): a host test's simulated function.
  IntelHdaAudioDevice(IHdaRegisters& registers, allocators::IAllocator& allocator, const IPciBus& pci, PciAddress function)
      : pci_(&pci), allocator_(allocator), st_(nullptr), external_(&registers) {
    device_.address = function;
  }

  const char* name() const override { return "Intel High Definition Audio"; }
  bool initialize() override;
  // The output goes to another pin: the codec is asked again which digital pin has a display (or analog pin has a jack), the old pin is silenced and the new one set up for the running
  // stream. False if the device is not running or no pin is left (then it stays silent).
  bool retarget() override;
  // The pin and converter the output goes through, and whether the pin is a digital (HDMI or DisplayPort) one with a display behind it (presence detect and a valid ELD)
  uint8_t output_pin() const { return path_length_ ? path_[0].nid : 0; }
  uint8_t output_converter() const { return path_length_ ? path_[path_length_ - 1].nid : 0; }
  bool output_is_digital() const { return digital_; }
  bool output_has_display() const { return display_present_; }
  void shutdown() override;
  IAudioCapture* capture() override { return capture_.active() ? &capture_ : nullptr; }
  // Wakes a suspended controller first when `pcm` has sound in it (a stream opened): the caller may wait for the resume (and the GPU behind it).
  uint32_t write(ConstByteView pcm) override;

  // --- Runtime power
  enum class PowerState : uint8_t { Active, Suspended, Failed };   // Failed: a resume did not bring the controller back; it plays nothing
  static constexpr uint32_t kDefaultIdleSuspendMs = 5000;
  // Stops the hardware (see above). False, and nothing changes, if the device is not running, audio is still queued or the capture side runs.
  // True if it is suspended (also when it already was).
  [[nodiscard]] bool suspend();
  // Brings a suspended controller back; true if it is running (also when it was not suspended).
  [[nodiscard]] bool resume();
  // The idle policy: suspends once nothing has been written for idle_suspend_ms() (measured on `now_ms`, any monotonic clock in milliseconds).
  void service(uint64_t now_ms);
  // 0 turns the idle policy off (the default until an observer is set; suspend() still works).
  void set_idle_suspend_ms(uint32_t milliseconds);
  uint32_t idle_suspend_ms() const { return __atomic_load_n(&idle_ms_, __ATOMIC_RELAXED); }
  // `observer` (null: none) learns of the function's idle / busy changes from now on; when the device is suspended at that moment it is told
  // function_idle() at once. A non-null observer also turns the idle policy on (`idle_ms`) unless it was set already. The observer must
  // outlive the device or be replaced by null first.
  void set_power_observer(IPciFunctionPowerObserver* observer, uint32_t idle_ms = kDefaultIdleSuspendMs);
  PowerState power_state() const { return static_cast<PowerState>(__atomic_load_n(&power_state_, __ATOMIC_ACQUIRE)); }
  // A display came or went on the GPU this controller belongs to (Multi/GpuHotplug: the GPU's hot-plug detection was dark while it slept, and
  // the codec's unsolicited responses are not used here). The codec's pins are read again (presence detect and ELD valid) and the output moves
  // to the pin with a display, as retarget() does; a suspended controller is resumed for it (its resume rediscovers the codec) and goes idle
  // again after the idle period. True if it runs afterwards with an output pin; false when it has failed or was never running.
  [[nodiscard]] bool connections_changed();
  uint32_t connection_changes() const { return __atomic_load_n(&connection_changes_, __ATOMIC_RELAXED); }
  // The PCI function (zero for a controller given by its registers alone).
  PciAddress pci_address() const { return device_.address; }
  // How many suspends and resumes there were (statistics, tests).
  uint32_t suspend_count() const { return __atomic_load_n(&suspends_, __ATOMIC_RELAXED); }
  uint32_t resume_count() const { return __atomic_load_n(&resumes_, __ATOMIC_RELAXED); }

 private:
  struct BufferDescriptor {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
  };
  // One node on the way from the output pin to the converter.
  struct Hop {
    uint8_t nid;
    uint8_t select;       // index of the connection that leads on towards the converter
    uint8_t connections;  // how many connections the node has
  };
  struct Amplifier {
    bool present = false;
    bool can_mute = false;
    uint8_t steps = 0;    // gain values run 0 .. steps
    uint8_t offset = 0;   // the gain value for 0 dB
  };
  static constexpr uint32_t kMaxPath = 5;

  // The recording side: its own path through the codec, stream descriptor, buffer list and ring.
  class Capture final : public RingAudioCapture {
   public:
    explicit Capture(IntelHdaAudioDevice& owner) : owner_(owner) {}
    const char* name() const override { return "Intel HD Audio input"; }
    bool start();
    void stop();
    bool active() const { return running_; }

   private:
    uint32_t hardware_position() override;
    bool hardware_gain() const override;
    void apply_gain() override;
    IntelHdaAudioDevice& owner_;
    ArrayView<BufferDescriptor> descriptors_;
    Hop path_[kMaxPath] = {};
    uint32_t path_length_ = 0;
    uint32_t pin_control_ = 0x20;
    Amplifier amp_;       // the input amplifier that sets the level
    uint8_t amp_nid_ = 0;
    uint8_t amp_index_ = 0;
    uint32_t stream_ = 0;  // register offset of input stream 0's descriptor
    bool owns_stream_ = false;
  };

  uint8_t read8(uint32_t reg) const { return regs_->read8(reg); }
  uint16_t read16(uint32_t reg) const { return regs_->read16(reg); }
  uint32_t read32(uint32_t reg) const { return regs_->read32(reg); }
  void write8(uint32_t reg, uint8_t value) const { regs_->write8(reg, value); }
  void write16(uint32_t reg, uint16_t value) const { regs_->write16(reg, value); }
  void write32(uint32_t reg, uint32_t value) const { regs_->write32(reg, value); }
  bool wait_register(uint32_t reg, uint32_t mask, uint32_t value, uint32_t timeout_us) const;

  bool reset_controller();
  [[nodiscard]] bool suspend_locked();          // (power_mutex_ held)
  [[nodiscard]] bool resume_locked();
  [[nodiscard]] bool retarget_locked();          // (power_mutex_ held)
  void wake_function();                         // observer busy, D0, the saved header back (power_mutex_ held)
  bool hardware_usable() const { return !__atomic_load_n(&hw_suspended_, __ATOMIC_ACQUIRE); }
  void set_hardware_usable(bool usable) { __atomic_store_n(&hw_suspended_, !usable, __ATOMIC_RELEASE); }   // (mutex_ held)
  void set_power_state(PowerState s) { __atomic_store_n(&power_state_, static_cast<uint8_t>(s), __ATOMIC_RELEASE); }
  void refresh() override;
  bool start_command_rings();
  bool verb(uint8_t nid, uint32_t command, uint32_t* response);
  bool get_parameter(uint8_t nid, uint8_t parameter, uint32_t* response);
  bool find_output_path();
  bool search_converter(uint8_t nid, uint32_t depth);
  uint32_t connections(uint8_t nid, uint8_t* out, uint32_t capacity);
  Amplifier amplifier(uint8_t nid, uint8_t parameter);
  bool configure_path();
  void silence_path();                                  // the pin, converter and stream of the current path switched off
  bool pin_is_digital(uint8_t nid, uint32_t pin_caps) const;
  uint32_t pin_sense(uint8_t nid);                      // GET_PIN_SENSE: presence detect (bit 31) and ELD valid (bit 30)
  void send_audio_infoframe(uint8_t pin);
  bool pick_pin(uint8_t wanted, int sense_level);       // find_output_path's search for one kind of pin (sense_level: 2 needs a display and a valid ELD, 1 a display, 0 neither)
  bool run_stream(uint32_t stream, ArrayView<BufferDescriptor>& descriptors, uint64_t ring_address, uint8_t tag);
  bool search_pin(uint8_t nid, uint32_t depth, uint8_t wanted_device, Hop* path, uint32_t* length, uint8_t* device);
  bool dma_reachable(uint64_t address, uint64_t bytes) const;

  void apply_volume() override;
  uint32_t hardware_position() override;

  const IPciBus* pci_;
  PciDeviceInfo device_ = {};
  allocators::IAllocator& allocator_;
  efi::SystemTable* st_;

  IHdaRegisters* external_ = nullptr;      // the registers given to the constructor, or null
  PhysicalHdaRegisters physical_;
  IHdaRegisters* regs_ = nullptr;         // whichever of the two
  uint64_t registers_ = 0;
  uint32_t stream_ = 0;  // register offset of output stream 0's descriptor
  bool dma64_ = false;
  uint32_t input_streams_ = 0;
  uint8_t codec_ = 0;

  ByteView corb_, rirb_;
  uint32_t corb_entries_ = 0, rirb_entries_ = 0;
  uint32_t corb_write_ = 0, rirb_read_ = 0;
  ArrayView<BufferDescriptor> descriptors_;

  uint8_t function_group_ = 0;
  uint32_t first_widget_ = 0, widget_count_ = 0;
  kernel::Mutex codec_mutex_;  // one command at a time on the CORB/RIRB, from the playback and capture sides
  uint32_t widget_caps_[256] = {};  // audio widget capabilities by node id (0 = no such widget)
  Hop path_[kMaxPath] = {};
  uint32_t path_length_ = 0;
  uint32_t pin_control_ = 0x40;
  bool pin_has_eapd_ = false;
  bool digital_ = false;                  // the output pin is an HDMI or DisplayPort one
  bool display_present_ = false;          // and had presence detect and a valid ELD when it was chosen
  bool controller_owned_ = false;         // the controller is programmed by this object (and must be stopped by it)
  Amplifier volume_amp_;
  uint8_t volume_nid_ = 0;
  Capture capture_{*this};
  bool owns_device_ = false;

  // Runtime power. Lock order: power_mutex_, then mutex_ (the playback side's), then codec_mutex_.
  kernel::Mutex power_mutex_;              // one transition at a time; also held by retarget() and shutdown()
  uint8_t power_state_ = 0;                // PowerState (atomic loads and stores; changed under power_mutex_)
  bool hw_suspended_ = false;              // the registers must not be touched (changed under mutex_ and power_mutex_; atomic)
  IPciFunctionPowerObserver* observer_ = nullptr;   // (power_mutex_)
  uint32_t idle_ms_ = 0;                   // (atomic)
  bool idle_ms_set_ = false;               // (power_mutex_)
  uint64_t activity_ = 0;                  // bumped by every write() with sound in it (atomic)
  uint64_t seen_activity_ = 0;             // (power_mutex_) what service() saw last
  uint64_t last_active_ms_ = 0;            // (power_mutex_) when service() last saw activity
  bool timer_started_ = false;             // (power_mutex_) last_active_ms_ is set
  PciSavedHeader saved_header_;            // (power_mutex_) the function's header from the last suspend
  uint32_t suspends_ = 0, resumes_ = 0;    // (atomic)
  uint32_t connection_changes_ = 0;        // (atomic) connections_changed() calls that re-read the pins
};

}  // namespace hardware
