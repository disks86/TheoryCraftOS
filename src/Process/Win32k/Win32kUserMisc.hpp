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
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "Process/Win32k/Win32k.hpp"
#include "Process/Win32k/Win32kGdi.hpp"
#include "Process/Win32k/Win32kUser.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/UniquePointer.hpp"

// The user-misc module of win32k: menus, cursors and icons, the clipboard, accelerator tables, monitors and display settings, SystemParametersInfo and the system metrics and
// colours, keyboard layouts, input contexts, raw input, shell and process-level services, and the NtUserCallNoParam/OneParam/TwoParam multiplexers. The cross-module contract
// (user_misc:: in Win32kUser.hpp) is implemented here; this header holds what the files of the module share and a few additions other modules may use. See docs/win32k/UserMisc.md.
namespace kernel { class Clipboard; }
namespace process::win32k::user_misc {

using gdi::Rect;

// --- State that lives as long as the system: made on first use from the allocator of the system.
template <class S>
S* lazy_state(UniquePointer<S>& slot) {
  if (!slot) {
    allocators::IAllocator* allocator = system_allocator();
    if (!allocator) return nullptr;
    slot = UniquePointer<S>::make(*allocator, *allocator);
  }
  return slot.get();
}

// The system lock for the few handlers that register with locked = false (they call into the window manager, whose cursor functions take the lock themselves).
class SystemLock {
 public:
  SystemLock() : sync_(system_objects() ? &system_objects()->sync() : nullptr) { if (sync_) sync_->lock(); }
  ~SystemLock() { if (sync_) sync_->unlock(); }
  SystemLock(const SystemLock&) = delete;
  SystemLock& operator=(const SystemLock&) = delete;

 private:
  ISystemSync* sync_;
};

// --- Objects with Windows-like handle values. A handle is (generation << 16) | (slot + 1): the generation starts at a base of its own per kind and counts up each time a slot is
// reused, so a stale or foreign handle is found out. Objects never move (Hive), so a pointer stays valid until the object is destroyed.
template <class T>
class Registry {
 public:
  Registry(allocators::IAllocator& allocator, uint32_t generation_base) : allocator_(allocator), objects_(allocator), slots_(allocator), free_(allocator), base_(generation_base) {}

  // Makes an object (T(allocator, args...)) and gives its handle; null if memory ran out or all 65535 slots are in use.
  template <class... Args>
  T* create(uint32_t* handle, Args&&... args) {
    uint64_t index;
    if (free_.count() != 0) {
      index = free_[free_.count() - 1];
      free_.remove_at(free_.count() - 1);
    } else {
      if (slots_.count() >= 0xFFFF) return nullptr;
      if (!slots_.push_back(Slot{nullptr, uint16_t(base_)})) return nullptr;
      index = slots_.count() - 1;
    }
    T* object = objects_.emplace(allocator_, static_cast<Args&&>(args)...);
    if (!object) {
      free_.push_back(uint32_t(index));
      return nullptr;
    }
    slots_[index].object = object;
    *handle = uint32_t(slots_[index].generation) << 16 | uint32_t(index + 1);
    return object;
  }
  T* find(uint32_t handle) {
    const uint32_t index = (handle & 0xFFFF);
    if (index == 0) return nullptr;
    Slot* slot = slots_.get(index - 1);
    return slot && slot->object && uint32_t(slot->generation) == (handle >> 16) ? slot->object : nullptr;
  }
  bool contains(uint32_t handle) { return find(handle) != nullptr; }
  bool destroy(uint32_t handle) {
    const uint32_t index = (handle & 0xFFFF);
    if (index == 0) return false;
    Slot* slot = slots_.get(index - 1);
    if (!slot || !slot->object || uint32_t(slot->generation) != (handle >> 16)) return false;
    objects_.erase(slot->object);
    slot->object = nullptr;
    slot->generation = uint16_t(slot->generation + 1 == 0 ? 1 : slot->generation + 1);
    free_.push_back(index - 1);
    return true;
  }
  uint64_t count() const { return objects_.count(); }
  // The handle of every live object, appended to `out` (to walk while destroying).
  void handles(List<uint32_t>* out) const {
    for (uint64_t i = 0; i < slots_.count(); ++i) if (slots_[i].object) out->push_back(uint32_t(slots_[i].generation) << 16 | uint32_t(i + 1));
  }

