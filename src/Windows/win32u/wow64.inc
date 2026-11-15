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

/* win32u.dll for i386: the services whose arguments are structures that a 32-bit program lays out differently (docs/Wow64.md). Included by win32u.c
 * when it is built for i386, after the upcall code.
 *
 * user32.dll and gdi32.dll are the same sources for both widths and hand a service what the program gave them, in the program's layout. The kernel
 * reads and writes the 64-bit layout. For the services below the exported function converts on the way in and on the way out; every other service
 * goes to the kernel as it is, which is right when its structures hold no handle or pointer (RECT, POINT, LOGFONT, BITMAPINFO, ...).
 * What is not converted yet is listed in docs/Wow64.md. */

/* (the compiler makes calls of these for a copy or a fill whose size it does not know, and this library imports nothing) */
void* memcpy(void* destination, const void* source, __SIZE_TYPE__ bytes) {
  U8* d = destination;
  const U8* s = source;
  while (bytes--) *d++ = *s++;
  return destination;
}
void* memset(void* destination, int value, __SIZE_TYPE__ bytes) {
  U8* d = destination;
  while (bytes--) *d++ = (U8)value;
  return destination;
}

/* A service called with arguments that are 64 bits already (a value that does not fit a 32-bit argument). */
static U wow_service64(U32 number, const U64* arguments) {
  U result;
  __asm__ volatile("int $0x2e" : "=a"(result) : "a"(number), "d"(arguments) : "memory", "cc");
  return result;
}

/* The wait a service may ask for: window procedure calls to make, then the service is continued (TCOS_UPCALL_DUE) or called again (TCOS_DRAIN_RETRY). */
#define WOW_RUN(result, call)                                                                    \
  for ((result) = (call);;) {                                                                    \
    if ((result) == TCOS_UPCALL_DUE) { drain_upcalls(); (result) = NtUserTcosResume(); }         \
    else if ((result) == TCOS_DRAIN_RETRY) { drain_upcalls(); (result) = (call); }               \
    else break;                                                                                  \
  }

/* ---- UNICODE_STRING ------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { U16 length, maximum; U32 buffer; } String32;
typedef struct { U16 length, maximum; U32 pad; U64 buffer; } String64;
/* The 64-bit form of the string at `in` (0 stays 0); the result is what the service is given. */
static U string_to_64(String64* out, U in) {
  if (!in) return 0;
  const String32* s = (const String32*)in;
  out->length = s->length; out->maximum = s->maximum; out->pad = 0; out->buffer = s->buffer;
  return (U)out;
}

/* ---- window classes ---------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { U32 size, style, proc; int class_extra, window_extra; U32 instance, icon, cursor, background, menu_name, class_name, small_icon; } WndClass32;
typedef struct { U32 size, style; U64 proc; int class_extra, window_extra; U64 instance, icon, cursor, background, menu_name, class_name, small_icon; } WndClass64;
static void class_to_64(WndClass64* out, const WndClass32* in) {
  out->size = sizeof(*out); out->style = in->style; out->proc = in->proc; out->class_extra = in->class_extra; out->window_extra = in->window_extra;
  out->instance = in->instance; out->icon = in->icon; out->cursor = in->cursor; out->background = in->background;
  out->menu_name = in->menu_name; out->class_name = in->class_name; out->small_icon = in->small_icon;
}
static void class_to_32(WndClass32* out, const WndClass64* in) {
  out->size = sizeof(*out); out->style = in->style; out->proc = (U32)in->proc; out->class_extra = in->class_extra; out->window_extra = in->window_extra;
  out->instance = (U32)in->instance; out->icon = (U32)in->icon; out->cursor = (U32)in->cursor; out->background = (U32)in->background;
  out->menu_name = (U32)in->menu_name; out->class_name = (U32)in->class_name; out->small_icon = (U32)in->small_icon;
}

extern U NtUserRegisterClassExWOW_raw(U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserRegisterClassExWOW(U wc, U name, U version, U menu, U fnid, U flags, U wow) {
  WndClass64 wide;
  String64 name64, version64;
  U64 menu64[3] = {0, 0, 0};      /* (the menu name as the program gave it: three pointers) */
  if (!wc) return NtUserRegisterClassExWOW_raw(0, 0, 0, 0, fnid, flags, wow);
  class_to_64(&wide, (const WndClass32*)wc);
  if (menu) for (int i = 0; i < 3; ++i) menu64[i] = ((const U32*)menu)[i];
  return NtUserRegisterClassExWOW_raw((U)&wide, string_to_64(&name64, name), string_to_64(&version64, version), menu ? (U)menu64 : 0, fnid, flags, wow);
}
extern U NtUserGetClassInfoEx_raw(U, U, U, U, U);
__attribute__((dllexport)) U NtUserGetClassInfoEx(U instance, U name, U wc, U menu, U ansi) {
  WndClass64 wide;
  String64 name64;
  U64 menu64 = 0;
  if (wc) class_to_64(&wide, (const WndClass32*)wc);      /* (the kernel keeps the two name pointers the caller put there) */
  const U atom = NtUserGetClassInfoEx_raw(instance, string_to_64(&name64, name), wc ? (U)&wide : 0, menu ? (U)&menu64 : 0, ansi);
  if (atom && wc) class_to_32((WndClass32*)wc, &wide);
  if (atom && menu) *(U32*)menu = (U32)menu64;
  return atom;
}
extern U NtUserUnregisterClass_raw(U, U, U);
__attribute__((dllexport)) U NtUserUnregisterClass(U name, U instance, U menu) {
  String64 name64;
  U64 menu64[3] = {0, 0, 0};
  const U result = NtUserUnregisterClass_raw(string_to_64(&name64, name), instance, menu ? (U)menu64 : 0);
  if (menu) for (int i = 0; i < 3; ++i) ((U32*)menu)[i] = (U32)menu64[i];
  return result;
}
extern U NtUserGetClassName_raw(U, U, U);
__attribute__((dllexport)) U NtUserGetClassName(U hwnd, U real, U name) {
  String64 name64;
  return NtUserGetClassName_raw(hwnd, real, string_to_64(&name64, name));
}
extern U NtUserRegisterWindowMessage_raw(U);
__attribute__((dllexport)) U NtUserRegisterWindowMessage(U name) {
  String64 name64;
  return NtUserRegisterWindowMessage_raw(string_to_64(&name64, name));
}
extern U NtUserGetAtomName_raw(U, U);
__attribute__((dllexport)) U NtUserGetAtomName(U atom, U name) {
  String64 name64;
  return NtUserGetAtomName_raw(atom, string_to_64(&name64, name));
}
extern U NtUserFindWindowEx_raw(U, U, U, U, U);
__attribute__((dllexport)) U NtUserFindWindowEx(U parent, U child, U class_name, U title, U unknown) {
  String64 class64, title64;
  return NtUserFindWindowEx_raw(parent, child, string_to_64(&class64, class_name), string_to_64(&title64, title), unknown);
}

