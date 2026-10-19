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

#include "Hardware/HyperV/HyperVIntegration.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
// Interface GUIDs, in wire byte order (the first three fields little-endian).
constexpr uint8_t kShutdownGuid[16] = {0xf7, 0x0f, 0x65, 0xb6, 0xbc, 0x33, 0x40, 0x48, 0x80, 0x48, 0xe0, 0x67, 0x67, 0x86, 0xf3, 0x93};
constexpr uint8_t kTimeSyncGuid[16] = {0x30, 0xe6, 0x27, 0x95, 0xae, 0xd0, 0x7b, 0x49, 0xad, 0xce, 0xe8, 0x0a, 0xb0, 0x17, 0x5c, 0xaf};
constexpr uint8_t kHeartbeatGuid[16] = {0x39, 0x4f, 0x16, 0x57, 0x15, 0x91, 0x78, 0x4e, 0xab, 0x55, 0x38, 0x2f, 0x3b, 0xd5, 0x42, 0x2d};

struct Version { uint16_t major, minor; };
// Most preferred first. The framework version is the packet format; the service version is the service's own protocol.
constexpr Version kFramework[] = {{3, 0}, {1, 0}};
constexpr Version kHeartbeatVersions[] = {{3, 0}, {1, 0}};
constexpr Version kShutdownVersions[] = {{3, 2}, {3, 1}, {3, 0}, {1, 0}};
constexpr Version kTimeSyncVersions[] = {{4, 0}, {3, 0}, {1, 0}};

// Offsets in a packet: the pipe header (8 bytes), then the message header (20), then the body.
constexpr uint32_t kHeader = 8, kBody = 28;
constexpr uint32_t kTypeNegotiate = 0, kTypeHeartbeat = 1, kTypeShutdown = 3, kTypeTimeSync = 4;
constexpr uint8_t kFlagTransaction = 1, kFlagResponse = 2;
constexpr uint32_t kStatusFail = 0x80004005u;

uint16_t rd16(const uint8_t* p) { uint16_t v; __builtin_memcpy(&v, p, 2); return v; }
uint32_t rd32(const uint8_t* p) { uint32_t v; __builtin_memcpy(&v, p, 4); return v; }
uint64_t rd64(const uint8_t* p) { uint64_t v; __builtin_memcpy(&v, p, 8); return v; }
void wr16(uint8_t* p, uint16_t v) { __builtin_memcpy(p, &v, 2); }
void wr32(uint8_t* p, uint32_t v) { __builtin_memcpy(p, &v, 4); }
void wr64(uint8_t* p, uint64_t v) { __builtin_memcpy(p, &v, 8); }

bool offered(const uint8_t* list, uint32_t count, Version v) {
  for (uint32_t i = 0; i < count; ++i)
    if (rd16(list + 4 * i) == v.major && rd16(list + 4 * i + 2) == v.minor) return true;
  return false;
}
}  // namespace

uint32_t HyperVIntegration::start() {
  const uint8_t* guids[kServices] = {kHeartbeatGuid, kShutdownGuid, kTimeSyncGuid};
  uint32_t opened = 0;
  for (uint32_t s = 0; s < kServices; ++s) {
    const int index = bus_.find(guids[s]);
    if (index < 0) continue;
    channels_[s] = bus_.open(static_cast<uint32_t>(index));
    if (channels_[s]) ++opened;
  }
  return opened;
}

void HyperVIntegration::stop() {
  for (VmBusChannel*& c : channels_) {
    if (c) bus_.close(c);
    c = nullptr;
  }
}

