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

// The display engine of NVIDIA GPUs from Tesla to Turing, programmed directly (nouveau's nvkm/engine/disp + dispnv50), for the first thing
// worth doing with it: showing a framebuffer of the OS's own in video memory in the mode the firmware lit the screen in (the counterpart of
// the AMD Raven "swap the surface address" takeover, AmdDcn1.hpp).
//
// The display engine is driven through "EVO" DMA channels: a push buffer of method/data words the engine fetches, a PUT pointer the driver
// writes and a GET pointer the engine advances. Methods land in the channel's assembly state; an UPDATE method makes them the armed state
// (what is shown) and, when asked, writes a notifier in memory. Objects named in methods (the context DMAs that say which memory a surface
// or notifier is in) are looked up through a hash table (RAMHT) in the display's instance memory.
//
//   Tesla (NV50..GT21x and GF100..GF110, class 507d family): core channel; a head's surface is a core channel method.
//   GF119..Pascal (907d family): the same with other method numbers and a linear readback of the armed state at 0x640000.
//   Volta/Turing (c37d/c57d + c37e/c57e): the surface belongs to a window channel owned by a head; the core channel does the rest.
//   GA102..GA107 (c67d + c67e): Turing's channels and registers (nouveau's ga102_disp is tu102_disp_init + gv100's channels, without GSP),
//   other SOR link and clock registers (NvDisplayMode). Driven only from scratch (NvidiaSecondaryGpu): the takeover refuses it.
//   Ada and later: nouveau has no GSP-less display path for them (ad102 is r535-only): refused.
//
// Taking over (claim) follows *_disp_init: the capability mirrors, the handshake that takes the display from the VBIOS, the instance memory
// with the RAMHT and the context DMAs, the core channel (and on Volta+ the window channel that shows the firmware's picture), no interrupts
// (the kernel takes none from the GPU yet; completion is polled). The firmware's surface is read back from the armed state BEFORE the
// handshake, and only the surface methods are sent: the timings, the output resource and the clocks are left as the firmware armed them.
//
// Every register and method number is in NvDisplayHw.hpp, with how sure it is. Nothing here has run on real hardware: the host tests drive
// it against a model of the display engine that decodes the push buffers (tests/NvDisplayTest.cpp).
#pragma once
#include <stdint.h>
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvChip.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvMemory.hpp"
#include "Hardware/GraphicsProcessingUnit/NVIDIA/Nvkm/NvRegisters.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {
namespace nv {

enum class DisplayClass : uint8_t {
  None,       // not a GPU with a display this driver knows (pre-Tesla, unknown)
  Nv50,       // 507d family: Tesla and GF100..GF110
  Gf119,      // 907d family: GF119/GF117, Kepler, Maxwell, Pascal
  Gv100,      // c37d + c37e
  Tu102,      // c57d + c57e
  Ga102,      // c67d + c67e: GA102..GA107 (GA100 has no display)
  Gsp,        // Ada and later (and Hopper/Blackwell): refused
};
DisplayClass display_class(const Chip& chip);
const char* display_class_name(DisplayClass display_class);

enum class DisplayStatus : uint8_t {
  Ok,
  NotNative,          // the chip's display is not one this driver programs (Chip::display_native false, unknown family)
  NeedsGsp,           // Ada and later (and the takeover on GA102)
  NoHeads,            // the display reports no heads
  NoMemory,           // instance memory, push buffer, notifier or framebuffer could not be had (or the CPU cannot see it)
  OwnershipTimeout,   // the VBIOS did not let go of the display
  ChannelTimeout,     // a channel did not come up (or go down)
  PushTimeout,        // the engine did not fetch the push buffer (GET never reached PUT)
  NotifierTimeout,    // the UPDATE's notifier never said done
  NotLatched,         // after the UPDATE the armed state does not hold what was sent
  NoFirmwareSurface,  // no head shows a surface of the firmware's geometry
  UnsupportedSurface, // the head's surface is not linear 32-bit, or its geometry is out of range
  BadArgument,
  NotClaimed,         // a channel operation before claim()
  NotImplemented,     // a step this driver does not do for this generation
  LinkTraining,       // DisplayPort link training failed at every link configuration that carries the mode
  NoOutput,           // the DCB output cannot be routed (no OR or head it may use), or is of a type not driven here
};
const char* display_status_text(DisplayStatus status);

// What a head (pre-Volta) or the window a head owns (Volta+) scans out.
struct ScanoutSurface {
  uint64_t offset = 0;              // VRAM byte offset, a multiple of 256
  uint32_t width = 0, height = 0;
  uint32_t pitch_bytes = 0;
  uint8_t format = 0;               // disp_hw::kFormat* code
  bool pitch_linear = false;
  uint32_t context_dma = 0;         // the ISO context DMA handle the offset is relative to (0 when read back from a firmware we cannot see into)
  uint32_t point_in = 0;            // the viewport (pre-Volta) / window (Volta+) source point, y << 16 | x, as read back
  uint32_t size_in = 0, size_out = 0;   // Volta+: the window's source and destination size, as read back (sent unchanged)
  bool valid() const { return width != 0 && height != 0 && pitch_bytes != 0; }
  bool linear_32bpp() const;
};

struct DisplayTimeouts {
  uint32_t attempts = 200000;       // reads of a status before giving up
  uint32_t poll_us = 10;            // pause between them (2 s in all, nouveau's nvkm_msec(2000))
};

// A list of methods for one channel, sent as one batch (consecutive methods share one header).
class MethodList {
 public:
  static constexpr uint32_t kCapacity = 96;
  [[nodiscard]] bool add(uint32_t method, uint32_t value);
  uint32_t size() const { return count_; }
  uint32_t method(uint32_t index) const { return method_[index]; }
  uint32_t value(uint32_t index) const { return value_[index]; }
  bool overflowed() const { return overflow_; }
  void clear() { count_ = 0; overflow_ = false; }
  // Push buffer words this list takes (headers included).
  uint32_t push_words() const;

