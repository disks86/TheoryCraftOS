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

/* Menus of user32.dll, the data half (included by window.inc after the dialogs; the part that shows and tracks them is menuui.inc). The menus themselves are win32k's HMENU objects
 * (src/Process/Win32k/Win32kUserMiscMenu.cpp): items with a string, id, type, state, submenu, check-mark bitmaps, item data and bitmap, and the menu and system menu of a window.
 * This file converts the classic calls (AppendMenu, InsertMenu, ModifyMenu with MF_ flags) and the A strings to the services' MENUITEMINFOW, and loads menus from resources. */

#define MIIM_STATE_ 0x1
#define MIIM_ID_ 0x2
#define MIIM_SUBMENU_ 0x4
#define MIIM_CHECKMARKS_ 0x8
#define MIIM_TYPE_ 0x10
#define MIIM_DATA_ 0x20
#define MIIM_STRING_ 0x40
#define MIIM_BITMAP_ 0x80
#define MIIM_FTYPE_ 0x100
#define MFT_BITMAP_ 0x4
#define MFT_OWNERDRAW_ 0x100
#define MFT_SEPARATOR_ 0x800
#define MF_BYPOSITION_ 0x400
#define MF_POPUP_ 0x10
#define MF_END_ 0x80
#define MF_SEPARATOR_ 0x800
#define MF_STRING_FLAGS_ (MFT_BITMAP_ | MFT_OWNERDRAW_ | MFT_SEPARATOR_)
#define MIM_HELPID_ 0x4
#define RT_MENU_ 4
#define ERROR_INVALID_MENU_HANDLE_ 1401
#define ERROR_MENU_ITEM_NOT_FOUND_ 1456

typedef struct { UINT cbSize, fMask, fType, fState, wID; HANDLE hSubMenu, hbmpChecked, hbmpUnchecked; ULONG_PTR dwItemData; WCHAR *dwTypeData; UINT cch; HANDLE hbmpItem; } MII_;
typedef struct { UINT cbSize, fMask, dwStyle, cyMax; HANDLE hbrBack; DWORD dwContextHelpID; ULONG_PTR dwMenuData; } MENUINFO_;
enum { MENU_SET_ITEM_INFO, MENU_INSERT_ITEM, MENU_CHECK_RADIO, MENU_GET_DEFAULT, MENU_GET_ID, MENU_GET_INFO_A, MENU_GET_INFO_W, MENU_GET_STATE, MENU_GET_SUB };

SVC NtUserCreateMenu(void);
SVC NtUserCreatePopupMenu(void);
SVC NtUserDestroyMenu(ULONG_PTR menu);
SVC NtUserRemoveMenu(ULONG_PTR menu, ULONG_PTR position, ULONG_PTR flags);
SVC NtUserDeleteMenu(ULONG_PTR menu, ULONG_PTR position, ULONG_PTR flags);
SVC NtUserCheckMenuItem(ULONG_PTR menu, ULONG_PTR position, ULONG_PTR flags);
SVC NtUserEnableMenuItem(ULONG_PTR menu, ULONG_PTR position, ULONG_PTR flags);
SVC NtUserHiliteMenuItem(ULONG_PTR hwnd, ULONG_PTR menu, ULONG_PTR position, ULONG_PTR flags);
SVC NtUserSetMenuDefaultItem(ULONG_PTR menu, ULONG_PTR item, ULONG_PTR by_position);
SVC NtUserSetMenuContextHelpId(ULONG_PTR menu, ULONG_PTR id);
SVC NtUserThunkedMenuInfo(ULONG_PTR menu, ULONG_PTR info);
SVC NtUserThunkedMenuItemInfo(ULONG_PTR menu, ULONG_PTR position, ULONG_PTR flags, ULONG_PTR method, ULONG_PTR info, ULONG_PTR text);
SVC NtUserSetMenu(ULONG_PTR hwnd, ULONG_PTR menu);
SVC NtUserDrawMenuBar(ULONG_PTR hwnd);
SVC NtUserGetSystemMenu(ULONG_PTR hwnd, ULONG_PTR revert);
SVC NtUserSetSystemMenu(ULONG_PTR hwnd, ULONG_PTR menu);

