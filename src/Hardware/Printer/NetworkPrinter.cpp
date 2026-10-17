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

#include "Hardware/Printer/NetworkPrinter.hpp"

namespace hardware {

bool NetworkPrinter::submit(ConstByteView job, const char* job_name) {
  if (count_ >= kQueueDepth || job.empty()) return false;
  UniquePointer<Job> entry = UniquePointer<Job>::make(allocator_, allocator_);
  if (!entry || !entry->data.append(job)) return false;
  uint32_t n = 0;
  for (const char* c = job_name; c && *c && n + 1 < sizeof entry->name; ++c) entry->name[n++] = *c;
  queue_[(head_ + count_) % kQueueDepth] = util::move(entry);
  ++count_;
  return true;
}

bool NetworkPrinter::set_snmp_v3_user(const char* user, network::UsmAuth auth, const char* auth_password, network::UsmPriv priv, const char* priv_password) {
  auto copy = [](char* dst, uint32_t capacity, const char* src) {
    uint32_t n = 0;
    for (; src && src[n] && n + 1 < capacity; ++n) dst[n] = src[n];
    dst[n] = 0;
    return !src || !src[n];
  };
  if (!user || !copy(v3_user_, sizeof v3_user_, user) || !copy(v3_auth_password_, sizeof v3_auth_password_, auth_password) ||
      !copy(v3_priv_password_, sizeof v3_priv_password_, priv_password))
    return false;
  v3_auth_ = auth;
  v3_priv_ = priv;
  v3_ = true;
  monitor_.reset();  // the next report starts from a fresh session (and a fresh engine discovery)
  return true;
}

bool NetworkPrinter::request_report() {
  if (!monitor_) {
    monitor_ = UniquePointer<PrinterMonitor>::make(allocator_, stack_, allocator_, address_, community_);
    if (monitor_ && v3_ && !monitor_->client().set_v3_user(v3_user_, v3_auth_, v3_auth_password_, v3_priv_, v3_priv_password_)) monitor_.reset();
  }
  return monitor_ && monitor_->start();
}

void NetworkPrinter::poll() {
  if (monitor_ && monitor_->busy()) {
    monitor_->poll();
    if (monitor_->succeeded()) {
      report_ = monitor_->report();
      have_report_ = true;
    }
  }
  if (count_ == 0) return;
  Job& job = *queue_[head_];
  if (!transport_) {
    if (protocol_ == PrintProtocol::Lpd)
      transport_ = UniquePointer<IPrintTransport>(UniquePointer<LpdTransport>::make(allocator_, stack_, address_, allocator_, lpd_queue_));
    else
      transport_ = UniquePointer<IPrintTransport>(UniquePointer<RawSocketTransport>::make(allocator_, stack_, address_));
    if (!transport_) return;
    ++job.attempts;
    if (!transport_->start(job.data.view(), job.name)) {
      // (state Failed: handled below)
    }
  }
  transport_->poll();
  const TransportState s = transport_->state();
  if (s == TransportState::Done) {
    ++stats_.jobs_sent;
    stats_.bytes_sent += job.data.size();
    last_failed_ = false;
  } else if (s == TransportState::Failed) {
    if (job.attempts < 3) {
      transport_.reset();  // try the same job again on the next poll
      return;
    }
    ++stats_.jobs_failed;
    last_failed_ = true;
  } else {
    return;
  }
  transport_.reset();
  queue_[head_].reset();
  head_ = (head_ + 1) % kQueueDepth;
  --count_;
}

void NetworkPrinter::cancel_all() {
  if (transport_) transport_->cancel();
  transport_.reset();
  for (UniquePointer<Job>& j : queue_) j.reset();
  head_ = count_ = 0;
}

}  // namespace hardware
