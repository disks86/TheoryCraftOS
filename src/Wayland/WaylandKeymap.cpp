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

// The keyboard the compositor's clients are told of: the HID usages the desktop reports as Linux evdev key codes (what wl_keyboard.key carries; xkb key
// codes are those plus 8), and the keymap that goes with them - a US layout in xkb_v1 text, complete in itself (no include files), so the client's
// libxkbcommon needs nothing from the OS's file system. The modifier masks of wl_keyboard.modifiers are its real modifiers: Shift 1, Lock 2, Control 4,
// Mod1 (Alt) 8, Mod2 (NumLock) 16, Mod4 (Super) 64.
#include "Wayland/WaylandServer.hpp"

namespace wayland {

namespace {

// HID usage (keyboard page) -> evdev code; 0: none. As the Linux kernel's hid-input maps them.
constexpr uint8_t kEvdev[256] = {
    0, 0, 0, 0, 30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38,
    50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44, 2, 3,
    4, 5, 6, 7, 8, 9, 10, 11, 28, 1, 14, 15, 57, 12, 13, 26,
    27, 43, 43, 39, 40, 41, 51, 52, 53, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 87, 88, 99, 70, 119, 110, 102, 104, 111, 107, 109, 106,
    105, 108, 103, 69, 98, 55, 74, 78, 96, 79, 80, 81, 75, 76, 77, 71,
    72, 73, 82, 83, 86, 127, 116, 117, 183, 184, 185, 186, 187, 188, 189, 190,
    191, 192, 193, 194, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 113,
    115, 114, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    29, 42, 56, 125, 97, 54, 100, 126, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

constexpr char kKeymap[] = R"XKB(xkb_keymap {
xkb_keycodes "tcos" {
  minimum = 8;
  maximum = 255;
  <TLDE> = 49;
  <AE01> = 10;
  <AE02> = 11;
  <AE03> = 12;
  <AE04> = 13;
  <AE05> = 14;
  <AE06> = 15;
  <AE07> = 16;
  <AE08> = 17;
  <AE09> = 18;
  <AE10> = 19;
  <AE11> = 20;
  <AE12> = 21;
  <AD01> = 24;
  <AD02> = 25;
  <AD03> = 26;
  <AD04> = 27;
  <AD05> = 28;
  <AD06> = 29;
  <AD07> = 30;
  <AD08> = 31;
  <AD09> = 32;
  <AD10> = 33;
  <AD11> = 34;
  <AD12> = 35;
  <BKSL> = 51;
  <AC01> = 38;
  <AC02> = 39;
  <AC03> = 40;
  <AC04> = 41;
  <AC05> = 42;
  <AC06> = 43;
  <AC07> = 44;
  <AC08> = 45;
  <AC09> = 46;
  <AC10> = 47;
  <AC11> = 48;
  <AB01> = 52;
  <AB02> = 53;
  <AB03> = 54;
  <AB04> = 55;
  <AB05> = 56;
  <AB06> = 57;
  <AB07> = 58;
  <AB08> = 59;
  <AB09> = 60;
  <AB10> = 61;
  <LSGT> = 94;
  <SPCE> = 65;
  <ESC> = 9;
  <BKSP> = 22;
  <TAB> = 23;
  <RTRN> = 36;
  <LCTL> = 37;
  <RCTL> = 105;
  <LFSH> = 50;
  <RTSH> = 62;
  <LALT> = 64;
  <RALT> = 108;
  <LWIN> = 133;
  <RWIN> = 134;
  <COMP> = 135;
  <CAPS> = 66;
  <NMLK> = 77;
  <SCLK> = 78;
  <PAUS> = 127;
  <PRSC> = 107;
  <HOME> = 110;
  <END> = 115;
  <PGUP> = 112;
  <PGDN> = 117;
  <INS> = 118;
  <DELE> = 119;
  <UP> = 111;
  <DOWN> = 116;
  <LEFT> = 113;
  <RGHT> = 114;
  <KPDV> = 106;
  <KPMU> = 63;
  <KPSU> = 82;
  <KPAD> = 86;
  <KPEN> = 104;
  <KPEQ> = 125;
  <KP7> = 79;
  <KP8> = 80;
  <KP9> = 81;
  <KP4> = 83;
  <KP5> = 84;
  <KP6> = 85;
  <KP1> = 87;
  <KP2> = 88;
  <KP3> = 89;
  <KP0> = 90;
  <KPDL> = 91;
  <MUTE> = 121;
  <VOL-> = 122;
  <VOL+> = 123;
  <POWR> = 124;
  <FK01> = 67;
  <FK02> = 68;
  <FK03> = 69;
  <FK04> = 70;
  <FK05> = 71;
  <FK06> = 72;
  <FK07> = 73;
  <FK08> = 74;
  <FK09> = 75;
  <FK10> = 76;
  <FK11> = 95;
  <FK12> = 96;
  <FK13> = 191;
  <FK14> = 192;
  <FK15> = 193;
  <FK16> = 194;
  <FK17> = 195;
  <FK18> = 196;
  <FK19> = 197;
  <FK20> = 198;
  <FK21> = 199;
  <FK22> = 200;
  <FK23> = 201;
  <FK24> = 202;
  indicator 1 = "Caps Lock";
  indicator 2 = "Num Lock";
};
xkb_types "tcos" {
  virtual_modifiers NumLock,Alt,Super;
  type "ONE_LEVEL" { modifiers = none; level_name[Level1] = "Any"; };
  type "TWO_LEVEL" { modifiers = Shift; map[Shift] = Level2; level_name[Level1] = "Base"; level_name[Level2] = "Shift"; };
  type "ALPHABETIC" { modifiers = Shift+Lock; map[Shift] = Level2; map[Lock] = Level2; level_name[Level1] = "Base"; level_name[Level2] = "Caps"; };
  type "KEYPAD" { modifiers = Shift+NumLock; map[None] = Level1; map[Shift] = Level2; map[NumLock] = Level2; map[Shift+NumLock] = Level1; level_name[Level1] = "Base"; level_name[Level2] = "Number"; };
};
xkb_compatibility "tcos" {
  virtual_modifiers NumLock,Alt,Super;
  interpret.useModMapMods = AnyLevel;
  interpret.repeat = False;
  interpret Shift_L+AnyOfOrNone(all) { action = SetMods(modifiers=Shift,clearLocks); };
  interpret Shift_R+AnyOfOrNone(all) { action = SetMods(modifiers=Shift,clearLocks); };
  interpret Control_L+AnyOfOrNone(all) { action = SetMods(modifiers=Control,clearLocks); };
  interpret Control_R+AnyOfOrNone(all) { action = SetMods(modifiers=Control,clearLocks); };
  interpret Alt_L+AnyOf(all) { virtualModifier = Alt; action = SetMods(modifiers=modMapMods,clearLocks); };
  interpret Alt_R+AnyOf(all) { virtualModifier = Alt; action = SetMods(modifiers=modMapMods,clearLocks); };
  interpret Super_L+AnyOf(all) { virtualModifier = Super; action = SetMods(modifiers=modMapMods,clearLocks); };
  interpret Super_R+AnyOf(all) { virtualModifier = Super; action = SetMods(modifiers=modMapMods,clearLocks); };
  interpret Caps_Lock+AnyOfOrNone(all) { action = LockMods(modifiers=Lock); };
  interpret Num_Lock+AnyOf(all) { virtualModifier = NumLock; action = LockMods(modifiers=NumLock); };
  interpret Any+Exactly(Lock) { action = LockMods(modifiers=Lock); };
  interpret Any+AnyOf(all) { action = SetMods(modifiers=modMapMods,clearLocks); };
  indicator "Caps Lock" { whichModState = locked; modifiers = Lock; };
  indicator "Num Lock" { whichModState = locked; modifiers = NumLock; };
};
xkb_symbols "tcos" {
  name[group1] = "English (US)";
  key <TLDE> { [ grave, asciitilde ] };
  key <AE01> { [ 1, exclam ] };
  key <AE02> { [ 2, at ] };
  key <AE03> { [ 3, numbersign ] };
  key <AE04> { [ 4, dollar ] };
  key <AE05> { [ 5, percent ] };
  key <AE06> { [ 6, asciicircum ] };
  key <AE07> { [ 7, ampersand ] };
  key <AE08> { [ 8, asterisk ] };
  key <AE09> { [ 9, parenleft ] };
  key <AE10> { [ 0, parenright ] };
  key <AE11> { [ minus, underscore ] };
  key <AE12> { [ equal, plus ] };
  key <AD01> { [ q, Q ] };
  key <AD02> { [ w, W ] };
  key <AD03> { [ e, E ] };
  key <AD04> { [ r, R ] };
  key <AD05> { [ t, T ] };
  key <AD06> { [ y, Y ] };
  key <AD07> { [ u, U ] };
  key <AD08> { [ i, I ] };
  key <AD09> { [ o, O ] };
  key <AD10> { [ p, P ] };
  key <AD11> { [ bracketleft, braceleft ] };
  key <AD12> { [ bracketright, braceright ] };
  key <BKSL> { [ backslash, bar ] };
  key <AC01> { [ a, A ] };
  key <AC02> { [ s, S ] };
  key <AC03> { [ d, D ] };
  key <AC04> { [ f, F ] };
  key <AC05> { [ g, G ] };
  key <AC06> { [ h, H ] };
  key <AC07> { [ j, J ] };
  key <AC08> { [ k, K ] };
  key <AC09> { [ l, L ] };
  key <AC10> { [ semicolon, colon ] };
  key <AC11> { [ apostrophe, quotedbl ] };
  key <AB01> { [ z, Z ] };
  key <AB02> { [ x, X ] };
  key <AB03> { [ c, C ] };
  key <AB04> { [ v, V ] };
  key <AB05> { [ b, B ] };
  key <AB06> { [ n, N ] };
  key <AB07> { [ m, M ] };
  key <AB08> { [ comma, less ] };
  key <AB09> { [ period, greater ] };
  key <AB10> { [ slash, question ] };
  key <LSGT> { [ less, greater ] };
  key <SPCE> { [ space ] };
  key <ESC> { [ Escape ] };
  key <BKSP> { [ BackSpace ] };
  key <TAB> { [ Tab, ISO_Left_Tab ] };
  key <RTRN> { [ Return ] };
  key <LCTL> { [ Control_L ] };
  key <RCTL> { [ Control_R ] };
  key <LFSH> { [ Shift_L ] };
  key <RTSH> { [ Shift_R ] };
  key <LALT> { [ Alt_L ] };
  key <RALT> { [ Alt_R ] };
  key <LWIN> { [ Super_L ] };
  key <RWIN> { [ Super_R ] };
  key <COMP> { [ Menu ] };
  key <CAPS> { [ Caps_Lock ] };
  key <NMLK> { [ Num_Lock ] };
  key <SCLK> { [ Scroll_Lock ] };
  key <PAUS> { [ Pause ] };
  key <PRSC> { [ Print ] };
  key <HOME> { [ Home ] };
  key <END> { [ End ] };
  key <PGUP> { [ Prior ] };
  key <PGDN> { [ Next ] };
  key <INS> { [ Insert ] };
  key <DELE> { [ Delete ] };
  key <UP> { [ Up ] };
  key <DOWN> { [ Down ] };
  key <LEFT> { [ Left ] };
  key <RGHT> { [ Right ] };
  key <KPDV> { [ KP_Divide ] };
  key <KPMU> { [ KP_Multiply ] };
  key <KPSU> { [ KP_Subtract ] };
  key <KPAD> { [ KP_Add ] };
  key <KPEN> { [ KP_Enter ] };
  key <KPEQ> { [ KP_Equal ] };
  key <KP7> { [ KP_Home, KP_7 ] };
  key <KP8> { [ KP_Up, KP_8 ] };
  key <KP9> { [ KP_Prior, KP_9 ] };
  key <KP4> { [ KP_Left, KP_4 ] };
  key <KP5> { [ KP_Begin, KP_5 ] };
  key <KP6> { [ KP_Right, KP_6 ] };
  key <KP1> { [ KP_End, KP_1 ] };
  key <KP2> { [ KP_Down, KP_2 ] };
  key <KP3> { [ KP_Next, KP_3 ] };
  key <KP0> { [ KP_Insert, KP_0 ] };
  key <KPDL> { [ KP_Delete, KP_Decimal ] };
  key <MUTE> { [ XF86AudioMute ] };
  key <VOL-> { [ XF86AudioLowerVolume ] };
  key <VOL+> { [ XF86AudioRaiseVolume ] };
  key <POWR> { [ XF86PowerOff ] };
  key <FK01> { [ F1 ] };
  key <FK02> { [ F2 ] };
  key <FK03> { [ F3 ] };
  key <FK04> { [ F4 ] };
  key <FK05> { [ F5 ] };
  key <FK06> { [ F6 ] };
  key <FK07> { [ F7 ] };
  key <FK08> { [ F8 ] };
  key <FK09> { [ F9 ] };
  key <FK10> { [ F10 ] };
  key <FK11> { [ F11 ] };
  key <FK12> { [ F12 ] };
  key <FK13> { [ F13 ] };
  key <FK14> { [ F14 ] };
  key <FK15> { [ F15 ] };
  key <FK16> { [ F16 ] };
  key <FK17> { [ F17 ] };
  key <FK18> { [ F18 ] };
  key <FK19> { [ F19 ] };
  key <FK20> { [ F20 ] };
  key <FK21> { [ F21 ] };
  key <FK22> { [ F22 ] };
  key <FK23> { [ F23 ] };
  key <FK24> { [ F24 ] };
  modifier_map Shift { <LFSH>, <RTSH> };
  modifier_map Lock { <CAPS> };
  modifier_map Control { <LCTL>, <RCTL> };
  modifier_map Mod1 { <LALT>, <RALT> };
  modifier_map Mod2 { <NMLK> };
  modifier_map Mod4 { <LWIN>, <RWIN> };
};
};
)XKB";

}  // namespace

uint32_t Server::evdev_code(uint8_t hid_usage) { return kEvdev[hid_usage]; }

ConstByteView Server::keymap_text() { return StringView(kKeymap, sizeof(kKeymap)).bytes(); }      // (with the NUL: the size wl_keyboard.keymap gives)

}  // namespace wayland