EXPORT HANDLE WINAPI CreateMenu(void) { return (HANDLE)NtUserCreateMenu(); }
EXPORT HANDLE WINAPI CreatePopupMenu(void) { return (HANDLE)NtUserCreatePopupMenu(); }
EXPORT BOOL WINAPI DestroyMenu(HANDLE menu) { return (NtUserDestroyMenu((ULONG_PTR)menu) & 0xFFFFFFFFu) != 0; }
EXPORT int WINAPI GetMenuItemCount(HANDLE menu) {
  const int n = (int)(LONG)(NtUserCallOneParam((ULONG_PTR)menu, 4 /* GetMenuItemCount */) & 0xFFFFFFFFu);
  if (n < 0) SetLastError(ERROR_INVALID_MENU_HANDLE_);
  return n;
}
EXPORT BOOL WINAPI IsMenu(HANDLE menu) { return (int)(LONG)(NtUserCallOneParam((ULONG_PTR)menu, 4) & 0xFFFFFFFFu) >= 0; }

/* ---- items --------------------------------------------------------------------------------------------------------------------------------------------------------- */
static BOOL mii_has_text(const MII_ *m) { return (m->fMask & MIIM_STRING_) || ((m->fMask & MIIM_TYPE_) && !(m->fType & MF_STRING_FLAGS_)); }

