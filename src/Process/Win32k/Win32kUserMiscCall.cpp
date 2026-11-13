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
#include "Process/Win32k/Win32kUserMisc.hpp"

// NtUserCallNoParam, NtUserCallOneParam and NtUserCallTwoParam: the multiplexers of the services that need no more than two arguments. The codes are the ones of Wine's
// ntuser.h ("not compatible with Windows"): the enumerators count up from 0 in the order they are listed there. A code this module cannot serve answers as the failing
// call would (0, with the error ERROR_INVALID_PARAMETER); codes for windows are the window manager's and are not part of the multiplexers registered here.
namespace process::win32k::user_misc {

namespace {

enum NoParam : uint32_t { kGetDesktopWindow, kGetDialogBaseUnits, kGetLastInputTime, kGetProgmanWindow, kGetShellWindow, kGetTaskmanWindow, kDisplayModeChanged, kExitingThread, kThreadDetach };
enum OneParam : uint32_t {
  kCreateCursorIcon, kEnableDC, kEnableThunkLock, kGetIconParam, kGetMenuItemCount, kGetPrimaryMonitorRect, kGetSysColor, kGetSysColorBrush, kGetSysColorPen, kGetSystemMetrics,
  kGetVirtualScreenRect1, kSetKeyboardAutoRepeat, kSetThreadDpiAwarenessContext, kD3dkmtOpenAdapter, kGetAsyncKeyboardState, kGetDeskPattern
};
enum TwoParam : uint32_t {
  kGetDialogProc, kGetMenuInfo, kGetMonitorInfo, kGetSystemMetricsForDpi, kMonitorFromRect, kSetIconParam, kSetImeCompositionRect, kAdjustWindowRect, kGetVirtualScreenRect2, kAllocWinProc,
  // TheoryCraftOS's own (far from Wine's): user32 collects the files the desktop dropped (WM_DROPFILES with wParam a ticket, lParam user::kDropTicketMark).
  // (ticket, 0) answers the DROPFILES block's size; (ticket, buffer of that size) fills it and the drop is gone.
  kTcosTakeDrop = 0x7C00,
  // Touches (Win32kUserTouch.cpp): (pointer id, POINTER_TOUCH_INFO of 144 bytes) fills it; (WM_TOUCH handle, 0) answers how many TOUCHINPUTs it has,
  // (handle, 48 bytes) fills them; (handle, 0) closes it; (window, op | flags << 2): RegisterTouchWindow (op 1), UnregisterTouchWindow (0), IsTouchWindow (2).
  kTcosPointerTouchInfo = 0x7C01, kTcosTouchInputInfo = 0x7C02, kTcosCloseTouchInput = 0x7C03, kTcosTouchWindow = 0x7C04
};

}  // namespace

namespace {

uint32_t g_keyboard_auto_repeat = 1;
SolidObjectFactory g_brush_factory = nullptr, g_pen_factory = nullptr;

struct BrushCache { uint32_t brush[31], pen[31], color[31]; };
BrushCache g_brushes = {};

uint64_t no_param(Call& c) {
  switch (c.arg32(0)) {
    case kGetDesktopWindow: return user::desktop_window();
    case kGetDialogBaseUnits: return 16u << 16 | 8u;      // the base units of the system font at 96 dpi: 8 x 16
    case kGetLastInputTime: return last_input_ms(c);
    case kGetProgmanWindow: return progman_window();
    case kGetShellWindow: return shell_window();
    case kGetTaskmanWindow: return taskman_window();
    case kDisplayModeChanged: return 1;      // the monitor is sized from the screen surface on every call: nothing to refresh
    case kExitingThread:
    case kThreadDetach: {
      // The thread ends: what it had is dropped (its cursor, keyboard layout and DPI context).
      thread_forget(c.tid());
      return 0;
    }
    default: return c.fail(kErrorInvalidParameter);
  }
}

uint64_t one_param(Call& c) {
  const uint64_t arg = c.arg(0);
  switch (c.arg32(1)) {
    case kCreateCursorIcon: {
      const uint32_t handle = create_cursor_icon(c.pid(), arg != 0);
      return handle ? handle : c.fail(kErrorNoMoreUserHandles);
    }
    case kEnableThunkLock: {
      ProcessRecord* p = process_record(c.pid());
      if (!p) return 0;
      p->thunk_lock_callback = arg;
      return 0;
    }
    case kGetIconParam: return icon_param(uint32_t(arg));
    case kGetMenuItemCount: return uint64_t(int64_t(menu_item_count(uint32_t(arg)))) & 0xFFFFFFFFu;
    case kGetPrimaryMonitorRect: return c.write_struct(arg, primary_monitor_rect()) ? 1 : c.fail(kErrorNoAccess);
    case kGetSysColor: return system_color(uint32_t(arg));
    case kGetSysColorBrush:
    case kGetSysColorPen: {
      const bool pen = c.arg32(1) == kGetSysColorPen;
      const uint32_t index = uint32_t(arg);
      if (index > 30) return c.fail(kErrorInvalidParameter);
      const SolidObjectFactory make = pen ? g_pen_factory : g_brush_factory;
      if (!make) return c.fail(kErrorNotSupported);      // the GDI module has not said how to make solid brushes / pens
      const uint32_t color = system_color(index);
      uint32_t& slot = pen ? g_brushes.pen[index] : g_brushes.brush[index];
      if (slot == 0 || g_brushes.color[index] != color) {      // (the colour changed: a new object; the old one stays with its owner)
        slot = make(c.pid(), color);
        g_brushes.color[index] = color;
      }
      return slot;
    }
    case kGetSystemMetrics: return uint32_t(system_metrics(uint32_t(arg), thread_dpi(c)));
    case kGetVirtualScreenRect1: return c.write_struct(arg, virtual_screen_rect()) ? 1 : c.fail(kErrorNoAccess);
    case kSetKeyboardAutoRepeat: {
      const uint32_t previous = g_keyboard_auto_repeat;
      g_keyboard_auto_repeat = arg != 0 ? 1 : 0;
      return previous;
    }
    case kSetThreadDpiAwarenessContext: return set_thread_dpi_context(c, uint32_t(arg));
    default: return c.fail(kErrorInvalidParameter);      // EnableDC (device contexts), the Direct3D adapter and the async key state are other modules' services
  }
}

uint64_t two_param(Call& c) {
  const uint64_t a = c.arg(0), b = c.arg(1);
  switch (c.arg32(2)) {
    case kGetDialogProc: return a;      // (a procedure handle, not a thunk: it is its own ANSI and Unicode version)
    case kGetMenuInfo: return get_menu_info(c, uint32_t(a), b) ? 1 : 0;
    case kGetMonitorInfo: return get_monitor_info(c, uint32_t(a), b) ? 1 : 0;
    case kGetSystemMetricsForDpi: return uint32_t(system_metrics(uint32_t(a), uint32_t(b)));
    case kMonitorFromRect: {
      Rect r;
      if (!c.read_struct(a, &r)) return c.fail(kErrorNoAccess);
      return monitor_from_rect(r, uint32_t(b));
    }
    case kSetIconParam: {
      uint64_t callback = 0, param = 0;      // struct free_icon_params { UINT64 callback; UINT64 param; }
      if (!c.read_u64(b, &callback) || !c.read_u64(b + 8, &param)) return c.fail(kErrorNoAccess);
      if (!icon_exists(uint32_t(a))) return c.fail(kErrorInvalidCursorHandle);
      return set_icon_param(uint32_t(a), param);      // (the previous parameter would be handed to the callback to free: there is no way to call into the process from here)
    }
    case kAdjustWindowRect: {
      Rect r;
      uint8_t params[16];
      if (!c.read_struct(a, &r) || !c.read(b, ByteView(params, 16))) return c.fail(kErrorNoAccess);
      auto u32 = [&](uint32_t o) { return uint32_t(params[o]) | uint32_t(params[o + 1]) << 8 | uint32_t(params[o + 2]) << 16 | uint32_t(params[o + 3]) << 24; };
      if (!user::adjust_window_rect(&r, u32(0), u32(4), u32(8) != 0, u32(12))) return c.fail(kErrorInvalidParameter);
      return c.write_struct(a, r) ? 1 : c.fail(kErrorNoAccess);
    }
    case kGetVirtualScreenRect2: return c.write_struct(a, virtual_screen_rect()) ? 1 : c.fail(kErrorNoAccess);
    case kTcosTakeDrop: {
      const uint64_t size = user::take_drop(c.pid(), uint32_t(a), ByteView());
      if (size == 0) return c.fail(kErrorInvalidParameter);
      if (b == 0) return size;
      allocators::IAllocator* alloc = c.allocator();
      if (!alloc) return c.fail(kErrorNotEnoughMemory);
      ByteBuffer block(*alloc);
      if (!block.put_zeros(size)) return c.fail(kErrorNotEnoughMemory);
      if (!c.write(b, block.view())) return c.fail(kErrorNoAccess);      // (the caller's buffer is writable: the drop is taken)
      if (user::take_drop(c.pid(), uint32_t(a), block.writable_view()) != size || !c.write(b, block.view())) return c.fail(kErrorNoAccess);
      return size;
    }
    case kTcosPointerTouchInfo: {
      uint8_t info[144];
      if (!user::pointer_touch_info(c.pid(), uint32_t(a), ByteView(info, sizeof info))) return c.fail(kErrorInvalidParameter);
      return c.write(b, ConstByteView(info, sizeof info)) ? 1 : c.fail(kErrorNoAccess);
    }
    case kTcosTouchInputInfo: {
      if (b == 0) return user::touch_input_info(c.pid(), uint32_t(a), ByteView());
      uint8_t input[48];
      if (user::touch_input_info(c.pid(), uint32_t(a), ByteView(input, sizeof input)) != 1) return c.fail(kErrorInvalidHandle);
      return c.write(b, ConstByteView(input, sizeof input)) ? 1 : c.fail(kErrorNoAccess);
    }
    case kTcosCloseTouchInput: return user::close_touch_input(c.pid(), uint32_t(a)) ? 1 : c.fail(kErrorInvalidHandle);
    case kTcosTouchWindow: {
      if (!user::is_window(uint32_t(a))) return c.fail(kErrorInvalidWindowHandle);
      return user::touch_window_state(uint32_t(a), uint32_t(b & 3), uint32_t(b >> 2));
    }
    default: return c.fail(kErrorInvalidParameter);
  }
}

}  // namespace

void set_solid_brush_factory(SolidObjectFactory factory) { g_brush_factory = factory; }
void set_solid_pen_factory(SolidObjectFactory factory) { g_pen_factory = factory; }

void install_calls() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserCallNoParam, no_param);
  register_handler(N::NtUserCallOneParam, one_param);
  register_handler(N::NtUserCallTwoParam, two_param);
}

}  // namespace process::win32k::user_misc
