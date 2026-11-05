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

// Menus: HMENU objects with their items (strings, ids, states, submenus, check-mark bitmaps, owner-draw data), the menu of a window (SetMenu) and its system menu. Layout and
// the modal tracking loop (TrackPopupMenuEx, GetMenuItemRect, MenuItemFromPoint, GetMenuBarInfo) need the font metrics of the GDI module and are not part of this module.
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kMiimState = 1, kMiimId = 2, kMiimSubmenu = 4, kMiimCheckmarks = 8, kMiimType = 0x10, kMiimData = 0x20, kMiimString = 0x40, kMiimBitmap = 0x80, kMiimFtype = 0x100;
constexpr uint32_t kMftString = 0, kMftBitmap = 4, kMftOwnerDraw = 0x100, kMftSeparator = 0x800;
constexpr uint32_t kMftValid = 0x4 | 0x20 | 0x40 | 0x100 | 0x200 | 0x800 | 0x2000 | 0x4000;
constexpr uint32_t kMfsValid = 0x1 | 0x2 | 0x8 | 0x80 | 0x1000, kMfsGrayedBits = 3, kMfsChecked = 8, kMfsHilite = 0x80, kMfsDefault = 0x1000, kMftRadioCheck = 0x200;
constexpr uint32_t kMimMaxHeight = 1, kMimBackground = 2, kMimHelpId = 4, kMimMenuData = 8, kMimStyle = 0x10, kMimApplyToSubmenus = 0x80000000u;
constexpr uint32_t kMnsValid = 0x80000000u | 0x40000000u | 0x20000000u | 0x10000000u | 0x08000000u | 0x04000000u;
constexpr uint32_t kMiiSize80 = 80, kMiiSize72 = 72, kMenuInfoSize = 40;
constexpr uint32_t kMaxItemText = 0x10000, kMaxItems = 0xFFFF, kMaxDepth = 32;
constexpr uint32_t kNotFound = 0xFFFFFFFFu;
constexpr uint32_t kMftRightJustify = 0x4000;
constexpr uint32_t kClassicCall = 0x40000000u;      // in the flags of ThunkedMenuItemInfo: the call comes from InsertMenu / AppendMenu / ModifyMenu (MF_BITMAP is allowed there)

struct MenuItem {
  uint32_t id = 0, type = 0, state = 0, submenu = 0;
  uint64_t bmp_checked = 0, bmp_unchecked = 0, item_data = 0, bmp_item = 0;
  WideString text;
  explicit MenuItem(allocators::IAllocator& a) : text(a) {}
};

struct Menu {
  Hive<MenuItem> store;
  List<MenuItem*> items;
  uint64_t pid;
  bool popup;
  uint32_t style = 0, max_height = 0, help_id = 0;
  uint64_t background = 0, menu_data = 0;
  bool right_to_left = false;
  Menu(allocators::IAllocator& a, uint64_t owner, bool is_popup) : store(a), items(a), pid(owner), popup(is_popup) {}
};

struct WindowMenu { uint32_t hwnd, menu; };

struct MenuState {
  Registry<Menu> menus;
  List<WindowMenu> bars, system_menus;
  explicit MenuState(allocators::IAllocator& a) : menus(a, 0x0001), bars(a), system_menus(a) {}
};
UniquePointer<MenuState> g_menus;
MenuState* st() { return lazy_state(g_menus); }

struct Mii {
  uint32_t size = 0, mask = 0, type = 0, state = 0, id = 0, cch = 0;
  uint64_t submenu = 0, bmp_checked = 0, bmp_unchecked = 0, data = 0, type_data = 0, bmp_item = 0;
};