 private:
  uint32_t method_[kCapacity] = {};
  uint32_t value_[kCapacity] = {};
  uint32_t count_ = 0;
  bool overflow_ = false;
};

// Called while an UPDATE is waited for: the display's supervisor (which asks the driver to set clocks during a mode set) is polled from here,
// as nothing takes the GPU's interrupt yet.
class IDisplayPoller {
 public:
  virtual void poll() = 0;

 protected:
  IDisplayPoller() = default;
  ~IDisplayPoller() = default;
};

class NvDisplayEngine {
 public:
  static constexpr uint32_t kMaxHeads = 8, kMaxSors = 8, kMaxWindows = 32;
  static constexpr uint32_t kPushBytes = 0x1000;
  static constexpr uint32_t kNotifierBytes = 0x1000;
  static constexpr uint32_t kCoreNotifierOffset = 0x000, kWindowNotifierOffset = 0x100;

  NvDisplayEngine(INvRegisters& registers, INvMemory& memory, const Chip& chip, DisplayTimeouts timeouts = {});
  ~NvDisplayEngine();
  NvDisplayEngine(const NvDisplayEngine&) = delete;
  NvDisplayEngine& operator=(const NvDisplayEngine&) = delete;

  // Which display this is and what it has (reads only). Fails for a display this driver does not program.
  [[nodiscard]] DisplayStatus probe();
  DisplayClass display_class() const { return class_; }
  uint32_t head_count() const { return heads_; }
  uint32_t head_mask() const { return head_mask_; }
  uint32_t sor_mask() const { return sor_mask_; }
  uint32_t window_mask() const { return window_mask_; }

  // The surface `head` shows now (the armed state). False if the head shows nothing readable. On Volta+ the window it shows it through
  // goes to `*window` (if given).
  [[nodiscard]] bool armed_surface(uint32_t head, ScanoutSurface* out, uint32_t* window = nullptr);

