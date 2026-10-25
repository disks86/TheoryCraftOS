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

// Host-side tests for Wi-Fi: the software radio (scanning order, joining with the right and wrong password, open and enterprise networks, frames
// flowing only while joined, an access point that goes away, an allocation that fails) against a fake uplink.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <deque>
#include <vector>
#include "Hardware/WirelessInterface/Software/SoftwareWirelessInterface.hpp"

using namespace hardware;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  bool fail = false;
  struct Header { uint64_t size; uint64_t pad; };
  void* allocate(uint64_t size) override {
    if (fail) return nullptr;
    Header* h = static_cast<Header*>(malloc(sizeof(Header) + size));
    if (!h) return nullptr;
    h->size = size;
    ++live;
    return h + 1;
  }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override {
    if (!p) return;
    --live;
    free(static_cast<Header*>(p) - 1);
  }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

// The wired side: frames sent are kept, frames to receive are queued by the test.
struct FakeUplink final : INetworkInterface {
  const char* name() const override { return "Fake uplink"; }
  bool initialize() override { return true; }
  void shutdown() override {}
  MacAddress mac_address() const override { return MacAddress{{0x52, 0x54, 0x00, 1, 2, 3}}; }
  bool link_up() const override { return up; }
  uint32_t link_speed_mbps() const override { return 1000; }
  bool transmit(ConstByteView frame) override {
    sent.emplace_back(frame.data(), frame.data() + frame.size());
    ++stats.frames_sent;
    return true;
  }
  bool receive(ByteView buffer, uint32_t* length) override {
    if (queue.empty()) return false;
    const std::vector<uint8_t> f = queue.front();
    queue.pop_front();
    *length = static_cast<uint32_t>(f.size());
    const size_t n = f.size() < buffer.size() ? f.size() : buffer.size();
    for (size_t i = 0; i < n; ++i) buffer[i] = f[i];
    ++stats.frames_received;
    return true;
  }
  NetworkStatistics statistics() const override { return stats; }
  bool up = true;
  std::vector<std::vector<uint8_t>> sent;
  std::deque<std::vector<uint8_t>> queue;
  NetworkStatistics stats;
};

static MacAddress bssid(uint8_t last) { return MacAddress{{0x02, 0, 0, 0, 0, last}}; }

static void fill(SoftwareWirelessInterface& radio) {
  CHECK(radio.add_access_point("Cafe", bssid(1), WifiSecurity::Open, 11, 40));
  CHECK(radio.add_access_point("Home", bssid(2), WifiSecurity::Wpa2, 6, 90, "correct horse"));
  CHECK(radio.add_access_point("Office", bssid(3), WifiSecurity::Enterprise, 36, 70));
  CHECK(radio.add_access_point("Old", bssid(4), WifiSecurity::Wep, 1, 20, "abcde"));
}

static void test_types() {
  CHECK(wifi_channel_frequency_mhz(1) == 2412 && wifi_channel_frequency_mhz(6) == 2437 && wifi_channel_frequency_mhz(14) == 2484);
  CHECK(wifi_channel_frequency_mhz(36) == 5180 && wifi_channel_frequency_mhz(0) == 0 && wifi_channel_frequency_mhz(20) == 0);
  CHECK(strcmp(wifi_security_name(WifiSecurity::Wpa3), "WPA3") == 0 && strcmp(wifi_state_name(WifiState::Connected), "Connected") == 0);
}

static void test_scan() {
  TestAllocator a;
  {
    FakeUplink uplink;
    SoftwareWirelessInterface radio(uplink, a);
    CHECK(radio.wireless() == &radio && uplink.wireless() == nullptr);
    WifiNetwork found[8];
    CHECK(radio.scan_results(ArrayView<WifiNetwork>(found)) == 0);   // nothing scanned yet
    fill(radio);
    CHECK(!radio.add_access_point("Home", bssid(9), WifiSecurity::Open, 1, 10));   // the name is taken
    CHECK(!radio.add_access_point("", bssid(9), WifiSecurity::Open, 1, 10));
    CHECK(!radio.add_access_point("123456789012345678901234567890123", bssid(9), WifiSecurity::Open, 1, 10));   // 33 characters
    CHECK(radio.start_scan());
    const uint32_t n = radio.scan_results(ArrayView<WifiNetwork>(found));
    CHECK(n == 4 && !strcmp(found[0].ssid, "Home") && !strcmp(found[1].ssid, "Office") && !strcmp(found[2].ssid, "Cafe") && !strcmp(found[3].ssid, "Old"));
    CHECK(found[0].channel == 6 && found[0].frequency_mhz == 2437 && found[0].security == WifiSecurity::Wpa2 && found[0].signal_percent == 90);
    CHECK(radio.scan_results(ArrayView<WifiNetwork>(found, 2)) == 2);   // as many as fit
    CHECK(radio.set_signal("Old", 99) && !radio.set_signal("Nope", 5) && radio.start_scan());
    CHECK(radio.scan_results(ArrayView<WifiNetwork>(found)) == 4 && !strcmp(found[0].ssid, "Old"));
  }
  CHECK(a.live == 0);
}

