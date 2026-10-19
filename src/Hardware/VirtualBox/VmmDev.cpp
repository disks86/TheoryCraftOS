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

#include "Hardware/VirtualBox/VmmDev.hpp"
#include "Hardware/IoPort.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint32_t kRequestVersion = 0x10001;
constexpr uint32_t kRequestBytes = 4096;
constexpr uint32_t kHeaderBytes = 24;

// Request types (VMMDevRequestType).
constexpr uint32_t kReqGetMouseStatus = 1, kReqSetMouseStatus = 2, kReqGetHostTime = 10, kReqAcknowledgeEvents = 41,
                   kReqCtlGuestFilterMask = 42, kReqReportGuestInfo = 50, kReqGetDisplayChangeRequest2 = 54,
                   kReqSetGuestCapabilities = 56, kReqReportGuestInfo2 = 58, kReqReportGuestStatus = 59,
                   kReqHeartbeatConfigure = 63, kReqGetMemBalloonChangeRequest = 70, kReqChangeMemBalloon = 72,
                   kReqGuestHeartbeat = 206;

// Host events.
constexpr uint32_t kEventMouseCapabilities = 1u << 0, kEventDisplayChange = 1u << 2, kEventBalloonChange = 1u << 6,
                   kEventMousePosition = 1u << 9;
constexpr uint32_t kMouseGuestCanAbsolute = 1u << 0, kMouseHostWantsAbsolute = 1u << 1;
constexpr uint32_t kCapabilityGraphics = 1u << 2;
constexpr uint32_t kInterfaceVersion = 0x00010004;
constexpr uint32_t kOsTypeUnknown64 = 0x100;
constexpr uint32_t kFacilityDriver = 20, kStatusActive = 50;

constexpr uint32_t kPciCommand = 0x04, kPciBar0 = 0x10;
constexpr uint64_t kPage = 4096;

struct Word {   // a little helper for the request body
  uint8_t* p;
  uint32_t get(uint32_t i) const { return reinterpret_cast<volatile uint32_t*>(p)[i]; }
  void set(uint32_t i, uint32_t v) const { reinterpret_cast<volatile uint32_t*>(p)[i] = v; }
};
}  // namespace

uint8_t* VmmDev::body() const { return request_.data() + kHeaderBytes; }

bool VmmDev::call(uint32_t type, uint32_t body_bytes) {
  uint32_t* h = reinterpret_cast<uint32_t*>(request_.data());
  h[0] = kHeaderBytes + body_bytes;
  h[1] = kRequestVersion;
  h[2] = type;
  h[3] = 0xFFFFFFFFu;   // rc: the host overwrites it
  h[4] = h[5] = 0;
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  IoPort(port_).write32(static_cast<uint32_t>(reinterpret_cast<uint64_t>(request_.data())));
  __atomic_thread_fence(__ATOMIC_SEQ_CST);
  return static_cast<int32_t>(static_cast<volatile uint32_t*>(h)[3]) >= 0;
}

bool VmmDev::initialize() {
  if (device_.vendor_id != kPciVendor || device_.device_id != kPciDevice) return false;
  const uint32_t bar = pci_.read32(device_.address, kPciBar0);
  if (!(bar & 1) || !(bar & 0xFFFC)) return false;   // BAR 0 is the request port
  port_ = static_cast<uint16_t>(bar & 0xFFFC);
  pci_.write32(device_.address, kPciCommand, (pci_.read32(device_.address, kPciCommand) & 0xFFFF) | 0x7);   // I/O, memory, bus master

  request_ = allocate_dma_view<uint8_t>(allocator_, kRequestBytes, 16);
  chunk_base_ = allocate_view<uint8_t*>(allocator_, kMaxBalloonChunks);
  if (request_.empty() || chunk_base_.empty()) {
    shutdown();
    return false;
  }
  started_ = true;
  kernel::ScopedMutex guard(mutex_);
  if (!report_guest()) {
    started_ = false;
    shutdown();
    return false;
  }
  last_heartbeat_us_ = last_balloon_check_us_ = uptime_us();
  return true;
}

