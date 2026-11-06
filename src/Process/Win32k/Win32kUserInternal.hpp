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
// The window manager's private state and the functions its source files share (see docs/win32k/User.md). Everything here is guarded by the system lock.
#include <stdint.h>
#include "ObjectContainer/Hive.hpp"
#include "ObjectContainer/List.hpp"
#include "Process/Win32k/Win32k.hpp"
#include "Process/Win32k/Win32kUser.hpp"

namespace process::win32k::user {

using gdi::Rect;

// ---- Windows constants (only the ones the window manager uses).
namespace wn {
constexpr uint32_t WS_POPUP = 0x80000000u, WS_CHILD = 0x40000000u, WS_MINIMIZE = 0x20000000u, WS_VISIBLE = 0x10000000u, WS_DISABLED = 0x08000000u, WS_CLIPSIBLINGS = 0x04000000u,
                   WS_CLIPCHILDREN = 0x02000000u, WS_MAXIMIZE = 0x01000000u, WS_CAPTION = 0x00C00000u, WS_BORDER = 0x00800000u, WS_DLGFRAME = 0x00400000u,
                   WS_VSCROLL = 0x00200000u, WS_HSCROLL = 0x00100000u, WS_SYSMENU = 0x00080000u, WS_THICKFRAME = 0x00040000u, WS_GROUP = 0x00020000u, WS_TABSTOP = 0x00010000u;
constexpr uint32_t WS_EX_DLGMODALFRAME = 1, WS_EX_NOPARENTNOTIFY = 4, WS_EX_TOPMOST = 8, WS_EX_TRANSPARENT = 0x20, WS_EX_MDICHILD = 0x40, WS_EX_TOOLWINDOW = 0x80,
                   WS_EX_WINDOWEDGE = 0x100, WS_EX_CLIENTEDGE = 0x200, WS_EX_STATICEDGE = 0x20000, WS_EX_APPWINDOW = 0x40000, WS_EX_LAYERED = 0x80000,
                   WS_EX_NOACTIVATE = 0x08000000, WS_EX_LEFTSCROLLBAR = 0x4000, WS_EX_RIGHT = 0x1000;
constexpr uint32_t CS_VREDRAW = 1, CS_HREDRAW = 2, CS_DBLCLKS = 8, CS_OWNDC = 0x20, CS_CLASSDC = 0x40, CS_PARENTDC = 0x80, CS_GLOBALCLASS = 0x4000, CS_SAVEBITS = 0x800;

constexpr uint32_t WM_NULL = 0, WM_CREATE = 1, WM_DESTROY = 2, WM_MOVE = 3, WM_SIZE = 5, WM_ACTIVATE = 6, WM_SETFOCUS = 7, WM_KILLFOCUS = 8, WM_ENABLE = 0xA, WM_SETREDRAW = 0xB,
                   WM_SETTEXT = 0xC, WM_GETTEXT = 0xD, WM_GETTEXTLENGTH = 0xE, WM_PAINT = 0xF, WM_CLOSE = 0x10, WM_QUIT = 0x12, WM_ERASEBKGND = 0x14, WM_SHOWWINDOW = 0x18,
                   WM_ACTIVATEAPP = 0x1C, WM_SETCURSOR = 0x20, WM_MOUSEACTIVATE = 0x21, WM_GETMINMAXINFO = 0x24, WM_STYLECHANGING = 0x7C, WM_STYLECHANGED = 0x7D,
                   WM_WINDOWPOSCHANGING = 0x46, WM_WINDOWPOSCHANGED = 0x47, WM_COPYDATA = 0x4A, WM_NCCREATE = 0x81, WM_NCDESTROY = 0x82, WM_NCCALCSIZE = 0x83,
                   WM_NCHITTEST = 0x84, WM_NCPAINT = 0x85, WM_NCACTIVATE = 0x86, WM_NCMOUSEMOVE = 0xA0, WM_NCLBUTTONDOWN = 0xA1, WM_NCLBUTTONUP = 0xA2,
                   WM_NCLBUTTONDBLCLK = 0xA3, WM_NCRBUTTONDOWN = 0xA4, WM_NCMBUTTONDOWN = 0xA7, WM_KEYFIRST = 0x100, WM_KEYDOWN = 0x100, WM_KEYUP = 0x101, WM_CHAR = 0x102,
                   WM_SYSKEYDOWN = 0x104, WM_SYSKEYUP = 0x105, WM_KEYLAST = 0x109, WM_TIMER = 0x113, WM_SYSTIMER = 0x118, WM_MOUSEFIRST = 0x200, WM_MOUSEMOVE = 0x200,
                   WM_LBUTTONDOWN = 0x201, WM_LBUTTONUP = 0x202, WM_LBUTTONDBLCLK = 0x203, WM_RBUTTONDOWN = 0x204, WM_RBUTTONUP = 0x205, WM_RBUTTONDBLCLK = 0x206,
                   WM_MBUTTONDOWN = 0x207, WM_MBUTTONUP = 0x208, WM_MBUTTONDBLCLK = 0x209, WM_MOUSEWHEEL = 0x20A, WM_XBUTTONDOWN = 0x20B, WM_XBUTTONUP = 0x20C,
                   WM_XBUTTONDBLCLK = 0x20D, WM_MOUSEHWHEEL = 0x20E, WM_MOUSELAST = 0x20E, WM_PARENTNOTIFY = 0x210, WM_CAPTURECHANGED = 0x215, WM_HOTKEY = 0x312,
                   WM_MOUSELEAVE = 0x2A3, WM_MOUSEHOVER = 0x2A1, WM_NCMOUSELEAVE = 0x2A2, WM_USER = 0x400, WM_APP = 0x8000;
constexpr uint32_t HTERROR = 0xFFFFFFFFu, HTTRANSPARENT = 0xFFFFFFFFu, HTNOWHERE = 0, HTCLIENT = 1, HTCAPTION = 2, HTSYSMENU = 3, HTSIZE = 4, HTMENU = 5, HTHSCROLL = 6,
                   HTVSCROLL = 7, HTMINBUTTON = 8, HTMAXBUTTON = 9, HTLEFT = 10, HTRIGHT = 11, HTTOP = 12, HTTOPLEFT = 13, HTTOPRIGHT = 14, HTBOTTOM = 15, HTBOTTOMLEFT = 16,
                   HTBOTTOMRIGHT = 17, HTBORDER = 18, HTCLOSE = 20;
constexpr uint32_t SWP_NOSIZE = 1, SWP_NOMOVE = 2, SWP_NOZORDER = 4, SWP_NOREDRAW = 8, SWP_NOACTIVATE = 0x10, SWP_FRAMECHANGED = 0x20, SWP_SHOWWINDOW = 0x40,
                   SWP_HIDEWINDOW = 0x80, SWP_NOCOPYBITS = 0x100, SWP_NOOWNERZORDER = 0x200, SWP_NOSENDCHANGING = 0x400, SWP_DEFERERASE = 0x2000, SWP_ASYNCWINDOWPOS = 0x4000;
constexpr uint32_t SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOWMINIMIZED = 2, SW_SHOWMAXIMIZED = 3, SW_SHOWNOACTIVATE = 4, SW_SHOW = 5, SW_MINIMIZE = 6, SW_SHOWMINNOACTIVE = 7,
                   SW_SHOWNA = 8, SW_RESTORE = 9, SW_SHOWDEFAULT = 10, SW_FORCEMINIMIZE = 11;
constexpr uint32_t GW_HWNDFIRST = 0, GW_HWNDLAST = 1, GW_HWNDNEXT = 2, GW_HWNDPREV = 3, GW_OWNER = 4, GW_CHILD = 5, GA_PARENT = 1, GA_ROOT = 2, GA_ROOTOWNER = 3;
constexpr int32_t GWL_WNDPROC = -4, GWL_HINSTANCE = -6, GWL_HWNDPARENT = -8, GWL_STYLE = -16, GWL_EXSTYLE = -20, GWL_USERDATA = -21, GWL_ID = -12;
constexpr int32_t GCL_MENUNAME = -8, GCL_HBRBACKGROUND = -10, GCL_HCURSOR = -12, GCL_HICON = -14, GCL_HMODULE = -16, GCL_CBWNDEXTRA = -18, GCL_CBCLSEXTRA = -20,
                  GCL_WNDPROC = -24, GCL_STYLE = -26, GCW_ATOM = -32, GCL_HICONSM = -34;
constexpr uint32_t RDW_INVALIDATE = 1, RDW_INTERNALPAINT = 2, RDW_ERASE = 4, RDW_VALIDATE = 8, RDW_NOINTERNALPAINT = 0x10, RDW_NOERASE = 0x20, RDW_NOCHILDREN = 0x40,
                   RDW_ALLCHILDREN = 0x80, RDW_UPDATENOW = 0x100, RDW_ERASENOW = 0x200, RDW_FRAME = 0x400, RDW_NOFRAME = 0x800;
constexpr uint32_t DCX_WINDOW = 1, DCX_CACHE = 2, DCX_PARENTCLIP = 0x20, DCX_CLIPSIBLINGS = 0x10, DCX_CLIPCHILDREN = 8, DCX_NORESETATTRS = 4, DCX_EXCLUDERGN = 0x40,
                   DCX_INTERSECTRGN = 0x80, DCX_LOCKWINDOWUPDATE = 0x400, DCX_USESTYLE = 0x10000;
constexpr uint32_t QS_KEY = 1, QS_MOUSEMOVE = 2, QS_MOUSEBUTTON = 4, QS_POSTMESSAGE = 8, QS_TIMER = 0x10, QS_PAINT = 0x20, QS_SENDMESSAGE = 0x40, QS_HOTKEY = 0x80,
                   QS_ALLPOSTMESSAGE = 0x100, QS_RAWINPUT = 0x400, QS_MOUSE = 6, QS_INPUT = 0x407, QS_ALLEVENTS = 0x4BF, QS_ALLINPUT = 0x4FF;
constexpr uint32_t PM_NOREMOVE = 0, PM_REMOVE = 1, PM_NOYIELD = 2;
constexpr uint32_t SMTO_NORMAL = 0, SMTO_BLOCK = 1, SMTO_ABORTIFHUNG = 2, SMTO_NOTIMEOUTIFNOTHUNG = 8, SMTO_ERRORONEXIT = 0x20;
constexpr uint32_t VK_LBUTTON = 1, VK_RBUTTON = 2, VK_MBUTTON = 4, VK_XBUTTON1 = 5, VK_XBUTTON2 = 6, VK_BACK = 8, VK_TAB = 9, VK_RETURN = 13, VK_SHIFT = 0x10, VK_CONTROL = 0x11,
                   VK_MENU = 0x12, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1B, VK_SPACE = 0x20, VK_LSHIFT = 0xA0, VK_RSHIFT = 0xA1, VK_LCONTROL = 0xA2, VK_RCONTROL = 0xA3,
                   VK_LMENU = 0xA4, VK_RMENU = 0xA5, VK_NUMLOCK = 0x90, VK_SCROLL = 0x91, VK_PACKET = 0xE7, VK_LWIN = 0x5B, VK_RWIN = 0x5C;
constexpr uint32_t MOD_ALT = 1, MOD_CONTROL = 2, MOD_SHIFT = 4, MOD_WIN = 8, MOD_NOREPEAT = 0x4000;
constexpr uint32_t SB_HORZ = 0, SB_VERT = 1, SB_CTL = 2, SB_BOTH = 3;
constexpr uint32_t SIF_RANGE = 1, SIF_PAGE = 2, SIF_POS = 4, SIF_DISABLENOSCROLL = 8, SIF_TRACKPOS = 0x10, SIF_ALL = 0x17, SIF_RETURNPREV = 0x1000;
constexpr uint32_t ATOM_MENU = 0x8000, ATOM_DESKTOP = 0x8001, ATOM_DIALOG = 0x8002, ATOM_SWITCH = 0x8003, ATOM_ICONTITLE = 0x8004;
}  // namespace wn

constexpr uint32_t kHwndBroadcast = 0xFFFF, kHwndTop = 0, kHwndBottom = 1, kHwndTopmost = 0xFFFFFFFFu, kHwndNotopmost = 0xFFFFFFFEu, kHwndMessage = 0xFFFFFFFDu;
// What a service answers when the calling thread has window-procedure (or hook) calls to make before it can go on: the user-mode half of the service (win32u.c) runs them and
// calls NtUserTcosResume. kRetry is what NtUserTcosResume answers when there is no operation to resume and the service has to be called again. Neither is a value any
// service can return otherwise (they are above any BOOL, count, atom or the 31-bit part of a handle in use; messages results travel in a separate buffer).
constexpr uint64_t kUpcallDue = 0x7FFFFFF1u, kRetry = 0x7FFFFFF2u, kDrainRetry = 0x7FFFFFF3u;
constexpr uint32_t kErrorNoaccess = 998;
constexpr uint32_t kMaxPostedMessages = 10000, kMaxPayload = 4096;

// ---- Regions as lists of disjoint rectangles.
bool rect_empty(const Rect& r);
bool rect_contains(const Rect& r, int32_t x, int32_t y);
Rect rect_intersect(const Rect& a, const Rect& b);
Rect rect_offset(const Rect& r, int32_t dx, int32_t dy);
bool rect_equal(const Rect& a, const Rect& b);
bool region_add(List<Rect>& region, const Rect& r);             // union
void region_subtract(List<Rect>& region, const Rect& r);
void region_intersect(List<Rect>& region, const Rect& r);
void region_offset(List<Rect>& region, int32_t dx, int32_t dy);
Rect region_bounds(const List<Rect>& region);
bool region_contains(const List<Rect>& region, int32_t x, int32_t y);
bool region_copy(List<Rect>& to, const List<Rect>& from);

struct Window;
struct Class;
struct Queue;
struct Desktop;

// ---- Window classes (RegisterClassEx). Per process, except the global ones.
struct Class {
  explicit Class(allocators::IAllocator& a) : name(a), menu_name(a), extra(a) {}
  uint32_t atom = 0;
  WideString name, menu_name;
  uint64_t pid = 0, instance = 0;
  uint32_t style = 0;
  uint64_t wndproc = 0;
  bool unicode = true;
  int32_t cb_class_extra = 0, cb_wnd_extra = 0;
  uint64_t icon = 0, icon_small = 0, cursor = 0, brush = 0;
  uint32_t fnid = 0;
  uint64_t menu_name_ptr = 0;
  bool is_system = false;      // made by the window manager itself (the desktop's class): lives forever
  uint32_t windows = 0;        // windows of this class (a class with windows cannot be unregistered)
  bool dead = false;           // unregistered while windows still use it (it goes with the last window)
  List<uint8_t> extra;
};

// ---- Windows.
struct Prop {
  uint32_t atom = 0;       // the atom of the string key (0 when the key is a plain number below 0x10000: `atom` then holds it, flagged)
  bool is_atom_key = true;
  uint64_t value = 0;
};
struct ScrollBar {
  int32_t min = 0, max = 100, page = 0, pos = 0, track_pos = 0;
  uint32_t disabled = 0;      // ESB_DISABLE_LEFT 1 | RIGHT 2
  bool visible = false;
};
struct Placement { Rect normal; int32_t min_x = -1, min_y = -1, max_x = -1, max_y = -1; bool has_normal = false, restore_maximized = false; };

struct Window {
  Window(allocators::IAllocator& a) : text(a), update(a), props(a), extra(a) {}
  uint32_t handle = 0;
  Window* parent = nullptr;      // null for the desktop window only (the root)
  Window* first_child = nullptr;      // children are in z-order: first = topmost
  Window* last_child = nullptr;
  Window* next = nullptr;        // the next sibling down in z-order
  Window* prev = nullptr;
  Window* owner = nullptr;       // owner window (popups); the owner pointer is cleared when the owner goes
  Class* cls = nullptr;
  Desktop* desktop = nullptr;
  uint64_t thread = 0, pid = 0;
  uint32_t style = 0, ex_style = 0;
  uint64_t wndproc = 0;
  bool unicode = true;
  uint64_t instance = 0, id = 0, userdata = 0;
  Rect bounds;                   // the window rectangle, in the coordinates of the parent's client area (screen coordinates for a top-level window)
  Rect client;                   // the client rectangle, in the same coordinates (not relative to the window)
  Placement placement;
  WideString text;
  List<Rect> update;             // the update region, in client coordinates
  bool paint_pending = false;    // RDW_INTERNALPAINT: a WM_PAINT is due although the update region is empty
  bool erase_pending = false;    // the background must be erased on the next BeginPaint
  bool nc_paint_pending = false;
  bool redraw_disabled = false;  // WM_SETREDRAW(FALSE)
  bool destroying = false;
  List<Prop> props;
  List<uint8_t> extra;           // cbWndExtra bytes
  ScrollBar scroll[2];
  uint32_t fnid = 0, help_id = 0, dpi_context = 0, display_affinity = 0, region = 0;
  uint32_t layered_key = 0, layered_flags = 0;
  uint8_t layered_alpha = 255;
  uint64_t dialog_info = 0, mdi_info = 0;
  uint64_t menu = 0, sys_menu = 0;
  uint64_t private_data[4] = {};
  uint32_t dc_cache_count = 0;
  Window* last_active_popup = nullptr;
  uint32_t creating_stage = 0;   // nonzero while WM_NCCREATE/WM_CREATE run
  uint32_t cs_style = 0, cs_ex_style = 0;    // what CreateWindowEx was given (the CREATESTRUCT of WM_NCCREATE and WM_CREATE), the extended style with WS_EX_WINDOWEDGE decided
  uint32_t destroy_stage = 0;    // 1: WM_DESTROY sent, 2: WM_NCDESTROY sent
};

// ---- Window stations and desktops.
struct WinSta {
  explicit WinSta(allocators::IAllocator& a) : name(a) {}
  uint32_t handle = 0;
  WideString name;
  uint32_t flags = 0;
  uint32_t refs = 0;
};
struct Desktop {
  explicit Desktop(allocators::IAllocator& a) : name(a) {}
  uint32_t handle = 0;
  WideString name;
  WinSta* station = nullptr;
  Window* root = nullptr;       // the desktop window
  Window* message_root = nullptr;      // the parent of message-only windows
  uint32_t refs = 0;
};
// A handle a process opened on a station or desktop.
struct ObjHandle {
  uint32_t handle = 0;
  uint64_t pid = 0;
  WinSta* station = nullptr;
  Desktop* desktop = nullptr;
  uint32_t flags = 0;
};

// ---- Messages, queues, operations.
struct Msg {
  uint32_t hwnd = 0, message = 0;
  uint64_t wparam = 0, lparam = 0;
  uint32_t time = 0;
  int32_t x = 0, y = 0;
  uint64_t extra = 0;
  uint32_t qs = 0;      // QS_* bit it counts as
  bool hardware = false;
};
// A call into user mode. kind: 1 window procedure (hwnd, msg, wparam, lparam), 2 hook procedure (code = msg, wparam, lparam), 3 WINEVENTPROC, 4 SENDASYNCPROC, 5 TIMERPROC.
struct Upcall {
  explicit Upcall(allocators::IAllocator& a) : payload(a) {}
  uint64_t id = 0;
  uint64_t thread = 0;          // the thread that makes the call
  uint32_t kind = 1, flags = 0;
  uint64_t proc = 0;
  uint32_t hwnd = 0, msg = 0;
  uint64_t wparam = 0, lparam = 0;
  uint64_t extra[3] = {};
  List<uint8_t> payload;       // a copy of the structure lparam points to (up to kMaxPayload bytes)
  uint16_t fix_at = 0xFFFF, fix_to = 0;      // the payload holds a pointer at fix_at to its own offset fix_to (set by the user-mode half)
  uint64_t out_addr = 0;       // cross-process send: where the changed payload goes back to in the sender (written by the sender when the call is done)
  enum State : uint8_t { Queued, Delivered, Done } state = Queued;
  bool abandoned = false;       // the sender gave up (timeout): whoever finishes it erases it
  bool failed = false;          // could not be made (the thread went away)
  uint64_t result = 0;
  uint64_t op = 0;              // the operation that waits for it (0: a sender in another thread waits)
  uint64_t sender_thread = 0;
  uint64_t sender_pid = 0;
};
constexpr uint32_t kUpcallWindowProc = 1, kUpcallHook = 2, kUpcallWinEvent = 3, kUpcallSendAsync = 4, kUpcallTimer = 5;
constexpr uint32_t UPF_CREATESTRUCT = 1;      // the payload is a CREATESTRUCTW: the user-mode half fills in the name and class pointers
constexpr uint32_t UPF_PAYLOAD_LPARAM = 2;    // lparam is the address of a copy of the payload; the payload is sent back changed in UpcallDone
constexpr uint32_t UPF_WNDPROC_ANSI = 4;

struct Op;
using OpStep = uint64_t (*)(Call& call, Op& op);
// An operation of a thread that needs window-procedure calls in the middle (CreateWindowEx, SendMessage to the own thread, ...). The thread's operations form a stack.
struct Op {
  explicit Op(allocators::IAllocator& a) : payload(a), notes(a) {}
  uint64_t id = 0;
  uint64_t thread = 0;
  OpStep step = nullptr;        // called when every call the operation made has finished; answers the result of the service or kUpcallDue after queueing more calls
  uint32_t phase = 0;
  uint64_t a[12] = {};          // a[7] after a wait for a call in another thread: 0 answered, 1 timed out, 2 failed
  uint64_t result = 0;
  uint64_t waiting_on = 0;      // an upcall made in another thread whose result the operation waits for
  uint64_t deadline_ns = 0;     // for waiting_on: ~0 = none
  uint32_t outstanding = 0;     // own-thread upcalls not done yet
  uint64_t first_result = 0;    // the result of the last finished own-thread upcall
  List<uint8_t> payload;        // what the last finished upcall sent back
  List<uint64_t> notes;         // messages still to send, three words each: hwnd | message << 32, wparam, lparam (sent one after the other before the next step)
};

struct InputState {      // shared by attached threads
  Window* active = nullptr;
  Window* focus = nullptr;
  Window* capture = nullptr;
  Window* caret_window = nullptr;
  int32_t caret_x = 0, caret_y = 0, caret_w = 0, caret_h = 0, caret_hide = 1;
  bool caret_visible = false;
  uint64_t caret_bitmap = 0;
  uint8_t key_state[256] = {};      // GetKeyState: bit 7 down, bit 0 toggled
  uint32_t refs = 0;
  Window* mouse_window = nullptr;      // the window the pointer was last over (for WM_MOUSELEAVE)
  uint32_t track_hwnd = 0, track_flags = 0;
};

struct Queue {
  explicit Queue(allocators::IAllocator& a) : posted(a), input(a), upcalls(a), ops(a) {}
  uint64_t thread = 0, pid = 0;
  List<Msg> posted;
  List<Msg> input;      // hardware messages
  List<uint64_t> upcalls;      // ids of the Queued upcalls for this thread, oldest first
  List<uint64_t> ops;          // the operation stack
  InputState* input_state = nullptr;
  bool quit = false;
  int32_t quit_code = 0;
  uint32_t changed = 0;        // QS_* bits of things that arrived since the last look
  uint32_t last_time = 0;
  int32_t last_x = 0, last_y = 0;
  uint64_t extra_info = 0;
  uint64_t current_send_sender = 0;      // thread whose message the thread is handling (InSendMessage), 0 if none
  uint32_t in_send_depth = 0;
  uint64_t replied_result = 0;
  uint64_t current_upcall = 0;
  bool last_due_has_op = false;
  uint64_t desktop_handle = 0;
  uint32_t message_source = 0;
  bool dead = false;
};

struct Timer {
  uint64_t thread = 0, pid = 0;
  uint32_t hwnd = 0;
  uint64_t id = 0;
  uint32_t elapse_ms = 0;
  uint64_t due_ns = 0;
  uint64_t proc = 0;
  bool system = false;
  bool ready = false;         // due and not retrieved yet
};

struct Hook {
  uint32_t handle = 0;
  uint64_t thread = 0;         // 0: global
  uint64_t pid = 0;            // the process that installed it
  int32_t id = 0;
  uint64_t proc = 0, module = 0;
  bool unicode = true;
  bool removed = false;
  uint64_t seq = 0;
};
struct WinEventHook {
  uint32_t handle = 0;
  uint64_t pid = 0, thread = 0, target_pid = 0, target_thread = 0;
  uint32_t event_min = 0, event_max = 0, flags = 0;
  uint64_t proc = 0;
};
struct HotKey {
  uint64_t thread = 0;
  uint32_t hwnd = 0;
  int32_t id = 0;
  uint32_t modifiers = 0, vk = 0;
};
struct DeferPos {
  uint32_t handle = 0;
  uint64_t thread = 0;
  struct Item { uint32_t hwnd = 0, after = 0; int32_t x = 0, y = 0, cx = 0, cy = 0; uint32_t flags = 0; };
  List<Item> items;
  explicit DeferPos(allocators::IAllocator& a) : items(a) {}
};
struct Atom {
  explicit Atom(allocators::IAllocator& a) : name(a) {}
  uint32_t id = 0;
  WideString name;
};
struct ProcessInfo {      // per process
  uint64_t pid = 0;
  uint32_t winsta = 0;                          // the process window station (handle)
  uint64_t client_procs[3][17] = {};            // the arrays of NtUserInitializeClientPfnArrays (A, W, workers)
  bool pfn_set = false;
  uint64_t user_module = 0;
  uint32_t dpi_awareness = 0;
  uint32_t default_layout = 0;
  uint32_t startup_flags = 0;
};
struct ThreadDesktop { uint64_t thread = 0; uint32_t handle = 0; };
struct WinDc { uint32_t dc = 0, hwnd = 0; uint64_t pid = 0; bool window_dc = false; bool clip_children = false, clip_siblings = false; };

// The handle table of one kind of object (windows, hooks, ...): Hive storage, handles = generation << 16 | (slot + 0x20).
template <class T>
class HandleTable {
 public:
  explicit HandleTable(allocators::IAllocator& a) : items_(a), slots_(a), gens_(a), free_(a) {}
  template <class... Args>
  T* create(Args&&... args) {
    uint32_t slot;
    if (free_.count() != 0) {
      slot = uint32_t(free_[free_.count() - 1]);
      free_.remove_at(free_.count() - 1);
    } else {
      if (slots_.count() >= 0xFF00) return nullptr;
      slot = uint32_t(slots_.count());
      if (!slots_.push_back(nullptr) || !gens_.push_back(1)) return nullptr;
    }
    T* item = items_.emplace(static_cast<Args&&>(args)...);
    if (!item) { free_.push_back(slot); return nullptr; }
    slots_[slot] = item;
    item->handle = (uint32_t{gens_[slot]} << 16) | (slot + 0x20);
    return item;
  }
  T* find(uint32_t handle) const {
    const uint32_t slot = (handle & 0xFFFF) - 0x20;
    if ((handle & 0xFFFF) < 0x20 || slot >= slots_.count()) return nullptr;
    T* item = slots_[slot];
    return item && item->handle == handle ? item : nullptr;
  }
  void destroy(T* item) {
    const uint32_t slot = (item->handle & 0xFFFF) - 0x20;
    if (slot < slots_.count() && slots_[slot] == item) {
      slots_[slot] = nullptr;
      uint16_t g = uint16_t(gens_[slot] + 1);
      gens_[slot] = g > 0x7FFF ? uint16_t(1) : g;
      free_.push_back(slot);
    }
    items_.erase(item);
  }
  Hive<T>& items() { return items_; }

