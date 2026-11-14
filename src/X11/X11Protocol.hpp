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

// Numbers of the X Window System protocol, version 11 (the core protocol document and the ICCCM / EWMH conventions the window manager part follows).
#pragma once
#include <stdint.h>
#include "Utility/StringView.hpp"

namespace x11 {

namespace error {
constexpr uint8_t kRequest = 1, kValue = 2, kWindow = 3, kPixmap = 4, kAtom = 5, kCursor = 6, kFont = 7, kMatch = 8, kDrawable = 9, kAccess = 10, kAlloc = 11,
                  kColormap = 12, kGContext = 13, kIdChoice = 14, kName = 15, kLength = 16, kImplementation = 17;
}

namespace event {
constexpr uint8_t kKeyPress = 2, kKeyRelease = 3, kButtonPress = 4, kButtonRelease = 5, kMotionNotify = 6, kEnterNotify = 7, kLeaveNotify = 8, kFocusIn = 9,
                  kFocusOut = 10, kKeymapNotify = 11, kExpose = 12, kGraphicsExpose = 13, kNoExpose = 14, kVisibilityNotify = 15, kCreateNotify = 16,
                  kDestroyNotify = 17, kUnmapNotify = 18, kMapNotify = 19, kMapRequest = 20, kReparentNotify = 21, kConfigureNotify = 22,
                  kConfigureRequest = 23, kGravityNotify = 24, kResizeRequest = 25, kCirculateNotify = 26, kCirculateRequest = 27, kPropertyNotify = 28,
                  kSelectionClear = 29, kSelectionRequest = 30, kSelectionNotify = 31, kColormapNotify = 32, kClientMessage = 33, kMappingNotify = 34;
}

// Event masks (SETofEVENT).
namespace mask {
constexpr uint32_t kKeyPress = 1u << 0, kKeyRelease = 1u << 1, kButtonPress = 1u << 2, kButtonRelease = 1u << 3, kEnterWindow = 1u << 4, kLeaveWindow = 1u << 5,
                   kPointerMotion = 1u << 6, kPointerMotionHint = 1u << 7, kButton1Motion = 1u << 8, kButtonMotion = 1u << 13, kKeymapState = 1u << 14,
                   kExposure = 1u << 15, kVisibilityChange = 1u << 16, kStructureNotify = 1u << 17, kResizeRedirect = 1u << 18,
                   kSubstructureNotify = 1u << 19, kSubstructureRedirect = 1u << 20, kFocusChange = 1u << 21, kPropertyChange = 1u << 22,
                   kColormapChange = 1u << 23, kOwnerGrabButton = 1u << 24;
constexpr uint32_t kAll = (1u << 25) - 1;
constexpr uint32_t kPointerEvents = kButtonPress | kButtonRelease | kEnterWindow | kLeaveWindow | kPointerMotion | kPointerMotionHint | (0x1Fu << 8) |
                                    kButtonMotion | kKeymapState;
}

// Key and button state bits (SETofKEYBUTMASK).
namespace state {
constexpr uint16_t kShift = 1, kLock = 2, kControl = 4, kMod1 = 8, kMod2 = 16, kMod3 = 32, kMod4 = 64, kMod5 = 128, kButton1 = 256;
}

// The atoms every server has (1 .. kLastPredefinedAtom), in their numbered order.
inline constexpr const char* kPredefinedAtoms[] = {
    "PRIMARY", "SECONDARY", "ARC", "ATOM", "BITMAP", "CARDINAL", "COLORMAP", "CURSOR", "CUT_BUFFER0", "CUT_BUFFER1", "CUT_BUFFER2", "CUT_BUFFER3",
    "CUT_BUFFER4", "CUT_BUFFER5", "CUT_BUFFER6", "CUT_BUFFER7", "DRAWABLE", "FONT", "INTEGER", "PIXMAP", "POINT", "RECTANGLE", "RESOURCE_MANAGER",
    "RGB_COLOR_MAP", "RGB_BEST_MAP", "RGB_BLUE_MAP", "RGB_DEFAULT_MAP", "RGB_GRAY_MAP", "RGB_GREEN_MAP", "RGB_RED_MAP", "STRING", "VISUALID", "WINDOW",
    "WM_COMMAND", "WM_HINTS", "WM_CLIENT_MACHINE", "WM_ICON_NAME", "WM_ICON_SIZE", "WM_NAME", "WM_NORMAL_HINTS", "WM_SIZE_HINTS", "WM_ZOOM_HINTS",
    "MIN_SPACE", "NORM_SPACE", "MAX_SPACE", "END_SPACE", "SUPERSCRIPT_X", "SUPERSCRIPT_Y", "SUBSCRIPT_X", "SUBSCRIPT_Y", "UNDERLINE_POSITION",
    "UNDERLINE_THICKNESS", "STRIKEOUT_ASCENT", "STRIKEOUT_DESCENT", "ITALIC_ANGLE", "X_HEIGHT", "QUAD_WIDTH", "WEIGHT", "POINT_SIZE", "RESOLUTION",
    "COPYRIGHT", "NOTICE", "FONT_NAME", "FAMILY_NAME", "FULL_NAME", "CAP_HEIGHT", "WM_CLASS", "WM_TRANSIENT_FOR"};
constexpr uint32_t kLastPredefinedAtom = sizeof kPredefinedAtoms / sizeof kPredefinedAtoms[0];      // 68

namespace atom {
constexpr uint32_t kAtom = 4, kCardinal = 6, kString = 31, kWindow = 33, kWmHints = 35, kWmIconName = 37, kWmName = 39, kWmNormalHints = 40,
                   kWmSizeHints = 41, kWmClass = 67, kWmTransientFor = 68, kResourceManager = 23;
}

// The keyboard: keycode = USB HID usage + 8 (the evdev convention is the same offset over different numbers; clients read the map, not the numbers).
constexpr uint8_t kMinKeycode = 8, kMaxKeycode = 255;
// The XInputExtension (X11Input2.cpp): its major opcode and the bases of its (XI 1) events and errors, as QueryExtension gives them.
constexpr uint8_t kXInputOpcode = 131, kXInputFirstEvent = 64, kXInputFirstError = 129;
constexpr uint32_t kKeysymsPerKeycode = 2;
// The keysyms of a keycode (US layout): the plain one and the shifted one; false for a keycode with no key.
bool keysyms_of_keycode(uint8_t keycode, uint32_t* plain, uint32_t* shifted);

// The pointer shape of a glyph of the standard "cursor" font (X11/cursorfont.h index), as the desktop's system cursor (the Windows OCR_ numbers the desktop
// session speaks: 32512 arrow, 32513 I-beam, ...).
uint32_t system_cursor_of_glyph(uint32_t glyph);

// The colour of a name from the X colour database (case and spaces ignored: "Light Gray" = "lightgray"), or "#rgb" / "#rrggbb" / "rgb:r/g/b". 16-bit
// channels. False when unknown.
bool lookup_color(StringView name, uint16_t* red, uint16_t* green, uint16_t* blue);

// Glob match of a font name pattern ('*' any run, '?' one character; case ignored), as ListFonts uses.
bool font_pattern_matches(StringView pattern, StringView name);

}  // namespace x11