bool read_mii(Call& call, uint64_t address, Mii* out) {
  uint32_t size = 0;
  if (!call.read_u32(address, &size)) { call.set_last_error(kErrorNoAccess); return false; }
  if (size != kMiiSize80 && size != kMiiSize72) { call.set_last_error(kErrorInvalidParameter); return false; }
  uint8_t raw[kMiiSize80] = {};
  if (!call.read(address, ByteView(raw, size))) { call.set_last_error(kErrorNoAccess); return false; }
  auto u32 = [&](uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
  auto u64 = [&](uint32_t o) { return uint64_t(u32(o)) | uint64_t(u32(o + 4)) << 32; };
  out->size = size;
  out->mask = u32(4); out->type = u32(8); out->state = u32(12); out->id = u32(16);
  out->submenu = u64(24); out->bmp_checked = u64(32); out->bmp_unchecked = u64(40); out->data = u64(48); out->type_data = u64(56); out->cch = u32(64);
  out->bmp_item = size == kMiiSize80 ? u64(72) : 0;
  return true;
}

bool write8(Call& c, uint64_t address, uint8_t value) { return c.write(address, ConstByteView(&value, 1)); }

bool is_string_item(const MenuItem& item) { return (item.type & (kMftBitmap | kMftOwnerDraw | kMftSeparator)) == 0; }

// Is `target` the menu `from` or one of the menus below it?
bool menu_contains(MenuState& s, uint32_t from, uint32_t target, uint32_t depth) {
  if (from == target) return true;
  Menu* menu = s.menus.find(from);
  if (!menu || depth >= kMaxDepth) return false;
  for (MenuItem* item : menu->items.view()) if (item->submenu != 0 && menu_contains(s, item->submenu, target, depth + 1)) return true;
  return false;
}

// Finds an item the way Windows does: by position in `*handle`, or by command id in `*handle` and then (recursively) in its submenus. Sets `*handle` to the menu that holds the
// item and `*index` to its position.
MenuItem* find_item(MenuState& s, uint32_t* handle, uint32_t id_or_pos, uint32_t flags, uint64_t* index, uint32_t depth = 0) {
  Menu* menu = s.menus.find(*handle);
  if (!menu) return nullptr;
  if (flags & kMenuByPosition) {
    if (id_or_pos >= menu->items.count()) return nullptr;
    *index = id_or_pos;
    return menu->items[id_or_pos];
  }
  if (depth >= kMaxDepth) return nullptr;
  for (uint64_t i = 0; i < menu->items.count(); ++i) {
    MenuItem* item = menu->items[i];
    if (item->submenu != 0 && s.menus.find(item->submenu)) {
      uint32_t sub = item->submenu;
      uint64_t sub_index = 0;
      if (MenuItem* found = find_item(s, &sub, id_or_pos, flags, &sub_index, depth + 1)) { *handle = sub; *index = sub_index; return found; }
      if (item->id == id_or_pos) { *index = i; return item; }
    } else if (item->id == id_or_pos) {
      *index = i;
      return item;
    }
  }
  return nullptr;
}

void drop_window_references(MenuState& s, uint32_t menu) {
  for (uint64_t i = 0; i < s.bars.count();) { if (s.bars[i].menu == menu) s.bars.remove_at(i); else ++i; }
  for (uint64_t i = 0; i < s.system_menus.count();) { if (s.system_menus[i].menu == menu) s.system_menus.remove_at(i); else ++i; }
}

void destroy_menu_tree(MenuState& s, uint32_t handle, uint32_t depth) {
  Menu* menu = s.menus.find(handle);
  if (!menu) return;
  List<uint32_t> subs(*system_allocator());
  for (MenuItem* item : menu->items.view()) if (item->submenu != 0) subs.push_back(item->submenu);
  drop_window_references(s, handle);
  s.menus.destroy(handle);      // (first, so that a cycle ends at the already destroyed menu)
  if (depth >= kMaxDepth) return;
  for (uint32_t sub : subs.view()) destroy_menu_tree(s, sub, depth + 1);
}

// Applies the fields of `mii` selected by its mask to `item`. `text`: the string for MIIM_STRING / MIIM_TYPE (null = none given).
bool apply_mii(MenuState& s, Call& call, MenuItem& item, uint32_t menu_handle, const Mii& mii, const WideString* text) {
  if (mii.mask & kMiimSubmenu) {
    if (mii.submenu != 0) {
      const uint32_t sub = uint32_t(mii.submenu);
      if (!s.menus.find(sub) || (mii.submenu >> 32) != 0) { call.set_last_error(kErrorInvalidMenuHandle); return false; }
      if (menu_contains(s, sub, menu_handle, 0)) { call.set_last_error(kErrorInvalidParameter); return false; }
    }
  }
  if (mii.mask & (kMiimType | kMiimFtype)) {
    item.type = mii.type & kMftValid;
    if (mii.type & kMftBitmap) item.type |= kMftBitmap;
  }
  if (mii.mask & kMiimState) item.state = mii.state & kMfsValid;
  if (mii.mask & kMiimId) item.id = mii.id;
  if (mii.mask & kMiimSubmenu) item.submenu = uint32_t(mii.submenu);
  if (mii.mask & kMiimCheckmarks) { item.bmp_checked = mii.bmp_checked; item.bmp_unchecked = mii.bmp_unchecked; }
  if (mii.mask & kMiimData) item.item_data = mii.data;
  if (mii.mask & kMiimBitmap) item.bmp_item = mii.bmp_item;
  if ((mii.mask & kMiimType) && (item.type & kMftBitmap)) item.bmp_item = mii.type_data;
  if ((mii.mask & kMiimType) && (item.type & kMftOwnerDraw)) item.item_data = mii.type_data;
  if (text && ((mii.mask & kMiimString) || ((mii.mask & kMiimType) && is_string_item(item)))) {
    item.text.clear();
    item.text.append(text->view());
  }
  // A string item given no string (a null pointer) is a separator, and a separator is disabled unless its state was given.
  if (((mii.mask & kMiimString) || ((mii.mask & kMiimType) && is_string_item(item))) && mii.type_data == 0 && is_string_item(item)) item.type |= kMftSeparator;
  if ((item.type & kMftSeparator) && !(mii.mask & kMiimState)) item.state |= kMfsGrayedBits;
  return true;
}

// The text of an insert / set call: the UNICODE_STRING if given, else the NUL-terminated dwTypeData.
bool read_item_text(Call& call, const Mii& mii, uint64_t str, bool* have, WideString* out) {
  *have = false;
  if (str != 0) {
    if (!call.read_unicode_string(str, out)) { call.set_last_error(kErrorNoAccess); return false; }
    *have = true;
    return true;
  }
  const bool wants = (mii.mask & kMiimString) || ((mii.mask & kMiimType) && (mii.type & (kMftBitmap | kMftOwnerDraw | kMftSeparator)) == 0);
  if (!wants) return true;
  *have = true;
  if (mii.type_data == 0) return true;
  if (!call.read_wide_z(mii.type_data, kMaxItemText, out)) { call.set_last_error(kErrorNoAccess); return false; }
  return true;
}

uint32_t new_menu(Call& call, bool popup) {
  MenuState* s = st();
  if (!s) return 0;
  uint32_t handle = 0;
  if (!s->menus.create(&handle, call.pid(), popup)) { call.set_last_error(kErrorNoMoreUserHandles); return 0; }
  return handle;
}

bool insert_item(MenuState& s, Menu& menu, uint64_t position, MenuItem** out) {
  if (menu.items.count() >= kMaxItems) return false;
  MenuItem* item = menu.store.emplace(*system_allocator());
  if (!item) return false;
  if (!menu.items.insert(position, item)) { menu.store.erase(item); return false; }
  *out = item;
  return true;
}

void remove_item(Menu& menu, uint64_t index) {
  MenuItem* item = menu.items[index];
  menu.items.remove_at(index);
  menu.store.erase(item);
}

uint64_t menu_state_bits(MenuState& s, const MenuItem& item) {
  if (item.submenu != 0) {
    Menu* sub = s.menus.find(item.submenu);
    const uint32_t count = sub ? uint32_t(sub->items.count()) : 0;
    return uint64_t((count & 0xFF) << 8) | ((item.state | item.type | kMenuPopup) & 0xFF);
  }
  return uint64_t(item.type | item.state);
}

// --- default item
uint32_t default_item_of(MenuState& s, uint32_t menu_handle, bool by_position, uint32_t flags, uint32_t depth) {
  Menu* menu = s.menus.find(menu_handle);
  if (!menu) return kNotFound;
  for (uint64_t i = 0; i < menu->items.count(); ++i) {
    MenuItem* item = menu->items[i];
    if (!(item->state & kMfsDefault)) continue;
    if ((item->state & kMfsGrayedBits) && !(flags & 1)) return kNotFound;
    if (item->submenu != 0 && (flags & 2) && depth < kMaxDepth) {
      const uint32_t inner = default_item_of(s, item->submenu, by_position, flags, depth + 1);
      if (inner != kNotFound) return inner;
    }
    return by_position ? uint32_t(i) : item->id;
  }
  return kNotFound;
}

// --- the services
uint64_t svc_create_menu(Call& c) { return new_menu(c, false); }
uint64_t svc_create_popup_menu(Call& c) { return new_menu(c, true); }

uint64_t svc_destroy_menu(Call& c) {
  MenuState* s = st();
  const uint32_t handle = c.arg32(0);
  if (!s || !s->menus.find(handle)) return c.fail(kErrorInvalidMenuHandle);
  destroy_menu_tree(*s, handle, 0);
  return 1;
}

bool arg_handle_ok(Call& c, uint64_t value) { return (value >> 32) == 0 || (c.set_last_error(kErrorInvalidMenuHandle), false); }

uint64_t remove_or_delete(Call& c, bool destroy_submenu) {
  MenuState* s = st();
  uint32_t handle = c.arg32(0);
  if (!s || !arg_handle_ok(c, c.arg(0)) || !s->menus.find(handle)) return c.fail(kErrorInvalidMenuHandle);
  uint32_t holder = handle;
  uint64_t index = 0;
  MenuItem* item = find_item(*s, &holder, c.arg32(1), c.arg32(2), &index);
  if (!item) return c.fail(kErrorMenuItemNotFound);
  Menu* menu = s->menus.find(holder);
  const uint32_t sub = item->submenu;
  remove_item(*menu, index);
  if (destroy_submenu && sub != 0) destroy_menu_tree(*s, sub, 0);
  return 1;
}
uint64_t svc_remove_menu(Call& c) { return remove_or_delete(c, false); }
uint64_t svc_delete_menu(Call& c) { return remove_or_delete(c, true); }

uint64_t svc_check_menu_item(Call& c) {
  MenuState* s = st();
  uint32_t holder = c.arg32(0);
  uint64_t index = 0;
  MenuItem* item = s && arg_handle_ok(c, c.arg(0)) ? find_item(*s, &holder, c.arg32(1), c.arg32(2), &index) : nullptr;
  if (!item) return 0xFFFFFFFFu;
  const uint32_t previous = item->state & kMfsChecked;
  item->state = (item->state & ~kMfsChecked) | (c.arg32(2) & kMfsChecked);
  return previous;
}

uint64_t svc_enable_menu_item(Call& c) {
  MenuState* s = st();
  uint32_t holder = c.arg32(0);
  uint64_t index = 0;
  MenuItem* item = s && arg_handle_ok(c, c.arg(0)) ? find_item(*s, &holder, c.arg32(1), c.arg32(2), &index) : nullptr;
  if (!item) return 0xFFFFFFFFu;
  const uint32_t previous = item->state & kMfsGrayedBits;
  item->state = (item->state & ~kMfsGrayedBits) | (c.arg32(2) & kMfsGrayedBits);
  return previous;
}

uint64_t svc_hilite_menu_item(Call& c) {
  MenuState* s = st();
  if (!s) return 0;
  if (!user::is_window(c.arg32(0))) return c.fail(kErrorInvalidWindowHandle);
  uint32_t holder = c.arg32(1);
  uint64_t index = 0;
  MenuItem* item = arg_handle_ok(c, c.arg(1)) ? find_item(*s, &holder, c.arg32(2), c.arg32(3), &index) : nullptr;
  if (!item) return 0;
  Menu* menu = s->menus.find(holder);
  for (MenuItem* other : menu->items.view()) other->state &= ~kMfsHilite;
  if (c.arg32(3) & 0x80) item->state |= kMfsHilite;
  return 1;
}

uint64_t svc_set_menu_default_item(Call& c) {
  MenuState* s = st();
  Menu* menu = s && arg_handle_ok(c, c.arg(0)) ? s->menus.find(c.arg32(0)) : nullptr;
  if (!menu) return c.fail(kErrorInvalidMenuHandle);
  MenuItem* target = nullptr;
  const uint32_t item = c.arg32(1);
  if (item != kNotFound) {
    uint32_t holder = c.arg32(0);
    uint64_t index = 0;
    // The default item is looked up in this menu only (recursion into submenus is for commands: a default belongs to the menu it is in).
    target = find_item(*s, &holder, item, c.arg32(2) ? kMenuByPosition : 0, &index);
    if (!target || holder != c.arg32(0)) return c.fail(kErrorMenuItemNotFound);
  }
  for (MenuItem* other : menu->items.view()) other->state &= ~kMfsDefault;
  if (target) target->state |= kMfsDefault;
  return 1;
}

uint64_t svc_set_menu_context_help_id(Call& c) {
  MenuState* s = st();
  Menu* menu = s && arg_handle_ok(c, c.arg(0)) ? s->menus.find(c.arg32(0)) : nullptr;
  if (!menu) return c.fail(kErrorInvalidMenuHandle);
  menu->help_id = c.arg32(1);
  return 1;
}

uint64_t svc_set_menu_flag_rtol(Call& c) {
  MenuState* s = st();
  Menu* menu = s && arg_handle_ok(c, c.arg(0)) ? s->menus.find(c.arg32(0)) : nullptr;
  if (!menu) return c.fail(kErrorInvalidMenuHandle);
  menu->right_to_left = true;
  return 1;
}

// --- MENUINFO
uint64_t svc_thunked_menu_info(Call& c) {
  MenuState* s = st();
  Menu* menu = s && arg_handle_ok(c, c.arg(0)) ? s->menus.find(c.arg32(0)) : nullptr;
  if (!menu) return c.fail(kErrorInvalidMenuHandle);
  uint8_t raw[kMenuInfoSize];
  if (!c.read(c.arg(1), ByteView(raw, kMenuInfoSize))) return c.fail(kErrorNoAccess);
  auto u32 = [&](uint32_t o) { return uint32_t(raw[o]) | uint32_t(raw[o + 1]) << 8 | uint32_t(raw[o + 2]) << 16 | uint32_t(raw[o + 3]) << 24; };
  if (u32(0) != kMenuInfoSize) return c.fail(kErrorInvalidParameter);
  const uint32_t mask = u32(4), style = u32(8), max_height = u32(12), help = u32(24);
  const uint64_t brush = uint64_t(u32(16)) | uint64_t(u32(20)) << 32, data = uint64_t(u32(32)) | uint64_t(u32(36)) << 32;
  if ((mask & ~(kMimMaxHeight | kMimBackground | kMimHelpId | kMimMenuData | kMimStyle | kMimApplyToSubmenus)) != 0) return c.fail(kErrorInvalidParameter);
  if ((mask & kMimStyle) && (style & ~kMnsValid) != 0) return c.fail(kErrorInvalidParameter);
  // The menu and, with MIM_APPLYTOSUBMENUS, every menu below it.
  List<uint32_t> todo(*system_allocator());
  todo.push_back(c.arg32(0));
  for (uint64_t i = 0; i < todo.count() && i < 4096; ++i) {
    Menu* m = s->menus.find(todo[i]);
    if (!m) continue;
    if (mask & kMimMaxHeight) m->max_height = max_height;
    if (mask & kMimBackground) m->background = brush;
    if (mask & kMimHelpId) m->help_id = help;
    if (mask & kMimMenuData) m->menu_data = data;
    if (mask & kMimStyle) m->style = style;
    if (mask & kMimApplyToSubmenus) for (MenuItem* item : m->items.view()) if (item->submenu != 0) todo.push_back(item->submenu);
  }
  return 1;
}

// --- NtUserThunkedMenuItemInfo
enum Method : uint32_t { kSetMenuItemInfo, kInsertMenuItem, kCheckMenuRadioItem, kGetMenuDefaultItem, kGetMenuItemID, kGetMenuItemInfoA, kGetMenuItemInfoW, kGetMenuState, kGetSubMenu };

uint64_t set_or_insert(Call& c, MenuState& s, uint32_t handle, uint32_t pos, uint32_t flags, bool insert, uint64_t info, uint64_t str) {
  Mii mii;
  if (!read_mii(c, info, &mii)) return 0;
  // MIIM_TYPE is the old way to give the type and string: it cannot be combined with the new ones, and a bitmap type is not set with MIIM_FTYPE or next to MIIM_BITMAP.
  if (((mii.mask & kMiimType) && (mii.mask & (kMiimString | kMiimFtype | kMiimBitmap))) || (!(flags & kClassicCall) && (mii.mask & kMiimFtype) && (mii.type & kMftBitmap))) return c.fail(kErrorInvalidParameter);
  WideString text(*c.allocator());
  bool have_text = false;
  if (!read_item_text(c, mii, str, &have_text, &text)) return 0;
  uint32_t holder = handle;
  uint64_t index = 0;
  if (!insert) {
    MenuItem* item = find_item(s, &holder, pos, flags, &index);
    if (!item) return c.fail(kErrorMenuItemNotFound);
    return apply_mii(s, c, *item, holder, mii, have_text ? &text : nullptr) ? 1 : 0;
  }
  Menu* menu = s.menus.find(handle);
  uint64_t where = menu->items.count();
  if (flags & kMenuByPosition) {
    if (pos < menu->items.count()) where = pos;
  } else if (find_item(s, &holder, pos, flags, &index) && holder == handle) {
    where = index;      // before the item with that id (an item in a submenu is not a position here: the new item is appended)
  }
  // The system buttons of an MDI child (right-justified items with a magic bitmap) stay at the right end of a menu bar: an item appended goes before them.
  if (where == menu->items.count()) while (where > 0 && (menu->items[where - 1]->type & kMftRightJustify) && menu->items[where - 1]->bmp_item >= 1 && menu->items[where - 1]->bmp_item <= 6) --where;
  MenuItem* item = nullptr;
  if (!insert_item(s, *menu, where, &item)) return c.fail(kErrorNotEnoughMemory);
  if (!(mii.mask & (kMiimType | kMiimFtype | kMiimString))) item->type = kMftString;
  if (!apply_mii(s, c, *item, handle, mii, have_text ? &text : nullptr)) {
    uint64_t at = 0;
    for (uint64_t i = 0; i < menu->items.count(); ++i) if (menu->items[i] == item) at = i;
    remove_item(*menu, at);
    return 0;
  }
  return 1;
}

uint64_t get_item_info(Call& c, MenuState& s, MenuItem& item, uint64_t address, bool ansi) {
  Mii mii;
  if (!read_mii(c, address, &mii)) return 0;
  if ((mii.mask & kMiimType) && (mii.mask & (kMiimString | kMiimFtype | kMiimBitmap))) return c.fail(kErrorInvalidParameter);
  bool ok = true;
  if (mii.mask & (kMiimType | kMiimFtype)) ok = ok && c.write_u32(address + 8, item.type);
  if (mii.mask & kMiimState) ok = ok && c.write_u32(address + 12, item.state);
  if (mii.mask & kMiimId) ok = ok && c.write_u32(address + 16, item.id);
  if (mii.mask & kMiimSubmenu) ok = ok && c.write_u64(address + 24, item.submenu);
  if (mii.mask & kMiimCheckmarks) ok = ok && c.write_u64(address + 32, item.bmp_checked) && c.write_u64(address + 40, item.bmp_unchecked);
  if (mii.mask & kMiimData) ok = ok && c.write_u64(address + 48, item.item_data);
  if ((mii.mask & kMiimBitmap) && mii.size == kMiiSize80) ok = ok && c.write_u64(address + 72, item.bmp_item);
  if (mii.mask & (kMiimString | kMiimType)) {
    // An owner-draw item may carry a string as well (set with MIIM_STRING); the old MIIM_TYPE request answers the item's data in dwTypeData for the types without one.
    const bool string_item = is_string_item(item), has_text = string_item || item.text.size() != 0;
    const uint32_t length = has_text ? uint32_t(item.text.size()) : 0;
    if ((mii.mask & kMiimType) && !string_item) {
      ok = ok && c.write_u64(address + 56, (item.type & kMftBitmap) ? item.bmp_item : item.item_data) && c.write_u32(address + 64, length);
    } else if (!has_text) {      // an empty string
      ok = ok && c.write_u32(address + 64, 0);
      if (mii.type_data != 0 && mii.cch != 0) ok = ok && (ansi ? write8(c, mii.type_data, 0) : c.write_u16(mii.type_data, 0));
    } else if (mii.type_data == 0) {
      ok = ok && c.write_u32(address + 64, length);      // no buffer: only the length
    } else if (mii.cch != 0) {
      const uint32_t room = mii.cch - 1;
      const uint32_t n = item.text.size() < room ? uint32_t(item.text.size()) : room;
      if (ansi) {
        for (uint32_t i = 0; ok && i < n; ++i) { const char16_t ch = item.text[i]; ok = write8(c, mii.type_data + i, ch < 256 ? uint8_t(ch) : '?'); }
        ok = ok && write8(c, mii.type_data + n, 0);
      } else {
        ok = ok && c.write_wide(mii.type_data, item.text.view().first(n), mii.cch) >= 0;
      }
      ok = ok && c.write_u32(address + 64, n);
    }
  }
  if (!ok) return c.fail(kErrorNoAccess);
  return 1;
}

uint64_t svc_thunked_menu_item_info(Call& c) {
  MenuState* s = st();
  const uint64_t menu_arg = c.arg(0);
  const uint32_t handle = uint32_t(menu_arg), pos = c.arg32(1), flags = c.arg32(2), method = c.arg32(3);
  const uint64_t info = c.arg(4), str = c.arg(5);
  if (!s) return 0;
  Menu* menu = (menu_arg >> 32) == 0 ? s->menus.find(handle) : nullptr;
  if (!menu) return c.fail(kErrorInvalidMenuHandle, method == kGetMenuDefaultItem || method == kGetMenuItemID || method == kGetMenuState ? kNotFound : 0);
  uint32_t holder = handle;
  uint64_t index = 0;
  switch (method) {
    case kSetMenuItemInfo: return set_or_insert(c, *s, handle, pos, flags, false, info, str);
    case kInsertMenuItem: return set_or_insert(c, *s, handle, pos, flags, true, info, str);
    case kCheckMenuRadioItem: {      // (menu, first, last, method, check, by_position)
      const uint32_t first = pos, last = flags, check = uint32_t(info), mode = str != 0 ? kMenuByPosition : 0;
      uint32_t h1 = handle, h2 = handle, h3 = handle;
      uint64_t i1 = 0, i2 = 0, i3 = 0;
      MenuItem* a = find_item(*s, &h1, first, mode, &i1);
      MenuItem* b = find_item(*s, &h2, last, mode, &i2);
      if (!a || !b) return c.fail(kErrorMenuItemNotFound);
      Menu* range = s->menus.find(h1);
      MenuItem* checked = find_item(*s, &h3, check, mode, &i3);
      if (checked && h1 != h3) return c.fail(kErrorMenuItemNotFound);      // (the range starts in one menu, which may be a submenu, and the checked item is in it; the end of the range in another menu is the end of this one)
      if (h2 != h1) i2 = range->items.count() - 1;
      bool found = false;
      for (uint64_t i = i1; i <= i2 && i < range->items.count(); ++i) {
        MenuItem* item = range->items[i];
        if (checked && h3 == h1 && item == checked) { item->state |= kMfsChecked; item->type |= kMftRadioCheck; found = true; }
        else item->state &= ~kMfsChecked;
      }
      return found ? 1 : 0;
    }
    case kGetMenuDefaultItem: return default_item_of(*s, handle, pos != 0, flags, 0);
    case kGetMenuItemID: {
      MenuItem* item = find_item(*s, &holder, pos, kMenuByPosition, &index);
      if (!item || item->submenu != 0) return kNotFound;
      return item->id;
    }
    case kGetMenuItemInfoA:
    case kGetMenuItemInfoW: {
      MenuItem* item = find_item(*s, &holder, pos, flags, &index);
      if (!item) return c.fail(kErrorMenuItemNotFound);
      return get_item_info(c, *s, *item, info, method == kGetMenuItemInfoA);
    }
    case kGetMenuState: {
      MenuItem* item = find_item(*s, &holder, pos, flags, &index);
      if (!item) return kNotFound;
      return menu_state_bits(*s, *item);
    }
    case kGetSubMenu: {
      MenuItem* item = find_item(*s, &holder, pos, kMenuByPosition, &index);
      return item ? item->submenu : 0;
    }
    default: return c.fail(kErrorInvalidParameter);
  }
}

// --- the menu of a window
uint64_t svc_set_menu(Call& c) {
  MenuState* s = st();
  const uint32_t hwnd = c.arg32(0), menu = c.arg32(1);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (menu != 0) {
    Menu* m = (c.arg(1) >> 32) == 0 ? s->menus.find(menu) : nullptr;
    if (!m) return c.fail(kErrorInvalidMenuHandle);
  }
  user::set_window_menu(hwnd, menu);
  for (uint64_t i = 0; i < s->bars.count(); ++i) {
    if (s->bars[i].hwnd != hwnd) continue;
    if (menu == 0) s->bars.remove_at(i); else s->bars[i].menu = menu;
    return 1;
  }
  if (menu != 0) s->bars.push_back(WindowMenu{hwnd, menu});
  return 1;
}

uint64_t svc_draw_menu_bar(Call& c) {
  const uint32_t hwnd = c.arg32(0);
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  user::post_message(hwnd, kWmNcPaint, 1, 0);      // the frame (with the menu bar) is repainted
  return 1;
}

void build_system_menu(MenuState& s, uint32_t menu_handle) {
  struct Entry { uint32_t id, state; const char* text; };
  static const Entry entries[] = {{0xF120, 3, "&Restore"}, {0xF010, 0, "&Move"}, {0xF000, 0, "&Size"}, {0xF020, 0, "Mi&nimize"}, {0xF030, 0, "Ma&ximize"}, {0, 0, nullptr},
                                  {0xF060, kMfsDefault, "&Close\tAlt+F4"}};
  Menu* menu = s.menus.find(menu_handle);
  for (const Entry& e : entries) {
    MenuItem* item = nullptr;
    if (!insert_item(s, *menu, menu->items.count(), &item)) return;
    item->id = e.id;
    item->state = e.state;
    if (e.text) item->text.append_ascii(e.text); else item->type = kMftSeparator;
  }
}

uint32_t system_menu_of(MenuState& s, uint32_t hwnd, uint64_t pid) {
  for (const WindowMenu& w : s.system_menus.view()) if (w.hwnd == hwnd) return w.menu;
  uint32_t handle = 0;
  if (!s.menus.create(&handle, pid, true)) return 0;
  build_system_menu(s, handle);
  s.system_menus.push_back(WindowMenu{hwnd, handle});
  return handle;
}

uint64_t svc_get_system_menu(Call& c) {
  MenuState* s = st();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (c.arg_bool(1)) {      // revert to the default menu: the changed copy is destroyed
    for (uint64_t i = 0; i < s->system_menus.count(); ++i) {
      if (s->system_menus[i].hwnd != hwnd) continue;
      const uint32_t menu = s->system_menus[i].menu;
      destroy_menu_tree(*s, menu, 0);
      break;
    }
    return 0;
  }
  return system_menu_of(*s, hwnd, c.pid());
}

uint64_t svc_set_system_menu(Call& c) {
  MenuState* s = st();
  const uint32_t hwnd = c.arg32(0), menu = c.arg32(1);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  if (!s->menus.find(menu) || (c.arg(1) >> 32) != 0) return c.fail(kErrorInvalidMenuHandle);
  for (uint64_t i = 0; i < s->system_menus.count(); ++i) {
    if (s->system_menus[i].hwnd != hwnd) continue;
    const uint32_t old = s->system_menus[i].menu;
    if (old == menu) return 1;
    destroy_menu_tree(*s, old, 0);      // (drops the entry of the window as well)
    break;
  }
  s->system_menus.push_back(WindowMenu{hwnd, menu});
  return 1;
}

}  // namespace

