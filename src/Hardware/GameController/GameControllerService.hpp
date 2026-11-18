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

// Every game controller of the machine, in player slots: the USB pads present at start-up and those plugged in later, and pads other drivers
// add (Bluetooth, say). A pad gets the lowest free slot when it arrives, keeps it until it leaves, and is shown its number (the Xbox ring, the
// DualSense and Switch player LEDs, the PlayStation light bar's colour). Programs read the slots: the Windows layer's XInput (slots 0..3 are
// XInput's four users), and anything else through snapshot().
//
// A thread polls the pads every 4 ms and looks for new ones four times a second (step() does one round, for tests and machines without threads).
// One mutex covers the slots and the drivers, so a program's call (snapshot, rumble) never meets a pad being polled or dropped halfway.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Hardware/Bus/Usb/IUsbBus.hpp"
#include "Hardware/Bus/Usb/UsbPortWatcher.hpp"
#include "Hardware/GameController/IGameController.hpp"
#include "Kernel/Sync.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace kernel { struct Thread; }

namespace hardware {

class GameControllerService {
 public:
  static constexpr uint32_t kMaxPlayers = 8;
  static constexpr uint32_t kRoundsPerScan = 64;     // (a round is about 4 ms)

  // `usb` may be null: then only added controllers are served.
  GameControllerService(IUsbBus* usb, allocators::IAllocator& allocator);
  ~GameControllerService();
  GameControllerService(const GameControllerService&) = delete;
  GameControllerService& operator=(const GameControllerService&) = delete;

  // Told when a controller arrives or leaves ("Xbox Series X|S Controller: player 1"), from the polling thread. May be null.
  using Notifier = void (*)(void* context, StringView text);
  void set_notifier(Notifier notifier, void* context) { notifier_ = notifier; notifier_context_ = context; }

  // Takes over the controllers present now and starts the thread (if the kernel runs threads). False only if the thread cannot be started.
  [[nodiscard]] bool start();
  void stop();
  // One round: polls every pad; with `scan` also takes up new USB pads and drops those that left.
  void step(bool scan);

  // A controller from another driver: it gets a slot (its number, or -1 when every slot is taken and it was dropped).
  int32_t add(UniquePointer<IGameController> controller);

  // What a slot holds. `packet` grows by one whenever the state changed (XInput's dwPacketNumber). False for an empty slot.
  [[nodiscard]] bool snapshot(uint32_t player, gamepad::State* state, uint32_t* packet) const;
  [[nodiscard]] bool info(uint32_t player, gamepad::Info* out) const;
  // The motors of a slot's pad (0..65535 each); false for an empty slot or a pad without motors.
  [[nodiscard]] bool set_vibration(uint32_t player, uint16_t low_frequency, uint16_t high_frequency);
  // The light bar's colour (PlayStation pads); false for an empty slot or a pad without one.
  [[nodiscard]] bool set_light(uint32_t player, uint8_t red, uint8_t green, uint8_t blue);
  uint32_t count() const;

  // Whether the device on a root port is a game controller this service takes (so that keyboard and mouse drivers leave it alone).
  static bool is_game_controller(IUsbHostController& host, uint32_t port);

  uint32_t added() const { return added_; }
  uint32_t removed() const { return removed_; }

 private:
  struct Slot {
    UniquePointer<IGameController> controller;
    gamepad::Output output;
    uint32_t packet = 0;
  };
  static void thread_main(void* self);
  void scan_all();                                   // (locked) every port
  void take_up(IUsbHostController& host, uint32_t port);   // (locked)
  void drop_gone();                                  // (locked)
  int32_t place(UniquePointer<IGameController> controller);   // (locked)
  void notify(const char* name, const char* what, int32_t player);

  IUsbBus* usb_;
  allocators::IAllocator& allocator_;
  mutable kernel::Mutex mutex_;
  Slot slots_[kMaxPlayers];
  UsbPortWatcher watcher_;
  Notifier notifier_ = nullptr;
  void* notifier_context_ = nullptr;
  kernel::Thread* thread_ = nullptr;
  bool stop_ = false;
  bool started_ = false;
  uint32_t added_ = 0, removed_ = 0;
};

}  // namespace hardware
