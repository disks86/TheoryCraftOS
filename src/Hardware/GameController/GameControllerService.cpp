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

#include "Hardware/GameController/GameControllerService.hpp"
#include "Hardware/GameController/Usb/UsbGameController.hpp"
#include "Hardware/GameController/Usb/UsbXbox360Wireless.hpp"
#include "Hardware/GameController/Usb/XboxWirelessAdapter.hpp"
#include "Kernel/Thread.hpp"
#include "Utility/String.hpp"

namespace hardware {

namespace {
// The light bar colour each player gets, as a PlayStation console gives them: blue, red, green, pink, then the rest.
struct Colour { uint8_t r, g, b; };
constexpr Colour kPlayerColours[GameControllerService::kMaxPlayers] = {
    {0x00, 0x00, 0x40}, {0x40, 0x00, 0x00}, {0x00, 0x40, 0x00}, {0x20, 0x00, 0x20},
    {0x20, 0x20, 0x00}, {0x00, 0x20, 0x20}, {0x20, 0x20, 0x20}, {0x30, 0x10, 0x00}};

bool same_state(const gamepad::State& a, const gamepad::State& b) {
  return a.buttons == b.buttons && a.left_x == b.left_x && a.left_y == b.left_y && a.right_x == b.right_x && a.right_y == b.right_y &&
         a.left_trigger == b.left_trigger && a.right_trigger == b.right_trigger;
}
}  // namespace

GameControllerService::GameControllerService(IUsbBus* usb, allocators::IAllocator& allocator)
    : usb_(usb), allocator_(allocator), watcher_(usb) {}

GameControllerService::~GameControllerService() { stop(); }

bool GameControllerService::is_game_controller(IUsbHostController& host, uint32_t port) {
  UsbGameControllerProbe probe;
  return probe_usb_game_controller(host, port, &probe);
}

void GameControllerService::notify(const char* name, const char* what, int32_t player) {
  if (!notifier_) return;
  String text(allocator_);
  text.assign(name);
  text.append(": ");
  text.append(what);
  if (player >= 0) {
    text.append(" player ");
    const char digit[2] = {static_cast<char>('1' + player), 0};
    text.append(digit);
  }
  notifier_(notifier_context_, text.view());
}

void GameControllerService::park(UniquePointer<IGameController> controller) {
  for (UniquePointer<IGameController>& idle : idle_) {
    if (idle) continue;
    idle = util::move(controller);
    return;
  }
  controller->shutdown();
}

int32_t GameControllerService::place(UniquePointer<IGameController> controller) {
  if (!controller) return -1;
  if (!controller->connected()) {             // (not there yet: it waits)
    park(util::move(controller));
    return -1;
  }
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    Slot& s = slots_[i];
    if (s.controller) continue;
    s.controller = util::move(controller);
    s.output = gamepad::Output();
    s.output.player = static_cast<uint8_t>(i < 4 ? i + 1 : 0);
    s.output.red = kPlayerColours[i].r;
    s.output.green = kPlayerColours[i].g;
    s.output.blue = kPlayerColours[i].b;
    (void)s.controller->set_output(s.output);     // shows the player number (a pad with no lights refuses: fine)
    ++s.packet;
    ++added_;
    notify(s.controller->info().name, "connected as", static_cast<int32_t>(i));
    return static_cast<int32_t>(i);
  }
  controller->shutdown();
  return -1;
}

void GameControllerService::take_up(IUsbHostController& host, uint32_t port) {
  // An Xbox Wireless Adapter: a pad per client slot, each waiting until a controller associates (the adapter starts once its firmware can be read).
  {
    UsbDeviceDescriptor d;
    if (host.device_descriptor(port, &d)) {
      const uint16_t vendor = d.vendor_id, product = d.product_id;   // (copies: the descriptor is packed)
      if (is_xbox_wireless_adapter(vendor, product)) {
        UniquePointer<IGameController> pads[XboxWirelessAdapter::kMaxClients];
        const uint32_t n = open_xbox_wireless_adapter(host, port, allocator_, ArrayView<UniquePointer<IGameController>>(pads));
        for (uint32_t i = 0; i < n && i < XboxWirelessAdapter::kMaxClients; ++i) park(util::move(pads[i]));
        return;
      }
    }
  }
  // An Xbox 360 Wireless Receiver: a pad per channel, each waiting until a pad pairs with it.
  uint8_t config[512];
  uint16_t length = 0;
  UsbDeviceDescriptor device;
  UsbWirelessChannel channels[4];
  uint32_t wireless = 0;
  if (host.device_descriptor(port, &device) && host.configuration_descriptor(port, ByteView(config), &length))
    wireless = find_xbox360_wireless_channels(ConstByteView(config).at_most(length), ArrayView<UsbWirelessChannel>(channels));
  if (wireless > 0) {
    const uint16_t vendor = device.vendor_id, product = device.product_id;   // (copies: the descriptor is packed)
    for (uint32_t i = 0; i < wireless && i < 4; ++i) {
      UniquePointer<UsbXbox360WirelessPad> pad = UniquePointer<UsbXbox360WirelessPad>::make(allocator_, host, port, channels[i], vendor, product);
      if (pad && pad->initialize()) park(UniquePointer<IGameController>(util::move(pad)));
    }
    return;
  }
  UniquePointer<UsbGameController> pad = UsbGameController::open(host, port, allocator_);
  if (pad) (void)place(UniquePointer<IGameController>(util::move(pad)));
}