bool is_menu(uint32_t handle) { return g_menus && g_menus->menus.contains(handle); }

int32_t menu_item_count(uint32_t handle) {
  if (!g_menus) return -1;
  Menu* m = g_menus->menus.find(handle);
  return m ? int32_t(m->items.count()) : -1;
}

bool get_menu_info(Call& c, uint32_t handle, uint64_t info) {
  MenuState* s = st();
  Menu* m = s ? s->menus.find(handle) : nullptr;
  if (!m) { c.set_last_error(kErrorInvalidMenuHandle); return false; }
  uint32_t size = 0, mask = 0;
  if (!c.read_u32(info, &size) || !c.read_u32(info + 4, &mask)) { c.set_last_error(kErrorNoAccess); return false; }
  if (size != kMenuInfoSize) { c.set_last_error(kErrorInvalidParameter); return false; }
  bool ok = true;
  if (mask & kMimStyle) ok = ok && c.write_u32(info + 8, m->style);
  if (mask & kMimMaxHeight) ok = ok && c.write_u32(info + 12, m->max_height);
  if (mask & kMimBackground) ok = ok && c.write_u64(info + 16, m->background);
  if (mask & kMimHelpId) ok = ok && c.write_u32(info + 24, m->help_id);
  if (mask & kMimMenuData) ok = ok && c.write_u64(info + 32, m->menu_data);
  if (!ok) c.set_last_error(kErrorNoAccess);
  return ok;
}

