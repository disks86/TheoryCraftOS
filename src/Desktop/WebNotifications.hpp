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

// What the desktop does for the notifications of web pages: a notification is a toast, and the question "may this site show notifications?" is a dialog that waits for the user (one
// at a time; a site that is asked about already is not asked twice). pump() runs on the desktop's loop: it takes the answer of the dialog, asks the next question, and tells the
// service worker hub which toasts were clicked or have gone. prompt(), show() and close() may be called from any thread (the pages' and the workers').
#pragma once
#include "Browser/ServiceWorker.hpp"
#include "Desktop/WebBrowser.hpp"
#include "Gui/Controls/ToastCenter.hpp"
#include "Gui/MessageDialog.hpp"
#include "Gui/Windows/WindowManager.hpp"
#include "Kernel/Sync.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"
#include "Utility/UniquePointer.hpp"

namespace desktop {

class WebNotifications final : public INotificationSink {
 public:
  static constexpr uint32_t kMaxQuestions = 8, kDurationMs = 8000;

  WebNotifications(allocators::IAllocator& allocator, gui::ToastCenter& toasts, gui::MessageDialog& dialog, gui::WindowManager& windows, browser::ServiceWorkerHub& hub);

  int prompt(StringView origin) override;
  // The microphone: what the user decided for the origin (1 allowed, 0 blocked, -1 not asked / not answered), and asking (the answer comes through pump(); -1 until then).
  int microphone_state(StringView origin) const;
  int ask_microphone(StringView origin);
  uint32_t show(StringView origin, StringView title, StringView body, bool sticky, StringView actions) override;
  void close(uint32_t number) override;
  void pump();

  uint32_t showing() const;   // notifications on show

 private:
  struct Entry {
    uint32_t number = 0, toast = 0;
    bool clicked = false;   // set by the toast's handlers (atomically)
    int32_t action = -1;    // the button that was clicked (set before `clicked`)
  };
  static void on_click(void* context, gui::Control& sender);
  static void on_action(void* context, gui::Control& sender);

  allocators::IAllocator& allocator_;
  gui::ToastCenter& toasts_;
  gui::MessageDialog& dialog_;
  gui::WindowManager& windows_;
  browser::ServiceWorkerHub& hub_;
  mutable kernel::RecursiveMutex mutex_;
  List<UniquePointer<Entry>> entries_;
  List<String> questions_;   // the origins waiting to be asked about, each with a prefix: "n:" for notifications, "m:" for the microphone
  String asking_;            // the question the dialog is about (same form)
  List<String> mic_allowed_, mic_blocked_;
  uint32_t next_number_ = 1;
};

}  // namespace desktop
