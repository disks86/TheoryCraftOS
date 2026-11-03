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
#include "Allocators/IAllocator.hpp"
#include "Process/Objects.hpp"
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"
#include "Process/WindowsSyscalls.hpp"
#include "Utility/ArrayView.hpp"
#include "ObjectContainer/List.hpp"

// win32k: the kernel side of the Windows window system. The 1540 services numbered from kWin32kServiceBase (NtUser*, NtGdi*, NtDxgk*, NtDComposition*, ...) are the entry points
// that win32u.dll exports (src/Windows/win32u); user32.dll and gdi32.dll call them. This header is the framework the modules share:
//
//   - the service table (register_handler / dispatch): every module registers the services it implements at install() time; a service nobody registered answers like
//     before, STATUS_NOT_IMPLEMENTED;
//   - Call: one system call as a handler sees it - the arguments (register and stack), the caller's identity, and bounds-checked access to the caller's memory;
//   - the system lock: all win32k state is guarded by the one lock of the system (ObjectManager::sync()), which dispatch() takes around a handler (a handler that blocks gives
//     it up meanwhile: wait_until). A handler must therefore NOT call ObjectManager methods that take the lock themselves, and must never block any other way.
//
// Handlers return the raw value of RAX: a BOOL, a handle, a count, an NTSTATUS - whatever the service returns on Windows (see the prototypes in tests/windows/wine/include/ntuser.h
// and ntgdi.h, which follow the real ones). Errors that Windows reports through GetLastError are written with Call::set_last_error().
namespace process::win32k {

constexpr uint32_t kServiceBase = kWin32kServiceBase, kServiceEnd = kWin32kServiceEnd;

// NTSTATUS values the services use.
constexpr uint32_t kStatusSuccess = 0, kStatusPending = 0x103, kStatusNotImplemented = 0xC0000002u, kStatusInvalidInfoClass = 0xC0000003u, kStatusInfoLengthMismatch = 0xC0000004u,
                   kStatusAccessViolation = 0xC0000005u, kStatusInvalidHandle = 0xC0000008u, kStatusInvalidParameter = 0xC000000Du, kStatusNoMemory = 0xC0000017u,
                   kStatusAccessDenied = 0xC0000022u, kStatusBufferTooSmall = 0xC0000023u, kStatusObjectNameNotFound = 0xC0000034u, kStatusNotSupported = 0xC00000BBu,
                   kStatusNoSuchDevice = 0xC000000Eu, kStatusTimeout = 0x102, kStatusUnsuccessful = 0xC0000001u, kStatusInvalidDeviceState = 0xC0000184u,
                   kStatusBufferOverflow = 0x80000005u, kStatusNoMoreEntries = 0x8000001Au, kStatusIllegalFunction = 0xC00000AFu, kStatusNotFound = 0xC0000225u,
                   kStatusObjectTypeMismatch = 0xC0000024u, kStatusInvalidWindowHandle = 0xC0000300u;

// Win32 error codes (GetLastError) the services set.
constexpr uint32_t kErrorSuccess = 0, kErrorInvalidFunction = 1, kErrorFileNotFound = 2, kErrorAccessDenied = 5, kErrorInvalidHandle = 6, kErrorNotEnoughMemory = 8,
                   kErrorNotSupported = 50, kErrorInvalidParameter = 87, kErrorInsufficientBuffer = 122, kErrorNoMoreItems = 259, kErrorInvalidFlags = 1004,
                   kErrorResourceNameNotFound = 1814, kErrorNoAssociation = 1155, kErrorNoMoreUserHandles = 1158, kErrorMessageSyncOnly = 1159,
                   kErrorInvalidWindowHandle = 1400, kErrorInvalidMenuHandle = 1401, kErrorInvalidCursorHandle = 1402, kErrorInvalidAccelHandle = 1403,
                   kErrorInvalidHookHandle = 1404, kErrorInvalidDwpHandle = 1405, kErrorCannotFindWndClass = 1407, kErrorWindowOfOtherThread = 1408,
                   kErrorHotkeyAlreadyRegistered = 1409, kErrorClassAlreadyExists = 1410, kErrorClassDoesNotExist = 1411, kErrorClassHasWindows = 1412,
                   kErrorInvalidIndex = 1413, kErrorInvalidIconHandle = 1414, kErrorClipboardNotOpen = 1418, kErrorHotkeyNotRegistered = 1419, kErrorDcNotFound = 1425,
                   kErrorInvalidHookFilter = 1426, kErrorHookNeedsHmod = 1428, kErrorGlobalOnlyHook = 1429, kErrorHookNotInstalled = 1431, kErrorInvalidSpiValue = 1439,
                   kErrorHwndsHaveDiffParent = 1441, kErrorNotChildWindow = 1442, kErrorInvalidGwCommand = 1443, kErrorInvalidThreadId = 1444,
                   kErrorInvalidScrollbarRange = 1448, kErrorInvalidShowwinCommand = 1449, kErrorTimeout = 1460, kErrorInvalidMonitorHandle = 1461;

// UTF-16 text (window titles, class names, menu strings, device names): UnicodeString.hpp is not usable, so win32k has this small allocator-backed one.
class WideString {
 public:
  explicit WideString(allocators::IAllocator& allocator) : chars_(allocator) {}
  bool append(char16_t c) { return chars_.push_back(c); }
  bool append(ArrayView<const char16_t> text) {
    for (char16_t c : text) if (!chars_.push_back(c)) return false;
    return true;
  }
  bool append_ascii(const char* text) {
    for (; *text; ++text) if (!chars_.push_back(char16_t(uint8_t(*text)))) return false;
    return true;
  }
  void clear() { chars_.clear(); }
  uint64_t size() const { return chars_.count(); }
  bool empty() const { return chars_.count() == 0; }
  char16_t operator[](uint64_t i) const { return chars_[i]; }
  ArrayView<const char16_t> view() const { return chars_.view(); }
  // Equal to `other`, comparing ASCII letters without regard to case (class names, atoms).
  bool equals(ArrayView<const char16_t> other, bool ignore_case = false) const {
    if (other.size() != size()) return false;
    for (uint64_t i = 0; i < other.size(); ++i) if (fold(chars_[i], ignore_case) != fold(other[i], ignore_case)) return false;
    return true;
  }
  static char16_t fold(char16_t c, bool ignore_case) { return ignore_case && c >= u'a' && c <= u'z' ? char16_t(c - 32) : c; }

