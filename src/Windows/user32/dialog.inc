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

/* Dialogs of user32.dll (included by window.inc after the controls): templates (DLGTEMPLATE and DLGTEMPLATEEX), CreateDialog* and DialogBox* (the modal loop), EndDialog, the "#32770"
 * class and DefDlgProc, IsDialogMessage (Tab, Enter, Escape, arrows), the dialog item functions (GetDlgItem, SetDlgItemText, CheckDlgButton, ...) and the tab and group order. The state
 * of a dialog the program cannot reach is a block whose pointer win32k keeps for the window (NtUserCallHwndParam SetDialogInfo / GetDialogInfo); the dialog procedure, the message result
 * and the user data are window extra bytes (DWLP_MSGRESULT 0, DWLP_DLGPROC 8, DWLP_USER 16) of the class. Not done: DlgDirList and friends, menus in templates, fonts other than a size ratio
 * of the system font for dialog units, MS-DOS style accelerator mnemonics (WM_SYSCHAR), DS_* positioning beyond centering. */
#define DWLP_MSGRESULT_ 0
#define DWLP_DLGPROC_ 8
#define DWLP_USER_ 16
#define DS_ABSALIGN_ 0x1
#define DS_SETFONT_ 0x40
#define DS_MODALFRAME_ 0x80
#define DS_CENTER_ 0x800
#define DS_CONTROL_ 0x400
#define DS_SETFOREGROUND_ 0x200
#define DM_GETDEFID_ 0x400
#define DM_SETDEFID_ 0x401
#define DM_REPOSITION_ 0x402
#define DC_HASDEFID_ 0x534B
#define IDOK_ 1
#define IDCANCEL_ 2
#define WM_INITDIALOG_ 0x110
#define RT_DIALOG_ 5
#define ERROR_CONTROL_ID_NOT_FOUND_ 1421

typedef struct { HANDLE focus; UINT default_id; int xbase, ybase; HANDLE font; BOOL ended; LONG_PTR result; } DialogInfo;
typedef struct { LONG_PTR (WINAPI *proc)(HANDLE, UINT, ULONG_PTR, LONG_PTR); LONG_PTR param; int point_size; BOOL ansi; } DialogCreate;

static DialogInfo *dialog_info(HANDLE hwnd) { return (DialogInfo *)NtUserCallHwnd((ULONG_PTR)hwnd, 1 /* GetDialogInfo */); }
static void dialog_set_info(HANDLE hwnd, DialogInfo *info) { NtUserCallHwndParam((ULONG_PTR)hwnd, (ULONG_PTR)info, 24 /* SetDialogInfo */); }
EXPORT DWORD WINAPI GetDialogBaseUnits(void) { return (DWORD)NtUserCallNoParam(1); }

/* ---- the template ---------------------------------------------------------------------------------------------------------------------------------------------------- */
typedef struct { const WORD *p; const WORD *end; } TemplateReader;
/* A string or an ordinal in a template: answers the pointer after it; `ordinal` is set (and `text` NULL) for the 0xFFFF form. */
static const WORD *dlg_sz_or_ord(const WORD *p, const WCHAR **text, WORD *ordinal) {
  *text = NULL; *ordinal = 0;
  if (*p == 0xFFFF) { *ordinal = p[1]; return p + 2; }
  if (*p == 0) return p + 1;
  *text = (const WCHAR *)p;
  while (*p) ++p;
  return p + 1;
}
static const WORD *dlg_skip_string(const WORD *p) { while (*p) ++p; return p + 1; }

/* Dialog units to pixels: a dialog unit is 1/4 of the base width and 1/8 of the base height of the font. */
static int dlg_x(int v, int xbase) { return v * xbase / 4; }
static int dlg_y(int v, int ybase) { return v * ybase / 8; }
EXPORT BOOL WINAPI MapDialogRect(HANDLE hwnd, RECT_ *r) {
  DialogInfo *info = dialog_info(hwnd);
  if (!info || !r) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; }
  r->left = dlg_x(r->left, info->xbase); r->right = dlg_x(r->right, info->xbase);
  r->top = dlg_y(r->top, info->ybase); r->bottom = dlg_y(r->bottom, info->ybase);
  return TRUE;
}

static const WCHAR *const dlg_classes[] = {L"Button", L"Edit", L"Static", L"ListBox", L"ScrollBar", L"ComboBox"};
EXPORT HANDLE WINAPI GetNextDlgTabItem(HANDLE dialog, HANDLE control, BOOL previous);
EXPORT BOOL WINAPI AdjustWindowRectEx(RECT_ *rect, DWORD style, BOOL menu, DWORD ex_style);
#define WS_VISIBLE_ 0x10000000u

