#include "console.hpp"
#include "arch/x86_64/cpu.hpp"

namespace {
efi::SystemTable* g_st;
constexpr uint16_t kDebugcon = 0x402;

void putc(char c) {
  tcos_outb(kDebugcon, static_cast<uint8_t>(c));
  if (g_st && g_st->con_out) {
    efi::Char16 buf[3] = {};
    if (c == '\n') { buf[0] = u'\r'; buf[1] = u'\n'; }
    else           { buf[0] = static_cast<efi::Char16>(c); }
    g_st->con_out->output_string(g_st->con_out, buf);
  }
}
}  // namespace

namespace console {
void init(efi::SystemTable* st) { g_st = st; }

void puts(const char* s) {
  while (*s) putc(*s++);
}

void put_hex(uint64_t v) {
  puts("0x");
  for (int shift = 60; shift >= 0; shift -= 4)
    putc("0123456789abcdef"[(v >> shift) & 0xF]);
}
}  // namespace console