// Identifies the guest to the host and switches on the features: what the host may now send, and that this guest takes absolute pointers.
bool VmmDev::report_guest() {
  Word w{body()};
  w.set(0, kInterfaceVersion);
  w.set(1, kOsTypeUnknown64);
  if (!call(kReqReportGuestInfo, 8)) return false;

  __builtin_memset(body(), 0, 144);
  uint16_t* version = reinterpret_cast<uint16_t*>(body());
  version[0] = 7;   // the Guest Additions version this driver claims to speak
  version[1] = 0;
  const char label[] = "TheoryCraftOS";
  for (uint32_t i = 0; i < sizeof label; ++i) body()[16 + i] = static_cast<uint8_t>(label[i]);
  call(kReqReportGuestInfo2, 16 + 128);

  w.set(0, kFacilityDriver);
  w.set(1, kStatusActive);
  w.set(2, 0);
  call(kReqReportGuestStatus, 12);

  w.set(0, kCapabilityGraphics);   // or-mask: can resize to the host's request
  w.set(1, 0);
  call(kReqSetGuestCapabilities, 8);

  w.set(0, kEventMouseCapabilities | kEventDisplayChange | kEventBalloonChange | kEventMousePosition);
  w.set(1, 0);
  if (!call(kReqCtlGuestFilterMask, 8)) return false;

  w.set(0, kMouseGuestCanAbsolute);
  w.set(1, 0);
  w.set(2, 0);
  call(kReqSetMouseStatus, 12);
  refresh_mouse();

  // Heartbeat: struct { bool enabled; u64 interval_ns; } packed to 4 bytes.
  __builtin_memset(body(), 0, 12);
  body()[0] = 1;
  if (call(kReqHeartbeatConfigure, 12)) {
    uint64_t interval_ns;
    __builtin_memcpy(&interval_ns, body() + 4, 8);
    heartbeat_interval_us_ = interval_ns / 1000 / 2;   // twice per interval
  }

  // Anything the host raised before we were listening - the first display size among it.
  w.set(0, 0);
  if (call(kReqAcknowledgeEvents, 4)) handle_events(w.get(0) | kEventDisplayChange);
  else handle_events(kEventDisplayChange);
  return true;
}

void VmmDev::refresh_mouse() {
  Word w{body()};
  w.set(0, 0);
  w.set(1, 0);
  w.set(2, 0);
  if (!call(kReqGetMouseStatus, 12)) return;
  const uint32_t features = w.get(0);
  __atomic_store_n(&wants_absolute_, (features & kMouseHostWantsAbsolute) != 0, __ATOMIC_RELEASE);
  uint32_t x = w.get(1) & kPointerMax, y = w.get(2) & kPointerMax;
  kernel::ScopedMutex guard(pointer_lock_);
  pointer_x_ = x;
  pointer_y_ = y;
  pointer_dirty_ = true;
}

void VmmDev::handle_events(uint32_t events) {
  if (events & (kEventMouseCapabilities | kEventMousePosition)) refresh_mouse();
  if (events & kEventDisplayChange) {
    Word w{body()};
    w.set(0, 0);
    w.set(1, 0);
    w.set(2, 0);
    w.set(3, kEventDisplayChange);   // acknowledge this event
    w.set(4, 0);                     // display 0
    if (call(kReqGetDisplayChangeRequest2, 20) && w.get(0) >= 320 && w.get(1) >= 200 && w.get(0) <= 16384 && w.get(1) <= 16384) {
      kernel::ScopedMutex guard(pointer_lock_);
      display_w_ = w.get(0);
      display_h_ = w.get(1);
      display_pending_ = true;
    }
  }
  if (events & kEventBalloonChange) last_balloon_check_us_ = 0;   // look at it now
}

void VmmDev::service() {
  if (!started_) return;
  kernel::ScopedMutex guard(mutex_);
  Word w{body()};
  w.set(0, 0);
  if (call(kReqAcknowledgeEvents, 4)) handle_events(w.get(0));
  const uint64_t now = uptime_us();
  if (heartbeat_interval_us_ && now - last_heartbeat_us_ >= heartbeat_interval_us_) {
    last_heartbeat_us_ = now;
    call(kReqGuestHeartbeat, 0);
  }
  if (now - last_balloon_check_us_ >= 1000000) {
    last_balloon_check_us_ = now;
    follow_balloon();
  }
}