static HANDLE dialog_create(HANDLE instance, const void *template_data, HANDLE owner, LONG_PTR (WINAPI *proc)(HANDLE, UINT, ULONG_PTR, LONG_PTR), LONG_PTR param, BOOL ansi) {
  const WORD *p = template_data;
  DWORD style, ex_style;
  UINT count;
  int x, y, cx, cy, point_size = 0;
  const BOOL extended = p[0] == 1 && p[1] == 0xFFFF;
  const WCHAR *menu, *cls, *title, *face = NULL;
  WORD menu_ord, cls_ord, title_ord;
  if (!template_data) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  if (extended) { ex_style = *(const DWORD *)(p + 4); style = *(const DWORD *)(p + 6); p += 8; }
  else { style = *(const DWORD *)p; ex_style = *(const DWORD *)(p + 2); p += 4; }
  count = *p++;
  x = (short)p[0]; y = (short)p[1]; cx = (short)p[2]; cy = (short)p[3]; p += 4;
  p = dlg_sz_or_ord(p, &menu, &menu_ord);
  p = dlg_sz_or_ord(p, &cls, &cls_ord);
  p = dlg_sz_or_ord(p, &title, &title_ord);
  if (style & DS_SETFONT_) {
    point_size = *p++;
    if (extended) p += 3;      /* weight, italic, charset */
    face = (const WCHAR *)p;
    p = dlg_skip_string(p);
  }
  (void)face;
  /* the base units: the system font's, scaled for a dialog font of another size (8 points is the reference) */
  const DWORD base = GetDialogBaseUnits();
  int xbase = (int)(base & 0xFFFF), ybase = (int)(base >> 16);
  if (point_size > 0) { xbase = 6 * point_size / 8; ybase = 13 * point_size / 8; if (xbase < 1) xbase = 1; if (ybase < 1) ybase = 1; }

  RECT_ rect = {dlg_x(x, xbase), dlg_y(y, ybase), dlg_x(x + cx, xbase), dlg_y(y + cy, ybase)};
  DWORD window_style = (style & ~(DWORD)WS_VISIBLE_) | 0x4 /* DS_3DLOOK: every dialog has it */;
  if (style & 0x400 /* DS_CONTROL */) { window_style &= ~(DWORD)(0x00C00000 | 0x00080000); ex_style |= 0x10000 /* WS_EX_CONTROLPARENT */; }      /* (a control-like dialog has no caption or system menu) */
  else if (!(style & WS_CHILD_)) ex_style |= 0x10000;
  /* the size is the client size: the window is as much bigger as the frame needs */
  RECT_ frame = {0, 0, rect.right - rect.left, rect.bottom - rect.top};
  AdjustWindowRectEx(&frame, window_style, FALSE, ex_style);
  int wx = rect.left, wy = rect.top;
  if (style & DS_CENTER_) {
    wx = (GetSystemMetrics(0) - (frame.right - frame.left)) / 2;
    wy = (GetSystemMetrics(1) - (frame.bottom - frame.top)) / 2;
  } else if (!(style & WS_CHILD_) && owner && !(style & DS_ABSALIGN_)) {
    RECT_ o;
    if (GetWindowRect(owner, &o)) { wx += o.left; wy += o.top; }
  }
  if (style & DS_MODALFRAME_) ex_style |= 0x1 /* WS_EX_DLGMODALFRAME */;
  DialogCreate create = {proc, param, point_size, ansi};
  const WCHAR *class_name = cls ? cls : (cls_ord ? NULL : L"#32770");
  HANDLE hwnd = CreateWindowExW(ex_style, class_name ? class_name : L"#32770", title ? title : L"", window_style, wx, wy, frame.right - frame.left, frame.bottom - frame.top, owner, NULL, instance, &create);
  if (!hwnd) return NULL;
  DialogInfo *info = dialog_info(hwnd);
  if (info) { info->xbase = xbase; info->ybase = ybase; }
  /* the items */
  for (UINT i = 0; i < count; ++i) {
    DWORD istyle, iexstyle;
    int ix, iy, icx, icy;
    LONG_PTR id;
    const WCHAR *icls, *ititle;
    WORD icls_ord, ititle_ord;
    p = (const WORD *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    if (extended) { iexstyle = *(const DWORD *)(p + 2); istyle = *(const DWORD *)(p + 4); p += 6; ix = (short)p[0]; iy = (short)p[1]; icx = (short)p[2]; icy = (short)p[3]; id = *(const DWORD *)(p + 4); p += 6; }
    else { istyle = *(const DWORD *)p; iexstyle = *(const DWORD *)(p + 2); p += 4; ix = (short)p[0]; iy = (short)p[1]; icx = (short)p[2]; icy = (short)p[3]; id = (short)p[4]; p += 5; }
    p = dlg_sz_or_ord(p, &icls, &icls_ord);
    p = dlg_sz_or_ord(p, &ititle, &ititle_ord);
    const WORD extra = *p++;
    const void *extra_data = extra ? p : NULL;
    p = (const WORD *)((const char *)p + extra);
    if (!icls) { if (icls_ord >= 0x80 && icls_ord <= 0x85) icls = dlg_classes[icls_ord - 0x80]; else continue; }
    const WCHAR *item_title = ititle ? ititle : (ititle_ord ? (const WCHAR *)(ULONG_PTR)ititle_ord : L"");
    HANDLE item = CreateWindowExW(iexstyle, icls, item_title, istyle & ~(DWORD)0, dlg_x(ix, xbase), dlg_y(iy, ybase), dlg_x(icx, xbase), dlg_y(icy, ybase), hwnd, (HANDLE)id, instance, (void *)extra_data);
    if (!item) { if (!(istyle & 0x0)) { /* (the dialog goes on without an item that cannot be made, as Windows does for unknown classes only when it is told to fail; here it fails) */ DestroyWindow(hwnd); return NULL; } }
  }
  /* WM_INITDIALOG: the first tab stop takes the focus unless the procedure answers FALSE */
  HANDLE first = GetNextDlgTabItem(hwnd, NULL, FALSE);
  const LONG_PTR handled = SendMessageW(hwnd, WM_INITDIALOG_, (ULONG_PTR)first, param);
  if (handled && first && IsWindow(first) && !(GetWindowLongW(first, -16) & WS_DISABLED_)) SetFocus(first);
  if (style & WS_VISIBLE_) ShowWindow(hwnd, 5 /* SW_SHOW */);
  return hwnd;
}

/* ---- the dialog procedure ---------------------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HANDLE WINAPI GetDlgItem(HANDLE dialog, int id);
EXPORT BOOL WINAPI IsChild(HANDLE parent, HANDLE hwnd);
EXPORT BOOL WINAPI IsWindowEnabled(HANDLE hwnd);
static LONG_PTR dialog_call_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  LONG_PTR (WINAPI *proc)(HANDLE, UINT, ULONG_PTR, LONG_PTR) = (void *)GetWindowLongPtrW(hwnd, DWLP_DLGPROC_);
  return proc ? proc(hwnd, message, wparam, lparam) : 0;
}
/* The messages whose result the dialog procedure gives back directly (the others are answered by DWLP_MSGRESULT when it returns TRUE). */
static int dialog_result_is_direct(UINT m) {
  return m == WM_INITDIALOG_ || m == 0x0027 /* WM_VKEYTOITEM */ || m == 0x0028 /* WM_CHARTOITEM */ || m == 0x0039 /* WM_COMPAREITEM */ || m == 0x0037 /* WM_QUERYDRAGICON */ || (m >= 0x0132 && m <= 0x0138);
}
EXPORT LONG_PTR WINAPI DefDlgProcW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam);