 private:
  List<char16_t> chars_;
};

// One system call, as a handler sees it.
class Call {
 public:
  Call(Process& process, const SyscallRequest& request) : process_(process), request_(request) {}

  Process& process() { return process_; }
  const SyscallRequest& request() const { return request_; }
  uint64_t pid() const { return process_.id(); }
  uint64_t tid();      // the calling thread (the process id if the runtime cannot say)
  uint32_t service() const { return uint32_t(request_.number); }
  allocators::IAllocator* allocator() const;      // null if the system has no object manager yet

  // Argument `index` (0-based): the first four are registers, the rest are read from the stack. An unreadable stack slot reads as 0 and sets bad_stack().
  uint64_t arg(uint32_t index);
  uint32_t arg32(uint32_t index) { return uint32_t(arg(index)); }
  int32_t argi(uint32_t index) { return int32_t(arg(index)); }
  bool arg_bool(uint32_t index) { return (arg(index) & 0xFFFFFFFFu) != 0; }      // a BOOL (32 bits)
  bool bad_stack() const { return bad_stack_; }

  // Memory of the caller. Every call fails (false) instead of faulting when the range is not mapped (writable).
  bool read(uint64_t address, ByteView out) { return address != 0 && process_.read_user(address, out); }
  bool write(uint64_t address, ConstByteView data) { return address != 0 && process_.write_user(address, data); }
  bool read_u8(uint64_t address, uint8_t* out) { return read(address, ByteView(out, 1)); }
  bool read_u16(uint64_t address, uint16_t* out);
  bool read_u32(uint64_t address, uint32_t* out);
  bool read_u64(uint64_t address, uint64_t* out);
  bool write_u16(uint64_t address, uint16_t value);
  bool write_u32(uint64_t address, uint32_t value);
  bool write_u64(uint64_t address, uint64_t value);
  // A plain structure of the caller's (an array of integers, RECT, POINT, LOGFONTW, ...): its bytes are copied. T must be trivially copyable and have no padding the caller
  // would not have (the layout is the Windows x64 one: keep the members the same size and order).
  template <class T>
  bool read_struct(uint64_t address, T* out) { return read(address, struct_bytes(out)); }
  template <class T>
  bool write_struct(uint64_t address, const T& value) { return write(address, ConstByteView(struct_bytes(const_cast<T*>(&value)))); }
  // `count` elements of an array.
  template <class T>
  bool read_array(uint64_t address, T* out, uint32_t count) { return read(address, ByteView(raw_bytes(out), uint64_t{count} * sizeof(T))); }
  template <class T>
  bool write_array(uint64_t address, const T* values, uint32_t count) { return write(address, ConstByteView(raw_bytes(const_cast<T*>(values)), uint64_t{count} * sizeof(T))); }

