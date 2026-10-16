#include "hardware/TextConsole.hpp"

namespace hardware {
void TextConsole::put(char c) const {
  if (!st_ || !st_->con_out) return;
  efi::Char16 buf[3] = {};
  if (c == '\n') { buf[0] = u'\r'; buf[1] = u'\n'; }
  else           { buf[0] = static_cast<efi::Char16>(c); }
  st_->con_out->output_string(st_->con_out, buf);
}
}  // namespace hardware
