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

/* The "ComboBox" class (included by window.inc after listbox.inc). Like Windows it is made of other windows: an Edit control child (simple and drop-down boxes) and a list box (the
 * "ComboLBox" class: a child for the simple box, a popup owned by the combo box for the others). The CB_* messages go to the list box or the edit control, the notifications of both come
 * back as WM_COMMAND to the combo box, which sends the CBN_* codes to its parent. Plain layout and painting; not done: owner-draw items (the combo box does not send WM_DRAWITEM), extended
 * UI, auto-completion, CB_GETCOMBOBOXINFO. */
#define CBS_SIMPLE_ 1
#define CBS_DROPDOWN_ 2
#define CBS_DROPDOWNLIST_ 3
#define CBS_AUTOHSCROLL_ 0x40
#define CBS_SORT_ 0x100
#define CBS_HASSTRINGS_ 0x200
#define CBS_NOINTEGRALHEIGHT_ 0x400
#define CBS_OWNERDRAWFIXED_ 0x10
#define CBS_OWNERDRAWVARIABLE_ 0x20
#define CBN_SELCHANGE_ 1
#define CBN_DBLCLK_ 2
#define CBN_SETFOCUS_ 3
#define CBN_KILLFOCUS_ 4
#define CBN_EDITCHANGE_ 5
#define CBN_EDITUPDATE_ 6
#define CBN_DROPDOWN_ 7
#define CBN_CLOSEUP_ 8
#define CBN_SELENDOK_ 9
#define CBN_SELENDCANCEL_ 10
#define CB_ERR_ (-1)
enum { CB_GETEDITSEL_ = 0x140, CB_LIMITTEXT_, CB_SETEDITSEL_, CB_ADDSTRING_, CB_DELETESTRING_, CB_DIR_, CB_GETCOUNT_, CB_GETCURSEL_, CB_GETLBTEXT_, CB_GETLBTEXTLEN_, CB_INSERTSTRING_,
       CB_RESETCONTENT_, CB_FINDSTRING_, CB_SELECTSTRING_, CB_SETCURSEL_, CB_SHOWDROPDOWN_, CB_GETITEMDATA_, CB_SETITEMDATA_, CB_GETDROPPEDCONTROLRECT_, CB_SETITEMHEIGHT_, CB_GETITEMHEIGHT_,
       CB_SETEXTENDEDUI_, CB_GETEXTENDEDUI_, CB_GETDROPPEDSTATE_, CB_FINDSTRINGEXACT_, CB_SETLOCALE_, CB_GETLOCALE_, CB_GETTOPINDEX_, CB_SETTOPINDEX_, CB_GETHORIZONTALEXTENT_,
       CB_SETHORIZONTALEXTENT_, CB_GETDROPPEDWIDTH_, CB_SETDROPPEDWIDTH_, CB_INITSTORAGE_ };
#define COMBO_BUTTON 16
#define WS_CHILD_ 0x40000000u
#define WS_POPUP_ 0x80000000u
typedef struct { HANDLE parent, edit, list; BOOL dropped, extended_ui; int dropped_width, edit_height; HANDLE font; BOOL focused; } ComboState;