/* ---- creating a window ------------------------------------------------------------------------------------------------------------------------------------------- */
extern U NtUserCreateWindowEx_raw(U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserCreateWindowEx(U ex_style, U class_name, U version, U window_name, U style, U x, U y, U cx, U cy, U parent, U menu, U instance, U params, U flags,
                                                  U client_instance, U cls, U ansi) {
  String64 class64, version64, title64;
  const U64 title = window_name ? ((const String32*)window_name)->buffer : 0;      /* (remembered for the CREATESTRUCT of the creation messages, with the class name) */
  const int slot = create_context_enter(title, cls);
  U r;
  WOW_RUN(r, NtUserCreateWindowEx_raw(ex_style, string_to_64(&class64, class_name), string_to_64(&version64, version), string_to_64(&title64, window_name), style, x, y, cx, cy,
                                      parent, menu, instance, params, flags, client_instance, cls, ansi));
  create_context_leave(slot);
  return r;
}

/* ---- MSG ---------------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { U32 hwnd, message, wparam; int lparam; U32 time; int x, y; } Msg32;      /* (28 bytes: a 32-bit program's MSG has no private word) */
typedef struct { U64 hwnd; U32 message, pad; U64 wparam; long long lparam; U32 time; int x, y; U32 private_; } Msg64;
/* Two messages the window manager posts to itself through the program carry 64 bits in lparam (WM_SYSCOMMAND with the commands 0xF1E0 and 0xF1F0:
 * a window's new place, x, y, width and height in 16 bits each). A 32-bit MSG holds half of that. The whole value is remembered when the message is
 * taken from the queue and put back when the program dispatches it or gives it to DefWindowProc. */
#define WM_SYSCOMMAND_ 0x0112u
#define WOW_WIDE_MESSAGES 8
static struct { volatile U32 hwnd, wparam; U64 lparam; } g_wide_lparam[WOW_WIDE_MESSAGES];
static volatile U32 g_wide_next;
static int carries_wide_lparam(U32 message, U32 wparam) { return message == WM_SYSCOMMAND_ && ((wparam & 0xFFF0u) == 0xF1E0u || (wparam & 0xFFF0u) == 0xF1F0u); }
static void remember_wide_lparam(U32 hwnd, U32 wparam, U64 lparam) {
  const U32 slot = __atomic_fetch_add(&g_wide_next, 1, __ATOMIC_RELAXED) % WOW_WIDE_MESSAGES;
  g_wide_lparam[slot].lparam = lparam; g_wide_lparam[slot].wparam = wparam; g_wide_lparam[slot].hwnd = hwnd;
}
/* The remembered value whose low half is `lparam`, else `lparam` with its sign. */
static U64 wide_lparam(U32 hwnd, U32 message, U32 wparam, int lparam) {
  if (carries_wide_lparam(message, wparam)) {
    for (int i = 0; i < WOW_WIDE_MESSAGES; ++i)
      if (g_wide_lparam[i].hwnd == hwnd && g_wide_lparam[i].wparam == wparam && (U32)g_wide_lparam[i].lparam == (U32)lparam) return g_wide_lparam[i].lparam;
  }
  return (U64)(long long)lparam;
}
static void msg_to_64(Msg64* out, const Msg32* in) {
  out->hwnd = in->hwnd; out->message = in->message; out->pad = 0; out->wparam = in->wparam; out->time = in->time; out->x = in->x; out->y = in->y;
  out->lparam = (long long)wide_lparam(in->hwnd, in->message, in->wparam, in->lparam);
  out->private_ = 0;
}
static void msg_to_32(Msg32* out, const Msg64* in) {
  out->hwnd = (U32)in->hwnd; out->message = in->message; out->wparam = (U32)in->wparam; out->lparam = (int)in->lparam; out->time = in->time; out->x = in->x; out->y = in->y;
  if (carries_wide_lparam(in->message, (U32)in->wparam)) remember_wide_lparam((U32)in->hwnd, (U32)in->wparam, (U64)in->lparam);
}
extern U NtUserGetMessage_raw(U, U, U, U);
__attribute__((dllexport)) U NtUserGetMessage(U msg, U hwnd, U first, U last) {
  Msg64 wide;
  U r;
  __builtin_memset(&wide, 0, sizeof(wide));
  WOW_RUN(r, NtUserGetMessage_raw(msg ? (U)&wide : 0, hwnd, first, last));
  if (msg && r != (U)-1) msg_to_32((Msg32*)msg, &wide);
  return r;
}
extern U NtUserPeekMessage_raw(U, U, U, U, U);
__attribute__((dllexport)) U NtUserPeekMessage(U msg, U hwnd, U first, U last, U flags) {
  Msg64 wide;
  U r;
  __builtin_memset(&wide, 0, sizeof(wide));
  WOW_RUN(r, NtUserPeekMessage_raw(msg ? (U)&wide : 0, hwnd, first, last, flags));
  if (msg && (U32)r) msg_to_32((Msg32*)msg, &wide);
  return r;
}
extern U NtUserDispatchMessage_raw(U);
__attribute__((dllexport)) U NtUserDispatchMessage(U msg) {
  Msg64 wide;
  U r;
  if (msg) msg_to_64(&wide, (const Msg32*)msg);
  WOW_RUN(r, NtUserDispatchMessage_raw(msg ? (U)&wide : 0));
  return r;
}
extern U NtUserTranslateMessage_raw(U, U);
__attribute__((dllexport)) U NtUserTranslateMessage(U msg, U flags) {
  Msg64 wide;
  if (msg) msg_to_64(&wide, (const Msg32*)msg);
  return NtUserTranslateMessage_raw(msg ? (U)&wide : 0, flags);
}
extern U NtUserCallMsgFilter_raw(U, U);
__attribute__((dllexport)) U NtUserCallMsgFilter(U msg, U code) {
  Msg64 wide;
  U r;
  if (msg) msg_to_64(&wide, (const Msg32*)msg);
  WOW_RUN(r, NtUserCallMsgFilter_raw(msg ? (U)&wide : 0, code));
  return r;
}

/* ---- a message with a structure, given to the kernel (SendMessage, DefWindowProc) ---------------------------------------------------------------------------------
 * The other direction of message_to_32 above: the program's 32-bit structure becomes the 64-bit one the kernel reads, and what the kernel (or the
 * window procedure it calls) changed comes back. */
typedef union { U8 create[80]; U8 pos[48]; struct { U8 rects[48]; U64 pos_pointer; U8 pos[48]; } calc; struct { U64 data; U32 bytes, pad; U64 pointer; } copy; } Message64;
#define MC_SEND_MESSAGE_TIMEOUT_ 0x2b3u
static U message_to_64(U32 message, U wparam, U lparam, Message64* m) {
  if (!lparam) return lparam;
  if (message == WM_NCCREATE_ || message == WM_CREATE_) {
    const CreateStruct32* c = (const CreateStruct32*)lparam;
    U8* p = m->create;
    __builtin_memset(p, 0, sizeof(m->create));
    put64s(p, c->params); put64s(p + 8, c->instance); put64s(p + 16, c->menu); put64s(p + 24, c->parent);
    put32(p + 32, (U32)c->cy); put32(p + 36, (U32)c->cx); put32(p + 40, (U32)c->y); put32(p + 44, (U32)c->x); put32(p + 48, c->style);
    put64s(p + 56, c->name); put64s(p + 64, c->cls); put32(p + 72, c->ex_style);
    return (U)p;
  }
  if (message == WM_WINDOWPOSCHANGING_ || message == WM_WINDOWPOSCHANGED_) {
    __builtin_memset(m->pos, 0, sizeof(m->pos));
    window_pos_from_32(m->pos, (const WindowPos32*)lparam);
    return (U)m->pos;
  }
  if (message == WM_NCCALCSIZE_ && wparam) {
    const NcCalcSize32* c = (const NcCalcSize32*)lparam;
    __builtin_memcpy(m->calc.rects, c->rects, 48);
    __builtin_memset(m->calc.pos, 0, sizeof(m->calc.pos));
    if (c->pos) window_pos_from_32(m->calc.pos, (const WindowPos32*)c->pos);
    m->calc.pos_pointer = c->pos ? (U64)(U)m->calc.pos : 0;
    return (U)&m->calc;
  }
  if (message == WM_COPYDATA_) {
    const CopyData32* c = (const CopyData32*)lparam;
    m->copy.data = c->data; m->copy.bytes = c->bytes; m->copy.pad = 0; m->copy.pointer = c->pointer;
    return (U)&m->copy;
  }
  return lparam;
}
static void message_back_to_32(U32 message, U wparam, U lparam, const Message64* m) {
  if (!lparam) return;
  if (message == WM_WINDOWPOSCHANGING_ || message == WM_WINDOWPOSCHANGED_) window_pos_to_32((WindowPos32*)lparam, m->pos);
  else if (message == WM_NCCALCSIZE_ && wparam) {
    NcCalcSize32* c = (NcCalcSize32*)lparam;
    __builtin_memcpy(c->rects, m->calc.rects, 48);
    if (c->pos) window_pos_to_32((WindowPos32*)c->pos, m->calc.pos);
  }
}
extern U NtUserMessageCall_raw(U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserMessageCall(U hwnd, U message, U wparam, U lparam, U info, U type, U ansi) {
  Message64 wide;
  struct { U32 flags, timeout; U64 result; } timeout64;      /* SendMessageTimeout: the kernel writes a 64-bit result */
  const U lparam64 = message_to_64((U32)message, wparam, lparam, &wide);
  const int timed = type == MC_SEND_MESSAGE_TIMEOUT_ && info;
  if (timed) { timeout64.flags = ((const U32*)info)[0]; timeout64.timeout = ((const U32*)info)[1]; timeout64.result = 0; }
  U r;
  if (carries_wide_lparam((U32)message, (U32)wparam)) {      /* (the whole 64 bits of its lparam: see the MSG conversion) */
    const U64 arguments[7] = {(U64)(long long)(int)hwnd, message, wparam, wide_lparam((U32)hwnd, (U32)message, (U32)wparam, (int)lparam), (U64)(long long)(int)info, type, ansi};
    WOW_RUN(r, wow_service64(NT_SERVICE_NtUserMessageCall, arguments));
    return r;
  }
  WOW_RUN(r, NtUserMessageCall_raw(hwnd, message, wparam, lparam64, timed ? (U)&timeout64 : info, type, ansi));
  if (lparam64 != lparam) message_back_to_32((U32)message, wparam, lparam, &wide);
  if (timed) ((U32*)info)[2] = (U32)timeout64.result;
  return r;
}

/* ---- NtUserCallHwndParam: the calls whose parameter is a block with a pointer in it ---------------------------------------------------------------------------- */
enum { CHP_GET_SCROLL_INFO_ = 7, CHP_GET_WINDOW_RECT_ = 13, CHP_GET_CLIENT_RECT_ = 14, CHP_GET_PRESENT_RECT_ = 15, CHP_MAP_WINDOW_POINTS_ = 20, CHP_GET_PRIVATE_DATA_ = 30, CHP_SET_PRIVATE_DATA_ = 31 };
extern U NtUserCallHwndParam_raw(U, U, U);
__attribute__((dllexport)) U NtUserCallHwndParam(U hwnd, U param, U code) {
  if (!param) return NtUserCallHwndParam_raw(hwnd, param, code);
  const U32* in = (const U32*)param;
  switch (code) {
    case CHP_GET_WINDOW_RECT_: case CHP_GET_CLIENT_RECT_: case CHP_GET_PRESENT_RECT_: {      /* the block is one pointer: the RECT to fill */
      U64 rect = in[0];
      return NtUserCallHwndParam_raw(hwnd, (U)&rect, code);
    }
    case CHP_GET_SCROLL_INFO_: {      /* {bar, pad, SCROLLINFO*} */
      struct { U32 bar, pad; U64 info; } block = {in[0], 0, in[2]};
      return NtUserCallHwndParam_raw(hwnd, (U)&block, code);
    }
    case CHP_MAP_WINDOW_POINTS_: {      /* {HWND to, POINT*, count, dpi} */
      struct { U64 to, points; U32 count, dpi; } block = {in[0], in[1], in[2], in[3]};
      return NtUserCallHwndParam_raw(hwnd, (U)&block, code);
    }
    case CHP_GET_PRIVATE_DATA_: case CHP_SET_PRIVATE_DATA_: {      /* {offset, size, value} */
      struct { U32 offset, size; U64 value; } block = {in[0], in[1], in[2]};
      return NtUserCallHwndParam_raw(hwnd, (U)&block, code);
    }
    default: return NtUserCallHwndParam_raw(hwnd, param, code);
  }
}

/* ---- lists of windows: the kernel writes 64-bit handles ---------------------------------------------------------------------------------------------------------- */
#define WOW_HWND_LIST 2048
extern U NtUserBuildHwndList_raw(U, U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserBuildHwndList(U desktop, U parent, U children, U non_immersive, U thread, U count, U buffer, U needed) {
  static U64 wide[WOW_HWND_LIST];      /* (shared: guarded by the flag below, a second caller at the same moment uses its own small list) */
  static volatile int busy;
  U64 own[64];
  U64* list = own;
  U capacity = 64;
  int taken = 0;
  if (count > 64 && __atomic_exchange_n(&busy, 1, __ATOMIC_ACQUIRE) == 0) { list = wide; capacity = WOW_HWND_LIST; taken = 1; }
  if (count < capacity) capacity = count;
  const U status = NtUserBuildHwndList_raw(desktop, parent, children, non_immersive, thread, capacity, (U)list, needed);
  if ((int)status >= 0 && buffer) {
    const U written = needed ? *(const U32*)needed : capacity;
    for (U i = 0; i < written && i < capacity; ++i) ((U32*)buffer)[i] = (U32)list[i];
  }
  if (taken) __atomic_store_n(&busy, 0, __ATOMIC_RELEASE);
  return status;
}

/* ---- painting ------------------------------------------------------------------------------------------------------------------------------------------------------ */
typedef struct { U32 dc, erase; int rect[4]; U32 restore, update; U8 reserved[32]; } Paint32;
typedef struct { U64 dc; U32 erase; int rect[4]; U32 restore, update; U8 reserved[32]; U32 pad; } Paint64;
extern U NtUserBeginPaint_raw(U, U);
__attribute__((dllexport)) U NtUserBeginPaint(U hwnd, U paint) {
  Paint64 wide;
  U r;
  __builtin_memset(&wide, 0, sizeof(wide));
  WOW_RUN(r, NtUserBeginPaint_raw(hwnd, paint ? (U)&wide : 0));
  if (paint) {
    Paint32* out = (Paint32*)paint;
    out->dc = (U32)wide.dc; out->erase = wide.erase; __builtin_memcpy(out->rect, wide.rect, 16); out->restore = wide.restore; out->update = wide.update;
    __builtin_memcpy(out->reserved, wide.reserved, 32);
  }
  return r;
}
extern U NtUserEndPaint_raw(U, U);
__attribute__((dllexport)) U NtUserEndPaint(U hwnd, U paint) {
  Paint64 wide;
  __builtin_memset(&wide, 0, sizeof(wide));
  if (paint) { const Paint32* in = (const Paint32*)paint; wide.dc = in->dc; wide.erase = in->erase; __builtin_memcpy(wide.rect, in->rect, 16); }
  return NtUserEndPaint_raw(hwnd, paint ? (U)&wide : 0);
}

/* ---- the state of a thread's windows ------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { U32 size, flags, windows[6]; int caret[4]; } GuiThreadInfo32;
typedef struct { U32 size, flags; U64 windows[6]; int caret[4]; } GuiThreadInfo64;
extern U NtUserGetGUIThreadInfo_raw(U, U);
__attribute__((dllexport)) U NtUserGetGUIThreadInfo(U thread, U info) {
  GuiThreadInfo64 wide;
  GuiThreadInfo32* out = (GuiThreadInfo32*)info;
  if (!out || out->size != sizeof(*out)) return NtUserGetGUIThreadInfo_raw(thread, 0);      /* (the kernel sets the error) */
  __builtin_memset(&wide, 0, sizeof(wide));
  wide.size = sizeof(wide);
  const U ok = NtUserGetGUIThreadInfo_raw(thread, (U)&wide);
  if ((U32)ok) {
    out->flags = wide.flags;
    for (int i = 0; i < 6; ++i) out->windows[i] = (U32)wide.windows[i];
    __builtin_memcpy(out->caret, wide.caret, 16);
  }
  return ok;
}

/* ---- the rest of the window system: menus, icons and cursors, the clipboard, keyboard layouts, raw input, input, settings, displays, stations -------------------- */

/* MENUITEMINFO: 48 bytes here (44 without hbmpItem), 80 (72) in the kernel. */
typedef struct { U32 size, mask, type, state, id, submenu, checked, unchecked, item_data, type_data, cch, bitmap; } MenuItem32;
typedef struct { U32 size, mask, type, state, id, pad; U64 submenu, checked, unchecked, item_data, type_data; U32 cch, pad2; U64 bitmap; } MenuItem64;
enum { MENU_SET_ITEM_INFO_ = 0, MENU_INSERT_ITEM_ = 1, MENU_GET_INFO_A_ = 5, MENU_GET_INFO_W_ = 6 };
extern U NtUserThunkedMenuItemInfo_raw(U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserThunkedMenuItemInfo(U menu, U position, U flags, U method, U info, U text) {
  String64 text64;
  MenuItem64 wide;
  const int structured = method == MENU_SET_ITEM_INFO_ || method == MENU_INSERT_ITEM_ || method == MENU_GET_INFO_A_ || method == MENU_GET_INFO_W_;
  MenuItem32* in = (MenuItem32*)info;
  if (!structured || !info || (in->size != sizeof(MenuItem32) && in->size != sizeof(MenuItem32) - 4))
    return NtUserThunkedMenuItemInfo_raw(menu, position, flags, method, structured ? 0 : info, string_to_64(&text64, text));      /* (a size that is not a MENUITEMINFO's: the kernel's error) */
  const int with_bitmap = in->size == sizeof(MenuItem32);
  __builtin_memset(&wide, 0, sizeof(wide));
  wide.size = with_bitmap ? 80 : 72; wide.mask = in->mask; wide.type = in->type; wide.state = in->state; wide.id = in->id;
  wide.submenu = in->submenu; wide.checked = in->checked; wide.unchecked = in->unchecked; wide.item_data = in->item_data; wide.type_data = in->type_data; wide.cch = in->cch;
  if (with_bitmap) wide.bitmap = in->bitmap;
  const U result = NtUserThunkedMenuItemInfo_raw(menu, position, flags, method, (U)&wide, string_to_64(&text64, text));
  if ((U32)result && (method == MENU_GET_INFO_A_ || method == MENU_GET_INFO_W_)) {
    in->type = wide.type; in->state = wide.state; in->id = wide.id; in->submenu = (U32)wide.submenu; in->checked = (U32)wide.checked; in->unchecked = (U32)wide.unchecked;
    in->item_data = (U32)wide.item_data; in->type_data = (U32)wide.type_data; in->cch = wide.cch;
    if (with_bitmap) in->bitmap = (U32)wide.bitmap;
  }
  return result;
}
/* MENUINFO: 28 bytes here, 40 in the kernel. */
typedef struct { U32 size, mask, style, max_height, background, help_id, menu_data; } MenuInfo32;
typedef struct { U32 size, mask, style, max_height; U64 background; U32 help_id, pad; U64 menu_data; } MenuInfo64;
extern U NtUserThunkedMenuInfo_raw(U, U);
__attribute__((dllexport)) U NtUserThunkedMenuInfo(U menu, U info) {
  const MenuInfo32* in = (const MenuInfo32*)info;
  if (!in || in->size != sizeof(*in)) return NtUserThunkedMenuInfo_raw(menu, 0);
  MenuInfo64 wide = {sizeof(wide), in->mask, in->style, in->max_height, in->background, in->help_id, 0, in->menu_data};
  return NtUserThunkedMenuInfo_raw(menu, (U)&wide);
}
enum { CTP_GET_MENU_INFO_ = 1, CTP_SET_ICON_PARAM_ = 5 };
extern U NtUserCallTwoParam_raw(U, U, U);
__attribute__((dllexport)) U NtUserCallTwoParam(U a, U b, U code) {
  if (code == CTP_GET_MENU_INFO_ && b) {
    MenuInfo32* out = (MenuInfo32*)b;
    if (out->size != sizeof(*out)) return NtUserCallTwoParam_raw(a, 0, code);
    MenuInfo64 wide;
    __builtin_memset(&wide, 0, sizeof(wide));
    wide.size = sizeof(wide); wide.mask = out->mask;
    const U result = NtUserCallTwoParam_raw(a, (U)&wide, code);
    if ((U32)result) { out->style = wide.style; out->max_height = wide.max_height; out->background = (U32)wide.background; out->help_id = wide.help_id; out->menu_data = (U32)wide.menu_data; }
    return result;
  }
  if (code == CTP_SET_ICON_PARAM_ && b) {      /* {callback, parameter} */
    const U64 block[2] = {((const U32*)b)[0], ((const U32*)b)[1]};
    return NtUserCallTwoParam_raw(a, (U)block, code);
  }
  return NtUserCallTwoParam_raw(a, b, code);
}

/* Icons and cursors. */
typedef struct { U32 flags, steps, frames, delay, frames_at, sequence_at, rates_at, resource; } IconData32;
typedef struct { U32 flags, steps, frames, delay; U64 frames_at, sequence_at, rates_at, resource; } IconData64;
typedef struct { U32 width, height, color, alpha, mask; int hot_x, hot_y; } IconFrame32;
typedef struct { U32 width, height; U64 color, alpha, mask; int hot_x, hot_y; } IconFrame64;
#define WOW_ICON_FRAMES 64
extern U NtUserSetCursorIconData_raw(U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserSetCursorIconData(U handle, U module, U name, U data, U a, U b) {
  String64 module64, name64;
  IconData64 wide;
  IconFrame64 frames[WOW_ICON_FRAMES];
  const IconData32* in = (const IconData32*)data;
  if (!in) return NtUserSetCursorIconData_raw(handle, string_to_64(&module64, module), string_to_64(&name64, name), 0, a, b);
  if (in->frames > WOW_ICON_FRAMES) return 0;      /* (an animated cursor of more frames than this converts) */
  const IconFrame32* from = (const IconFrame32*)in->frames_at;
  for (U32 i = 0; from && i < in->frames; ++i) {
    frames[i].width = from[i].width; frames[i].height = from[i].height; frames[i].color = from[i].color; frames[i].alpha = from[i].alpha; frames[i].mask = from[i].mask;
    frames[i].hot_x = from[i].hot_x; frames[i].hot_y = from[i].hot_y;
  }
  wide.flags = in->flags; wide.steps = in->steps; wide.frames = in->frames; wide.delay = in->delay;
  wide.frames_at = from ? (U64)(U)frames : 0; wide.sequence_at = in->sequence_at; wide.rates_at = in->rates_at; wide.resource = in->resource;
  return NtUserSetCursorIconData_raw(handle, string_to_64(&module64, module), string_to_64(&name64, name), (U)&wide, a, b);
}
extern U NtUserFindExistingCursorIcon_raw(U, U, U);
__attribute__((dllexport)) U NtUserFindExistingCursorIcon(U module, U name, U resource) {
  String64 module64, name64;
  return NtUserFindExistingCursorIcon_raw(string_to_64(&module64, module), string_to_64(&name64, name), resource);
}
typedef struct { U32 is_icon, hot_x, hot_y, mask, color; } IconInfo32;
typedef struct { U32 is_icon, hot_x, hot_y, pad; U64 mask, color; } IconInfo64;
/* (a name the kernel writes: its length, and the resource's number in place of the text when it has no name) */
static void string_back_to_32(U out, const String64* wide) {
  if (!out) return;
  String32* s = (String32*)out;
  s->length = wide->length;
  if (wide->length == 0) s->buffer = (U32)wide->buffer;
}
extern U NtUserGetIconInfo_raw(U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserGetIconInfo(U handle, U info, U module, U name, U bpp, U a) {
  String64 module64, name64;
  IconInfo64 wide;
  __builtin_memset(&wide, 0, sizeof(wide));
  const U result = NtUserGetIconInfo_raw(handle, info ? (U)&wide : 0, string_to_64(&module64, module), string_to_64(&name64, name), bpp, a);
  if ((U32)result) {
    if (info) { IconInfo32* out = (IconInfo32*)info; out->is_icon = wide.is_icon; out->hot_x = wide.hot_x; out->hot_y = wide.hot_y; out->mask = (U32)wide.mask; out->color = (U32)wide.color; }
    if (module) string_back_to_32(module, &module64);
    if (name) string_back_to_32(name, &name64);
  }
  return result;
}
typedef struct { U32 size, flags, cursor; int x, y; } CursorInfo32;
typedef struct { U32 size, flags; U64 cursor; int x, y; } CursorInfo64;
extern U NtUserGetCursorInfo_raw(U);
__attribute__((dllexport)) U NtUserGetCursorInfo(U info) {
  CursorInfo32* out = (CursorInfo32*)info;
  if (!out || out->size != sizeof(*out)) return NtUserGetCursorInfo_raw(0);
  CursorInfo64 wide = {sizeof(wide), 0, 0, 0, 0};
  const U result = NtUserGetCursorInfo_raw((U)&wide);
  if ((U32)result) { out->flags = wide.flags; out->cursor = (U32)wide.cursor; out->x = wide.x; out->y = wide.y; }
  return result;
}

/* The clipboard: user32's own blocks, {data, size, cache only, sequence} and {data, capacity, size, sequence}. */
extern U NtUserSetClipboardData_raw(U, U, U);
__attribute__((dllexport)) U NtUserSetClipboardData(U format, U handle, U params) {
  if (!params) return NtUserSetClipboardData_raw(format, handle, 0);
  U32* in = (U32*)params;
  struct { U64 data, size; U8 cache_only, pad[3]; U32 sequence; } wide = {in[0], in[1], (U8)in[2], {0, 0, 0}, in[3]};
  const U result = NtUserSetClipboardData_raw(format, handle, (U)&wide);
  in[3] = wide.sequence;
  return result;
}
extern U NtUserGetClipboardData_raw(U, U);
__attribute__((dllexport)) U NtUserGetClipboardData(U format, U params) {
  if (!params) return NtUserGetClipboardData_raw(format, 0);
  U32* in = (U32*)params;
  struct { U64 data, capacity, size; U32 sequence, pad; } wide = {in[0], in[1], in[2], in[3], 0};
  const U result = NtUserGetClipboardData_raw(format, (U)&wide);
  in[2] = (U32)wide.size; in[3] = wide.sequence;
  return result;
}

/* Lists of handles the kernel writes 8 bytes each: keyboard layouts, input contexts. */
#define WOW_HANDLE_LIST 256
extern U NtUserGetKeyboardLayoutList_raw(U, U);
__attribute__((dllexport)) U NtUserGetKeyboardLayoutList(U size, U list) {
  U64 wide[WOW_HANDLE_LIST];
  if (!list || !size) return NtUserGetKeyboardLayoutList_raw(size, 0);
  const U capacity = size < WOW_HANDLE_LIST ? size : WOW_HANDLE_LIST;
  const U count = NtUserGetKeyboardLayoutList_raw(capacity, (U)wide);
  for (U i = 0; i < count && i < capacity; ++i) ((U32*)list)[i] = (U32)wide[i];
  return count;
}
extern U NtUserBuildHimcList_raw(U, U, U, U);
__attribute__((dllexport)) U NtUserBuildHimcList(U thread, U count, U buffer, U size) {
  U64 wide[WOW_HANDLE_LIST];
  const U capacity = count < WOW_HANDLE_LIST ? count : WOW_HANDLE_LIST;
  const U status = NtUserBuildHimcList_raw(thread, capacity, buffer ? (U)wide : 0, size);
  if ((int)status >= 0 && buffer) {
    const U written = size ? *(const U32*)size : capacity;
    for (U i = 0; i < written && i < capacity; ++i) ((U32*)buffer)[i] = (U32)wide[i];
  }
  return status;
}
extern U NtUserLoadKeyboardLayoutEx_raw(U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserLoadKeyboardLayoutEx(U file, U offset, U old, U name, U id, U flags) {
  String64 name64;
  return NtUserLoadKeyboardLayoutEx_raw(file, offset, old, string_to_64(&name64, name), id, flags);
}
extern U NtUserGetImeHotKey_raw(U, U, U, U);
__attribute__((dllexport)) U NtUserGetImeHotKey(U id, U modifiers, U key, U layout) {
  U64 wide = 0;
  const U result = NtUserGetImeHotKey_raw(id, modifiers, key, layout ? (U)&wide : 0);
  if ((U32)result && layout) *(U32*)layout = (U32)wide;
  return result;
}

/* Raw input: RAWINPUTDEVICE is 12 bytes here and 16 in the kernel, RAWINPUTDEVICELIST 8 and 16, RAWINPUTHEADER 16 and 24. */
typedef struct { U16 page, usage; U32 flags, target; } RawDevice32;
typedef struct { U16 page, usage; U32 flags; U64 target; } RawDevice64;
#define WOW_RAW_DEVICES 64
extern U NtUserRegisterRawInputDevices_raw(U, U, U);
__attribute__((dllexport)) U NtUserRegisterRawInputDevices(U devices, U count, U size) {
  RawDevice64 wide[WOW_RAW_DEVICES];
  if (size != sizeof(RawDevice32) || !devices || count > WOW_RAW_DEVICES) return NtUserRegisterRawInputDevices_raw(0, count, 0);      /* (the kernel's error for a wrong size) */
  const RawDevice32* in = (const RawDevice32*)devices;
  for (U i = 0; i < count; ++i) { wide[i].page = in[i].page; wide[i].usage = in[i].usage; wide[i].flags = in[i].flags; wide[i].target = in[i].target; }
  return NtUserRegisterRawInputDevices_raw((U)wide, count, sizeof(RawDevice64));
}
extern U NtUserGetRegisteredRawInputDevices_raw(U, U, U);
__attribute__((dllexport)) U NtUserGetRegisteredRawInputDevices(U devices, U count, U size) {
  RawDevice64 wide[WOW_RAW_DEVICES];
  if (size != sizeof(RawDevice32) || !count) return NtUserGetRegisteredRawInputDevices_raw(0, count, 0);
  U32 room = *(U32*)count;
  if (room > WOW_RAW_DEVICES) room = WOW_RAW_DEVICES;
  U32 wide_count = devices ? room : 0;
  const U result = NtUserGetRegisteredRawInputDevices_raw(devices ? (U)wide : 0, (U)&wide_count, sizeof(RawDevice64));
  *(U32*)count = wide_count;
  if ((int)result > 0 && devices) {
    RawDevice32* out = (RawDevice32*)devices;
    for (U i = 0; i < result && i < room; ++i) { out[i].page = wide[i].page; out[i].usage = wide[i].usage; out[i].flags = wide[i].flags; out[i].target = (U32)wide[i].target; }
  }
  return result;
}
typedef struct { U32 device, type; } RawDeviceEntry32;
typedef struct { U64 device; U32 type, pad; } RawDeviceEntry64;
extern U NtUserGetRawInputDeviceList_raw(U, U, U);
__attribute__((dllexport)) U NtUserGetRawInputDeviceList(U list, U count, U size) {
  RawDeviceEntry64 wide[WOW_RAW_DEVICES];
  if (size != sizeof(RawDeviceEntry32) || !count) return NtUserGetRawInputDeviceList_raw(0, count, 0);
  U32 room = *(U32*)count;
  if (room > WOW_RAW_DEVICES) room = WOW_RAW_DEVICES;
  U32 wide_count = list ? room : 0;
  const U result = NtUserGetRawInputDeviceList_raw(list ? (U)wide : 0, (U)&wide_count, sizeof(RawDeviceEntry64));
  *(U32*)count = wide_count;
  if ((int)result > 0 && list) {
    RawDeviceEntry32* out = (RawDeviceEntry32*)list;
    for (U i = 0; i < result && i < room; ++i) { out[i].device = (U32)wide[i].device; out[i].type = wide[i].type; }
  }
  return result;
}
#define RID_HEADER_ 0x10000005u
#define WOW_RAW_BYTES 1024
extern U NtUserGetRawInputData_raw(U, U, U, U, U);
__attribute__((dllexport)) U NtUserGetRawInputData(U input, U command, U data, U size, U header_size) {
  U8 wide[WOW_RAW_BYTES + 8];
  if (header_size != 16 || !size) return NtUserGetRawInputData_raw(input, command, 0, size, 0);
  /* (the header is 8 bytes longer in the kernel: so is every size it is asked with and answers) */
  U32 wide_size = data ? *(U32*)size + 8 : 0;
  if (wide_size > sizeof(wide)) wide_size = sizeof(wide);
  const U result = NtUserGetRawInputData_raw(input, command, data ? (U)wide : 0, (U)&wide_size, 24);
  *(U32*)size = wide_size >= 8 ? wide_size - 8 : wide_size;
  if ((int)result <= 0 || !data) return result;      /* (the size asked for, or an error) */
  U8* out = (U8*)data;
  const U32 bytes = (U32)result;
  put32(out, get32(wide)); put32(out + 4, get32(wide + 4) >= 8 ? get32(wide + 4) - 8 : 0); put32(out + 8, (U32)get64(wide + 8)); put32(out + 12, (U32)get64(wide + 16));
  if (command != RID_HEADER_ && bytes > 24) __builtin_memcpy(out + 16, wide + 24, bytes - 24);
  return bytes >= 24 ? bytes - 8 : bytes;
}

/* SendInput: an INPUT is 28 bytes here (the union starts at 4) and 40 in the kernel (it starts at 8). TrackMouseEvent: 16 and 24. */
typedef struct { U32 type; union { struct { int dx, dy; U32 data, flags, time, extra; } mouse; struct { U16 key, scan; U32 flags, time, extra; } keyboard; U8 raw[24]; } u; } Input32;
typedef struct { U32 type, pad; union { struct { int dx, dy; U32 data, flags, time, pad; U64 extra; } mouse; struct { U16 key, scan; U32 flags, time, pad; U64 extra; } keyboard; U8 raw[32]; } u; } Input64;
#define WOW_INPUTS 128
extern U NtUserSendInput_raw(U, U, U);
__attribute__((dllexport)) U NtUserSendInput(U count, U inputs, U size) {
  Input64 wide[WOW_INPUTS];
  if (size != sizeof(Input32) || !inputs || count > WOW_INPUTS) return NtUserSendInput_raw(inputs ? count : 0, 0, 0);
  const Input32* in = (const Input32*)inputs;
  __builtin_memset(wide, 0, sizeof(Input64) * count);
  for (U i = 0; i < count; ++i) {
    wide[i].type = in[i].type;
    if (in[i].type == 0) {
      wide[i].u.mouse.dx = in[i].u.mouse.dx; wide[i].u.mouse.dy = in[i].u.mouse.dy; wide[i].u.mouse.data = in[i].u.mouse.data; wide[i].u.mouse.flags = in[i].u.mouse.flags;
      wide[i].u.mouse.time = in[i].u.mouse.time; wide[i].u.mouse.extra = in[i].u.mouse.extra;
    } else if (in[i].type == 1) {
      wide[i].u.keyboard.key = in[i].u.keyboard.key; wide[i].u.keyboard.scan = in[i].u.keyboard.scan; wide[i].u.keyboard.flags = in[i].u.keyboard.flags;
      wide[i].u.keyboard.time = in[i].u.keyboard.time; wide[i].u.keyboard.extra = in[i].u.keyboard.extra;
    } else {
      __builtin_memcpy(wide[i].u.raw, in[i].u.raw, 8);      /* (a hardware message: three numbers without a pointer) */
    }
  }
  return NtUserSendInput_raw(count, (U)wide, sizeof(Input64));
}
typedef struct { U32 size, flags, window, hover; } TrackMouse32;
typedef struct { U32 size, flags; U64 window; U32 hover, pad; } TrackMouse64;
extern U NtUserTrackMouseEvent_raw(U);
__attribute__((dllexport)) U NtUserTrackMouseEvent(U info) {
  TrackMouse32* in = (TrackMouse32*)info;
  if (!in || in->size != sizeof(*in)) return NtUserTrackMouseEvent_raw(0);
  TrackMouse64 wide = {sizeof(wide), in->flags, in->window, in->hover, 0};
  const U result = NtUserTrackMouseEvent_raw((U)&wide);
  if ((U32)result) { in->flags = wide.flags; in->window = (U32)wide.window; in->hover = wide.hover; }
  return result;
}

/* SystemParametersInfo: the default input language is 8 bytes in the kernel, and HIGHCONTRAST {size, flags, scheme} is 12 bytes here and 16 there. */
#define SPI_GETHIGHCONTRAST_ 0x42u
#define SPI_SETHIGHCONTRAST_ 0x43u
#define SPI_GETDEFAULTINPUTLANG_ 0x59u
#define SPI_SETDEFAULTINPUTLANG_ 0x5Au
static U system_parameters(U (*raw)(U, U, U, U, U), U action, U param, U data, U flags, U dpi) {
  if (!data) return raw(action, param, data, flags, dpi);
  if (action == SPI_GETDEFAULTINPUTLANG_) {
    U64 wide = 0;
    const U result = raw(action, param, (U)&wide, flags, dpi);
    if ((U32)result) *(U32*)data = (U32)wide;
    return result;
  }
  if (action == SPI_SETDEFAULTINPUTLANG_) {
    U64 wide = *(const U32*)data;
    return raw(action, param, (U)&wide, flags, dpi);
  }
  if (action == SPI_GETHIGHCONTRAST_ || action == SPI_SETHIGHCONTRAST_) {
    U32* in = (U32*)data;
    struct { U32 size, flags; U64 scheme; } wide = {16, in[1], in[2]};
    if (in[0] != 12) return raw(action, param, 0, flags, dpi);
    const U result = raw(action, param ? 16 : 0, (U)&wide, flags, dpi);
    if ((U32)result && action == SPI_GETHIGHCONTRAST_) { in[1] = wide.flags; in[2] = (U32)wide.scheme; }
    return result;
  }
  return raw(action, param, data, flags, dpi);
}
extern U NtUserSystemParametersInfo_raw(U, U, U, U);
extern U NtUserSystemParametersInfoForDpi_raw(U, U, U, U, U);
static U spi_plain(U action, U param, U data, U flags, U dpi) { (void)dpi; return NtUserSystemParametersInfo_raw(action, param, data, flags); }
__attribute__((dllexport)) U NtUserSystemParametersInfo(U action, U param, U data, U flags) { return system_parameters(spi_plain, action, param, data, flags, 0); }
__attribute__((dllexport)) U NtUserSystemParametersInfoForDpi(U action, U param, U data, U flags, U dpi) {
  return system_parameters(NtUserSystemParametersInfoForDpi_raw, action, param, data, flags, dpi);
}

/* Displays: the device's name. */
extern U NtUserEnumDisplaySettings_raw(U, U, U, U);
__attribute__((dllexport)) U NtUserEnumDisplaySettings(U device, U mode, U devmode, U flags) {
  String64 device64;
  return NtUserEnumDisplaySettings_raw(string_to_64(&device64, device), mode, devmode, flags);
}
extern U NtUserEnumDisplayDevices_raw(U, U, U, U);
__attribute__((dllexport)) U NtUserEnumDisplayDevices(U device, U index, U info, U flags) {
  String64 device64;
  return NtUserEnumDisplayDevices_raw(string_to_64(&device64, device), index, info, flags);
}
extern U NtUserChangeDisplaySettings_raw(U, U, U, U, U);
__attribute__((dllexport)) U NtUserChangeDisplaySettings(U device, U devmode, U hwnd, U flags, U lparam) {
  String64 device64;
  return NtUserChangeDisplaySettings_raw(string_to_64(&device64, device), devmode, hwnd, flags, lparam);
}
extern U NtUserDefSetText_raw(U, U);
__attribute__((dllexport)) U NtUserDefSetText(U hwnd, U text) {
  String64 text64;
  return NtUserDefSetText_raw(hwnd, string_to_64(&text64, text));
}

/* Window stations and desktops: OBJECT_ATTRIBUTES, 24 bytes here and 48 in the kernel, with the name it points to. */
typedef struct { U32 length, root, name, attributes, descriptor, quality; } Attributes32;
typedef struct { U32 length, pad; U64 root, name; U32 attributes, pad2; U64 descriptor, quality; String64 text; } Attributes64;
static U attributes_to_64(Attributes64* out, U in) {
  if (!in) return 0;
  const Attributes32* a = (const Attributes32*)in;
  out->length = 48; out->pad = 0; out->root = (U64)(long long)(int)a->root; out->name = string_to_64(&out->text, a->name);
  out->attributes = a->attributes; out->pad2 = 0; out->descriptor = a->descriptor; out->quality = a->quality;
  return (U)out;
}
extern U NtUserCreateWindowStation_raw(U, U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserCreateWindowStation(U attributes, U access, U a3, U a4, U a5, U a6, U a7) {
  Attributes64 wide;
  return NtUserCreateWindowStation_raw(attributes_to_64(&wide, attributes), access, a3, a4, a5, a6, a7);
}
extern U NtUserOpenWindowStation_raw(U, U);
__attribute__((dllexport)) U NtUserOpenWindowStation(U attributes, U access) {
  Attributes64 wide;
  return NtUserOpenWindowStation_raw(attributes_to_64(&wide, attributes), access);
}
extern U NtUserCreateDesktopEx_raw(U, U, U, U, U, U);
__attribute__((dllexport)) U NtUserCreateDesktopEx(U attributes, U device, U devmode, U flags, U access, U heap) {
  Attributes64 wide;
  String64 device64;
  return NtUserCreateDesktopEx_raw(attributes_to_64(&wide, attributes), string_to_64(&device64, device), devmode, flags, access, heap);
}
extern U NtUserOpenDesktop_raw(U, U, U);
__attribute__((dllexport)) U NtUserOpenDesktop(U attributes, U flags, U access) {
  Attributes64 wide;
  return NtUserOpenDesktop_raw(attributes_to_64(&wide, attributes), flags, access);
}

/* Hooks. The first hook of a chain is asked for with a handle that has bit 32 set in a 64-bit program; user32.dll for i386 sets bit 30 instead
 * (WOW_HOOK_CHAIN_START), which becomes the kernel's value here. The structures a hook procedure gets are user32's own and are not the kernel's. */
#define WOW_HOOK_CHAIN_START 0x40000000u
__attribute__((dllexport)) U NtUserCallNextHookEx(U hook, U code, U wparam, U lparam) {
  const U64 arguments[4] = {(hook & WOW_HOOK_CHAIN_START) ? ((U64)1 << 32) | (hook & ~WOW_HOOK_CHAIN_START) : (U64)(long long)(int)hook, (U64)(long long)(int)code,
                            (U64)(long long)(int)wparam, (U64)(long long)(int)lparam};
  U r;
  WOW_RUN(r, wow_service64(NT_SERVICE_NtUserCallNextHookEx, arguments));
  return r;
}

/* The 64-bit layouts are what the kernel reads: their sizes are fixed. */
_Static_assert(sizeof(String64) == 16 && sizeof(WndClass64) == 80 && sizeof(Msg64) == 48 && sizeof(Paint64) == 72 && sizeof(GuiThreadInfo64) == 72, "the kernel's layouts");
_Static_assert(sizeof(String32) == 8 && sizeof(WndClass32) == 48 && sizeof(Msg32) == 28 && sizeof(Paint32) == 64 && sizeof(GuiThreadInfo32) == 48, "a 32-bit program's layouts");
_Static_assert(sizeof(MenuItem64) == 80 && sizeof(MenuItem32) == 48 && sizeof(MenuInfo64) == 40 && sizeof(MenuInfo32) == 28 && sizeof(IconData64) == 48 && sizeof(IconFrame64) == 40 &&
               sizeof(IconFrame32) == 28 && sizeof(IconInfo64) == 32 && sizeof(IconInfo32) == 20 && sizeof(CursorInfo64) == 24 && sizeof(CursorInfo32) == 20 && sizeof(RawDevice64) == 16 &&
               sizeof(RawDevice32) == 12 && sizeof(Input64) == 40 && sizeof(Input32) == 28 && sizeof(TrackMouse64) == 24 && sizeof(TrackMouse32) == 16 &&
               __builtin_offsetof(Attributes64, text) == 48 && sizeof(Attributes32) == 24, "the layouts of the rest of the window system");
_Static_assert(sizeof(CreateStruct32) == 48 && sizeof(WindowPos32) == 28 && sizeof(NcCalcSize32) == 52 + 28 && sizeof(CopyData32) == 12, "a 32-bit program's message structures");