/* SetMenuItemInfo / InsertMenuItem: the A string is converted; the service takes the Unicode structure. */
static BOOL menu_item_set(HANDLE menu, UINT pos, UINT flags, int method, const MII_ *mii, BOOL ansi) {
  if (!mii || (mii->cbSize != sizeof(MII_) && mii->cbSize != 72)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  MII_ copy;
  for (size_t_ i = 0; i < sizeof copy; ++i) ((BYTE *)&copy)[i] = i < mii->cbSize ? ((const BYTE *)mii)[i] : 0;
  WCHAR *wide = NULL;
  if (ansi && mii_has_text(mii) && mii->dwTypeData) { wide = wide_of((const char *)mii->dwTypeData); copy.dwTypeData = wide; }
  const BOOL ok = (NtUserThunkedMenuItemInfo((ULONG_PTR)menu, pos, flags, method, (ULONG_PTR)&copy, 0) & 0xFFFFFFFFu) != 0;
  u32_free(wide);
  return ok;
}
EXPORT BOOL WINAPI SetMenuItemInfoW(HANDLE menu, UINT item, BOOL by_position, const MII_ *mii) { return menu_item_set(menu, item, by_position ? MF_BYPOSITION_ : 0, MENU_SET_ITEM_INFO, mii, FALSE); }
EXPORT BOOL WINAPI SetMenuItemInfoA(HANDLE menu, UINT item, BOOL by_position, const MII_ *mii) { return menu_item_set(menu, item, by_position ? MF_BYPOSITION_ : 0, MENU_SET_ITEM_INFO, mii, TRUE); }
EXPORT BOOL WINAPI InsertMenuItemW(HANDLE menu, UINT item, BOOL by_position, const MII_ *mii) { return menu_item_set(menu, item, by_position ? MF_BYPOSITION_ : 0, MENU_INSERT_ITEM, mii, FALSE); }
EXPORT BOOL WINAPI InsertMenuItemA(HANDLE menu, UINT item, BOOL by_position, const MII_ *mii) { return menu_item_set(menu, item, by_position ? MF_BYPOSITION_ : 0, MENU_INSERT_ITEM, mii, TRUE); }
EXPORT BOOL WINAPI GetMenuItemInfoW(HANDLE menu, UINT item, BOOL by_position, MII_ *mii) {
  if (!mii || (mii->cbSize != sizeof(MII_) && mii->cbSize != 72)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserThunkedMenuItemInfo((ULONG_PTR)menu, item, by_position ? MF_BYPOSITION_ : 0, MENU_GET_INFO_W, (ULONG_PTR)mii, 0) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI GetMenuItemInfoA(HANDLE menu, UINT item, BOOL by_position, MII_ *mii) {
  if (!mii || (mii->cbSize != sizeof(MII_) && mii->cbSize != 72)) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
  return (NtUserThunkedMenuItemInfo((ULONG_PTR)menu, item, by_position ? MF_BYPOSITION_ : 0, MENU_GET_INFO_A, (ULONG_PTR)mii, 0) & 0xFFFFFFFFu) != 0;
}

/* The classic calls: MF_ flags to an item description. The flags that are also MFT_ types or MFS_ states keep their values; MF_POPUP makes `id` the handle of the submenu (which is also
 * the item's id); a bitmap or owner-draw item carries `item` as its bitmap or item data. */
static void flags_to_mii(MII_ *mii, UINT flags, ULONG_PTR id, const void *item, BOOL ansi_text, WCHAR **converted) {
  for (size_t_ i = 0; i < sizeof *mii; ++i) ((BYTE *)mii)[i] = 0;
  mii->cbSize = sizeof *mii;
  mii->fMask = MIIM_FTYPE_ | MIIM_STATE_ | MIIM_ID_ | MIIM_SUBMENU_;
  mii->fType = flags & (MFT_BITMAP_ | 0x20 | 0x40 | MFT_OWNERDRAW_ | MFT_SEPARATOR_ | 0x4000 | 0x2000);
  mii->fState = flags & (0x3 | 0x8 | 0x80 | 0x1000);
  mii->wID = (UINT)id;
  if (flags & MF_POPUP_) mii->hSubMenu = (HANDLE)id;
  if ((flags & MFT_SEPARATOR_) || (!(flags & (MFT_BITMAP_ | MFT_OWNERDRAW_)) && !item)) mii->fState |= 0x3;      /* a separator is disabled and grayed */
  if (flags & MFT_BITMAP_) { mii->fMask |= MIIM_BITMAP_; mii->hbmpItem = (HANDLE)item; }
  else if (flags & MFT_OWNERDRAW_) { mii->fMask |= MIIM_DATA_; mii->dwItemData = (ULONG_PTR)item; }
  else if (!(flags & MFT_SEPARATOR_) && !item) {      /* no string: a separator */
    mii->fType |= MFT_SEPARATOR_;
  } else if (!(flags & MFT_SEPARATOR_)) {
    mii->fMask |= MIIM_STRING_;
    if (ansi_text && item) { *converted = wide_of((const char *)item); mii->dwTypeData = *converted; } else mii->dwTypeData = (WCHAR *)item;
  }
}
static BOOL classic_insert(HANDLE menu, UINT pos, UINT flags, ULONG_PTR id, const void *item, BOOL ansi, BOOL modify) {
  MII_ mii;
  WCHAR *converted = NULL;
  flags_to_mii(&mii, flags, id, item, ansi, &converted);
  const BOOL ok = (NtUserThunkedMenuItemInfo((ULONG_PTR)menu, pos, (flags & MF_BYPOSITION_) | 0x40000000u /* the classic call */, modify ? MENU_SET_ITEM_INFO : MENU_INSERT_ITEM, (ULONG_PTR)&mii, 0) & 0xFFFFFFFFu) != 0;
  u32_free(converted);
  return ok;
}
EXPORT BOOL WINAPI InsertMenuW(HANDLE menu, UINT pos, UINT flags, ULONG_PTR id, LPCWSTR item) { return classic_insert(menu, pos, flags, id, item, FALSE, FALSE); }
EXPORT BOOL WINAPI InsertMenuA(HANDLE menu, UINT pos, UINT flags, ULONG_PTR id, LPCSTR item) { return classic_insert(menu, pos, flags, id, item, TRUE, FALSE); }
EXPORT BOOL WINAPI AppendMenuW(HANDLE menu, UINT flags, ULONG_PTR id, LPCWSTR item) { return classic_insert(menu, 0xFFFFFFFFu, flags | MF_BYPOSITION_, id, item, FALSE, FALSE); }
EXPORT BOOL WINAPI AppendMenuA(HANDLE menu, UINT flags, ULONG_PTR id, LPCSTR item) { return classic_insert(menu, 0xFFFFFFFFu, flags | MF_BYPOSITION_, id, item, TRUE, FALSE); }
EXPORT BOOL WINAPI ModifyMenuW(HANDLE menu, UINT pos, UINT flags, ULONG_PTR id, LPCWSTR item) { return classic_insert(menu, pos, flags, id, item, FALSE, TRUE); }
EXPORT BOOL WINAPI ModifyMenuA(HANDLE menu, UINT pos, UINT flags, ULONG_PTR id, LPCSTR item) { return classic_insert(menu, pos, flags, id, item, TRUE, TRUE); }
EXPORT BOOL WINAPI ChangeMenuW(HANDLE menu, UINT cmd, LPCWSTR item, UINT id, UINT flags) {
  if (flags & 0x100 /* MF_REMOVE */) return (NtUserRemoveMenu((ULONG_PTR)menu, cmd, flags & ~0x1000u) & 0xFFFFFFFFu) != 0;
  if (flags & 0x80 /* MF_CHANGE */) return classic_insert(menu, cmd, flags & ~0x80u, id, item, FALSE, TRUE);
  if (flags & 0x1000 /* MF_APPEND */) return classic_insert(menu, 0xFFFFFFFFu, (flags & ~0x1000u) | MF_BYPOSITION_, id, item, FALSE, FALSE);
  if (flags & 0x2000 /* MF_DELETE */) return (NtUserDeleteMenu((ULONG_PTR)menu, cmd, flags & ~0x2000u) & 0xFFFFFFFFu) != 0;
  return classic_insert(menu, cmd, flags, id, item, FALSE, FALSE);
}
EXPORT BOOL WINAPI ChangeMenuA(HANDLE menu, UINT cmd, LPCSTR item, UINT id, UINT flags) {
  if (flags & 0x80 /* MF_CHANGE */) return classic_insert(menu, cmd, flags & ~0x80u, id, item, TRUE, TRUE);
  if (flags & 0x1000 /* MF_APPEND */) return classic_insert(menu, 0xFFFFFFFFu, (flags & ~0x1000u) | MF_BYPOSITION_, id, item, TRUE, FALSE);
  return ChangeMenuW(menu, cmd, (LPCWSTR)item, id, flags);
}
EXPORT BOOL WINAPI RemoveMenu(HANDLE menu, UINT pos, UINT flags) { return (NtUserRemoveMenu((ULONG_PTR)menu, pos, flags) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI DeleteMenu(HANDLE menu, UINT pos, UINT flags) { return (NtUserDeleteMenu((ULONG_PTR)menu, pos, flags) & 0xFFFFFFFFu) != 0; }
EXPORT DWORD WINAPI CheckMenuItem(HANDLE menu, UINT pos, UINT flags) { return (DWORD)NtUserCheckMenuItem((ULONG_PTR)menu, pos, flags); }
EXPORT BOOL WINAPI EnableMenuItem(HANDLE menu, UINT pos, UINT flags) { return (DWORD)NtUserEnableMenuItem((ULONG_PTR)menu, pos, flags); }
EXPORT BOOL WINAPI CheckMenuRadioItem(HANDLE menu, UINT first, UINT last, UINT check, UINT flags) {
  return (NtUserThunkedMenuItemInfo((ULONG_PTR)menu, first, last, MENU_CHECK_RADIO, check, (flags & MF_BYPOSITION_) ? 1 : 0) & 0xFFFFFFFFu) != 0;
}
EXPORT BOOL WINAPI HiliteMenuItem(HANDLE hwnd, HANDLE menu, UINT pos, UINT flags) { return (NtUserHiliteMenuItem((ULONG_PTR)hwnd, (ULONG_PTR)menu, pos, flags) & 0xFFFFFFFFu) != 0; }
EXPORT UINT WINAPI GetMenuState(HANDLE menu, UINT pos, UINT flags) { return (UINT)NtUserThunkedMenuItemInfo((ULONG_PTR)menu, pos, flags, MENU_GET_STATE, 0, 0); }
EXPORT UINT WINAPI GetMenuItemID(HANDLE menu, int pos) { return (UINT)NtUserThunkedMenuItemInfo((ULONG_PTR)menu, (UINT)pos, 0, MENU_GET_ID, 0, 0); }
EXPORT HANDLE WINAPI GetSubMenu(HANDLE menu, int pos) { return (HANDLE)(NtUserThunkedMenuItemInfo((ULONG_PTR)menu, (UINT)pos, 0, MENU_GET_SUB, 0, 0) & 0xFFFFFFFFu); }
EXPORT UINT WINAPI GetMenuDefaultItem(HANDLE menu, UINT by_position, UINT flags) { return (UINT)NtUserThunkedMenuItemInfo((ULONG_PTR)menu, by_position, flags, MENU_GET_DEFAULT, 0, 0); }
EXPORT BOOL WINAPI SetMenuDefaultItem(HANDLE menu, UINT item, UINT by_position) { return (NtUserSetMenuDefaultItem((ULONG_PTR)menu, item, by_position) & 0xFFFFFFFFu) != 0; }
static int menu_string(HANDLE menu, UINT pos, void *buffer, int max, UINT flags, BOOL ansi) {
  MII_ mii = {sizeof mii, MIIM_STRING_ | MIIM_FTYPE_};
  if (buffer && max > 0) { mii.dwTypeData = buffer; mii.cch = (UINT)max; }
  if (!(NtUserThunkedMenuItemInfo((ULONG_PTR)menu, pos, flags & MF_BYPOSITION_, ansi ? MENU_GET_INFO_A : MENU_GET_INFO_W, (ULONG_PTR)&mii, 0) & 0xFFFFFFFFu)) {
    if (buffer && max > 0) { if (ansi) *(char *)buffer = 0; else *(WCHAR *)buffer = 0; }
    return 0;
  }
  return (int)mii.cch;
}
EXPORT int WINAPI GetMenuStringW(HANDLE menu, UINT pos, LPWSTR buffer, int max, UINT flags) { return menu_string(menu, pos, buffer, max, flags, FALSE); }
EXPORT int WINAPI GetMenuStringA(HANDLE menu, UINT pos, LPSTR buffer, int max, UINT flags) { return menu_string(menu, pos, buffer, max, flags, TRUE); }

EXPORT BOOL WINAPI SetMenuItemBitmaps(HANDLE menu, UINT pos, UINT flags, HANDLE unchecked, HANDLE checked) {
  MII_ mii = {sizeof mii, MIIM_CHECKMARKS_};
  mii.hbmpChecked = checked;
  mii.hbmpUnchecked = unchecked;
  return (NtUserThunkedMenuItemInfo((ULONG_PTR)menu, pos, flags & MF_BYPOSITION_, MENU_SET_ITEM_INFO, (ULONG_PTR)&mii, 0) & 0xFFFFFFFFu) != 0;
}
EXPORT LONG WINAPI GetMenuCheckMarkDimensions(void) { return (LONG)((GetSystemMetrics(71 /* SM_CXMENUCHECK */) & 0xFFFF) | (GetSystemMetrics(72 /* SM_CYMENUCHECK */) << 16)); }

EXPORT BOOL WINAPI SetMenuInfo(HANDLE menu, const MENUINFO_ *info) { return info && (NtUserThunkedMenuInfo((ULONG_PTR)menu, (ULONG_PTR)info) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI GetMenuInfo(HANDLE menu, MENUINFO_ *info) { return info && (NtUserCallTwoParam((ULONG_PTR)menu, (ULONG_PTR)info, 1 /* GetMenuInfo */) & 0xFFFFFFFFu) != 0; }
EXPORT BOOL WINAPI SetMenuContextHelpId(HANDLE menu, DWORD id) { return (NtUserSetMenuContextHelpId((ULONG_PTR)menu, id) & 0xFFFFFFFFu) != 0; }
EXPORT DWORD WINAPI GetMenuContextHelpId(HANDLE menu) {
  MENUINFO_ info = {sizeof info, MIM_HELPID_};
  return GetMenuInfo(menu, &info) ? info.dwContextHelpID : 0;
}

/* ---- the menu of a window ---------------------------------------------------------------------------------------------------------------------------------------- */
#define WS_SYSMENU_ 0x00080000
EXPORT HANDLE WINAPI GetMenu(HANDLE hwnd) {
  if (!IsWindow(hwnd)) return NULL;
  if (GetWindowLongW(hwnd, -16 /* GWL_STYLE */) & 0x40000000 /* WS_CHILD */) return NULL;
  return (HANDLE)GetWindowLongPtrW(hwnd, -12 /* GWLP_ID */);
}
EXPORT BOOL WINAPI SetMenu(HANDLE hwnd, HANDLE menu) {
  if (!(NtUserSetMenu((ULONG_PTR)hwnd, (ULONG_PTR)menu) & 0xFFFFFFFFu)) return FALSE;
  /* the client area changes with the menu bar */
  SetWindowPos(hwnd, NULL, 0, 0, 0, 0, 0x2 | 0x1 | 0x4 | 0x10 | 0x20 /* NOMOVE | NOSIZE | NOZORDER | NOACTIVATE | FRAMECHANGED */);
  return TRUE;
}
EXPORT HANDLE WINAPI GetSystemMenu(HANDLE hwnd, BOOL revert) {
  if (!IsWindow(hwnd)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE_); return NULL; }
  if (!(GetWindowLongW(hwnd, -16) & WS_SYSMENU_)) return NULL;
  return (HANDLE)(NtUserGetSystemMenu((ULONG_PTR)hwnd, revert) & 0xFFFFFFFFu);
}
EXPORT BOOL WINAPI SetSystemMenu(HANDLE hwnd, HANDLE menu) { return (NtUserSetSystemMenu((ULONG_PTR)hwnd, (ULONG_PTR)menu) & 0xFFFFFFFFu) != 0; }

/* ---- menus from resources --------------------------------------------------------------------------------------------------------------------------------------------- */
static const WORD *menu_parse_old(const WORD *p, HANDLE menu) {
  UINT flags;
  do {
    flags = *p++;
    UINT id = 0;
    if (!(flags & MF_POPUP_)) id = *p++;
    const WCHAR *text = (const WCHAR *)p;
    p += ws_len(text) + 1;
    const UINT item_flags = flags & ~(UINT)MF_END_;
    if (flags & MF_POPUP_) {
      HANDLE sub = CreatePopupMenu();
      if (!sub) return NULL;
      if (!(p = menu_parse_old(p, sub))) return NULL;
      AppendMenuW(menu, item_flags, (ULONG_PTR)sub, text);
    } else if (item_flags == 0 && id == 0 && !*text) {
      AppendMenuW(menu, MF_SEPARATOR_, 0, NULL);
    } else {
      AppendMenuW(menu, item_flags, id, *text ? text : NULL);
    }
  } while (!(flags & MF_END_));
  return p;
}
static const BYTE *menu_parse_ex(const BYTE *p, HANDLE menu) {
  UINT resinfo;
  do {
    MII_ mii = {sizeof mii, MIIM_STATE_ | MIIM_ID_ | MIIM_FTYPE_ | MIIM_STRING_};
    mii.fType = *(const DWORD *)p; p += 4;
    mii.fState = *(const DWORD *)p; p += 4;
    mii.wID = *(const DWORD *)p; p += 4;
    resinfo = *(const WORD *)p; p += 2;
    const WCHAR *text = (const WCHAR *)p;
    mii.dwTypeData = (WCHAR *)text;
    p += (ws_len(text) + 1) * 2;
    p = (const BYTE *)(((ULONG_PTR)p + 3) & ~(ULONG_PTR)3);
    if (resinfo & 1) {      /* a popup: the help id, then its items */
      const DWORD help = *(const DWORD *)p; p += 4;
      HANDLE sub = CreatePopupMenu();
      if (!sub) return NULL;
      if (!(p = menu_parse_ex(p, sub))) return NULL;
      SetMenuContextHelpId(sub, help);
      mii.fMask |= MIIM_SUBMENU_;
      mii.hSubMenu = sub;
    } else if (!*text && !(mii.fType & MFT_SEPARATOR_)) {
      mii.fType |= MFT_SEPARATOR_;
    }
    if (mii.fType & MFT_SEPARATOR_) mii.fMask &= ~(UINT)MIIM_STRING_;
    InsertMenuItemW(menu, 0xFFFFFFFFu, TRUE, &mii);
  } while (!(resinfo & MF_END_));
  return p;
}
EXPORT HANDLE WINAPI LoadMenuIndirectW(const void *template_) {
  if (!template_) return NULL;
  const WORD *p = (const WORD *)template_;
  const WORD version = *p++;
  if (version > 1) { SetLastError(ERROR_INVALID_PARAMETER); return NULL; }
  const WORD offset = *p++;
  const BYTE *items = (const BYTE *)p + offset;
  HANDLE menu = CreateMenu();
  if (!menu) return NULL;
  if (!(version == 0 ? (const void *)menu_parse_old((const WORD *)items, menu) : (const void *)menu_parse_ex(items, menu))) { DestroyMenu(menu); return NULL; }
  return menu;
}
EXPORT HANDLE WINAPI LoadMenuIndirectA(const void *template_) { return LoadMenuIndirectW(template_); }
EXPORT HANDLE WINAPI LoadMenuW(HANDLE module, LPCWSTR name) {
  HANDLE res = FindResourceW(module ? module : GetModuleHandleW(NULL), name, (LPCWSTR)(ULONG_PTR)RT_MENU_);
  if (!res) return NULL;
  HANDLE loaded = LoadResource(module ? module : GetModuleHandleW(NULL), res);
  return loaded ? LoadMenuIndirectW(LockResource(loaded)) : NULL;
}
EXPORT HANDLE WINAPI LoadMenuA(HANDLE module, LPCSTR name) {
  WCHAR *w = wide_of(name);
  HANDLE menu = LoadMenuW(module, w);
  wide_free(w, name);
  return menu;
}