/* The message of the list box for a CB_* message (0 when the message is not one that goes to the list). */
static UINT combo_list_message(UINT m) {
  switch (m) {
    case CB_ADDSTRING_: return LB_ADDSTRING_;
    case CB_DELETESTRING_: return LB_DELETESTRING_;
    case CB_GETCOUNT_: return LB_GETCOUNT_;
    case CB_GETLBTEXT_: return LB_GETTEXT_;
    case CB_GETLBTEXTLEN_: return LB_GETTEXTLEN_;
    case CB_INSERTSTRING_: return LB_INSERTSTRING_;
    case CB_RESETCONTENT_: return LB_RESETCONTENT_;
    case CB_FINDSTRING_: return LB_FINDSTRING_;
    case CB_FINDSTRINGEXACT_: return LB_FINDSTRINGEXACT_;
    case CB_GETITEMDATA_: return LB_GETITEMDATA_;
    case CB_SETITEMDATA_: return LB_SETITEMDATA_;
    case CB_SETITEMHEIGHT_: return LB_SETITEMHEIGHT_;
    case CB_GETITEMHEIGHT_: return LB_GETITEMHEIGHT_;
    case CB_SETLOCALE_: return LB_SETLOCALE_;
    case CB_GETLOCALE_: return LB_GETLOCALE_;
    case CB_GETTOPINDEX_: return LB_GETTOPINDEX_;
    case CB_SETTOPINDEX_: return LB_SETTOPINDEX_;
    case CB_GETHORIZONTALEXTENT_: return LB_GETHORIZONTALEXTENT_;
    case CB_SETHORIZONTALEXTENT_: return LB_SETHORIZONTALEXTENT_;
    case CB_INITSTORAGE_: return LB_INITSTORAGE_;
  }
  return 0;
}
static UINT combo_type(HANDLE hwnd) { return (UINT)ctl_style(hwnd) & 3; }
static void combo_notify(HANDLE hwnd, UINT code) { ctl_notify(hwnd, code); }

/* Sets the edit control (or the displayed text of a list-only box) from the list's selection. */
static void combo_sync_text(HANDLE hwnd, ComboState *st) {
  const int sel = (int)SendMessageW(st->list, LB_GETCURSEL_, 0, 0);
  if (!st->edit) { ctl_repaint(hwnd); return; }
  WCHAR text[256] = L"";
  if (sel >= 0 && SendMessageW(st->list, LB_GETTEXTLEN_, (ULONG_PTR)sel, 0) < 255) SendMessageW(st->list, LB_GETTEXT_, (ULONG_PTR)sel, (LONG_PTR)text);
  SendMessageW(st->edit, 0x000C /* WM_SETTEXT */, 0, (LONG_PTR)text);
  SendMessageW(st->edit, 0x00B1 /* EM_SETSEL */, 0, (LONG_PTR)-1);
}
static void combo_layout(HANDLE hwnd, ComboState *st) {
  RECT_ r;
  GetClientRect(hwnd, &r);
  const UINT type = combo_type(hwnd);
  const int edit_height = st->edit_height > 0 ? st->edit_height : 21;
  if (st->edit) SetWindowPos(st->edit, NULL, 0, 0, type == CBS_SIMPLE_ ? r.right : r.right - COMBO_BUTTON, edit_height, 0x14 /* SWP_NOZORDER | SWP_NOACTIVATE */);
  if (type == CBS_SIMPLE_) SetWindowPos(st->list, NULL, 0, edit_height, r.right, r.bottom - edit_height > 0 ? r.bottom - edit_height : 0, 0x14);
}
static void combo_show_dropdown(HANDLE hwnd, ComboState *st, BOOL show) {
  if (combo_type(hwnd) == CBS_SIMPLE_ || st->dropped == show) return;
  if (show) {
    RECT_ r;
    GetWindowRect(hwnd, &r);
    const int count = (int)SendMessageW(st->list, LB_GETCOUNT_, 0, 0), ih = (int)SendMessageW(st->list, LB_GETITEMHEIGHT_, 0, 0);
    const int rows = count < 1 ? 1 : count > 6 ? 6 : count;
    const int width = st->dropped_width > r.right - r.left ? st->dropped_width : r.right - r.left;
    combo_notify(hwnd, CBN_DROPDOWN_);
    st->dropped = TRUE;
    SetWindowPos(st->list, NULL, r.left, r.bottom, width, rows * (ih > 0 ? ih : 16) + 2, 0x10 | 0x40 /* SWP_NOACTIVATE | SWP_SHOWWINDOW */);
  } else {
    st->dropped = FALSE;
    ShowWindow(st->list, 0);
    combo_notify(hwnd, CBN_CLOSEUP_);
  }
  ctl_repaint(hwnd);
}
static void combo_paint(HANDLE hwnd, HANDLE hdc, ComboState *st) {
  RECT_ r;
  GetClientRect(hwnd, &r);
  const UINT type = combo_type(hwnd);
  const int saved = SaveDC(hdc);
  ctl_select_font(hdc, st->font);
  if (type == CBS_DROPDOWNLIST_) {
    RECT_ box = {r.left, r.top, r.right - COMBO_BUTTON, r.top + (st->edit_height > 0 ? st->edit_height : 21)};
    WCHAR text[256] = L"";
    const int sel = (int)SendMessageW(st->list, LB_GETCURSEL_, 0, 0);
    if (sel >= 0 && SendMessageW(st->list, LB_GETTEXTLEN_, (ULONG_PTR)sel, 0) < 255) SendMessageW(st->list, LB_GETTEXT_, (ULONG_PTR)sel, (LONG_PTR)text);
    FillRect(hdc, &box, GetSysColorBrush(COLOR_WINDOW_));
    SetBkMode(hdc, 1);
    SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT_));
    RECT_ tr = {box.left + 2, box.top, box.right, box.bottom};
    ctl_text(hdc, text, &tr, DT_VCENTER_ | DT_SINGLELINE_ | DT_NOPREFIX_, NULL);
  }
  if (type != CBS_SIMPLE_) {
    RECT_ b = {r.right - COMBO_BUTTON, r.top, r.right, r.top + (st->edit_height > 0 ? st->edit_height : 21)};
    FillRect(hdc, &b, GetSysColorBrush(COLOR_BTNFACE_));
    FrameRect(hdc, &b, GetSysColorBrush(COLOR_BTNSHADOW_));
  }
  RestoreDC(hdc, saved);
}