static LONG_PTR dialog_proc_common(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  DialogInfo *info = message == 0x0081 ? NULL : dialog_info(hwnd);
  if (message == 0x0081) {      /* WM_NCCREATE: the creation record and the state block */
    const CreateStruct_ *cs = (const CreateStruct_ *)lparam;
    const DialogCreate *create = (const DialogCreate *)cs->create_params;
    if (create && cs->create_params) {
      DialogInfo *made = HeapAlloc(GetProcessHeap(), 8, sizeof *made);
      if (!made) return FALSE;
      made->xbase = 6; made->ybase = 13;
      dialog_set_info(hwnd, made);
      LONG_PTR (WINAPI *proc)(HANDLE, UINT, ULONG_PTR, LONG_PTR) = create->ansi ? (void *)ansi_wrap((WNDPROC_)create->proc) : create->proc;
      SetWindowLongPtrW(hwnd, DWLP_DLGPROC_, (LONG_PTR)proc);
      SetWindowLongPtrW(hwnd, DWLP_USER_, 0);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }
  if (!info && message != 0x0082) return DefWindowProcW(hwnd, message, wparam, lparam);
  if (message != 0x0082) {      /* the dialog procedure sees the message first */
    const LONG_PTR r = dialog_call_proc(hwnd, message, wparam, lparam);
    if (message == WM_INITDIALOG_) return r;
    if (r) return dialog_result_is_direct(message) ? r : GetWindowLongPtrW(hwnd, DWLP_MSGRESULT_);
  }
  switch (message) {
    case 0x0082:      /* WM_NCDESTROY */
      if (info) { dialog_set_info(hwnd, NULL); HeapFree(GetProcessHeap(), 0, info); }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0014: { RECT_ r; GetClientRect(hwnd, &r); FillRect((HANDLE)wparam, &r, GetSysColorBrush(COLOR_BTNFACE_)); return 1; }      /* WM_ERASEBKGND */
    case 0x0132: case 0x0133: case 0x0134: case 0x0135: case 0x0136: case 0x0137: case 0x0138: {      /* WM_CTLCOLOR*: the system colours */
      HANDLE hdc = (HANDLE)wparam;
      const BOOL edit_like = message == 0x0133 || message == 0x0134;
      SetTextColor(hdc, GetSysColor(edit_like ? COLOR_WINDOWTEXT_ : COLOR_BTNTEXT_));
      SetBkColor(hdc, GetSysColor(edit_like ? COLOR_WINDOW_ : COLOR_BTNFACE_));
      return (LONG_PTR)GetSysColorBrush(edit_like ? COLOR_WINDOW_ : COLOR_BTNFACE_);
    }
    case 0x0010:      /* WM_CLOSE: the Cancel button is "pressed" */
      SendMessageW(hwnd, 0x0111, (ULONG_PTR)(IDCANCEL_ | (0 << 16)), (LONG_PTR)GetDlgItem(hwnd, IDCANCEL_));
      return 0;
    case 0x0007:      /* WM_SETFOCUS: back to the control that had it */
      if (info->focus && IsWindow(info->focus) && IsWindowEnabled(info->focus)) SetFocus(info->focus);
      else { HANDLE first = GetNextDlgTabItem(hwnd, NULL, FALSE); if (first) SetFocus(first); }
      return 0;
    case 0x0008: { HANDLE f = GetFocus(); if (f && IsChild(hwnd, f)) info->focus = f; return 0; }      /* WM_KILLFOCUS */
    case 0x0086: {      /* WM_NCACTIVATE */
      if (!wparam) { HANDLE f = GetFocus(); if (f && IsChild(hwnd, f)) info->focus = f; }
      return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case 0x0006: {      /* WM_ACTIVATE: the control that had the focus gets it back */
      if ((wparam & 0xFFFF) != 0) { if (info->focus && IsWindow(info->focus)) SetFocus(info->focus); else { HANDLE first = GetNextDlgTabItem(hwnd, NULL, FALSE); if (first) SetFocus(first); } }
      return 0;
    }
    case 0x0087: return 0;      /* WM_GETDLGCODE */
    case DM_GETDEFID_: return info->default_id ? (LONG_PTR)(((ULONG_PTR)DC_HASDEFID_ << 16) | info->default_id) : 0;
    case DM_SETDEFID_: {
      if (info->default_id == (UINT)wparam) return TRUE;
      HANDLE old = info->default_id ? GetDlgItem(hwnd, (int)info->default_id) : NULL;
      if (old && (GetWindowLongW(old, -16) & 0xF) == 1) SendMessageW(old, 0xF4 /* BM_SETSTYLE */, 0 /* BS_PUSHBUTTON */, TRUE);
      info->default_id = (UINT)wparam;
      HANDLE now = wparam ? GetDlgItem(hwnd, (int)wparam) : NULL;
      if (now && (SendMessageW(now, 0x0087, 0, 0) & 0x20 /* DLGC_UNDEFPUSHBUTTON */)) SendMessageW(now, 0xF4, 1 /* BS_DEFPUSHBUTTON */, TRUE);
      return TRUE;
    }
    case DM_REPOSITION_: return 0;
    case 0x0030: info->font = (HANDLE)wparam; return 0;      /* WM_SETFONT */
    case 0x0031: return (LONG_PTR)info->font;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
EXPORT LONG_PTR WINAPI DefDlgProcW(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return dialog_proc_common(hwnd, message, wparam, lparam); }
static LONG_PTR call_def_dlg(void *ctx, HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { (void)ctx; return dialog_proc_common(hwnd, message, wparam, lparam); }
EXPORT LONG_PTR WINAPI DefDlgProcA(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { return a_to_w(call_def_dlg, NULL, hwnd, message, wparam, lparam); }

/* ---- items, tab and group order ----------------------------------------------------------------------------------------------------------------------------------- */
EXPORT HANDLE WINAPI GetDlgItem(HANDLE dialog, int id) {
  for (HANDLE c = GetWindow(dialog, 5 /* GW_CHILD */); c; c = GetWindow(c, 2 /* GW_HWNDNEXT */)) if ((int)(GetWindowLongPtrW(c, -12 /* GWLP_ID */) & 0xFFFF) == (id & 0xFFFF) && (id >= 0 ? TRUE : TRUE)) return c;
  SetLastError(ERROR_CONTROL_ID_NOT_FOUND_);
  return NULL;
}
EXPORT int WINAPI GetDlgCtrlID(HANDLE hwnd) { return (int)GetWindowLongPtrW(hwnd, -12); }
EXPORT LONG_PTR WINAPI SendDlgItemMessageW(HANDLE dialog, int id, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { HANDLE c = GetDlgItem(dialog, id); return c ? SendMessageW(c, message, wparam, lparam) : 0; }
EXPORT LONG_PTR WINAPI SendDlgItemMessageA(HANDLE dialog, int id, UINT message, ULONG_PTR wparam, LONG_PTR lparam) { HANDLE c = GetDlgItem(dialog, id); return c ? SendMessageA(c, message, wparam, lparam) : 0; }
EXPORT BOOL WINAPI SetDlgItemTextW(HANDLE dialog, int id, LPCWSTR text) { return SendDlgItemMessageW(dialog, id, 0x000C, 0, (LONG_PTR)text) != 0; }
EXPORT BOOL WINAPI SetDlgItemTextA(HANDLE dialog, int id, LPCSTR text) { return SendDlgItemMessageA(dialog, id, 0x000C, 0, (LONG_PTR)text) != 0; }
EXPORT UINT WINAPI GetDlgItemTextW(HANDLE dialog, int id, LPWSTR buffer, int count) {
  HANDLE c = GetDlgItem(dialog, id);
  if (buffer && count > 0) buffer[0] = 0;
  return c && count > 0 ? (UINT)GetWindowTextW(c, buffer, count) : 0;
}
EXPORT UINT WINAPI GetDlgItemTextA(HANDLE dialog, int id, LPSTR buffer, int count) {
  HANDLE c = GetDlgItem(dialog, id);
  if (buffer && count > 0) buffer[0] = 0;
  return c && count > 0 ? (UINT)GetWindowTextA(c, buffer, count) : 0;
}
EXPORT BOOL WINAPI SetDlgItemInt(HANDLE dialog, int id, UINT value, BOOL is_signed) {
  WCHAR text[16], reversed[16];
  int n = 0;
  const BOOL negative = is_signed && (int)value < 0;
  UINT v = negative ? (UINT)(-(long long)(int)value) : value;
  do { reversed[n++] = (WCHAR)(L'0' + v % 10); v /= 10; } while (v);
  int len = 0;
  if (negative) text[len++] = L'-';
  while (n) text[len++] = reversed[--n];
  text[len] = 0;
  return SetDlgItemTextW(dialog, id, text);
}
EXPORT UINT WINAPI GetDlgItemInt(HANDLE dialog, int id, BOOL *translated, BOOL is_signed) {
  WCHAR text[32];
  unsigned long long v = 0;
  int i = 0;
  BOOL negative = FALSE, ok = FALSE;
  if (translated) *translated = FALSE;
  if (!GetDlgItemTextW(dialog, id, text, 32)) return 0;
  while (text[i] == L' ') ++i;
  if (is_signed && text[i] == L'-') { negative = TRUE; ++i; }
  for (; text[i] >= L'0' && text[i] <= L'9'; ++i) { v = v * 10 + (unsigned)(text[i] - L'0'); ok = TRUE; if (v > 0xFFFFFFFFull + 1) return 0; }
  if (text[i] || !ok) return 0;
  if (is_signed ? (negative ? v > 0x80000000ull : v > 0x7FFFFFFFull) : v > 0xFFFFFFFFull) return 0;
  if (translated) *translated = TRUE;
  return negative ? (UINT)(-(long long)v) : (UINT)v;
}
EXPORT BOOL WINAPI CheckDlgButton(HANDLE dialog, int id, UINT check) { return SendDlgItemMessageW(dialog, id, 0xF1 /* BM_SETCHECK */, check, 0) || TRUE; }
EXPORT UINT WINAPI IsDlgButtonChecked(HANDLE dialog, int id) { return (UINT)SendDlgItemMessageW(dialog, id, 0xF0 /* BM_GETCHECK */, 0, 0); }

/* The tab order is the z order of the children; a control counts when it is visible, enabled and (for the tab order) has WS_TABSTOP. */
static BOOL dialog_tab_ok(HANDLE c) { const LONG s = GetWindowLongW(c, -16); return (s & WS_VISIBLE_) && !(s & WS_DISABLED_) && (s & WS_TABSTOP_); }
static BOOL dialog_group_ok(HANDLE c) { const LONG s = GetWindowLongW(c, -16); return (s & WS_VISIBLE_) && !(s & WS_DISABLED_); }
EXPORT HANDLE WINAPI GetNextDlgTabItem(HANDLE dialog, HANDLE control, BOOL previous) {
  HANDLE start = control && IsChild(dialog, control) ? control : NULL, c, fallback = NULL;
  c = start ? start : (previous ? GetWindow(GetWindow(dialog, 5), 1 /* GW_HWNDLAST */) : GetWindow(dialog, 5));
  for (int guard = 0; c && guard < 1000; ++guard) {
    if (!(start && c == start && guard == 0)) { if (dialog_tab_ok(c)) return c; if (!fallback && dialog_group_ok(c)) fallback = c; }
    c = previous ? GetWindow(c, 3 /* GW_HWNDPREV */) : GetWindow(c, 2);
    if (!c) c = previous ? GetWindow(GetWindow(dialog, 5), 1) : GetWindow(dialog, 5);
    if (c == start) break;
  }
  return start && dialog_tab_ok(start) ? start : (fallback ? fallback : start);
}
EXPORT HANDLE WINAPI GetNextDlgGroupItem(HANDLE dialog, HANDLE control, BOOL previous) {
  HANDLE c = control && IsChild(dialog, control) ? control : GetWindow(dialog, 5), first_in_group = c;
  if (!c) return NULL;
  if (!control) { /* the first (or last) control of the dialog */ if (previous) { HANDLE last = GetWindow(c, 1); return last; } return c; }
  if (previous) {
    /* the previous control of the group; from the first of a group to the last of it */
    HANDLE p = GetWindow(control, 3);
    if (!(GetWindowLongW(control, -16) & WS_GROUP_) && p) { while (p && !dialog_group_ok(p)) p = GetWindow(p, 3); if (p) return p; }
    HANDLE last = control;
    for (HANDLE n = GetWindow(control, 2); n && !(GetWindowLongW(n, -16) & WS_GROUP_); n = GetWindow(n, 2)) if (dialog_group_ok(n)) last = n;
    return last;
  }
  for (HANDLE n = GetWindow(control, 2); n && !(GetWindowLongW(n, -16) & WS_GROUP_); n = GetWindow(n, 2)) if (dialog_group_ok(n)) return n;
  /* the end of the group: back to its first control */
  for (HANDLE q = control; q; q = GetWindow(q, 3)) { first_in_group = q; if (GetWindowLongW(q, -16) & WS_GROUP_) break; }
  return first_in_group;
}
EXPORT BOOL WINAPI CheckRadioButton(HANDLE dialog, int first, int last, int check) {
  HANDLE start = GetDlgItem(dialog, first), end = GetDlgItem(dialog, last), checked = GetDlgItem(dialog, check);
  if (!start || !end || !checked) return FALSE;
  /* from the first to the last in z order (the ids' order is not the order) */
  HANDLE c = start;
  for (int guard = 0; c && guard < 1000; ++guard, c = GetWindow(c, 2)) {
    SendMessageW(c, 0xF1, c == checked ? 1 : 0, 0);
    if (c == end) break;
  }
  return TRUE;
}

/* ---- IsDialogMessage -------------------------------------------------------------------------------------------------------------------------------------------------- */
static void dialog_press_default(HANDLE dialog) {
  DialogInfo *info = dialog_info(dialog);
  UINT id = info && info->default_id ? info->default_id : IDOK_;
  HANDLE button = GetDlgItem(dialog, (int)id);
  if (button && IsWindowEnabled(button) && IsWindowVisible(button)) SendMessageW(dialog, 0x0111, (ULONG_PTR)(id | (0u << 16)), (LONG_PTR)button);
  else if (!button) SendMessageW(dialog, 0x0111, (ULONG_PTR)IDOK_, 0);
}
EXPORT BOOL WINAPI IsDialogMessageW(HANDLE dialog, MSG_ *msg) {
  if (!dialog || !msg) return FALSE;
  if (msg->hwnd != dialog && !IsChild(dialog, msg->hwnd)) return FALSE;
  if (msg->message != 0x0100 /* WM_KEYDOWN */ && msg->message != 0x0104 /* WM_SYSKEYDOWN */ && msg->message != 0x0102 && msg->message != 0x0106) return FALSE;
  HANDLE focus = GetFocus();
  if (!focus || (focus != dialog && !IsChild(dialog, focus))) focus = msg->hwnd;
  LONG_PTR code = focus != dialog ? SendMessageW(focus, 0x0087 /* WM_GETDLGCODE */, msg->wparam, (LONG_PTR)msg) : 0;
  if (msg->message == 0x0100) {
    switch (msg->wparam) {
      case 0x09: {      /* VK_TAB */
        if (code & 0x2 /* DLGC_WANTTAB */) return FALSE;
        HANDLE next = GetNextDlgTabItem(dialog, focus == dialog ? NULL : focus, (GetKeyState(0x10) & 0x8000) != 0);
        if (next) { SendMessageW(dialog, 0x0028 /* WM_NEXTDLGCTL */, (ULONG_PTR)next, TRUE); SetFocus(next); }
        return TRUE;
      }
      case 0x0D: {      /* VK_RETURN: the default button, or the focused push button */
        if (code & 0x4 /* DLGC_WANTALLKEYS */) return FALSE;
        if (code & (0x20 | 0x10 | 0x2000)) { if (code & (0x20 | 0x10)) { SendMessageW(focus, 0xF5 /* BM_CLICK */, 0, 0); return TRUE; } }
        dialog_press_default(dialog);
        return TRUE;
      }
      case 0x1B:      /* VK_ESCAPE */
        if (code & 0x4) return FALSE;
        SendMessageW(dialog, 0x0111, (ULONG_PTR)IDCANCEL_, (LONG_PTR)GetDlgItem(dialog, IDCANCEL_));
        return TRUE;
      case 0x25: case 0x26: case 0x27: case 0x28: {      /* the arrow keys move inside a group of buttons */
        if (code & 0x1 /* DLGC_WANTARROWS */) return FALSE;
        if (focus == dialog || !(code & (0x40 | 0x2000))) return FALSE;
        HANDLE next = GetNextDlgGroupItem(dialog, focus, (msg->wparam == 0x25 || msg->wparam == 0x26));
        if (next && next != focus) { SetFocus(next); if (SendMessageW(next, 0x0087, 0, 0) & 0x40) SendMessageW(next, 0xF5, 0, 0); }
        return TRUE;
      }
    }
  }
  return FALSE;
}
EXPORT BOOL WINAPI IsDialogMessageA(HANDLE dialog, MSG_ *msg) { return IsDialogMessageW(dialog, msg); }

/* ---- creating and running dialogs ---------------------------------------------------------------------------------------------------------------------------------------- */
typedef LONG_PTR (WINAPI *DlgProc_)(HANDLE, UINT, ULONG_PTR, LONG_PTR);
EXPORT HANDLE WINAPI CreateDialogIndirectParamW(HANDLE instance, const void *tmpl, HANDLE owner, DlgProc_ proc, LONG_PTR param) { return dialog_create(instance, tmpl, owner, proc, param, FALSE); }
EXPORT HANDLE WINAPI CreateDialogIndirectParamA(HANDLE instance, const void *tmpl, HANDLE owner, DlgProc_ proc, LONG_PTR param) { return dialog_create(instance, tmpl, owner, proc, param, TRUE); }
static const void *dialog_resource(HANDLE instance, const WCHAR *name) {
  HANDLE module = instance ? instance : GetModuleHandleW(NULL);
  HANDLE res = FindResourceW(module, name, (LPCWSTR)(ULONG_PTR)RT_DIALOG_);
  HANDLE loaded = res ? LoadResource(module, res) : NULL;
  return loaded ? LockResource(loaded) : NULL;
}
EXPORT HANDLE WINAPI CreateDialogParamW(HANDLE instance, const WCHAR *name, HANDLE owner, DlgProc_ proc, LONG_PTR param) {
  const void *tmpl = dialog_resource(instance, name);
  return tmpl ? dialog_create(instance, tmpl, owner, proc, param, FALSE) : NULL;
}
EXPORT HANDLE WINAPI CreateDialogParamA(HANDLE instance, const char *name, HANDLE owner, DlgProc_ proc, LONG_PTR param) {
  WCHAR *w = wide_of(name);
  const void *tmpl = dialog_resource(instance, w);
  wide_free(w, name);
  return tmpl ? dialog_create(instance, tmpl, owner, proc, param, TRUE) : NULL;
}
EXPORT BOOL WINAPI EndDialog(HANDLE hwnd, LONG_PTR result) {
  DialogInfo *info = dialog_info(hwnd);
  if (!info) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return FALSE; }
  info->ended = TRUE;
  info->result = result;
  /* the owner (or, for a child dialog, the parent: possibly a new one) is enabled again; the end of the modal loop does not do it */
  HANDLE owner = (GetWindowLongW(hwnd, -16) & 0x40000000 /* WS_CHILD */) ? GetParent(hwnd) : GetWindow(hwnd, 4 /* GW_OWNER */);
  if (owner && IsWindow(owner)) EnableWindow(owner, TRUE);
  ShowWindow(hwnd, 0);
  PostMessageW(hwnd, 0 /* WM_NULL: wakes the loop */, 0, 0);
  return TRUE;
}
/* The modal loop: the owner is disabled (a child window given as owner stands for its nearest ancestor that is not a child), messages go through IsDialogMessage until EndDialog, WM_QUIT or the
 * destruction of the dialog; WM_ENTERIDLE tells the owner once whenever no input is waiting. The answer is EndDialog's (1 when the loop ended otherwise, WM_QUIT included), -1 when the dialog could not be made. */
static LONG_PTR dialog_run(HANDLE instance, const void *tmpl, HANDLE owner, DlgProc_ proc, LONG_PTR param, BOOL ansi) {
  if (!tmpl) return -1;
  while (owner && IsWindow(owner) && (GetWindowLongW(owner, -16) & 0x40000000)) owner = GetParent(owner);
  if (owner && IsWindowEnabled(owner)) EnableWindow(owner, FALSE);
  const DWORD style = *(const WORD *)tmpl == 1 ? *(const DWORD *)((const WORD *)tmpl + 6) : *(const DWORD *)tmpl;
  HANDLE dlg = dialog_create(instance, tmpl, owner, proc, param, ansi);
  if (!dlg) { if (owner && IsWindow(owner)) EnableWindow(owner, TRUE); return -1; }
  if (!(style & WS_VISIBLE_)) ShowWindow(dlg, 5);
  LONG_PTR result = 1;
  DialogInfo *info = dialog_info(dlg);
  MSG_ msg;
  BOOL idle_sent = FALSE;
  while (info && !info->ended && IsWindow(dlg)) {
    if (!idle_sent && !PeekMessageW(&msg, NULL, 0, 0, 0 /* PM_NOREMOVE */)) {
      idle_sent = TRUE;
      if (owner && IsWindow(owner)) SendMessageW(owner, 0x121 /* WM_ENTERIDLE */, 0 /* MSGF_DIALOGBOX */, (LONG_PTR)dlg);
      info = IsWindow(dlg) ? dialog_info(dlg) : NULL;
      continue;
    }
    idle_sent = FALSE;
    const int got = GetMessageW(&msg, NULL, 0, 0);
    if (got == 0) { PostQuitMessage((int)msg.wparam); info = NULL; break; }      /* WM_QUIT ends the dialog (answer 1) and is posted again for the loop that owns the thread */
    if (got < 0) break;
    if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    info = IsWindow(dlg) ? dialog_info(dlg) : NULL;
  }
  if (info) result = info->result;
  if (owner && IsWindow(owner) && GetActiveWindow() == dlg) SetActiveWindow(owner);
  if (IsWindow(dlg)) DestroyWindow(dlg);
  return result;
}
EXPORT LONG_PTR WINAPI DialogBoxIndirectParamW(HANDLE instance, const void *tmpl, HANDLE owner, DlgProc_ proc, LONG_PTR param) { return dialog_run(instance, tmpl, owner, proc, param, FALSE); }
EXPORT LONG_PTR WINAPI DialogBoxIndirectParamA(HANDLE instance, const void *tmpl, HANDLE owner, DlgProc_ proc, LONG_PTR param) { return dialog_run(instance, tmpl, owner, proc, param, TRUE); }
EXPORT LONG_PTR WINAPI DialogBoxParamW(HANDLE instance, const WCHAR *name, HANDLE owner, DlgProc_ proc, LONG_PTR param) { return dialog_run(instance, dialog_resource(instance, name), owner, proc, param, FALSE); }
EXPORT LONG_PTR WINAPI DialogBoxParamA(HANDLE instance, const char *name, HANDLE owner, DlgProc_ proc, LONG_PTR param) {
  WCHAR *w = wide_of(name);
  const LONG_PTR r = dialog_run(instance, dialog_resource(instance, w), owner, proc, param, TRUE);
  wide_free(w, name);
  return r;
}