// The host opens every conversation with a version negotiation: it lists the framework and service versions it speaks, the guest answers with
// the one of each that it picks.
bool HyperVIntegration::negotiate(Service service, uint8_t* packet, uint32_t length) {
  if (length < kBody + 8) return false;
  const uint16_t frame_count = rd16(packet + kBody), service_count = rd16(packet + kBody + 2);
  const uint32_t list = kBody + 8;
  if (uint32_t{frame_count} + service_count > 20 || list + 4u * (frame_count + service_count) > length) return false;
  const uint8_t* frames = packet + list;
  const uint8_t* services = frames + 4u * frame_count;

  const Version* mine = service == Heartbeat ? kHeartbeatVersions : service == Shutdown ? kShutdownVersions : kTimeSyncVersions;
  const uint32_t mine_count = service == Heartbeat ? sizeof kHeartbeatVersions / sizeof(Version) : service == Shutdown ? sizeof kShutdownVersions / sizeof(Version) : sizeof kTimeSyncVersions / sizeof(Version);
  const Version* frame = nullptr;
  const Version* chosen = nullptr;
  for (const Version& v : kFramework) if (!frame && offered(frames, frame_count, v)) frame = &v;
  for (uint32_t i = 0; i < mine_count; ++i) if (!chosen && offered(services, service_count, mine[i])) chosen = &mine[i];
  if (!frame || !chosen) return false;

  wr16(packet + kBody, 1);
  wr16(packet + kBody + 2, 1);
  wr16(packet + list, frame->major);
  wr16(packet + list + 2, frame->minor);
  wr16(packet + list + 4, chosen->major);
  wr16(packet + list + 6, chosen->minor);
  wr16(packet + kHeader + 10, 0x10);   // the message is now just the negotiation record
  return true;
}

void HyperVIntegration::handle(Service service, uint8_t* packet, uint32_t length, uint64_t transaction) {
  if (length < kBody) return;
  const uint16_t message_type = rd16(packet + kHeader + 4);
  uint32_t status = 0;
  if (message_type == kTypeNegotiate) {
    if (!negotiate(service, packet, length)) status = kStatusFail;
  } else if (service == Heartbeat && message_type == kTypeHeartbeat && length >= kBody + 8) {
    wr64(packet + kBody, rd64(packet + kBody) + 1);   // the sequence number, incremented, is the answer
    ++heartbeats_;
  } else if (service == Shutdown && message_type == kTypeShutdown && length >= kBody + 12) {
    const uint32_t flags = rd32(packet + kBody + 8);
    shutdown_pending_ = true;
    restart_ = (flags & 1) != 0;
  } else if (service == TimeSync && message_type == kTypeTimeSync && length >= kBody + 16) {
    // Version 4 carries the host's time and our reference counter; version 3 the host's sample first. Either way the first word is the time.
    host_filetime_ = rd64(packet + kBody);
    host_time_taken_us_ = uptime_us();
    have_time_ = true;
  } else {
    status = kStatusFail;
  }
  wr32(packet + kHeader + 12, status);
  packet[kHeader + 16] = packet[kHeader + 16];   // (the transaction id inside the message stays as the host sent it)
  packet[kHeader + 17] = static_cast<uint8_t>(packet[kHeader + 17] | kFlagTransaction | kFlagResponse);
  channels_[service]->send(packet, length, VmBusChannel::kPacketDataInband, transaction);
}

void HyperVIntegration::service() {
  static uint8_t packet[512];   // (one thread drives this)
  for (uint32_t s = 0; s < kServices; ++s) {
    if (!channels_[s]) continue;
    uint32_t length;
    uint16_t type;
    uint64_t transaction;
    for (int n = 0; n < 8 && channels_[s]->receive(packet, sizeof packet, &length, &type, &transaction); ++n)
      if (type == VmBusChannel::kPacketDataInband) handle(static_cast<Service>(s), packet, length, transaction);
  }
}

bool HyperVIntegration::take_shutdown_request(bool* restart) {
  if (!shutdown_pending_) return false;
  shutdown_pending_ = false;
  *restart = restart_;
  return true;
}

bool HyperVIntegration::host_time(uint64_t* filetime, uint64_t* taken_us) const {
  if (!have_time_) return false;
  *filetime = host_filetime_;
  *taken_us = host_time_taken_us_;
  return true;
}

}  // namespace hardware
