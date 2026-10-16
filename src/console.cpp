#include "console.hpp"
#include "hardware/DebugConsole.hpp"
#include "hardware/TextConsole.hpp"

namespace {
// constexpr-constructed: no static initializers run in this freestanding image.
hardware::DebugConsole g_debug;
hardware::TextConsole g_text;

void putc(char c) {
  g_debug.put(c);
  g_text.put(c);
}
}  // namespace

namespace console {
void init(efi::SystemTable* st) {
  g_debug.initialize();
  g_text.bind(st);
  g_text.initialize();
}

void puts(const char* s) {
  while (*s) putc(*s++);
}

void put_hex(uint64_t v) {
  puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    putc("0123456789abcdef"[(v >> shift) & 0xF]);
}
}  // namespace console