bool attach_window_menu(uint32_t hwnd, uint32_t menu) {
  MenuState* s = st();
  if (!s || !s->menus.find(menu)) return false;
  for (WindowMenu& w : s->bars.view()) if (w.hwnd == hwnd) { w.menu = menu; return true; }
  s->bars.push_back(WindowMenu{hwnd, menu});
  return true;
}

uint32_t window_menu(uint32_t hwnd) {
  if (!g_menus) return 0;
  for (const WindowMenu& w : g_menus->bars.view()) if (w.hwnd == hwnd) return w.menu;
  return 0;
}

uint32_t window_system_menu(uint32_t hwnd) {
  MenuState* s = st();
  if (!s || !user::is_window(hwnd)) return 0;
  return system_menu_of(*s, hwnd, user::window_process(hwnd));
}

void menu_window_destroyed(uint32_t hwnd) {
  if (!g_menus) return;
  MenuState& s = *g_menus;
  for (uint64_t i = 0; i < s.bars.count(); ++i) {
    if (s.bars[i].hwnd != hwnd) continue;
    destroy_menu_tree(s, s.bars[i].menu, 0);      // the menu of a window goes with it
    break;
  }
  for (uint64_t i = 0; i < s.system_menus.count(); ++i) {
    if (s.system_menus[i].hwnd != hwnd) continue;
    destroy_menu_tree(s, s.system_menus[i].menu, 0);
    break;
  }
}