  // Takes the display from the VBIOS and brings the core channel up. `window`: on Volta+, the window channel to bring up as well (the one
  // the firmware's picture is in; ignored before Volta).
  [[nodiscard]] DisplayStatus claim(uint32_t window = 0);
  bool claimed() const { return claimed_; }

  // Points `head` at `surface` (offset relative to VRAM, the context DMA is this driver's VRAM one): the surface methods, UPDATE, the
  // notifier, and a readback of the armed state. On a failure after the UPDATE the previous surface is sent again.
  [[nodiscard]] DisplayStatus set_surface(uint32_t head, const ScanoutSurface& surface);

  // Sends `methods` on the core channel followed by an UPDATE and waits for its notifier (for mode setting, NvDisplayMode).
  [[nodiscard]] DisplayStatus core_update(const MethodList& methods);
  void set_poller(IDisplayPoller* poller) { poller_ = poller; }
  // Sends methods on the core channel without an UPDATE (completion is waited for: GET reaches PUT).
  [[nodiscard]] DisplayStatus core_send(const MethodList& methods);
  // Volta+: `methods` on the window channel claim() brought up, an UPDATE (not interlocked with the core) and its notifier.
  [[nodiscard]] DisplayStatus window_update(const MethodList& methods);
  // Volta+: `methods` and an UPDATE on that window's immediate channel (c37b; brought up on first use, no notifier: GET reaching PUT).
  [[nodiscard]] DisplayStatus window_immediate_update(const MethodList& methods);
  // Volta+: the window claim() brought up (-1 before Volta or before claim).
  int32_t claimed_window() const { return window_.up ? static_cast<int32_t>(window_.window) : -1; }
  bool volta_or_later() const { return !pre_volta(); }

  // Stops the channels (nouveau's *_fini), leaving the armed state on the screen. Called by the destructor.
  void shutdown();

  // The last failure, in words (a static string).
  const char* reason() const { return reason_; }
  INvRegisters& registers() { return r_; }
  const DisplayTimeouts& timeouts() const { return timeouts_; }

  // The VRAM context DMA's handle (offsets in ScanoutSurface are relative to VRAM through it).
  static uint32_t vram_handle();
  // The surface methods of `head` (pre-Volta: core channel head methods; Volta+: window channel methods), as set_surface sends them.
  void surface_methods(uint32_t head, const ScanoutSurface& surface, MethodList* out) const;

 private:
  struct Channel {
    bool up = false;
    uint32_t chid = 0;              // the RAMHT channel id (0 core, 1 + window on Volta+)
    uint32_t user = 0;              // BAR0 offset of PUT (GET is +4)
    uint32_t window = 0;
    GpuMemory push;
    uint32_t cursor = 0;            // in words
  };

  DisplayStatus fail(DisplayStatus status, const char* why);
  DisplayStatus take_from_vbios();
  void write_capabilities();
  DisplayStatus setup_instance();
  [[nodiscard]] bool add_context_dma(uint32_t chid, uint32_t handle, uint64_t start, uint64_t limit);
  [[nodiscard]] bool ramht_insert(uint32_t chid, uint32_t handle, uint32_t object_offset);
  DisplayStatus start_core();
  DisplayStatus start_window(uint32_t window);
  DisplayStatus start_window_immediate();
  uint32_t ramht_bytes() const { return pre_volta() ? 0x1000u : 0x2000u; }
  void stop_channel(Channel& channel);
  DisplayStatus submit(Channel& channel, const MethodList& methods);
  DisplayStatus update(Channel& channel, const MethodList& methods, bool window);
  [[nodiscard]] bool wait_register(uint32_t offset, uint32_t mask, uint32_t value);
  [[nodiscard]] bool wait_notifier(uint32_t offset, bool c37);
  bool push_target(const GpuMemory& memory, uint32_t* target) const;
  uint32_t armed(uint32_t method) const;           // core channel armed state (GF119 / Volta+)
  uint32_t window_armed(uint32_t window, uint32_t method) const;
  bool pre_volta() const { return class_ == DisplayClass::Nv50 || class_ == DisplayClass::Gf119; }