 private:
  Hive<T> items_;
  List<T*> slots_;
  List<uint16_t> gens_;
  List<uint64_t> free_;
};

// ---- The whole state.
struct State {
  explicit State(allocators::IAllocator& a);
  allocators::IAllocator& alloc;
  HandleTable<Window> windows;
  HandleTable<Hook> hooks;
  HandleTable<WinEventHook> win_events;
  HandleTable<DeferPos> defers;
  HandleTable<WinSta> stations;
  HandleTable<Desktop> desktops;
  HandleTable<ObjHandle> object_handles;      // handles to stations and desktops, per process
  Hive<Class> classes;
  Hive<Atom> atoms;
  Hive<Queue> queues;
  Hive<InputState> input_states;
  Hive<Upcall> upcalls;
  Hive<Op> ops;
  Hive<Timer> timers;
  Hive<HotKey> hotkeys;
  Hive<ProcessInfo> processes;
  Hive<WinDc> dcs;
  List<ThreadDesktop> thread_desktops;
  Class* desktop_class = nullptr;
  Class* message_class = nullptr;
  uint32_t next_atom = 0xC000;
  uint64_t next_upcall = 1, next_op = 1, next_timer = 1, next_hook_seq = 1;
  Desktop* initial_desktop = nullptr;
  Desktop* input_desktop = nullptr;
  uint32_t shell_window = 0, progman_window = 0, taskman_window = 0, shell_list_window = 0;
  uint64_t foreground_thread = 0;      // the queue that owns the foreground
  uint8_t async_keys[256] = {};        // physical key state: bit 7 down, bit 0 pressed since the last GetAsyncKeyState
  uint8_t toggle_keys[256] = {};
  int32_t cursor_x = 0, cursor_y = 0;
  bool cursor_init = false;
  int32_t clip_cursor_set = 0;
  Rect clip_cursor;
  uint32_t lock_update_window = 0;
  uint64_t lock_update_thread = 0;
  uint64_t last_click_ns = 0;
  uint32_t last_click_button = 0, last_click_hwnd = 0;
  int32_t last_click_x = 0, last_click_y = 0;
  uint32_t now_ms_base = 0;
  uint64_t last_now_ns = 0;
  uint32_t caret_blink_ms = 530;
  uint32_t double_click_ms = 500;
};

// ---- Accessors (Win32kUser.cpp).
State& state();                                   // creates the state on first use (needs the system allocator)
State* state_or_null();
uint64_t thread_of(Call& call);                   // the calling thread (tests override it per host thread)
uint32_t now_ms(Call& call);
uint32_t now_ms_from_ns(uint64_t ns);
Window* find_window(uint32_t handle);              // null for a handle that is not a window (sign-extended 64-bit values are accepted)
Window* window_arg(Call& call, uint32_t index);    // argument `index` as a window; sets ERROR_INVALID_WINDOW_HANDLE when there is none (null result)
Queue* find_queue(uint64_t thread);
Queue* queue_of(Call& call, bool create);          // the calling thread's queue
ProcessInfo& process_info(uint64_t pid);
Desktop* thread_desktop(Call& call);               // the desktop the calling thread works on (creates the default one)
Desktop* default_desktop();
bool is_child_of(const Window* ancestor, const Window* w);   // `w` is a descendant of `ancestor`
bool is_visible(const Window* w);
bool is_top_level(const Window* w);
bool is_enabled(const Window* w);
Window* top_level_of(Window* w);
Rect window_rect_screen(const Window* w);
Rect client_rect_screen(const Window* w);
void client_origin(const Window* w, int32_t* x, int32_t* y);       // the screen position of the client area's top-left
void parent_origin(const Window* w, int32_t* x, int32_t* y);
Window* get_window_relative(const Window* w, uint32_t rel);
Window* window_at_point(int32_t x, int32_t y, bool skip_transparent, uint64_t thread_for_transparent);
Window* child_at_point(Window* parent, int32_t x, int32_t y, bool real);
uint32_t hit_test(const Window* w, int32_t x, int32_t y);
void link_window(Window* w, Window* parent, Window* insert_after /* null: first */, bool at_end);
void unlink_window(Window* w);
void window_text_set(Window* w, ArrayView<const char16_t> text);

// Atoms.
uint32_t atom_find(ArrayView<const char16_t> name);
uint32_t atom_add(ArrayView<const char16_t> name);
const Atom* atom_by_id(uint32_t id);

// Classes.
Class* find_class(Call& call, uint32_t atom_or_0, ArrayView<const char16_t> name, uint64_t instance, bool check_instance);
Class* find_class_for_process(uint64_t pid, uint32_t atom, uint64_t instance, bool check_instance);
void class_window_gone(Class* cls);
void create_system_classes();
bool class_get_value(Call& call, Window* w, int32_t offset, uint32_t size, uint64_t* out);
bool class_set_value(Call& call, Window* w, int32_t offset, uint32_t size, uint64_t value, uint64_t* old);
void drop_process_classes(uint64_t pid);
bool read_class_name(Call& call, uint64_t unicode_string_addr, WideString* out, uint32_t* atom);

// Visible region and DCs (Win32kUserPaint.cpp).
bool visible_region(const Window* w, bool client_area, bool clip_children, bool clip_siblings, List<Rect>* out);
void invalidate_window(Window* w, const Rect* client_rect_or_null, bool erase);
void validate_window(Window* w, const Rect* client_rect_or_null);
void release_window_dcs(uint32_t hwnd);
void refresh_window_dcs(Window* w);
Upcall* queue_erase(Call& call, Op* op, Window* w, uint32_t* dc_out);
void paint_region_changed(Window* w);

// Operations and upcalls (Win32kUserMessage.cpp).
Op* op_begin(Call& call, OpStep step);
Upcall* upcall_to_self(Call& call, Op* op, uint32_t hwnd, uint32_t msg, uint64_t wparam, uint64_t lparam);
Upcall* send_proc_upcall(Call& call, Op* op, Window* w, uint32_t msg, uint64_t wparam, uint64_t lparam);   // a window-procedure call on the window's thread (this thread: part of op)
uint64_t op_continue(Call& call, Op& op);              // runs the operation as far as it goes: the result or kUpcallDue
uint64_t op_return(Call& call, Op* op, uint64_t immediate);   // `op` queued upcalls? then kUpcallDue (the result is `immediate` at the end), else erases it and answers immediate
bool post_to_queue(Queue& q, const Msg& m);
void queue_wake(Queue& q);
void thread_gone(uint64_t thread);
void kill_timers_of_window(uint32_t hwnd);
void kill_timers_of_thread(uint64_t thread);
void drop_hotkeys_of_window(uint32_t hwnd);
void remove_window_messages(Window* w);
void notify_input_state_window_gone(Window* w);
uint32_t qs_state(Queue& q, Call* call_for_paint);
void post_hardware(Queue& q, Msg m);
void pump_timers(uint64_t now_ns);

void on_process_exit_hook(uint64_t pid);
void install_user_services();
// Services registered by the other files.
void register_class_services();
void register_window_services();
void register_geometry_services();
void register_paint_services();
void register_message_services();
void register_input_services();
void register_hook_services();
void register_station_services();

// Destroys a window and everything below it (no messages are sent): the process-exit path and the second half of DestroyWindow.
void destroy_window_tree(Window* w);
void destroy_window_now(Window* w);

// SetWindowPos core, shared with ShowWindow/CreateWindow/Move.
struct SwpArgs { Window* w = nullptr; uint32_t after = 0; int32_t x = 0, y = 0, cx = 0, cy = 0; uint32_t flags = 0; };
uint64_t set_window_pos_op(Call& call, const SwpArgs& args, uint64_t final_result);
void apply_zorder(Window* w, uint32_t after);
Rect window_maximized_rect(Window* w);                   // where a maximized window goes
void compute_nc(Window* w, Rect* client_out);       // the default client rectangle for the window rectangle `w->bounds` (AdjustWindowRect's inverse)
uint64_t show_window_op(Call& call, Window* w, uint32_t cmd, bool async_ignored);
void activate_window_internal(Call& call, Window* w, bool notify);
void queue_destroy_focus_changes(Call& call, Op* op, Window* root);      // focus/activation leave `root` and its descendants (messages queued on the operation)
void activate_other_window(Call& call, Op* op, Window* w);
Upcall* queue_payload_upcall(Call& call, Op* op, Window* w, uint32_t msg, uint64_t wparam, const List<uint8_t>& payload, uint16_t fix_at, uint16_t fix_to, uint32_t flags);
void invalidate_screen_rect(const Rect& screen, const Window* except_tree);
uint64_t swp_op_begin(Call& call, const SwpArgs& args, uint32_t show_cmd, bool want_visible_result, bool defer_hdwp);

// Notifications: messages a service sends after changing state (focus, activation, ...). They go to an operation (sent in order on the calling thread / to the window's thread)
// or, when no thread is calling (the input path), are queued as system-sent messages for the window's thread.
void add_note(Op& op, uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam);
void system_send(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam);
struct Notes {
  explicit Notes(allocators::IAllocator& a) : list(a) {}
  List<uint64_t> list;
  void add(uint32_t hwnd, uint32_t message, uint64_t wparam, uint64_t lparam) {
    list.push_back(uint64_t{hwnd} | uint64_t{message} << 32);
    list.push_back(wparam);
    list.push_back(lparam);
  }
};
void flush_notes(Notes& notes, Op* op);
void activation_changes(Queue& q, Window* new_top, bool click, Notes& notes);
void focus_changes(Queue& q, Window* new_focus, Notes& notes);
uint64_t destroy_window_op(Call& call, Window* w);
uint64_t def_window_proc_entry(Call& call, uint32_t hwnd, uint32_t msg, uint64_t wparam, uint64_t lparam, bool ansi);
// Focus and activation, key state (Win32kUserInput.cpp).
InputState& input_of(Queue& q);
Window* set_focus_internal(Queue& q, Window* w);
void focus_window_gone(Window* w);
void key_message_to(Queue& q, Window* w, uint32_t message, uint32_t vk, uint32_t lparam);
void inject_key_locked(uint32_t vk, uint32_t scan, bool down, bool extended);
void inject_char_locked(char32_t c);
void inject_move_locked(int32_t x, int32_t y);
void inject_motion_locked(int32_t dx, int32_t dy);
void inject_button_locked(uint32_t button, bool down);
void inject_wheel_locked(int32_t delta, bool horizontal);
void set_cursor_pos_locked(int32_t x, int32_t y);
Window* foreground_window_ptr();
Queue* foreground_queue();
bool set_foreground_internal(Window* w);

// Hooks (Win32kUserHook.cpp).
bool has_hooks(int32_t id, uint64_t thread);

// Writes `text` cut to leave room for the terminating NUL; answers the number of characters written, or -1 if the memory is not writable.
int32_t write_wide_z(Call& call, uint64_t address, ArrayView<const char16_t> text, uint32_t capacity);
// Helpers for argument decoding.
struct Point { int32_t x = 0, y = 0; };
bool read_rect(Call& call, uint64_t address, Rect* out);
bool write_rect(Call& call, uint64_t address, const Rect& r);
bool read_point(Call& call, uint64_t address, Point* out);
bool write_point(Call& call, uint64_t address, const Point& p);
uint64_t sign_extend32(uint32_t v);
inline uint32_t hwnd_arg(uint64_t v) { return uint32_t(v); }

}  // namespace process::win32k::user
