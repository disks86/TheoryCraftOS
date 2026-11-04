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

// Keyboard layouts (the US English layout 00000409 is the one with tables; it is always loaded), the translation services that depend on the layout (MapVirtualKeyEx,
// VkKeyScanEx, ToUnicodeEx, GetKeyNameText) and the input contexts of the input method manager (HIMC objects, their association with windows).
namespace process::win32k::user_misc {

namespace {

constexpr uint32_t kUsLayout = 0x04090409;
constexpr uint32_t kVkBack = 0x08, kVkTab = 0x09, kVkReturn = 0x0D, kVkShift = 0x10, kVkControl = 0x11, kVkMenu = 0x12, kVkCapital = 0x14, kVkEscape = 0x1B, kVkSpace = 0x20,
                   kVkLShift = 0xA0, kVkRShift = 0xA1, kVkLControl = 0xA2, kVkRControl = 0xA3, kVkLMenu = 0xA4, kVkRMenu = 0xA5, kVkSnapshot = 0x2C, kVkPause = 0x13, kVkCancel = 0x03;

// --- the US tables. Scan codes of the main block (set 1) with the virtual key, the characters without and with shift, and the name GetKeyNameText gives.
struct Key { uint8_t scan; uint8_t vk; char normal, shifted; const char* name; };
constexpr Key kKeys[] = {
    {0x01, 0x1B, 0x1B, 0x1B, "Esc"},      {0x02, '1', '1', '!', "1"},         {0x03, '2', '2', '@', "2"},        {0x04, '3', '3', '#', "3"},        {0x05, '4', '4', '$', "4"},
    {0x06, '5', '5', '%', "5"},           {0x07, '6', '6', '^', "6"},         {0x08, '7', '7', '&', "7"},        {0x09, '8', '8', '*', "8"},        {0x0A, '9', '9', '(', "9"},
    {0x0B, '0', '0', ')', "0"},           {0x0C, 0xBD, '-', '_', "-"},        {0x0D, 0xBB, '=', '+', "="},       {0x0E, 0x08, 8, 8, "Backspace"},   {0x0F, 0x09, 9, 9, "Tab"},
    {0x10, 'Q', 'q', 'Q', "Q"},           {0x11, 'W', 'w', 'W', "W"},         {0x12, 'E', 'e', 'E', "E"},        {0x13, 'R', 'r', 'R', "R"},        {0x14, 'T', 't', 'T', "T"},
    {0x15, 'Y', 'y', 'Y', "Y"},           {0x16, 'U', 'u', 'U', "U"},         {0x17, 'I', 'i', 'I', "I"},        {0x18, 'O', 'o', 'O', "O"},        {0x19, 'P', 'p', 'P', "P"},
    {0x1A, 0xDB, '[', '{', "["},          {0x1B, 0xDD, ']', '}', "]"},        {0x1C, 0x0D, 13, 13, "Enter"},     {0x1D, 0x11, 0, 0, "Ctrl"},       {0x1E, 'A', 'a', 'A', "A"},
    {0x1F, 'S', 's', 'S', "S"},           {0x20, 'D', 'd', 'D', "D"},         {0x21, 'F', 'f', 'F', "F"},        {0x22, 'G', 'g', 'G', "G"},        {0x23, 'H', 'h', 'H', "H"},
    {0x24, 'J', 'j', 'J', "J"},           {0x25, 'K', 'k', 'K', "K"},         {0x26, 'L', 'l', 'L', "L"},        {0x27, 0xBA, ';', ':', ";"},       {0x28, 0xDE, '\'', '"', "'"},
    {0x29, 0xC0, '`', '~', "`"},          {0x2A, 0x10, 0, 0, "Shift"},        {0x2B, 0xDC, '\\', '|', "\\"},     {0x2C, 'Z', 'z', 'Z', "Z"},        {0x2D, 'X', 'x', 'X', "X"},
    {0x2E, 'C', 'c', 'C', "C"},           {0x2F, 'V', 'v', 'V', "V"},         {0x30, 'B', 'b', 'B', "B"},        {0x31, 'N', 'n', 'N', "N"},        {0x32, 'M', 'm', 'M', "M"},
    {0x33, 0xBC, ',', '<', ","},          {0x34, 0xBE, '.', '>', "."},        {0x35, 0xBF, '/', '?', "/"},       {0x36, 0x10, 0, 0, "Right Shift"}, {0x37, 0x6A, '*', '*', "Num *"},
    {0x38, 0x12, 0, 0, "Alt"},            {0x39, 0x20, ' ', ' ', "Space"},    {0x3A, 0x14, 0, 0, "Caps Lock"},   {0x3B, 0x70, 0, 0, "F1"},          {0x3C, 0x71, 0, 0, "F2"},
    {0x3D, 0x72, 0, 0, "F3"},             {0x3E, 0x73, 0, 0, "F4"},           {0x3F, 0x74, 0, 0, "F5"},          {0x40, 0x75, 0, 0, "F6"},          {0x41, 0x76, 0, 0, "F7"},
    {0x42, 0x77, 0, 0, "F8"},             {0x43, 0x78, 0, 0, "F9"},           {0x44, 0x79, 0, 0, "F10"},         {0x45, 0x90, 0, 0, "Pause"},       {0x46, 0x91, 0, 0, "Scroll Lock"},
    {0x47, 0x24, 0, 0, "Num 7"},          {0x48, 0x26, 0, 0, "Num 8"},        {0x49, 0x21, 0, 0, "Num 9"},       {0x4A, 0x6D, '-', '-', "Num -"},   {0x4B, 0x25, 0, 0, "Num 4"},
    {0x4C, 0x0C, 0, 0, "Num 5"},          {0x4D, 0x27, 0, 0, "Num 6"},        {0x4E, 0x6B, '+', '+', "Num +"},   {0x4F, 0x23, 0, 0, "Num 1"},       {0x50, 0x28, 0, 0, "Num 2"},
    {0x51, 0x22, 0, 0, "Num 3"},          {0x52, 0x2D, 0, 0, "Num 0"},        {0x53, 0x2E, 0, 0, "Num Del"},     {0x54, 0x2C, 0, 0, "Sys Req"},     {0x56, 0xE2, '\\', '|', "<"},
    {0x57, 0x7A, 0, 0, "F11"},            {0x58, 0x7B, 0, 0, "F12"},
};
// Keys with the E0 prefix: scan code, virtual key, name.
struct ExtKey { uint8_t scan; uint8_t vk; const char* name; };
constexpr ExtKey kExtKeys[] = {
    {0x1C, 0x0D, "Num Enter"}, {0x1D, 0xA3, "Right Ctrl"}, {0x35, 0x6F, "Num /"},     {0x37, 0x2C, "Prnt Scrn"}, {0x38, 0xA5, "Right Alt"}, {0x45, 0x90, "Num Lock"},
    {0x46, 0x03, "Break"},     {0x47, 0x24, "Home"},       {0x48, 0x26, "Up"},        {0x49, 0x21, "Page Up"},   {0x4B, 0x25, "Left"},      {0x4D, 0x27, "Right"},
    {0x4F, 0x23, "End"},       {0x50, 0x28, "Down"},       {0x51, 0x22, "Page Down"}, {0x52, 0x2D, "Insert"},    {0x53, 0x2E, "Delete"},    {0x5B, 0x5B, "Left Windows"},
    {0x5C, 0x5C, "Right Windows"}, {0x5D, 0x5D, "Application"},
};
// The numpad virtual keys and the scan codes that make them.
struct Numpad { uint8_t vk, scan; };
constexpr Numpad kNumpad[] = {{0x60, 0x52}, {0x61, 0x4F}, {0x62, 0x50}, {0x63, 0x51}, {0x64, 0x4B}, {0x65, 0x4C}, {0x66, 0x4D}, {0x67, 0x47}, {0x68, 0x48}, {0x69, 0x49},
                              {0x6A, 0x37}, {0x6B, 0x4E}, {0x6D, 0x4A}, {0x6E, 0x53}};

const Key* key_by_scan(uint8_t scan) {
  for (const Key& k : kKeys) if (k.scan == scan) return &k;
  return nullptr;
}
const ExtKey* ext_by_scan(uint8_t scan) {
  for (const ExtKey& k : kExtKeys) if (k.scan == scan) return &k;
  return nullptr;
}

// MapVirtualKeyEx
uint32_t map_vsc_to_vk(uint32_t code, bool exact) {
  const uint32_t prefix = code >> 8, scan = code & 0xFF;
  if (prefix == 0xE0) {
    const ExtKey* e = ext_by_scan(uint8_t(scan));
    if (!e) return 0;
    if (!exact) return e->vk == kVkRControl ? kVkControl : e->vk == kVkRMenu ? kVkMenu : e->vk;
    return e->vk;
  }
  if (prefix == 0xE1) return scan == 0x1D ? kVkPause : 0;
  if (prefix != 0) return 0;
  const Key* k = key_by_scan(uint8_t(scan));
  if (!k) return 0;
  if (exact) {
    if (scan == 0x1D) return kVkLControl;
    if (scan == 0x2A) return kVkLShift;
    if (scan == 0x36) return kVkRShift;
    if (scan == 0x38) return kVkLMenu;
    if (scan == 0x37) return 0x6A;
    if (scan == 0x45) return 0x90;
  }
  return k->vk;
}

uint32_t map_vk_to_vsc(uint32_t vk, bool exact) {
  switch (vk) {
    case kVkShift: case kVkLShift: return 0x2A;
    case kVkRShift: return 0x36;
    case kVkControl: case kVkLControl: return 0x1D;
    case kVkRControl: return exact ? 0xE01D : 0x1D;
    case kVkMenu: case kVkLMenu: return 0x38;
    case kVkRMenu: return exact ? 0xE038 : 0x38;
    case kVkSnapshot: return 0x54;
    case kVkPause: return exact ? 0xE11D : 0;
    case kVkCancel: return exact ? 0xE046 : 0x46;
    case 0x6F: return exact ? 0xE035 : 0x35;      // VK_DIVIDE
    case 0x90: return 0x45;
    default: break;
  }
  for (const Numpad& n : kNumpad) if (n.vk == vk) return n.scan;
  for (const Key& k : kKeys) if (k.vk == vk && k.scan != 0x36) return k.scan;
  for (const ExtKey& e : kExtKeys) if (e.vk == vk) return e.scan;
  return 0;
}

uint32_t map_vk_to_char(uint32_t vk) {
  if (vk >= 'A' && vk <= 'Z') return vk;
  if (vk >= '0' && vk <= '9') return vk;
  if (vk >= 0x60 && vk <= 0x69) return '0' + (vk - 0x60);
  switch (vk) {
    case kVkSpace: return ' ';
    case kVkBack: return 8;
    case kVkTab: return 9;
    case kVkReturn: return 13;
    case kVkEscape: return 0x1B;
    case 0x6A: return '*';
    case 0x6B: return '+';
    case 0x6D: return '-';
    case 0x6E: return '.';
    case 0x6F: return '/';
    default: break;
  }
  for (const Key& k : kKeys) if (k.vk == vk && k.normal != 0 && k.scan != 0x37 && k.scan != 0x4A && k.scan != 0x4E) return uint8_t(k.normal);
  return 0;
}

// The character a virtual key makes with the given modifiers; false if it makes none.
bool translate_key(uint32_t vk, bool shift, bool ctrl, bool alt, bool caps, char16_t* out) {
  if (ctrl && alt) return false;      // Ctrl+Alt is AltGr, which the US layout does not define
  char16_t c = 0;
  if (vk >= 'A' && vk <= 'Z') {
    const bool upper = shift != caps;
    c = char16_t(upper ? vk : vk + 32);
    if (ctrl) c = char16_t(vk & 0x1F);
  } else if (vk == kVkSpace) {
    c = ' ';
  } else if (vk == kVkReturn) {
    if (ctrl) { if (shift) return false; c = '\n'; } else c = '\r';
  } else if (vk == kVkTab) {
    if (ctrl) return false;
    c = '\t';
  } else if (vk == kVkBack) {
    c = ctrl ? 0x7F : 8;
  } else if (vk == kVkEscape) {
    c = 0x1B;
  } else if (vk >= 0x60 && vk <= 0x69) {
    if (ctrl) return false;
    c = char16_t('0' + (vk - 0x60));
  } else if (vk >= 0x6A && vk <= 0x6F && vk != 0x6C) {
    if (ctrl) return false;
    c = char16_t(map_vk_to_char(vk));
  } else {
    const Key* found = nullptr;
    for (const Key& k : kKeys) if (k.vk == vk && k.normal != 0 && (k.scan < 0x37 || k.scan == 0x56)) { found = &k; break; }
    if (!found) return false;
    c = uint8_t(shift ? found->shifted : found->normal);
    if (ctrl) {
      switch (c) {
        case '@': c = 0; break;
        case '[': c = 0x1B; break;
        case '\\': c = 0x1C; break;
        case ']': c = 0x1D; break;
        case '^': c = 0x1E; break;
        case '_': c = 0x1F; break;
        default: return false;
      }
    }
  }
  *out = c;
  return true;
}

// VkKeyScan: the lowest virtual key (the numpad excluded) and the modifier set that make the character.
uint32_t vk_key_scan(char16_t ch) {
  for (uint32_t vk = 1; vk < 256; ++vk) {
    if (vk >= 0x60 && vk <= 0x6F) continue;
    for (uint32_t state = 0; state < 4; ++state) {      // 1 shift, 2 ctrl
      char16_t out = 0;
      if (translate_key(vk, (state & 1) != 0, (state & 2) != 0, false, false, &out) && out == ch) return state << 8 | vk;
    }
  }
  return 0xFFFF;
}

// --- layouts
struct KeyboardState {
  List<uint32_t> loaded;
  explicit KeyboardState(allocators::IAllocator& a) : loaded(a) {}
};

struct InputContext {
  uint64_t pid, tid, client_ptr = 0;
  InputContext(allocators::IAllocator&, uint64_t owner, uint64_t thread) : pid(owner), tid(thread) {}
};
struct Association { uint32_t hwnd, context; };      // context 0: IME switched off for the window (ImmAssociateContext(hwnd, NULL))
struct ImeLevel { uint32_t hwnd, level; };
constexpr uint32_t kNoAssociation = 0xFFFFFFFFu;      // what GetWindowInputContext answers for a window nothing was associated with: it uses the default context of its thread

// IMEINFOEX (imm32): the description of an installed IME, kept as the bytes the program gave. hkl first, wszImeFile at 188.
constexpr uint32_t kImeInfoExSize = 352, kImeFileOffset = 188, kImeFileChars = 80;
struct ImeInfo {
  uint64_t hkl;
  uint8_t bytes[kImeInfoExSize];
};
struct ImeHotKey { uint32_t id, modifiers, vk, hkl; };
struct ImeWindowStatus { uint32_t hwnd, open, conversion; };
struct DisabledImeThread { uint64_t pid, tid; };      // tid 0xFFFFFFFF: every thread of the process

struct ImeState {
  Registry<InputContext> contexts;
  List<Association> associations;
  List<ImeLevel> levels;
  List<ImeInfo> infos;
  List<ImeHotKey> hotkeys;
  List<ImeWindowStatus> statuses;
  List<DisabledImeThread> disabled;
  uint32_t show_status = 0;
  explicit ImeState(allocators::IAllocator& a) : contexts(a, 0x0004), associations(a), levels(a), infos(a), hotkeys(a), statuses(a), disabled(a) {}
};

UniquePointer<KeyboardState> g_keyboard;
UniquePointer<ImeState> g_ime;
KeyboardState* kb() {
  if (!g_keyboard) {
    KeyboardState* s = lazy_state(g_keyboard);
    if (s) s->loaded.push_back(kUsLayout);
  }
  return g_keyboard.get();
}
ImeState* ime() { return lazy_state(g_ime); }

uint32_t layout_of(Call& c) {
  ThreadRecord* t = find_thread_record(c.tid());
  return t && t->layout ? t->layout : kUsLayout;
}

bool is_loaded(KeyboardState& s, uint32_t hkl) {
  for (uint32_t l : s.loaded.view()) if (l == hkl) return true;
  return false;
}

uint64_t svc_get_keyboard_layout(Call& c) {
  const uint32_t tid = c.arg32(0);
  if (tid == 0 || tid == c.tid()) return layout_of(c);
  ThreadRecord* t = find_thread_record(tid);
  return t && t->layout ? t->layout : kUsLayout;
}

uint64_t svc_get_keyboard_layout_list(Call& c) {
  KeyboardState* s = kb();
  const int32_t size = c.argi(0);
  if (!s) return 0;
  if (size < 0) return c.fail(kErrorInvalidParameter);
  if (size == 0 || c.arg(1) == 0) return s->loaded.count();
  uint64_t n = 0;
  for (uint32_t l : s->loaded.view()) {
    if (n >= uint32_t(size)) break;
    if (!c.write_u64(c.arg(1) + n * 8, l)) return c.fail(kErrorNoAccess);
    ++n;
  }
  return n;
}

uint64_t svc_get_keyboard_layout_name(Call& c) {
  char text[9];
  const uint32_t hkl = layout_of(c);
  const char* hex = "0123456789ABCDEF";
  const uint32_t klid = hkl & 0xFFFF;      // the keyboard layout id of the layout: its language for the layouts loaded here
  for (int i = 0; i < 8; ++i) text[i] = hex[(klid >> (4 * (7 - i))) & 15];
  text[8] = 0;
  WideString name(*c.allocator());
  name.append_ascii(text);
  return c.write_wide(c.arg(0), name.view(), 9) >= 0 ? 1 : c.fail(kErrorNoAccess);
}

uint64_t svc_get_keyboard_type(Call& c) {
  switch (c.arg32(0)) {
    case 0: return 4;      // an enhanced 101- or 102-key keyboard
    case 1: return 0;
    case 2: return 12;     // function keys
    default: return 0;
  }
}

uint64_t svc_activate_keyboard_layout(Call& c) {
  KeyboardState* s = kb();
  if (!s) return 0;
  uint32_t hkl = c.arg32(0);
  ThreadRecord* t = thread_record(c.tid(), c.pid());
  if (!t) return 0;
  const uint32_t previous = t->layout ? t->layout : kUsLayout;
  if (hkl == 0 || hkl == 1) {      // HKL_PREV / HKL_NEXT: there is one layout
    hkl = s->loaded.count() ? s->loaded[0] : kUsLayout;
  } else if (!is_loaded(*s, hkl)) {
    return c.fail(kErrorInvalidKeyboardHandle);
  }
  t->layout = hkl;
  return previous;
}

uint64_t svc_load_keyboard_layout_ex(Call& c) {
  KeyboardState* s = kb();
  if (!s) return 0;
  // (hFile, offTable, hOldKL, UNICODE_STRING* KLID, KLID, flags)
  uint32_t klid = c.arg32(4);
  if (c.arg(3) != 0) {
    WideString text(*c.allocator());
    if (!c.read_unicode_string(c.arg(3), &text)) return c.fail(kErrorNoAccess);
    klid = 0;
    if (text.size() == 0 || text.size() > 8) return c.fail(kErrorInvalidParameter);
    for (char16_t ch : text.view()) {
      uint32_t d;
      if (ch >= '0' && ch <= '9') d = ch - '0'; else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10; else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10; else return c.fail(kErrorInvalidParameter);
      klid = klid << 4 | d;
    }
  }
  if ((klid & 0xFFFF) != 0x0409) return c.fail(kErrorFileNotFound);      // only the US layout has tables
  const uint32_t hkl = (klid & 0xFFFF) << 16 | (klid & 0xFFFF);
  if (!is_loaded(*s, hkl)) s->loaded.push_back(hkl);
  if (c.arg32(5) & 1) {      // KLF_ACTIVATE
    if (ThreadRecord* t = thread_record(c.tid(), c.pid())) t->layout = hkl;
  }
  return hkl;
}

uint64_t svc_unload_keyboard_layout(Call& c) {
  KeyboardState* s = kb();
  const uint32_t hkl = c.arg32(0);
  if (!s) return 0;
  if (!is_loaded(*s, hkl)) return c.fail(kErrorInvalidKeyboardHandle);
  if (s->loaded.count() <= 1) return c.fail(kErrorInvalidParameter);      // the last layout stays
  if (hkl == layout_of(c)) return c.fail(kErrorInvalidParameter);
  for (uint64_t i = 0; i < s->loaded.count(); ++i) if (s->loaded[i] == hkl) { s->loaded.remove_at(i); break; }
  return 1;
}

uint64_t svc_map_virtual_key_ex(Call& c) {
  const uint32_t code = c.arg32(0), type = c.arg32(1);
  switch (type) {
    case 0: return map_vk_to_vsc(code & 0xFF, false) & 0xFF;
    case 1: return map_vsc_to_vk(code, false);
    case 2: return map_vk_to_char(code & 0xFF);
    case 3: return map_vsc_to_vk(code, true);
    case 4: return map_vk_to_vsc(code & 0xFF, true);
    default: return c.fail(kErrorInvalidParameter);
  }
}

uint64_t svc_vk_key_scan_ex(Call& c) { return vk_key_scan(char16_t(c.arg32(0))); }

uint64_t svc_to_unicode_ex(Call& c) {
  // (virtual key, scan code, key state[256], buffer, size, flags, hkl)
  const uint32_t vk = c.arg32(0), scan = c.arg32(1);
  const int32_t size = c.argi(4);
  if (c.arg(2) == 0) return 0;
  uint8_t state[256];
  if (!c.read(c.arg(2), ByteView(state, 256))) return c.fail(kErrorNoAccess);
  if (size < 0 || vk > 255) return 0;
  if (scan & 0x8000) return 0;      // a key release makes no character
  const bool shift = (state[kVkShift] & 0x80) != 0, ctrl = (state[kVkControl] & 0x80) != 0, alt = (state[kVkMenu] & 0x80) != 0, caps = (state[kVkCapital] & 1) != 0;
  char16_t ch = 0;
  if (!translate_key(vk, shift, ctrl, alt, caps, &ch)) return 0;
  if (size < 1) return 0;
  return c.write_wide(c.arg(3), ArrayView<const char16_t>(&ch, 1), uint32_t(size)) >= 0 ? 1 : c.fail(kErrorNoAccess);
}

uint64_t svc_get_key_name_text(Call& c) {
  const uint32_t lparam = c.arg32(0);
  const int32_t size = c.argi(2);
  if (size <= 0) return 0;
  const uint8_t scan = uint8_t(lparam >> 16);
  const bool extended = (lparam & 0x01000000) != 0, either = (lparam & 0x02000000) != 0;
  const char* name = nullptr;
  if (extended && !either) {
    if (const ExtKey* e = ext_by_scan(scan)) name = e->name;
  } else if (const Key* k = key_by_scan(scan)) {
    name = k->name;
  }
  if (name && either) {      // the left/right distinction is not wanted
    if (scan == 0x1D) name = "Ctrl";
    if (scan == 0x36 || scan == 0x2A) name = "Shift";
    if (scan == 0x38) name = "Alt";
  }
  WideString text(*c.allocator());
  if (name) text.append_ascii(name);
  const uint32_t room = uint32_t(size) - 1;
  const uint32_t n = text.size() < room ? uint32_t(text.size()) : room;
  const int32_t written = c.write_wide(c.arg(1), text.view().first(n), uint32_t(size));
  if (written < 0) return c.fail(kErrorNoAccess);
  return uint32_t(written);
}

// --- input contexts
bool thread_ime_disabled(ImeState& s, uint64_t pid, uint64_t tid) {
  for (const DisabledImeThread& d : s.disabled.view()) if (d.pid == pid && (d.tid == tid || d.tid == 0xFFFFFFFFu)) return true;
  return false;
}

uint64_t svc_create_input_context(Call& c) {
  ImeState* s = ime();
  if (!s) return 0;
  uint32_t handle = 0;
  if (thread_ime_disabled(*s, c.pid(), c.tid())) return c.fail(kErrorAccessDenied);
  InputContext* ctx = s->contexts.create(&handle, c.pid(), c.tid());
  if (!ctx) return c.fail(kErrorNoMoreUserHandles);
  ctx->client_ptr = c.arg(0);
  return handle;
}

uint64_t svc_destroy_input_context(Call& c) {
  ImeState* s = ime();
  InputContext* ctx = s && (c.arg(0) >> 32) == 0 ? s->contexts.find(c.arg32(0)) : nullptr;
  if (!ctx) return c.fail(kErrorInvalidHandle);
  if (ctx->pid != c.pid()) return c.fail(kErrorAccessDenied);
  const uint32_t handle = c.arg32(0);
  for (uint64_t i = 0; i < s->associations.count();) { if (s->associations[i].context == handle) s->associations.remove_at(i); else ++i; }
  s->contexts.destroy(handle);
  return 1;
}

uint64_t svc_update_input_context(Call& c) {
  ImeState* s = ime();
  InputContext* ctx = s && (c.arg(0) >> 32) == 0 ? s->contexts.find(c.arg32(0)) : nullptr;
  if (!ctx) return c.fail(kErrorInvalidHandle);
  switch (c.arg32(1)) {
    case 0: ctx->client_ptr = c.arg(2); return 1;      // NtUserInputContextClientPtr
    case 1: ctx->tid = c.arg(2); return 1;             // NtUserInputContextThreadId
    default: return c.fail(kErrorInvalidParameter);
  }
}

uint64_t svc_query_input_context(Call& c) {
  ImeState* s = ime();
  InputContext* ctx = s && (c.arg(0) >> 32) == 0 ? s->contexts.find(c.arg32(0)) : nullptr;
  if (!ctx) return c.fail(kErrorInvalidHandle);
  switch (c.arg32(1)) {
    case 0: return ctx->client_ptr;
    case 1: return ctx->tid;
    case 2: return ctx->pid;
    default: return c.fail(kErrorInvalidParameter);
  }
}

uint64_t svc_associate_input_context(Call& c) {
  constexpr uint32_t kAicrOk = 0, kAicrFocusChanged = 1, kAicrFailed = 2;
  ImeState* s = ime();
  const uint32_t hwnd = c.arg32(0), context = c.arg32(1);
  if (!s || !user::is_window(hwnd)) return kAicrFailed;
  if (context != 0) {
    InputContext* ctx = s->contexts.find(context);
    if (!ctx || ctx->tid != user::window_thread(hwnd)) return kAicrFailed;
  }
  bool replaced = false;
  for (Association& a : s->associations.view()) {
    if (a.hwnd != hwnd) continue;
    replaced = true;
    a.context = context;
    break;
  }
  if (!replaced && !s->associations.push_back(Association{hwnd, context})) return kAicrFailed;
  return user::focus_window() == hwnd ? kAicrFocusChanged : kAicrOk;
}

uint64_t svc_get_app_ime_level(Call& c) {
  ImeState* s = ime();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  for (const ImeLevel& l : s->levels.view()) if (l.hwnd == hwnd) return l.level;
  return 0;
}

uint64_t svc_set_app_ime_level(Call& c) {
  ImeState* s = ime();
  const uint32_t hwnd = c.arg32(0), level = c.arg32(1);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  for (ImeLevel& l : s->levels.view()) if (l.hwnd == hwnd) { l.level = level; return 1; }
  return s->levels.push_back(ImeLevel{hwnd, level}) ? 1 : c.fail(kErrorNotEnoughMemory);
}


// BOOL NtUserGetImeInfoEx(IMEINFOEX*, IMEINFOEXCLASS): 0 = by the layout in the structure, 1 = by the IME file name in it.
uint64_t svc_get_ime_info_ex(Call& c) {
  ImeState* s = ime();
  if (!s || c.arg(0) == 0) return c.fail(kErrorInvalidParameter);
  uint8_t request[kImeInfoExSize];
  if (!c.read(c.arg(0), ByteView(request, kImeInfoExSize))) return c.fail(kErrorNoAccess);
  uint64_t hkl = 0;
  for (int i = 7; i >= 0; --i) hkl = hkl << 8 | request[i];
  const uint32_t by = c.arg32(1);
  if (by > 1) return c.fail(kErrorInvalidParameter);
  for (const ImeInfo& info : s->infos.view()) {
    bool match = false;
    if (by == 0) {
      match = info.hkl == hkl;
    } else {
      match = true;
      for (uint32_t i = 0; i < kImeFileChars; ++i) {
        uint16_t a = uint16_t(request[kImeFileOffset + 2 * i] | request[kImeFileOffset + 2 * i + 1] << 8), b = uint16_t(info.bytes[kImeFileOffset + 2 * i] | info.bytes[kImeFileOffset + 2 * i + 1] << 8);
        if (a >= 'A' && a <= 'Z') a = uint16_t(a + 32);
        if (b >= 'A' && b <= 'Z') b = uint16_t(b + 32);
        if (a != b) { match = false; break; }
        if (a == 0) break;
      }
    }
    if (!match) continue;
    return c.write(c.arg(0), ConstByteView(info.bytes, kImeInfoExSize)) ? 1 : c.fail(kErrorNoAccess);
  }
  return c.fail(kErrorInvalidKeyboardHandle);
}

// BOOL NtUserSetImeInfoEx(IMEINFOEX*): the IME of a layout publishes its description (imm32 does it after loading the IME).
uint64_t svc_set_ime_info_ex(Call& c) {
  ImeState* s = ime();
  KeyboardState* k = kb();
  if (!s || !k || c.arg(0) == 0) return c.fail(kErrorInvalidParameter);
  ImeInfo info{};
  if (!c.read(c.arg(0), ByteView(info.bytes, kImeInfoExSize))) return c.fail(kErrorNoAccess);
  for (int i = 7; i >= 0; --i) info.hkl = info.hkl << 8 | info.bytes[i];
  const uint32_t hkl = uint32_t(info.hkl);
  if (info.hkl != hkl || (!is_loaded(*k, hkl) && (hkl >> 28) != 0xE)) return c.fail(kErrorInvalidKeyboardHandle);
  for (ImeInfo& have : s->infos.view()) if (have.hkl == info.hkl) { have = info; return 1; }
  return s->infos.push_back(info) ? 1 : c.fail(kErrorNotEnoughMemory);
}

// BOOL NtUserGetImeHotKey(id, UINT* modifiers, UINT* vk, HKL*)
uint64_t svc_get_ime_hot_key(Call& c) {
  ImeState* s = ime();
  if (!s) return 0;
  for (const ImeHotKey& h : s->hotkeys.view()) {
    if (h.id != c.arg32(0)) continue;
    if ((c.arg(1) && !c.write_u32(c.arg(1), h.modifiers)) || (c.arg(2) && !c.write_u32(c.arg(2), h.vk)) || (c.arg(3) && !c.write_u64(c.arg(3), h.hkl))) return c.fail(kErrorNoAccess);
    return 1;
  }
  return 0;
}

// BOOL NtUserSetImeHotKey(id, modifiers, vk, HKL, action): 0 sets, 1 deletes, 2 resets the table.
uint64_t svc_set_ime_hot_key(Call& c) {
  ImeState* s = ime();
  if (!s) return 0;
  const uint32_t id = c.arg32(0), action = c.arg32(4);
  if (action == 2) { s->hotkeys.clear(); return 1; }
  if (action > 1) return c.fail(kErrorInvalidParameter);
  for (uint64_t i = 0; i < s->hotkeys.count(); ++i) {
    if (s->hotkeys[i].id != id) continue;
    if (action == 1) { s->hotkeys.remove_at(i); return 1; }
    s->hotkeys[i] = ImeHotKey{id, c.arg32(1), c.arg32(2), c.arg32(3)};
    return 1;
  }
  if (action == 1) return c.fail(kErrorInvalidParameter);
  return s->hotkeys.push_back(ImeHotKey{id, c.arg32(1), c.arg32(2), c.arg32(3)}) ? 1 : c.fail(kErrorNotEnoughMemory);
}

// UINT NtUserBuildHimcList(DWORD tid, DWORD count, HIMC* out, UINT* needed): the contexts of a thread (0: the calling thread; -1: every thread of the process). Returns how many were written.
uint64_t svc_build_himc_list(Call& c) {
  ImeState* s = ime();
  if (!s) return 0;
  const uint32_t tid = c.arg32(0), count = c.arg32(1);
  List<uint32_t> all(*c.allocator());
  s->contexts.handles(&all);
  uint32_t matching = 0, written = 0;
  for (uint32_t h : all.view()) {
    InputContext* ctx = s->contexts.find(h);
    if (!ctx || ctx->pid != c.pid()) continue;
    if (tid != 0xFFFFFFFFu && ctx->tid != (tid ? tid : c.tid())) continue;
    ++matching;
    if (written < count && c.arg(2) != 0) {
      if (!c.write_u64(c.arg(2) + uint64_t{written} * 8, h)) return c.fail(kErrorNoAccess);
      ++written;
    }
  }
  if (c.arg(3) && !c.write_u32(c.arg(3), matching)) return c.fail(kErrorNoAccess);
  return written;
}

// void NtUserNotifyIMEStatus(HWND, open, conversion mode): what the shell shows of the IME of the focus window.
uint64_t svc_notify_ime_status(Call& c) {
  ImeState* s = ime();
  const uint32_t hwnd = c.arg32(0);
  if (!s) return 0;
  if (!user::is_window(hwnd)) return c.fail(kErrorInvalidWindowHandle);
  for (ImeWindowStatus& w : s->statuses.view()) if (w.hwnd == hwnd) { w.open = c.arg32(1) != 0; w.conversion = c.arg32(2); return 1; }
  return s->statuses.push_back(ImeWindowStatus{hwnd, c.arg32(1) != 0, c.arg32(2)}) ? 1 : c.fail(kErrorNotEnoughMemory);
}

// BOOL NtUserDisableThreadIme(tid): tid -1 is every thread of the process. The default contexts of the thread are gone for good.
uint64_t svc_disable_thread_ime(Call& c) {
  ImeState* s = ime();
  if (!s) return 0;
  const uint32_t tid = c.arg32(0);
  if (tid != 0 && tid != 0xFFFFFFFFu && tid != c.tid()) {
    ThreadRecord* t = find_thread_record(tid);
    if (t && t->pid != c.pid()) return c.fail(kErrorAccessDenied);
  }
  const uint64_t target = tid == 0 ? c.tid() : tid;
  if (!thread_ime_disabled(*s, c.pid(), target) && !s->disabled.push_back(DisabledImeThread{c.pid(), target})) return c.fail(kErrorNotEnoughMemory);
  List<uint32_t> all(*c.allocator());
  s->contexts.handles(&all);
  for (uint32_t h : all.view()) {
    InputContext* ctx = s->contexts.find(h);
    if (!ctx || ctx->pid != c.pid() || (target != 0xFFFFFFFFu && ctx->tid != target)) continue;
    for (uint64_t i = 0; i < s->associations.count();) { if (s->associations[i].context == h) s->associations.remove_at(i); else ++i; }
  }
  return 1;
}

uint64_t svc_broadcast_ime_show_status(Call& c) { ImeState* s = ime(); if (s) s->show_status = c.arg32(1) != 0; return 1; }
uint64_t svc_get_ime_show_status(Call&) { ImeState* s = ime(); return s ? s->show_status : 0; }
uint64_t svc_check_ime_show_status(Call&) { ImeState* s = ime(); return s ? s->show_status : 0; }
uint64_t svc_nls_ime_notification(Call&) { return 1; }
uint64_t svc_update_per_user_imm(Call&) { return 1; }

// BOOL NtUserSetImeOwnerWindow(HWND, BOOL): the window an IME window belongs to; nothing is shown, so only the handle is checked.
uint64_t svc_set_ime_owner_window(Call& c) {
  if (!user::is_window(c.arg32(0))) return c.fail(kErrorInvalidWindowHandle);
  return 1;
}

// BOOL NtUserSetThreadLayoutHandles(HKL new, HKL old): the layout the thread's IME runs with.
uint64_t svc_set_thread_layout_handles(Call& c) {
  KeyboardState* k = kb();
  const uint32_t hkl = c.arg32(0);
  ThreadRecord* t = thread_record(c.tid(), c.pid());
  if (!k || !t) return 0;
  if (!is_loaded(*k, hkl)) return c.fail(kErrorInvalidKeyboardHandle);
  t->layout = hkl;
  return 1;
}

}  // namespace

uint32_t window_input_context_raw(uint32_t hwnd) {
  if (!g_ime) return kNoAssociation;
  for (const Association& a : g_ime->associations.view()) if (a.hwnd == hwnd) return a.context;
  return kNoAssociation;
}

uint32_t window_input_context(uint32_t hwnd) {
  if (!g_ime) return 0;
  for (const Association& a : g_ime->associations.view()) if (a.hwnd == hwnd) return a.context;
  return 0;
}

uint32_t first_layout_of_system() { return kUsLayout; }

void keyboard_window_destroyed(uint32_t hwnd) {
  if (!g_ime) return;
  for (uint64_t i = 0; i < g_ime->associations.count();) { if (g_ime->associations[i].hwnd == hwnd) g_ime->associations.remove_at(i); else ++i; }
  for (uint64_t i = 0; i < g_ime->levels.count();) { if (g_ime->levels[i].hwnd == hwnd) g_ime->levels.remove_at(i); else ++i; }
  for (uint64_t i = 0; i < g_ime->statuses.count();) { if (g_ime->statuses[i].hwnd == hwnd) g_ime->statuses.remove_at(i); else ++i; }
}

void keyboard_process_exited(uint64_t pid) {
  if (!g_ime) return;
  for (uint64_t i = 0; i < g_ime->disabled.count();) { if (g_ime->disabled[i].pid == pid) g_ime->disabled.remove_at(i); else ++i; }
  List<uint32_t> all(*system_allocator());
  g_ime->contexts.handles(&all);
  for (uint32_t h : all.view()) {
    InputContext* ctx = g_ime->contexts.find(h);
    if (!ctx || ctx->pid != pid) continue;
    for (uint64_t i = 0; i < g_ime->associations.count();) { if (g_ime->associations[i].context == h) g_ime->associations.remove_at(i); else ++i; }
    g_ime->contexts.destroy(h);
  }
}

void install_keyboard() {
  using N = WindowsSyscallNumber;
  register_handler(N::NtUserGetKeyboardLayout, svc_get_keyboard_layout);
  register_handler(N::NtUserGetKeyboardLayoutList, svc_get_keyboard_layout_list);
  register_handler(N::NtUserGetKeyboardLayoutName, svc_get_keyboard_layout_name);
  register_handler(N::NtUserGetKeyboardType, svc_get_keyboard_type);
  register_handler(N::NtUserActivateKeyboardLayout, svc_activate_keyboard_layout);
  register_handler(N::NtUserLoadKeyboardLayoutEx, svc_load_keyboard_layout_ex);
  register_handler(N::NtUserUnloadKeyboardLayout, svc_unload_keyboard_layout);
  register_handler(N::NtUserMapVirtualKeyEx, svc_map_virtual_key_ex);
  register_handler(N::NtUserVkKeyScanEx, svc_vk_key_scan_ex);
  register_handler(N::NtUserToUnicodeEx, svc_to_unicode_ex);
  register_handler(N::NtUserGetKeyNameText, svc_get_key_name_text);
  register_handler(N::NtUserCreateInputContext, svc_create_input_context);
  register_handler(N::NtUserDestroyInputContext, svc_destroy_input_context);
  register_handler(N::NtUserUpdateInputContext, svc_update_input_context);
  register_handler(N::NtUserQueryInputContext, svc_query_input_context);
  register_handler(N::NtUserAssociateInputContext, svc_associate_input_context);
  register_handler(N::NtUserGetAppImeLevel, svc_get_app_ime_level);
  register_handler(N::NtUserSetAppImeLevel, svc_set_app_ime_level);
  register_handler(N::NtUserGetImeInfoEx, svc_get_ime_info_ex);
  register_handler(N::NtUserSetImeInfoEx, svc_set_ime_info_ex);
  register_handler(N::NtUserGetImeHotKey, svc_get_ime_hot_key);
  register_handler(N::NtUserSetImeHotKey, svc_set_ime_hot_key);
  register_handler(N::NtUserBuildHimcList, svc_build_himc_list);
  register_handler(N::NtUserNotifyIMEStatus, svc_notify_ime_status);
  register_handler(N::NtUserDisableThreadIme, svc_disable_thread_ime);
  register_handler(N::NtUserBroadcastImeShowStatusChange, svc_broadcast_ime_show_status);
  register_handler(N::NtUserGetIMEShowStatus, svc_get_ime_show_status);
  register_handler(N::NtUserCheckImeShowStatusInThread, svc_check_ime_show_status);
  register_handler(N::NtUserNlsKbdSendIMENotification, svc_nls_ime_notification);
  register_handler(N::NtUserUpdatePerUserImmEnabling, svc_update_per_user_imm);
  register_handler(N::NtUserSetImeOwnerWindow, svc_set_ime_owner_window);
  register_handler(N::NtUserSetThreadLayoutHandles, svc_set_thread_layout_handles);
}

}  // namespace process::win32k::user_misc