  INvRegisters& r_;
  INvMemory& memory_;
  Chip chip_;
  DisplayTimeouts timeouts_;
  DisplayClass class_ = DisplayClass::None;
  uint32_t heads_ = 0, head_mask_ = 0, sor_mask_ = 0, sors_ = 0, window_mask_ = 0, windows_ = 0;
  bool probed_ = false, claimed_ = false;
  GpuMemory instance_, notifier_;
  uint32_t instance_used_ = 0;      // bytes of the instance memory given out (the RAMHT first)
  Channel core_;
  Channel window_;
  Channel wimm_;                    // the window's immediate channel (Volta+, mode setting only)
  IDisplayPoller* poller_ = nullptr;
  const char* reason_ = "not probed";
};

// ---------------------------------------------------------------------------------------------------------------------------------------
// The OS's own framebuffer on the screen (milestone A), in the shape of amd_dce11::INativeDisplay so the GPU class can treat both alike.
// ---------------------------------------------------------------------------------------------------------------------------------------

// The firmware's framebuffer as the UEFI graphics output describes the current mode. `pixels` is the CPU's view of it (it may be empty: the
// picture is then not copied over, and nothing is copied back).
struct NvFirmwareFramebuffer {
  ByteView pixels;
  uint32_t width = 0, height = 0;
  uint32_t pitch_bytes = 0;
};

class INvNativeDisplay {
 public:
  // Shows a framebuffer of the OS's own in video memory in the firmware's mode; `*memory` is what to draw in (rows pitch_bytes apart).
  [[nodiscard]] virtual bool engage_display(const NvFirmwareFramebuffer& firmware, ByteView* memory) = 0;
  virtual bool display_engaged() const = 0;
  // Gives the screen back to the firmware's framebuffer (what was drawn copied back into it first).
  virtual void disengage_display() = 0;
  // Why the last engage failed (or "engaged").
  virtual const char* display_status() const = 0;

 protected:
  INvNativeDisplay() = default;
  ~INvNativeDisplay() = default;
};

class NvNativeDisplay final : public INvNativeDisplay {
 public:
  NvNativeDisplay(INvRegisters& registers, INvMemory& memory, const Chip& chip, DisplayTimeouts timeouts = {})
      : engine_(registers, memory, chip, timeouts), memory_(memory) {}
  ~NvNativeDisplay() { disengage_display(); release_framebuffer(); }
  NvNativeDisplay(const NvNativeDisplay&) = delete;
  NvNativeDisplay& operator=(const NvNativeDisplay&) = delete;

  bool engage_display(const NvFirmwareFramebuffer& firmware, ByteView* memory) override;
  bool display_engaged() const override { return engaged_; }
  void disengage_display() override;
  const char* display_status() const override { return status_; }

  NvDisplayEngine& engine() { return engine_; }
  uint32_t engaged_heads() const { return heads_; }   // bit per head switched
  const ScanoutSurface& original_surface() const { return original_; }
  uint64_t framebuffer_offset() const { return framebuffer_.gpu_address; }

 private:
  bool fail(const char* why) { status_ = why; return false; }
  void release_framebuffer();

  NvDisplayEngine engine_;
  INvMemory& memory_;
  GpuMemory framebuffer_;
  bool framebuffer_in_doubt_ = false;   // a head may still scan it out: never handed back to the allocator
  bool engaged_ = false;
  uint32_t heads_ = 0;
  ScanoutSurface original_;
  NvFirmwareFramebuffer firmware_;
  ByteView memory_view_;
  const char* status_ = "not engaged";
};

}  // namespace nv
}  // namespace hardware
