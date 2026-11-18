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

// Game controllers for Linux programs, through the standard Linux input API: /dev/input/event<N> (evdev: struct input_event, the EVIOC* ioctls,
// force feedback rumble) and /dev/input/js<N> (the old joystick API: struct js_event, the JSIOC* ioctls). Player slot N is event<N> and js<N>; only
// occupied slots exist, and /dev/input lists them. Numbers from kInputPadSlots up are left for other input devices.
//
// The pads are the game controller service's (src/Hardware/GameController), reached through an InputPadProvider that the kernel installs at start-up
// (hardware::install_linux_gamepads), so that the process layer does not depend on the hardware code. Without one, /dev/input is empty.
//
// Buttons and axes follow the Linux drivers: xpad for Xbox and generic HID pads (BTN_A/B/X/Y = 0x130/0x131/0x133/0x134 by label, so the west button
// is 0x133), hid-playstation and hid-nintendo for Sony and Nintendo pads (by position: BTN_NORTH 0x133, BTN_WEST 0x134, plus BTN_TL2/TR2). Sticks are
// -32768..32767 with Y down positive, triggers ABS_Z/ABS_RZ 0..255, the d-pad ABS_HAT0X/HAT0Y.
#pragma once
#include <stdint.h>
#include "Process/Process.hpp"
#include "Utility/FixedString.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"

namespace process {

constexpr uint32_t kInputPadSlots = 8;

// A pad's state as the provider reports it: the button bits of hardware::gamepad::Button (the same values), sticks signed 16-bit with UP positive,
// triggers 0..255.
struct InputPadState {
  uint32_t buttons = 0;
  int16_t left_x = 0, left_y = 0, right_x = 0, right_y = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
};
enum InputPadButton : uint32_t {
  kPadDpadUp = 0x0001, kPadDpadDown = 0x0002, kPadDpadLeft = 0x0004, kPadDpadRight = 0x0008,
  kPadStart = 0x0010, kPadBack = 0x0020, kPadLeftStick = 0x0040, kPadRightStick = 0x0080,
  kPadLeftShoulder = 0x0100, kPadRightShoulder = 0x0200, kPadGuide = 0x0400,
  kPadSouth = 0x1000, kPadEast = 0x2000, kPadWest = 0x4000, kPadNorth = 0x8000,
  kPadMisc = 0x20000,
};

// Which Linux driver's layout a pad gets.
enum class InputPadLayout : uint8_t { Xpad, PlayStation, Nintendo };

struct InputPadInfo {
  uint16_t vendor = 0, product = 0, version = 0;
  InputPadLayout layout = InputPadLayout::Xpad;
  bool rumble = false;
  FixedString<64> name;
};

// What the kernel gives this layer. Each function may be called from any thread, with the system's lock held (poll), and must not call back here.
struct InputPadProvider {
  void* context = nullptr;
  bool (*state)(void* context, uint32_t slot, InputPadState* out) = nullptr;      // false: the slot is empty
  bool (*info)(void* context, uint32_t slot, InputPadInfo* out) = nullptr;
  bool (*rumble)(void* context, uint32_t slot, uint16_t strong, uint16_t weak) = nullptr;      // strong: the heavy low-frequency motor
};
// `provider` must stay alive (static storage); null takes it away.
void set_input_pad_provider(const InputPadProvider* provider);

// The state of one open /dev/input node (in OpenObject::input).
class InputClient;

namespace linuxabi {

// What a name under /dev/input is, for stat and friends.
struct InputNodeMeta {
  bool directory = false;
  uint32_t mode = 0;          // with the S_IF* type bits
  uint64_t rdev = 0, ino = 0, modified = 0;
};
// 1 and *meta when `virtual_path` is /dev/input or one of its nodes, 0 when it is not under /dev/input, -errno (ENOENT for an empty slot's node).
// `now_seconds` (since 1970) dates a change of the set of pads (the directory's modification time).
int64_t input_node_at(StringView virtual_path, InputNodeMeta* meta, uint64_t now_seconds);
// Does a node exist there (/dev/input/eventN or jsN of an occupied slot)? (ProcessFiles::device_at)
bool input_device_exists(StringView virtual_path);
// open() of a path input_node_at() accepted: a new descriptor or -errno.
int64_t input_open(Process& p, StringView virtual_path, uint64_t flags, bool cloexec, bool must_be_directory);
uint64_t input_read(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t input_write(Process& p, uint64_t fd, uint64_t buffer, uint64_t count);
uint64_t input_ioctl(Process& p, uint64_t fd, uint64_t request, uint64_t argument);
// poll(): POLLIN/POLLRDNORM when there is something to read, POLLOUT/POLLWRNORM (evdev), POLLHUP/POLLERR once the pad is gone; POLLNVAL if `fd` is not one.
uint16_t input_poll(Process& p, uint64_t fd);
bool input_non_blocking(Process& p, uint64_t fd);
void set_input_non_blocking(Process& p, uint64_t fd, bool value);
// fstat of a descriptor (an input node, or the /dev/input directory): true and *meta.
bool input_descriptor_meta(Process& p, uint64_t fd, InputNodeMeta* meta);

}  // namespace linuxabi
}  // namespace process
