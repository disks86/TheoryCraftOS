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

#include "Desktop/CopyProgressDialog.hpp"
#include "Desktop/FileStatus.hpp"
#include "Desktop/Format.hpp"
#include "Gui/Window.hpp"
#include "Kernel/Sync.hpp"

namespace desktop {

using namespace gui;
using filesystem::Status;

CopyProgressDialog::CopyProgressDialog(allocators::IAllocator& allocator)
    : Frame(allocator), heading_(allocator), file_(allocator), bytes_(allocator), bar_(allocator), buttons_(allocator), gap_(allocator),
      cancel_button_(allocator), source_(allocator), target_(allocator), message_(allocator), scratch_(allocator), size_a_(allocator),
      size_b_(allocator) {
  set_title("Copying");
  set_minimize_enabled(false);
  set_maximize_enabled(false);
  set_close_enabled(false);   // Cancel is the way out: closing would leave the copy running unseen
  set_resizable(false);
  set_padding(Insets::all(10));

  heading_.set_dock(DockStyle::Top);
  heading_.set_size(0, 22);
  file_.set_dock(DockStyle::Top);
  file_.set_size(0, 22);
  bar_.set_dock(DockStyle::Top);
  bar_.set_size(0, 22);
  bytes_.set_dock(DockStyle::Top);
  bytes_.set_size(0, 22);
  buttons_.set_style(PanelStyle::Transparent);
  buttons_.set_dock(DockStyle::Bottom);
  buttons_.set_size(0, 28);
  gap_.set_style(PanelStyle::Transparent);
  gap_.set_dock(DockStyle::Fill);
  cancel_button_.set_dock(DockStyle::Right);
  cancel_button_.set_size(104, 0);
  cancel_button_.set_text("Cancel");
  cancel_button_.on_click(Handler::immediate(&CopyProgressDialog::on_cancel, this));
  buttons_.add(&cancel_button_);
  buttons_.add(&gap_);

  add(&buttons_);
  add(&heading_);
  add(&file_);
  add(&bar_);
  add(&bytes_);
}

void CopyProgressDialog::on_cancel(void* self, Control&) {
  auto* d = static_cast<CopyProgressDialog*>(self);
  d->cancel();
  d->cancel_button_.set_enabled(false);
  d->heading_.set_text("Cancelling...");
}

bool CopyProgressDialog::wait_until_idle(uint64_t timeout_us) {
  for (uint64_t waited = 0;; waited += 500) {
    if (!busy()) return true;
    if (waited >= timeout_us) return false;
    kernel::sleep_us(500);
  }
}

bool CopyProgressDialog::start(WindowManager& manager, filesystem::IFileSystem& fs, StringView source, StringView target, bool remove_source) {
  if (__atomic_exchange_n(&busy_, true, __ATOMIC_ACQ_REL)) return false;
  fs_ = &fs;
  manager_ = &manager;
  remove_source_ = remove_source;
  result_ = Status::Ok;
  message_.clear();
  __atomic_store_n(&cancel_, false, __ATOMIC_RELEASE);
  if (!source_.assign(source) || !target_.assign(target)) {
    __atomic_store_n(&busy_, false, __ATOMIC_RELEASE);
    return false;
  }
  set_title(remove_source ? "Moving" : "Copying");
  heading_.set_text("Preparing...");
  file_.set_text(source.substr(source.rfind('/') + 1));
  bytes_.set_text("");
  bar_.set_maximum(0);
  bar_.set_value(0);
  cancel_button_.set_enabled(true);
  const int32_t area_w = static_cast<int32_t>(manager.bounds().width), area_h = static_cast<int32_t>(manager.bounds().height);
  set_bounds({(area_w - kWidth) / 2 > 0 ? (area_w - kWidth) / 2 : 0, (area_h - kHeight) / 3 > 0 ? (area_h - kHeight) / 3 : 0,
              static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight)});
  if (!manager.add_window(this)) {
    __atomic_store_n(&busy_, false, __ATOMIC_RELEASE);
    return false;
  }
  Window* w = window();
  if (!w) {   // (no window to run the work for)
    manager.remove_window(this);
    __atomic_store_n(&busy_, false, __ATOMIC_RELEASE);
    return false;
  }
  w->layout();
  w->run_async(&CopyProgressDialog::work, this, {&CopyProgressDialog::done, this});
  return true;
}

void CopyProgressDialog::work(void* self) { static_cast<CopyProgressDialog*>(self)->run(); }

void CopyProgressDialog::on_file(void* self, StringView path, uint64_t files_done, uint64_t files_total) {
  auto* d = static_cast<CopyProgressDialog*>(self);
  d->scratch_.clear();
  d->scratch_.append(d->remove_source_ ? "Moving " : "Copying ");
  d->scratch_.append_uint(files_done + 1);
  d->scratch_.append(" of ");
  d->scratch_.append_uint(files_total);
  d->scratch_.append(files_total == 1 ? " file" : " files");
  d->heading_.set_text(d->scratch_.view());
  d->file_.set_text(path.substr(path.rfind('/') + 1));
}

void CopyProgressDialog::on_bytes(void* self, uint64_t bytes_done, uint64_t bytes_total) {
  auto* d = static_cast<CopyProgressDialog*>(self);
  d->bar_.set_maximum(bytes_total);
  d->bar_.set_value(bytes_done);
  format_size(bytes_done, &d->size_a_);
  format_size(bytes_total, &d->size_b_);
  d->scratch_.clear();
  d->scratch_.append(d->size_a_.view());
  d->scratch_.append(" of ");
  d->scratch_.append(d->size_b_.view());
  d->bytes_.set_text(d->scratch_.view());
}

// On the background thread.
void CopyProgressDialog::run() {
  FileTransfer transfer(allocator(), *fs_);
  transfer.set_callbacks(&CopyProgressDialog::on_file, &CopyProgressDialog::on_bytes, this);
  transfer.set_cancel_flag(&cancel_);
  Status status = transfer.measure(source_.view());
  if (status == Status::Ok) {
    bar_.set_maximum(transfer.bytes_total());
    status = transfer.copy(source_.view(), target_.view());
  }
  if (status == Status::Ok && remove_source_) {
    heading_.set_text("Removing the originals...");
    status = FileTransfer::remove_tree(allocator(), *fs_, source_.view());
  } else if (status != Status::Ok) {
    FileTransfer::remove_tree(allocator(), *fs_, target_.view());   // no half-copied leftovers
  }
  fs_->flush();
  result_ = status;
  message_.clear();
  if (status != Status::Ok && status != FileTransfer::kCancelled) {
    message_.append(remove_source_ ? "Cannot move " : "Cannot copy ");
    message_.append(source_.view().substr(source_.view().rfind('/') + 1));
    message_.append(": ");
    message_.append(describe_status(status));
  }
}

// On the handler thread, after run().
void CopyProgressDialog::done(void* self, Control&) {
  auto* d = static_cast<CopyProgressDialog*>(self);
  if (d->manager_) d->manager_->remove_window(d);
  __atomic_store_n(&d->busy_, false, __ATOMIC_RELEASE);
  d->fire(d->finished_);
}

}  // namespace desktop