  // UTF-16 text. read_wide: exactly `chars` characters; read_wide_z: up to the terminating NUL (at most `max_chars`, a longer text is an error); read_unicode_string: the
  // UNICODE_STRING at `address` ({USHORT Length, MaximumLength; PWSTR Buffer}); read_ansi_z: a NUL-terminated ANSI string, widened as ISO 8859-1.
  bool read_wide(uint64_t address, uint32_t chars, WideString* out);
  bool read_wide_z(uint64_t address, uint32_t max_chars, WideString* out);
  bool read_unicode_string(uint64_t address, WideString* out);
  bool read_ansi_z(uint64_t address, uint32_t max_chars, WideString* out);
  // Writes up to `capacity` characters (a NUL after them when there is room, if `terminate`); returns how many characters were written (not counting the NUL), or -1 if the
  // memory is not writable.
  int32_t write_wide(uint64_t address, ArrayView<const char16_t> text, uint32_t capacity, bool terminate = true);

  // The caller's last error (the LastErrorValue of the thread's TEB, which GetLastError reads).
  void set_last_error(uint32_t error);
  // Ends the call with a failure value after setting the error: `return fail(kErrorInvalidParameter);`
  uint64_t fail(uint32_t error, uint64_t value = 0) { set_last_error(error); return value; }

 private:
  template <class T>
  static ByteView struct_bytes(T* value) { return ByteView(raw_bytes(value), sizeof(T)); }
  // The one place where a typed object is seen as bytes (a trivially copyable object may always be: no alignment or lifetime issue).
  template <class T>
  static uint8_t* raw_bytes(T* value) { return reinterpret_cast<uint8_t*>(value); }

  Process& process_;
  const SyscallRequest& request_;
  bool bad_stack_ = false;
};

using Handler = uint64_t (*)(Call&);

// Registers the code of one service (a WindowsSyscallNumber from the NtUser/NtGdi/... range). `locked` (the default): dispatch holds the system lock around the handler. A
// handler that talks to a component with a lock of its own (the Direct3D kernel) registers with locked = false and takes care of everything itself. False if the number is
// outside the win32k range or already has a handler (the first one stays).
bool register_handler(WindowsSyscallNumber number, Handler handler, bool locked = true);
// The handler of a service, null if there is none.
Handler handler_of(uint32_t service);
// How many services have a handler / which numbers still have none (for the tests and the documentation).
uint32_t registered_count();
uint32_t duplicate_registrations();
uint32_t duplicate_service(uint32_t index);      // the service number of the index-th refused duplicate registration (0 past the end; the first 64 are kept)

// Runs a service. False if no handler is registered for the number (the caller answers STATUS_NOT_IMPLEMENTED).
bool dispatch(Process& process, const SyscallRequest& request, uint64_t* result);

// Registers the services of every module in this build and hooks win32k into the Windows system-call interface. Safe to call more than once.
void install();
// The module installers (each in its own file); install() calls them all. install_dxg() lives in Dxg/ and is called by the kernel because it needs the Direct3D kernel.
void install_gdi();
void install_user();
void install_user_misc();
void install_composition();
void install_dxg();

// Process teardown: every module drops what the process owned (windows, DCs, objects, hooks, ...). Called when a process ends, with the system lock held; a module
// registers its function once, from its installer. Hooks run in the order they were registered.
using ProcessExitHook = void (*)(uint64_t pid);
bool register_process_exit_hook(ProcessExitHook hook);
void process_exited(uint64_t pid);

// --- Waiting. All win32k state is guarded by the system lock, which a handler holds. wait_until runs `attempt()` (lock held) until it returns true, sleeping between tries (the
// lock and the process's own lock are given up while it sleeps; any module that changes state a waiter might look at calls wake_all() afterwards). `timeout_ns`: ~0 = forever.
// Returns true if attempt() succeeded; false on timeout, or when the process is ending (check call.process().exited()), or the runtime was interrupted. `deadline_hint`
// (optional): a function that says, each time round, at which monotonic time (ns) the state changes by itself (a timer is due); the sleep ends then.
void wake_all();
uint64_t now_ns(Process& process);
bool wait_loop(Call& call, uint64_t timeout_ns, bool (*attempt)(void* context), void* context, uint64_t (*next_event_ns)(void* context) = nullptr);
template <class Attempt>
inline bool wait_until(Call& call, uint64_t timeout_ns, Attempt&& attempt) {
  return wait_loop(call, timeout_ns, [](void* c) { return (*static_cast<Attempt*>(c))(); }, &attempt);
}

// The allocator of the system (null before the object manager exists).
allocators::IAllocator* system_allocator();
// Host tests with no thread runtime: the thread Call::tid() answers with (0: ask the runtime / use the process id).
void set_thread_override(uint64_t tid);

}  // namespace process::win32k