 private:
  struct Slot { T* object; uint16_t generation; };
  allocators::IAllocator& allocator_;
  Hive<T> objects_;
  List<Slot> slots_;
  List<uint32_t> free_;
  uint32_t base_;
};

// --- Process and thread records (the settings that belong to one process or thread).
struct ProcessRecord {
  uint64_t pid = 0;
  uint32_t dpi_context = 0;           // the process's DPI awareness context (the NTUSER_DPI_* encoding of ntuser.h); 0 = never set
  bool dpi_context_set = false;
  uint32_t default_layout = 0;        // SetProcessDefaultLayout
  bool touch_injection = false;
  uint32_t touch_max_contacts = 0;
  bool mouse_in_pointer = false;
  uint64_t thunk_lock_callback = 0;   // NtUserCallOneParam_EnableThunkLock
};
struct ThreadRecord {
  uint64_t tid = 0, pid = 0;
  uint32_t layout = 0;                // the active keyboard layout (HKL); 0 = the default one
  uint32_t cursor = 0;                // SetCursor
  bool cursor_set = false;            // SetCursor was called since the pointer last came into one of the thread's windows (else the window's class cursor shows)
  uint32_t dpi_context = 0;           // SetThreadDpiAwarenessContext; 0 = follows the process
  bool mouse_in_pointer = false;
};
ProcessRecord* process_record(uint64_t pid);                 // made on first use
ProcessRecord* find_process_record(uint64_t pid);
ThreadRecord* thread_record(uint64_t tid, uint64_t pid);     // made on first use
ThreadRecord* find_thread_record(uint64_t tid);
uint32_t first_layout_of_system();
void thread_forget(uint64_t tid);                            // the thread ended
uint32_t set_thread_dpi_context(Call& call, uint32_t context);      // SetThreadDpiAwarenessContext: the previous context, 0 for an invalid one
bool icon_exists(uint32_t icon);

// The DPI the calling thread sees the system at (96 for an unaware thread, the system DPI otherwise).
uint32_t thread_dpi(Call& call);
uint32_t system_dpi();
void set_system_dpi(uint32_t dpi);                           // 96 unless the display settings say otherwise (tests, mode changes)

// --- Constants the files share.
constexpr uint32_t kMenuByPosition = 0x400, kMenuChecked = 8, kMenuPopup = 0x10, kMenuSeparator = 0x800;
constexpr uint32_t kErrorMenuItemNotFound = 1456, kErrorInvalidKeyboardHandle = 1457, kErrorNoAccess = 998, kErrorPointNotFound = 1171;
constexpr uint32_t kWmClipboardUpdate = 0x31D, kWmDrawClipboard = 0x308, kWmRenderFormat = 0x305, kWmDestroyClipboard = 0x307, kWmChangeCbChain = 0x30D, kWmNcPaint = 0x85;

// --- Installers of the families (each in its own file), called by install_user_misc().
void install_calls();
void install_menus();
void install_cursors();
void install_accelerators();
void install_clipboard();
void install_settings();
void install_monitors();
void install_keyboard();
void install_input();
void install_shell();
void install_dwm();
// Per-family process teardown (system lock held) and window teardown.
void menus_process_exited(uint64_t pid);
void cursors_process_exited(uint64_t pid);
void accelerators_process_exited(uint64_t pid);
void clipboard_process_exited(uint64_t pid);
void settings_process_exited(uint64_t pid);
void keyboard_process_exited(uint64_t pid);
void input_process_exited(uint64_t pid);
void shell_process_exited(uint64_t pid);
void dwm_process_exited(uint64_t pid);
void clipboard_window_destroyed(uint32_t hwnd);
void menu_window_destroyed(uint32_t hwnd);
void keyboard_window_destroyed(uint32_t hwnd);
void shell_window_destroyed(uint32_t hwnd);
void dwm_window_destroyed(uint32_t hwnd);

// --- Functions of one family that others use.
uint32_t monitor_handle();                                    // the one monitor
bool is_monitor(uint32_t handle);
Rect primary_monitor_rect();
Rect virtual_screen_rect();
bool set_monitor_work_area(const Rect& rect);
// The monitors the kernel's desktop shows the programs on: their rectangles and work areas in screen coordinates, the primary first (at most 8). False for a bad list.
bool set_monitors(ArrayView<const Rect> rects, ArrayView<const Rect> works);
int32_t menu_item_count(uint32_t menu);                       // -1 for a handle that is not a menu
bool get_menu_info(Call& call, uint32_t menu, uint64_t info);
bool get_monitor_info(Call& call, uint32_t monitor, uint64_t info);
uint32_t create_cursor_icon(uint64_t pid, bool is_icon);
uint64_t icon_param(uint32_t icon);
uint64_t set_icon_param(uint32_t icon, uint64_t param);
void note_last_input(uint64_t now_ms);
uint64_t last_input_ms(Call& call);
uint32_t shell_window();
uint32_t progman_window();
uint32_t taskman_window();

// ---------------------------------------------------------------------------------------------------------------------------------------------------------------------
// Additions other modules may use.

// The menu of a window (SetMenu; 0 = none). The window manager answers GetMenu / GWLP_ID of a top-level window with it.
uint32_t window_menu(uint32_t hwnd);
// The window's system menu, made on first use (GetSystemMenu); 0 if the window does not exist.
uint32_t window_system_menu(uint32_t hwnd);
// The entries of an accelerator table, for TranslateAccelerator.
struct AcceleratorEntry { uint8_t virt = 0; uint16_t key = 0, command = 0; };
bool accelerator_entries(uint32_t table, List<AcceleratorEntry>* out);
// The windows registered for shell hook messages (RegisterShellHookWindow): the window manager posts HSHELL_* to them.
void shell_hook_windows(List<uint32_t>* out);
// The input context associated with a window (AssociateInputContext; 0 = none).
uint32_t window_input_context(uint32_t hwnd);
// The same but 0xFFFFFFFF for a window nothing was associated with (it uses the default context of its thread), 0 for one with the IME switched off.
uint32_t window_input_context_raw(uint32_t hwnd);
// The system cursors and icons by their OCR_/OIC_ id (OCR_NORMAL 32512, OCR_IBEAM 32513, ... OIC_SAMPLE 32512 ...): the handle of the stock object, made on first use.
uint32_t system_cursor(uint64_t pid, uint32_t ocr_id);
uint32_t system_icon(uint64_t pid, uint32_t oic_id);
// The cursor clip rectangle (the whole screen if none).
Rect cursor_clip_rect();
// Raw input: the input layer reports a device event; the record is kept for GetRawInputData (the returned HRAWINPUT goes to WM_INPUT's lparam) and, buffered, for GetRawInputBuffer.
// `data` is the RAWMOUSE (22 bytes: usFlags, ulButtons, ..., lLastX, lLastY, ulExtraInformation) or RAWKEYBOARD (16 bytes) payload; type 0 mouse, 1 keyboard.
uint32_t record_raw_input(uint64_t pid, uint32_t type, uint32_t device, uint64_t wparam, ConstByteView data, bool buffered);
// The text clipboard as UTF-16 (CF_UNICODETEXT without the NUL): for a bridge to the kernel's Clipboard (src/Kernel/Clipboard.hpp). The sequence number counts changes.
bool clipboard_unicode_text(List<char16_t>* out);
bool clipboard_set_unicode_text(ArrayView<const char16_t> text);
uint32_t clipboard_sequence();
// The two clipboards of the system are one: the kernel's (src/Kernel/Clipboard.hpp: text and a picture, used by the desktop's programs) and this module's (Windows formats, used by
// Windows programs). The bridge copies a change of either into the other (text as CF_UNICODETEXT, a picture as CF_DIB) whenever either is read or changed, and makes the Windows
// listeners and viewers hear of a change that came from the desktop. It works without a Windows program running: the Windows side is brought up to date by the first service call.
void install_clipboard_bridge(kernel::Clipboard* clipboard);
// The system colour brushes and pens (NtUserCallOneParam GetSysColorBrush / GetSysColorPen) are GDI objects, which the gdi:: contract does not offer to make: the GDI module (or the
// kernel) tells this module how, by giving a function that makes a solid brush / pen of a COLORREF owned by a process (0 on failure). Until then the two codes fail.
using SolidObjectFactory = uint32_t (*)(uint64_t pid, uint32_t colorref);
void set_solid_brush_factory(SolidObjectFactory factory);
void set_solid_pen_factory(SolidObjectFactory factory);
// Test hooks.
void set_clipboard_render_timeout_ns(uint64_t ns);
void note_beep(uint32_t* count, uint32_t* last_type);

}  // namespace process::win32k::user_misc
