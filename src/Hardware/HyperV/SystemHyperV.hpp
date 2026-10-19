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
#include "Hardware/HyperV/HyperV.hpp"
#include "Hardware/HyperV/HyperVIntegration.hpp"
#include "Hardware/HyperV/VmBus.hpp"
#include "Utility/UniquePointer.hpp"

namespace kernel {
struct Thread;
}

namespace hardware {

// Everything Hyper-V offers a guest, brought up together: the processor interface, the VMBus connection and the integration components.
// A thread pinned to the boot core runs the whole conversation (the SynIC belongs to one processor). Hyper-V's synthetic network, SCSI,
// video and input devices are not driven here; on a generation 2 machine the firmware's own drivers still show them to the UEFI fallbacks.
class SystemHyperV {
 public:
  explicit SystemHyperV(allocators::IAllocator& allocator) : allocator_(allocator) {}
  ~SystemHyperV() { shutdown(); }
  SystemHyperV(const SystemHyperV&) = delete;
  SystemHyperV& operator=(const SystemHyperV&) = delete;

  // Call on the boot core. Does nothing (and returns false) when the machine is not a Hyper-V guest.
  bool initialize();
  void shutdown();

  bool present() const { return hv_ != nullptr; }
  bool vmbus_connected() const { return bus_ && bus_->connected(); }
  HyperV* platform() { return hv_.get(); }
  VmBus* vmbus() { return bus_.get(); }
  HyperVIntegration* integration() { return integration_.get(); }

  // Runs the conversation with the host on a thread pinned to `core` (the boot core's index).
  bool start_service(int32_t core);
  void stop_service();

  // The host asked for a shutdown or restart (true once per request).
  bool take_shutdown_request(bool* restart) { return integration_ && integration_->take_shutdown_request(restart); }

 private:
  static void service_main(void* self);

  allocators::IAllocator& allocator_;
  UniquePointer<HyperV> hv_;
  UniquePointer<VmBus> bus_;
  UniquePointer<HyperVIntegration> integration_;
  kernel::Thread* service_ = nullptr;
  bool stop_ = false;
};

}  // namespace hardware
