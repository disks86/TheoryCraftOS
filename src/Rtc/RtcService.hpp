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

// The peer connections of a browser behind browser::IRtcService: they share the network, the random source and the certificate, and are driven by name from the script
// natives. Safe to call from any thread.
#pragma once
#include "Browser/IRtcService.hpp"
#include "Kernel/Sync.hpp"
#include "Rtc/PeerConnection.hpp"

namespace rtc {

class RtcService final : public browser::IRtcService {
 public:
  static constexpr uint32_t kMaxConnections = 32;
  RtcService(allocators::IAllocator& allocator, IRtcNetwork& network, crypto::IRandom& random);
  uint32_t create(StringView ice_servers, String* error) override;
  bool op(uint32_t id, StringView name, StringView a, StringView b, ConstByteView bytes, int64_t n1, int64_t n2, String* out) override;
  void release(uint32_t id) override;
  // The microphone and speaker of the machine (nullptr: none). They must outlive the service.
  void set_devices(IRtcAudioDevices* devices);

 private:
  struct Slot {
    uint32_t id = 0;
    UniquePointer<PeerConnection> pc;
  };
  struct Source {
    uint32_t handle = 0;
    UniquePointer<IRtcAudioSource> source;
  };
  bool service_op(StringView name, StringView origin, int64_t n1, String* out);
  allocators::IAllocator& allocator_;
  IRtcNetwork& net_;
  crypto::IRandom& random_;
  DtlsIdentity identity_;
  bool identity_ready_ = false;
  kernel::RecursiveMutex mutex_;
  List<UniquePointer<Slot>> slots_;
  uint32_t next_id_ = 1, next_source_ = 1;
  IRtcAudioDevices* devices_ = nullptr;
  List<UniquePointer<Source>> sources_;
};

}  // namespace rtc
