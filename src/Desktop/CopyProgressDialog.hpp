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

// The window that shows a copy or move between volumes, or from and to a remote share, while it runs: what it is doing ("Moving 3 of 12 files"),
// the file in progress, a progress bar with the bytes done, and a Cancel button. One transfer runs at a time (start() says no while busy). The
// work is done on a background thread so the desktop stays usable - the window is not modal. When it ends - done, failed or cancelled - the window
// closes, a failed or cancelled copy has its partial copy removed again, and the finished handler runs (result() and message() say how it went).
// A move removes the original only after everything was copied. The dialog must outlive the transfer: call cancel() and wait_until_idle() first.
#pragma once
#include "Desktop/FileTransfer.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/Button.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/ProgressBar.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Gui/Windows/WindowManager.hpp"

namespace desktop {

class CopyProgressDialog : public gui::Frame {
 public:
  static constexpr int32_t kWidth = 420, kHeight = 170;

  explicit CopyProgressDialog(allocators::IAllocator& allocator);

  // Opens the window in `manager` and copies `source` (a file or folder) to `target` (which must not exist) on `fs`; with `remove_source` it is a
  // move. False if a transfer is already running or the window could not be shown. `fs` must outlive the transfer.
  bool start(gui::WindowManager& manager, filesystem::IFileSystem& fs, StringView source, StringView target, bool remove_source);
  // Asks the running transfer to stop (the Cancel button does this).
  void cancel() { __atomic_store_n(&cancel_, true, __ATOMIC_RELEASE); }
  bool busy() const { return __atomic_load_n(&busy_, __ATOMIC_ACQUIRE); }
  bool cancelled() const { return __atomic_load_n(&cancel_, __ATOMIC_ACQUIRE); }
  // Waits for the transfer to end; false on timeout.
  bool wait_until_idle(uint64_t timeout_us);

  // Fired when a transfer ended (after the window closed).
  void on_finished(gui::Handler handler) { finished_ = handler; }
  // How the last transfer ended: Ok, kCancelled or the error. message() is a sentence for a notice (empty when it went well or was cancelled).
  filesystem::Status result() const { return result_; }
  StringView message() const { return message_.view(); }

  gui::Label& heading_label() { return heading_; }
  gui::Label& file_label() { return file_; }
  gui::Label& bytes_label() { return bytes_; }
  gui::ProgressBar& bar() { return bar_; }
  gui::Button& cancel_button() { return cancel_button_; }

 private:
  static void on_cancel(void* self, gui::Control&);
  static void work(void* self);
  static void done(void* self, gui::Control&);
  static void on_file(void* self, StringView path, uint64_t files_done, uint64_t files_total);
  static void on_bytes(void* self, uint64_t bytes_done, uint64_t bytes_total);
  void run();

  gui::Label heading_, file_, bytes_;
  gui::ProgressBar bar_;
  gui::Panel buttons_, gap_;
  gui::Button cancel_button_;
  gui::Handler finished_;
  filesystem::IFileSystem* fs_ = nullptr;
  gui::WindowManager* manager_ = nullptr;
  String source_, target_, message_, scratch_, size_a_, size_b_;
  bool remove_source_ = false;
  bool busy_ = false, cancel_ = false;
  filesystem::Status result_ = filesystem::Status::Ok;
};

}  // namespace desktop