void menus_process_exited(uint64_t pid) {
  if (!g_menus) return;
  List<uint32_t> all(*system_allocator());
  g_menus->menus.handles(&all);
  for (uint32_t handle : all.view()) {
    Menu* m = g_menus->menus.find(handle);
    if (m && m->pid == pid) destroy_menu_tree(*g_menus, handle, 0);
  }
}

void install_menus() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserCreateMenu, svc_create_menu);
  register_handler(N::NtUserCreatePopupMenu, svc_create_popup_menu);
  register_handler(N::NtUserDestroyMenu, svc_destroy_menu);
  register_handler(N::NtUserRemoveMenu, svc_remove_menu);
  register_handler(N::NtUserDeleteMenu, svc_delete_menu);
  register_handler(N::NtUserCheckMenuItem, svc_check_menu_item);
  register_handler(N::NtUserEnableMenuItem, svc_enable_menu_item);
  register_handler(N::NtUserHiliteMenuItem, svc_hilite_menu_item);
  register_handler(N::NtUserSetMenuDefaultItem, svc_set_menu_default_item);
  register_handler(N::NtUserSetMenuContextHelpId, svc_set_menu_context_help_id);
  register_handler(N::NtUserSetMenuFlagRtoL, svc_set_menu_flag_rtol);
  register_handler(N::NtUserThunkedMenuInfo, svc_thunked_menu_info);
  register_handler(N::NtUserThunkedMenuItemInfo, svc_thunked_menu_item_info);
  register_handler(N::NtUserSetMenu, svc_set_menu);
  register_handler(N::NtUserDrawMenuBar, svc_draw_menu_bar);
  register_handler(N::NtUserGetSystemMenu, svc_get_system_menu);
  register_handler(N::NtUserSetSystemMenu, svc_set_system_menu);
}

}  // namespace process::win32k::user_misc
