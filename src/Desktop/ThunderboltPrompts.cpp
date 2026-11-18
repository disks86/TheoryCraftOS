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

#include "Desktop/ThunderboltPrompts.hpp"
#include "Desktop/Format.hpp"

namespace desktop {

ThunderboltPrompts::ThunderboltPrompts(allocators::IAllocator& allocator, gui::WindowManager& windows, IThunderboltApprovals& approvals)
    : allocator_(allocator), windows_(windows), approvals_(approvals), dialog_(allocator) {}

void ThunderboltPrompts::pump() {
  if (asking_ != 0) {
    if (dialog_.showing()) return;                   // (still being asked)
    switch (dialog_.choice()) {
      case gui::MessageDialog::Choice::Accept: approvals_.approve(asking_, true); break;      // Always allow
      case gui::MessageDialog::Choice::Alternate: approvals_.approve(asking_, false); break;  // Allow once
      case gui::MessageDialog::Choice::Decline: approvals_.deny(asking_); break;              // Don't allow (also Escape and the close button)
    }
    asking_ = 0;
  }
  IThunderboltApprovals::Request request;
  if (!approvals_.next_request(&request) || request.id == 0) return;
  // What happened, what it means and what the user can do, in the user's words; "Don't allow" leaves everything as it is and is the default.
  String line1(allocator_), line2(allocator_);
  format_message(&line1, "\"%1\" from %2 was connected and asks for direct access to this computer's memory.", StringView(request.name), StringView(request.vendor));
  line2.assign("Allow it only if you trust the device. Its USB ports and displays work either way.");
  dialog_.set_texts("Thunderbolt", line1.view(), line2.view(), "Always Allow", "Don't Allow");
  dialog_.set_alternate("Allow Once");
  dialog_.set_default_choice(gui::MessageDialog::Choice::Decline);
  if (!dialog_.show(windows_)) {
    approvals_.deny(request.id);                     // (it cannot be asked: the answer is no)
    return;
  }
  asking_ = request.id;
}

}  // namespace desktop