bool VmmDev::take_pointer(uint32_t* x, uint32_t* y) {
  kernel::ScopedMutex guard(pointer_lock_);
  if (!pointer_dirty_) return false;
  pointer_dirty_ = false;
  *x = pointer_x_;
  *y = pointer_y_;
  return true;
}

bool VmmDev::take_display_request(uint32_t* width, uint32_t* height) {
  kernel::ScopedMutex guard(pointer_lock_);
  if (!display_pending_) return false;
  display_pending_ = false;
  *width = display_w_;
  *height = display_h_;
  return true;
}

bool VmmDev::host_time_ms(uint64_t* out) {
  if (!started_) return false;
  kernel::ScopedMutex guard(mutex_);
  __builtin_memset(body(), 0, 8);
  if (!call(kReqGetHostTime, 8)) return false;
  __builtin_memcpy(out, body(), 8);
  return true;
}

// One balloon chunk (1 MiB = 256 pages) goes to the host or comes back. The chunk is a block with a kept page at both ends
// (the allocator keeps its bookkeeping next to its blocks), so the pages given are the 256 in between.
bool VmmDev::balloon_exchange(uint8_t* block, bool inflate) {
  uint8_t* first = block + kPage;
  Word w{body()};
  w.set(0, kBalloonChunkPages);
  w.set(1, inflate ? 1 : 0);
  for (uint32_t i = 0; i < kBalloonChunkPages; ++i) {
    const uint64_t address = reinterpret_cast<uint64_t>(first) + uint64_t{i} * kPage;
    w.set(2 + 2 * i, static_cast<uint32_t>(address));
    w.set(3 + 2 * i, static_cast<uint32_t>(address >> 32));
  }
  return call(kReqChangeMemBalloon, 8 + 8 * kBalloonChunkPages);
}

void VmmDev::follow_balloon() {
  Word w{body()};
  w.set(0, 0);
  w.set(1, 0);
  w.set(2, kEventBalloonChange);
  if (!call(kReqGetMemBalloonChangeRequest, 12)) return;
  uint32_t want = w.get(0);
  requested_chunks_ = want;
  if (want > kMaxBalloonChunks) want = kMaxBalloonChunks;
  while (balloon_chunks_ < want) {
    uint8_t* block = static_cast<uint8_t*>(allocator_.allocate_aligned((kBalloonChunkPages + 2) * kPage, kPage));
    if (!block) break;
    chunk_base_[balloon_chunks_] = block;
    if (!balloon_exchange(block, true)) {
      allocator_.deallocate(block);   // refused: nothing was given away
      break;
    }
    ++balloon_chunks_;
  }
  while (balloon_chunks_ > want) {
    if (!balloon_exchange(chunk_base_[balloon_chunks_ - 1], false)) break;
    allocator_.deallocate(chunk_base_[--balloon_chunks_]);
  }
}

void VmmDev::shutdown() {
  if (started_ && request_.data()) {
    kernel::ScopedMutex guard(mutex_);
    started_ = false;
    // Put the ballooned memory back and tell the host the driver is going.
    while (balloon_chunks_ > 0) {
      if (!balloon_exchange(chunk_base_[balloon_chunks_ - 1], false)) break;
      allocator_.deallocate(chunk_base_[--balloon_chunks_]);
    }
    Word w{body()};
    w.set(0, 0);
    w.set(1, 0);
    call(kReqSetGuestCapabilities, 8);
    w.set(0, 0);
    w.set(1, 0xFFFFFFFFu);
    call(kReqCtlGuestFilterMask, 8);
    w.set(0, 0);
    w.set(1, 0);
    w.set(2, 0);
    call(kReqSetMouseStatus, 12);
  }
  started_ = false;
  release_view(allocator_, request_);
  release_view(allocator_, chunk_base_);
}

}  // namespace hardware