void GameControllerService::scan_all() {
  if (!usb_) return;
  for (uint32_t c = 0; c < usb_->controller_count(); ++c) {
    IUsbHostController* host = usb_->host_controller(c);
    for (uint32_t p = 0; host && p < host->port_count(); ++p) take_up(*host, p);
  }
}

void GameControllerService::follow_connections() {
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    Slot& s = slots_[i];
    if (!s.controller || !s.controller->present() || s.controller->connected()) continue;
    notify(s.controller->info().name, "disconnected,", static_cast<int32_t>(i));
    ++s.packet;
    ++removed_;
    park(util::move(s.controller));
    s.controller = UniquePointer<IGameController>();
  }
  for (UniquePointer<IGameController>& idle : idle_) {
    if (!idle) continue;
    if (!idle->present()) {                   // its device left
      idle->shutdown();
      idle = UniquePointer<IGameController>();
    } else if (idle->connected()) {
      UniquePointer<IGameController> pad = util::move(idle);
      idle = UniquePointer<IGameController>();
      (void)place(util::move(pad));
    }
  }
}

void GameControllerService::drop_gone() {
  for (uint32_t i = 0; i < kMaxPlayers; ++i) {
    Slot& s = slots_[i];
    if (!s.controller || s.controller->present()) continue;
    notify(s.controller->info().name, "disconnected,", static_cast<int32_t>(i));
    s.controller->shutdown();
    s.controller = UniquePointer<IGameController>();
    ++s.packet;
    ++removed_;
  }
}

bool GameControllerService::start() {
  if (started_) return true;
  started_ = true;
  {
    kernel::ScopedMutex lock(mutex_);
    scan_all();
  }
  watcher_.snapshot();
  if (kernel::running()) {
    __atomic_store_n(&stop_, false, __ATOMIC_RELEASE);
    thread_ = kernel::spawn(&GameControllerService::thread_main, this, "game-controllers");
    return thread_ != nullptr;
  }
  return true;
}

void GameControllerService::stop() {
  if (thread_) {
    __atomic_store_n(&stop_, true, __ATOMIC_RELEASE);
    kernel::join(thread_);
    thread_ = nullptr;
  }
  kernel::ScopedMutex lock(mutex_);
  for (Slot& s : slots_) {
    if (s.controller) s.controller->shutdown();
    s.controller = UniquePointer<IGameController>();
  }
  for (UniquePointer<IGameController>& idle : idle_) {
    if (idle) idle->shutdown();
    idle = UniquePointer<IGameController>();
  }
  started_ = false;
}

void GameControllerService::thread_main(void* self) {
  auto* s = static_cast<GameControllerService*>(self);
  uint32_t round = 0;
  while (!__atomic_load_n(&s->stop_, __ATOMIC_ACQUIRE)) {
    s->step(++round % kRoundsPerScan == 0);
    kernel::sleep_us(4000);
  }
}

void GameControllerService::step(bool scan) {
  kernel::ScopedMutex lock(mutex_);
  for (Slot& s : slots_) {
    if (!s.controller) continue;
    const gamepad::State before = s.controller->state();
    s.controller->poll();
    if (!same_state(before, s.controller->state())) ++s.packet;
  }
  for (UniquePointer<IGameController>& idle : idle_)
    if (idle) idle->poll();
  follow_connections();
  if (scan) {
    drop_gone();
    watcher_.scan([&](uint32_t, IUsbHostController& host, uint32_t port) { take_up(host, port); });
  }
}

int32_t GameControllerService::add(UniquePointer<IGameController> controller) {
  kernel::ScopedMutex lock(mutex_);
  return place(util::move(controller));
}

bool GameControllerService::snapshot(uint32_t player, gamepad::State* state, uint32_t* packet) const {
  kernel::ScopedMutex lock(mutex_);
  if (player >= kMaxPlayers || !slots_[player].controller) return false;
  if (state) *state = slots_[player].controller->state();
  if (packet) *packet = slots_[player].packet;
  return true;
}

bool GameControllerService::info(uint32_t player, gamepad::Info* out) const {
  kernel::ScopedMutex lock(mutex_);
  if (player >= kMaxPlayers || !slots_[player].controller) return false;
  *out = slots_[player].controller->info();
  return true;
}

bool GameControllerService::set_vibration(uint32_t player, uint16_t low_frequency, uint16_t high_frequency) {
  kernel::ScopedMutex lock(mutex_);
  if (player >= kMaxPlayers || !slots_[player].controller) return false;
  Slot& s = slots_[player];
  if (!(s.controller->info().capabilities & gamepad::kCapRumble)) return false;
  s.output.low_frequency = low_frequency;
  s.output.high_frequency = high_frequency;
  return s.controller->set_output(s.output);
}

bool GameControllerService::set_light(uint32_t player, uint8_t red, uint8_t green, uint8_t blue) {
  kernel::ScopedMutex lock(mutex_);
  if (player >= kMaxPlayers || !slots_[player].controller) return false;
  Slot& s = slots_[player];
  if (!(s.controller->info().capabilities & gamepad::kCapLightBar)) return false;
  s.output.red = red;
  s.output.green = green;
  s.output.blue = blue;
  return s.controller->set_output(s.output);
}

uint32_t GameControllerService::count() const {
  kernel::ScopedMutex lock(mutex_);
  uint32_t n = 0;
  for (const Slot& s : slots_) n += s.controller ? 1 : 0;
  return n;
}

}  // namespace hardware