static void test_connect() {
  TestAllocator a;
  {
    FakeUplink uplink;
    SoftwareWirelessInterface radio(uplink, a);
    fill(radio);
    CHECK(radio.status().state == WifiState::Disconnected && !radio.link_up() && radio.link_speed_mbps() == 0);
    CHECK(radio.connect("", "") == WifiResult::InvalidArgument);
    CHECK(radio.connect("Nowhere", "") == WifiResult::NoSuchNetwork);
    CHECK(radio.connect("Home", "wrong password") == WifiResult::BadPassphrase && radio.status().last_result == WifiResult::BadPassphrase);
    CHECK(radio.connect("Home", "short") == WifiResult::BadPassphrase);   // not a valid WPA passphrase
    CHECK(radio.connect("Office", "anything at all") == WifiResult::Unsupported);
    CHECK(radio.status().state == WifiState::Disconnected);
    CHECK(radio.connect("Home", "correct horse") == WifiResult::Ok);
    WifiStatus s = radio.status();
    CHECK(s.state == WifiState::Connected && !strcmp(s.network.ssid, "Home") && s.network.channel == 6 && s.last_result == WifiResult::Ok);
    CHECK(radio.link_up() && radio.link_speed_mbps() == 1000 && radio.mac_address() == uplink.mac_address());
    // Joined: frames go through both ways.
    const uint8_t frame[60] = {1, 2, 3};
    CHECK(radio.transmit(ConstByteView(frame, 60)) && uplink.sent.size() == 1);
    uplink.queue.push_back(std::vector<uint8_t>(64, 7));
    uint8_t buffer[128];
    uint32_t length = 0;
    CHECK(radio.receive(ByteView(buffer), &length) && length == 64 && buffer[0] == 7);
    CHECK(radio.set_signal("Home", 55) && radio.status().network.signal_percent == 55);
    // The uplink's cable comes out: no link.
    uplink.up = false;
    CHECK(!radio.link_up());
    uplink.up = true;
    // Another network: open ones ignore the password; a failed attempt leaves the old connection alone.
    CHECK(radio.connect("Old", "nope!") == WifiResult::BadPassphrase && radio.status().state == WifiState::Connected);
    CHECK(radio.connect("Cafe", "") == WifiResult::Ok && !strcmp(radio.status().network.ssid, "Cafe"));
    CHECK(radio.connect("Old", "abcde") == WifiResult::Ok && radio.status().network.security == WifiSecurity::Wep);
    // Leaving: no link, frames refused (and counted), received frames dropped.
    radio.disconnect();
    CHECK(radio.status().state == WifiState::Disconnected && !radio.link_up());
    CHECK(!radio.transmit(ConstByteView(frame, 60)) && uplink.sent.size() == 1);
    uplink.queue.push_back(std::vector<uint8_t>(64, 9));
    CHECK(!radio.receive(ByteView(buffer), &length) && uplink.queue.empty());
    const NetworkStatistics st = radio.statistics();
    CHECK(st.send_failures == 1 && st.frames_dropped == 1 && st.frames_sent == 1 && st.frames_received == 2);
    // The access point goes away while joined.
    CHECK(radio.connect("Cafe", "") == WifiResult::Ok && radio.remove_access_point("Cafe") && !radio.remove_access_point("Cafe"));
    CHECK(radio.status().state == WifiState::Disconnected && radio.connect("Cafe", "") == WifiResult::NoSuchNetwork);
    radio.shutdown();
  }
  CHECK(a.live == 0);
}

static void test_out_of_memory() {
  TestAllocator a;
  {
    FakeUplink uplink;
    SoftwareWirelessInterface radio(uplink, a);
    a.fail = true;
    CHECK(!radio.add_access_point("Home", bssid(2), WifiSecurity::Open, 6, 90));
    a.fail = false;
    CHECK(radio.add_access_point("Home", bssid(2), WifiSecurity::Open, 6, 90));
    a.fail = true;
    CHECK(!radio.start_scan());   // the list of results cannot grow
    a.fail = false;
    CHECK(radio.start_scan());
  }
  CHECK(a.live == 0);
}

int main() {
  test_types();
  test_scan();
  test_connect();
  test_out_of_memory();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("WirelessTest: all passed\n");
  return 0;
}
