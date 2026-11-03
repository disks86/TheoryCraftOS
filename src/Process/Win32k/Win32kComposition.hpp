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
#include <stdint.h>

// The composition and "other" services of win32k (docs/win32k/Composition.md): DirectComposition channels, composition surfaces and input sinks, flip objects, token manager,
// modern-core window targets, the raw input manager (RIM) and the minuser input services (MIT). The handlers are registered by install_composition().
namespace process::win32k::composition {

// Drops all state (every handle and object) while the allocator it was built with is alive. The kernel never calls this; host tests call it before their System goes away.
void shutdown();

// The machine's input as the MIT synthesize services pass it on: the window manager's inject_* functions when that module is linked in (Win32kUser.hpp), or whatever a test
// installs. Null members are skipped.
struct InputSink {
  void (*key)(uint32_t virtual_key, uint32_t scan_code, bool down, bool extended) = nullptr;
  void (*mouse_move)(int32_t x, int32_t y) = nullptr;
  void (*mouse_button)(uint32_t button, bool down) = nullptr;
  void (*mouse_wheel)(int32_t delta, bool horizontal) = nullptr;
  void (*character)(char32_t character) = nullptr;
  bool (*post_message)(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) = nullptr;      // false: no such window
  bool (*post_thread_message)(uint64_t thread, uint32_t message, uint64_t wparam, uint64_t lparam) = nullptr;
  void (*cursor_position)(int32_t* x, int32_t* y) = nullptr;
  bool (*screen_size)(uint32_t* width, uint32_t* height) = nullptr;
};
void set_input_sink(const InputSink& sink);
InputSink default_input_sink();      // the window manager's functions (as far as they are linked in)

// The kernel feeds the raw input manager with what the machine's devices report (the keyboard and mouse drivers call these; the MIT synthesize services use them too). They take
// the system lock themselves. `device` is the id of the virtual keyboard (1) or mouse (2).
constexpr uint32_t kRimKeyboardDevice = 1, kRimMouseDevice = 2;
void rim_feed_key(uint32_t virtual_key, uint32_t scan_code, bool down, bool extended, uint64_t time_ns);
void rim_feed_mouse(int32_t dx, int32_t dy, uint32_t button_flags, int32_t wheel, uint64_t time_ns);

}  // namespace process::win32k::composition
