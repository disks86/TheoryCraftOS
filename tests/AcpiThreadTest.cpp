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

// The interpreter, the embedded controller and what hangs on them from several threads at once, under ThreadSanitizer: one thread runs the ACPI events (the embedded controller reports a
// brightness key every round, which reaches the backlight through the listener), another changes and reads the backlight as the keys of the keyboard do, a third reads a thermal
// zone. Before the interpreter had its lock these were the races of the battery reading and of the brightness keys.
#include <stdint.h>
#include <atomic>
#include "AcpiTestSupport.hpp"
#include "Hardware/Backlight/AcpiBacklight.hpp"
#include "Hardware/Backlight/AcpiVideoKeys.hpp"
#include "Hardware/Power/Thermal.hpp"
#include "Kernel/Thread.hpp"

static Bytes make_dsdt() {
  const Bytes levels = package(6, cat({num(0), num(20), num(40), num(60), num(80), num(100)}));
  const Bytes lcd = device("LCD0", cat({name_def("_ADR", num(0x400)), name_def("_BCL", levels), method("_BCM", 1, store(b({0x68}), path("\\_SB.BRTV"))),
                                        method("_BQC", 0, cat({b({0xA4}), path("\\_SB.BRTV")}))}));
  const Bytes video = device("VID0", cat({name_def("_HID", num(0x0A0AD041)), lcd}));
  const Bytes ec = device("EC0", cat({name_def("_HID", num(0x090CD041)), name_def("_GPE", num(0x23)), method("_Q60", 0, notify("\\_SB.VID0.LCD0", 0x86))}));
  const Bytes tz = Bytes(cat({b({0x5B, 0x85}), pkg(cat({path("TZ00"), name_def("TMPV", num(3231)), method("_TMP", 0, cat({b({0xA4}), path("TMPV")})), name_def("_PSV", num(3581))}))}));
  return cat({scope("\\_SB", cat({name_def("BRTV", num(40)), video, ec})), scope("\\_TZ", tz)});
}

constexpr int kRounds = 1500;
std::atomic<int> g_steps{0};

struct Shared {
  AcpiEvents* events;
  FakeHost* host;
  FakePorts* ports;
  AcpiBacklight* light;
  AcpiThermalZone* zone;
  std::atomic<int> thermal_reads{0}, backlight_calls{0};
};

static void events_thread(void* p) {
  Shared* s = static_cast<Shared*>(p);
  for (int i = 0; i < kRounds; ++i) {
    s->host->ec_events.push_back(0x60);
    s->ports->raise(0x23);
    s->events->poll();
  }
}

static void backlight_thread(void* p) {
  Shared* s = static_cast<Shared*>(p);
  for (int i = 0; i < kRounds; ++i) {
    uint8_t now;
    s->light->set_percent(static_cast<uint8_t>(i % 101));
    if (s->light->percent(&now) && now <= 100) ++s->backlight_calls;
  }
}

static void thermal_thread(void* p) {
  Shared* s = static_cast<Shared*>(p);
  for (int i = 0; i < kRounds; ++i) {
    int32_t t;
    if (s->zone->read(&t) && t == 50000 && s->zone->limits().passive_mc == 85000) ++s->thermal_reads;
  }
}

int main() {
  TestAllocator alloc;
  FakeHost host;
  FakePorts ports;
  const Bytes dsdt = make_table("DSDT", make_dsdt());
  const Bytes fadt = make_fadt();
  const ConstByteView tables[1] = {ConstByteView(dsdt.data(), dsdt.size())};
  AcpiEvents events(host, ports, alloc);
  CHECK(events.initialize(tables, 1, ConstByteView(fadt.data(), fadt.size())) && events.enable_acpi_mode() && events.enable_embedded_controller());
  Node* found[2];
  CHECK(AcpiBacklight::find_devices(events.interpreter(), found, 2) == 1);
  AcpiBacklight light(events.interpreter(), found[0]);
  CHECK(light.initialize());
  AcpiVideoKeys keys(light, [](void*, bool) { ++g_steps; }, nullptr);
  CHECK(events.add_listener(&keys));
  Node* zones[2];
  CHECK(AcpiThermalZone::find_zones(events.interpreter(), zones, 2) == 1);
  AcpiThermalZone zone(events.interpreter(), zones[0]);
  Shared shared;
  shared.events = &events;
  shared.host = &host;
  shared.ports = &ports;
  shared.light = &light;
  shared.zone = &zone;
  kernel::Thread* threads[3] = {kernel::spawn(events_thread, &shared, "events"), kernel::spawn(backlight_thread, &shared, "backlight"), kernel::spawn(thermal_thread, &shared, "thermal")};
  for (kernel::Thread* t : threads) CHECK(t && kernel::join(t));
  CHECK(g_steps == kRounds);                                           // every brightness key of the controller reached the listener, once
  CHECK(shared.backlight_calls == kRounds && shared.thermal_reads == kRounds);
  printf(g_failed ? "AcpiThreadTest: %d FAILED\n" : "AcpiThreadTest: all passed\n", g_failed);
  return g_failed ? 1 : 0;
}
