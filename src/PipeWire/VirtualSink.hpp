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

// A virtual sink's audio (the adapter factory's support.null-audio-sink, Graph::create_virtual_node): a mixer of its own over a silent device that
// plays at the real rate on the graph's clock. Streams of any protocol (PipeWire, PulseAudio, Windows) play into it as they would into the speakers,
// mixed and paced the same way, and what records from it (its monitor) is a tap of that mixer. The graph pumps it (Graph::refresh).
#pragma once
#include <stdint.h>
#include "Audio/AudioMixer.hpp"
#include "Hardware/AudioDevice/Software/SoftwareAudioDevice.hpp"
#include "Hardware/Clock/IClock.hpp"

namespace pipewire {

// A clock of a function's nanoseconds (null: always 0).
class FunctionClock final : public hardware::IClock {
 public:
  explicit FunctionClock(uint64_t (*now_ns)() = nullptr) : now_ns_(now_ns) {}
  void set(uint64_t (*now_ns)()) { __atomic_store_n(&now_ns_, now_ns, __ATOMIC_RELEASE); }
  bool set_up() const { return __atomic_load_n(&now_ns_, __ATOMIC_ACQUIRE) != nullptr; }
  const char* name() const override { return "PipeWire graph clock"; }
  bool initialize() override { return set_up(); }
  void shutdown() override {}
  uint64_t now_us() const override {
    uint64_t (*f)() = __atomic_load_n(&now_ns_, __ATOMIC_ACQUIRE);
    return f ? f() / 1000 : 0;
  }

 private:
  uint64_t (*now_ns_)();
};

class VirtualOutput {
 public:
  VirtualOutput(allocators::IAllocator& allocator, const hardware::IClock& clock) : device_(clock, allocator), mixer_(device_, allocator) {}
  VirtualOutput(const VirtualOutput&) = delete;
  VirtualOutput& operator=(const VirtualOutput&) = delete;
  ~VirtualOutput() { device_.shutdown(); }
  bool initialize() { return device_.initialize() && mixer_.initialize(); }
  audio::AudioMixer& mixer() { return mixer_; }
  // Mixes what its streams gave into its device as far as the device has played (the clock moved on).
  void pump() { mixer_.pump(); }

 private:
  hardware::SoftwareAudioDevice device_;
  audio::AudioMixer mixer_;
};

}  // namespace pipewire