static LONG_PTR WINAPI combo_proc(HANDLE hwnd, UINT message, ULONG_PTR wparam, LONG_PTR lparam) {
  ComboState *st = message == 0x0081 ? NULL : ctl_state(hwnd);
  const LONG style = ctl_style(hwnd);
  const UINT type = (UINT)style & 3;
  if (!st && message != 0x0081 && message != 0x0082) return DefWindowProcW(hwnd, message, wparam, lparam);
  if (st) {
    const UINT lb = combo_list_message(message);
    if (lb) return SendMessageW(st->list, lb, wparam, lparam);
  }
  switch (message) {
    case 0x0081:
      if (!(st = ctl_alloc(hwnd, sizeof(ComboState)))) return FALSE;
      st->parent = (HANDLE)((CreateStruct_ *)lparam)->parent;
      st->edit_height = 21;
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0001: {      /* WM_CREATE: the children */
      const CreateStruct_ *cs = (const CreateStruct_ *)lparam;
      LONG list_style = LBS_NOTIFY_ | LBS_NOINTEGRALHEIGHT_;
      if (style & CBS_SORT_) list_style |= LBS_SORT_;
      if (style & CBS_HASSTRINGS_) list_style |= LBS_HASSTRINGS_;
      if (style & CBS_OWNERDRAWFIXED_) list_style |= LBS_OWNERDRAWFIXED_;
      if (style & CBS_OWNERDRAWVARIABLE_) list_style |= LBS_OWNERDRAWVARIABLE_;
      if (!(style & (CBS_OWNERDRAWFIXED_ | CBS_OWNERDRAWVARIABLE_))) list_style |= LBS_HASSTRINGS_;
      if (type == CBS_SIMPLE_) st->list = CreateWindowExW(0x200 /* WS_EX_CLIENTEDGE */, L"ComboLBox", NULL, WS_CHILD_ | 0x10000000 | 0x00800000 | (DWORD)list_style, 0, cs->cy > 0 ? 21 : 0, cs->cx, cs->cy > 21 ? cs->cy - 21 : 0, hwnd, (HANDLE)1000, cs->instance ? (HANDLE)cs->instance : NULL, NULL);
      else st->list = CreateWindowExW(0x80 /* WS_EX_TOOLWINDOW */, L"ComboLBox", NULL, WS_POPUP_ | 0x00800000 | (DWORD)list_style, 0, 0, 100, 100, hwnd, (HANDLE)1000, cs->instance ? (HANDLE)cs->instance : NULL, NULL);
      if (type != CBS_DROPDOWNLIST_) {
        DWORD es = WS_CHILD_ | 0x10000000 | ES_AUTOHSCROLL_;
        if (style & 0x2000) es |= ES_UPPERCASE_;
        if (style & 0x4000) es |= ES_LOWERCASE_;
        st->edit = CreateWindowExW(0, L"Edit", NULL, es, 0, 0, cs->cx > COMBO_BUTTON ? cs->cx - COMBO_BUTTON : cs->cx, 21, hwnd, (HANDLE)1001, cs->instance ? (HANDLE)cs->instance : NULL, NULL);
      }
      if (type != CBS_SIMPLE_) {      /* the box is one edit line high; the list is the dropped part */
        RECT_ w;
        GetWindowRect(hwnd, &w);
        SetWindowPos(hwnd, NULL, 0, 0, w.right - w.left, st->edit_height + 2 * 0, 0x16);
      }
      return (st->list && (type == CBS_DROPDOWNLIST_ || st->edit)) ? 0 : -1;
    }
    case 0x0082:
      if (st->list) DestroyWindow(st->list);
      ctl_free(hwnd);
      return DefWindowProcW(hwnd, message, wparam, lparam);
    case 0x0005: combo_layout(hwnd, st); return 0;      /* WM_SIZE */
    case 0x0014: return 1;
    case 0x000F: case 0x0318: {
      PAINTSTRUCT_ ps;
      HANDLE hdc = message == 0x0318 ? (HANDLE)wparam : BeginPaint(hwnd, &ps);
      if (hdc) { combo_paint(hwnd, hdc, st); if (message == 0x000F) EndPaint(hwnd, &ps); }
      return 0;
    }
    case 0x000A: ctl_repaint(hwnd); if (st->edit) EnableWindow(st->edit, wparam != 0); EnableWindow(st->list, wparam != 0); return 0;
    case 0x0030:
      st->font = (HANDLE)wparam;
      if (st->edit) SendMessageW(st->edit, message, wparam, lparam);
      SendMessageW(st->list, message, wparam, lparam);
      return 0;
    case 0x0031: return (LONG_PTR)st->font;
    case 0x0087: return DLGC_WANTCHARS_ | DLGC_WANTARROWS_ | (type == CBS_DROPDOWNLIST_ ? 0 : 0x8 /* DLGC_HASSETSEL */);
    case 0x0007:      /* WM_SETFOCUS: an edit takes the focus */
      st->focused = TRUE;
      if (st->edit) SetFocus(st->edit); else ctl_repaint(hwnd);
      combo_notify(hwnd, CBN_SETFOCUS_);
      return 0;
    case 0x0008: st->focused = FALSE; if (st->dropped) combo_show_dropdown(hwnd, st, FALSE); combo_notify(hwnd, CBN_KILLFOCUS_); ctl_repaint(hwnd); return 0;
    case 0x0201: {      /* WM_LBUTTONDOWN: the button (or the whole of a list-only box) opens and closes the list */
      RECT_ r;
      GetClientRect(hwnd, &r);
      const int x = (short)(lparam & 0xFFFF);
      if (type == CBS_SIMPLE_) return 0;
      if (type == CBS_DROPDOWNLIST_ || x >= r.right - COMBO_BUTTON) { SetFocus(hwnd); combo_show_dropdown(hwnd, st, !st->dropped); }
      return 0;
    }
    case 0x0111: {      /* WM_COMMAND from the list box and the edit control */
      const HANDLE from = (HANDLE)lparam;
      const UINT code = (UINT)(wparam >> 16);
      if (from == st->list) {
        switch (code) {
          case LBN_SELCHANGE_: combo_sync_text(hwnd, st); combo_notify(hwnd, CBN_SELCHANGE_); if (st->dropped) { combo_notify(hwnd, CBN_SELENDOK_); combo_show_dropdown(hwnd, st, FALSE); } break;
          case LBN_DBLCLK_: combo_notify(hwnd, CBN_DBLCLK_); break;
          case LBN_SELCANCEL_: combo_notify(hwnd, CBN_SELENDCANCEL_); break;
        }
      } else if (from == st->edit) {
        if (code == EN_CHANGE_) combo_notify(hwnd, CBN_EDITCHANGE_);
        else if (code == EN_UPDATE_) combo_notify(hwnd, CBN_EDITUPDATE_);
      }
      return 0;
    }
    case 0x000C: case 0x000D: case 0x000E: {      /* WM_SETTEXT, WM_GETTEXT, WM_GETTEXTLENGTH: the edit control's text (the selected item for a list-only box) */
      if (st->edit) return SendMessageW(st->edit, message, wparam, lparam);
      if (message == 0x000C) return CB_ERR_;
      const int sel = (int)SendMessageW(st->list, LB_GETCURSEL_, 0, 0);
      if (sel < 0) { if (message == 0x000D && lparam) *(WCHAR *)lparam = 0; return 0; }
      if (message == 0x000E) return SendMessageW(st->list, LB_GETTEXTLEN_, (ULONG_PTR)sel, 0);
      { WCHAR text[256] = L""; if (SendMessageW(st->list, LB_GETTEXTLEN_, (ULONG_PTR)sel, 0) >= 255) return 0;
        SendMessageW(st->list, LB_GETTEXT_, (ULONG_PTR)sel, (LONG_PTR)text);
        UINT n = 0; WCHAR *out = (WCHAR *)lparam;
        while (text[n] && n + 1 < wparam) { out[n] = text[n]; ++n; }
        out[n] = 0; return n; }
    }
    case CB_GETCURSEL_: return SendMessageW(st->list, LB_GETCURSEL_, 0, 0);
    case CB_SETCURSEL_: {
      const LONG_PTR r = SendMessageW(st->list, LB_SETCURSEL_, wparam, 0);
      if (r != CB_ERR_ || (int)wparam == -1) { if ((int)wparam == -1 && st->edit) SendMessageW(st->edit, 0x000C, 0, (LONG_PTR)L""); else combo_sync_text(hwnd, st); }
      return r;
    }
    case CB_SELECTSTRING_: {
      const LONG_PTR r = SendMessageW(st->list, LB_SELECTSTRING_, wparam, lparam);
      if (r >= 0) combo_sync_text(hwnd, st);
      return r;
    }
    case CB_SHOWDROPDOWN_: combo_show_dropdown(hwnd, st, wparam != 0); return TRUE;
    case CB_GETDROPPEDSTATE_: return st->dropped;
    case CB_GETDROPPEDCONTROLRECT_: { RECT_ *out = (RECT_ *)lparam; if (!out) return CB_ERR_; if (type == CBS_SIMPLE_) { GetWindowRect(st->list, out); } else GetWindowRect(st->list, out); return TRUE; }
    case CB_SETEXTENDEDUI_: if (wparam && type == CBS_SIMPLE_) return CB_ERR_; st->extended_ui = wparam != 0; return 0;
    case CB_GETEXTENDEDUI_: return st->extended_ui;
    case CB_GETDROPPEDWIDTH_: { RECT_ r; GetWindowRect(hwnd, &r); return st->dropped_width > r.right - r.left ? st->dropped_width : r.right - r.left; }
    case CB_SETDROPPEDWIDTH_: if (type == CBS_SIMPLE_) return CB_ERR_; st->dropped_width = (int)wparam; return (LONG_PTR)(st->dropped_width);
    case CB_LIMITTEXT_: if (!st->edit) return TRUE; SendMessageW(st->edit, 0x00C5 /* EM_SETLIMITTEXT */, wparam, 0); return TRUE;
    case CB_GETEDITSEL_: if (!st->edit) return CB_ERR_; return SendMessageW(st->edit, 0x00B0, wparam, lparam);
    case CB_SETEDITSEL_: if (!st->edit) return CB_ERR_; return SendMessageW(st->edit, 0x00B1, (ULONG_PTR)(short)(lparam & 0xFFFF), (LONG_PTR)(short)((lparam >> 16) & 0xFFFF));
    case CB_DIR_: return CB_ERR_;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}
