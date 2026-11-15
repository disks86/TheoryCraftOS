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
static void msg_to_64(Msg64* out, const Msg32* in) {
  out->hwnd = in->hwnd; out->message = in->message; out->pad = 0; out->wparam = in->wparam; out->lparam = in->lparam; out->time = in->time; out->x = in->x; out->y = in->y;
  out->private_ = 0;
}
static void msg_to_32(Msg32* out, const Msg64* in) {
  out->hwnd = (U32)in->hwnd; out->message = in->message; out->wparam = (U32)in->wparam; out->lparam = (int)in->lparam; out->time = in->time; out->x = in->x; out->y = in->y;
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
  WOW_RUN(r, NtUserMessageCall_raw(hwnd, message, wparam, lparam64, timed ? (U)&timeout64 : info, type, ansi));
  if (lparam64 != lparam) message_back_to_32((U32)message, wparam, lparam, &wide);
  if (timed) ((U32*)info)[2] = (U32)timeout64.result;
  return r;
}

/* ---- NtUserCallHwndParam: the calls whose parameter is a block with a pointer in it ---------------------------------------------------------------------------- */
enum { CHP_GET_SCROLL_INFO_ = 7, CHP_GET_WINDOW_RECT_ = 13, CHP_GET_CLIENT_RECT_ = 14, CHP_MAP_WINDOW_POINTS_ = 20, CHP_GET_PRIVATE_DATA_ = 30, CHP_SET_PRIVATE_DATA_ = 31 };
extern U NtUserCallHwndParam_raw(U, U, U);
__attribute__((dllexport)) U NtUserCallHwndParam(U hwnd, U param, U code) {
  if (!param) return NtUserCallHwndParam_raw(hwnd, param, code);
  const U32* in = (const U32*)param;
  switch (code) {
    case CHP_GET_WINDOW_RECT_: case CHP_GET_CLIENT_RECT_: {      /* the block is one pointer: the RECT to fill */
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

/* The 64-bit layouts are what the kernel reads: their sizes are fixed. */
_Static_assert(sizeof(String64) == 16 && sizeof(WndClass64) == 80 && sizeof(Msg64) == 48 && sizeof(Paint64) == 72 && sizeof(GuiThreadInfo64) == 72, "the kernel's layouts");
_Static_assert(sizeof(String32) == 8 && sizeof(WndClass32) == 48 && sizeof(Msg32) == 28 && sizeof(Paint32) == 64 && sizeof(GuiThreadInfo32) == 48, "a 32-bit program's layouts");
_Static_assert(sizeof(CreateStruct32) == 48 && sizeof(WindowPos32) == 28 && sizeof(NcCalcSize32) == 52 + 28 && sizeof(CopyData32) == 12, "a 32-bit program's message structures");
