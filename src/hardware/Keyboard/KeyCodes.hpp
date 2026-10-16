// Key identities are USB HID usage IDs (usage page 0x07). Every keyboard class
// translates its native codes (PS/2 scan codes, Linux key codes) to these.
#pragma once
#include <stdint.h>

namespace hardware {

namespace key {
constexpr uint8_t kA = 0x04;          // ... kZ = 0x1D
constexpr uint8_t k1 = 0x1E;          // ... k9 = 0x26, k0 = 0x27
constexpr uint8_t kEnter = 0x28;
constexpr uint8_t kEscape = 0x29;
constexpr uint8_t kBackspace = 0x2A;
constexpr uint8_t kTab = 0x2B;
constexpr uint8_t kSpace = 0x2C;
constexpr uint8_t kCapsLock = 0x39;
constexpr uint8_t kF1 = 0x3A;         // ... kF12 = 0x45
constexpr uint8_t kRight = 0x4F;
constexpr uint8_t kLeft = 0x50;
constexpr uint8_t kDown = 0x51;
constexpr uint8_t kUp = 0x52;
constexpr uint8_t kLeftControl = 0xE0;
constexpr uint8_t kLeftShift = 0xE1;
constexpr uint8_t kLeftAlt = 0xE2;
constexpr uint8_t kLeftGui = 0xE3;
constexpr uint8_t kRightControl = 0xE4;
constexpr uint8_t kRightShift = 0xE5;
constexpr uint8_t kRightAlt = 0xE6;
constexpr uint8_t kRightGui = 0xE7;
}  // namespace key

// Modifier bits, in the order of the USB HID boot report (usage 0xE0 + bit).
namespace modifier {
constexpr uint8_t kLeftControl = 1 << 0;
constexpr uint8_t kLeftShift = 1 << 1;
constexpr uint8_t kLeftAlt = 1 << 2;
constexpr uint8_t kLeftGui = 1 << 3;
constexpr uint8_t kRightControl = 1 << 4;
constexpr uint8_t kRightShift = 1 << 5;
constexpr uint8_t kRightAlt = 1 << 6;
constexpr uint8_t kRightGui = 1 << 7;
constexpr uint8_t kShift = kLeftShift | kRightShift;
constexpr uint8_t kControl = kLeftControl | kRightControl;
}  // namespace modifier

// US layout translation: the character a key produces with these modifiers, or
// 0 for keys that produce none (arrows, function keys, modifiers, ...).
char key_to_ascii(uint8_t usage, uint8_t modifiers);

// Native code -> usage translation (0 = no such key).
// Scan code set 1: `code` is the make code without the release bit; `extended`
// means it followed a 0xE0 prefix.
uint8_t usage_from_scan_code_set1(uint8_t code, bool extended);
// Linux input-subsystem key codes (as used by virtio-input).
uint8_t usage_from_linux_key(uint16_t code);

}  // namespace hardware
