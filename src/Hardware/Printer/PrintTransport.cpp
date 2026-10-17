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

#include "Hardware/Printer/PrintTransport.hpp"

namespace hardware {

using namespace network;

namespace {
constexpr uint64_t kConnectTimeoutMs = 10000;
constexpr uint64_t kStallTimeoutMs = 30000;  // no progress at all (a printer out of paper or memory can hold the connection a while)
}  // namespace

// ---- raw port 9100

bool RawSocketTransport::start(ConstByteView job, const char*) {
  if (state_ == TransportState::Connecting || state_ == TransportState::Sending || state_ == TransportState::Finishing) return false;
  socket_ = stack_.tcp_open();
  if (!socket_ || !socket_->connect(printer_, port_)) {
    state_ = TransportState::Failed;
    return false;
  }
  job_ = job;
  sent_ = 0;
  deadline_ms_ = stack_.now_ms() + kConnectTimeoutMs;
  last_progress_ms_ = stack_.now_ms();
  state_ = TransportState::Connecting;
  return true;
}

void RawSocketTransport::poll() {
  if (!socket_) return;
  const uint64_t now = stack_.now_ms();
  switch (state_) {
    case TransportState::Connecting:
      if (socket_->failed() || now > deadline_ms_) {
        socket_->abort();
        state_ = TransportState::Failed;
      } else if (socket_->is_connected()) {
        last_progress_ms_ = now;
        state_ = TransportState::Sending;
      }
      break;
    case TransportState::Sending: {
      if (socket_->failed()) {
        state_ = TransportState::Failed;
        break;
      }
      while (sent_ < job_.size()) {
        const uint32_t n = socket_->send(job_.subview(sent_));
        if (n == 0) break;
        sent_ += n;
        last_progress_ms_ = now;
      }
      if (sent_ == job_.size()) {
        socket_->close();  // a FIN after the data: the printer sees the end of the job
        state_ = TransportState::Finishing;
      } else if (now - last_progress_ms_ > kStallTimeoutMs) {
        socket_->abort();
        state_ = TransportState::Failed;
      }
      break;
    }
    case TransportState::Finishing:
      if (socket_->failed()) {
        state_ = TransportState::Failed;
      } else if (socket_->unsent_or_unacknowledged() == 0) {
        state_ = TransportState::Done;  // the printer has all of it
      } else if (now - last_progress_ms_ > kStallTimeoutMs) {
        socket_->abort();
        state_ = TransportState::Failed;
      }
      break;
    default:
      break;
  }
}

void RawSocketTransport::cancel() {
  if (socket_) socket_->abort();
  socket_ = SharedPointer<ITcpSocket>();
  state_ = TransportState::Idle;
}

// ---- LPD

uint32_t LpdTransport::next_job_number_ = 1;

static void append_text(ByteBuffer* b, const char* s) {
  while (*s) b->put8(static_cast<uint8_t>(*s++));
}
static void append_number(ByteBuffer* b, uint64_t v) {
  char d[24];
  int n = 0;
  do {
    d[n++] = static_cast<char>('0' + v % 10);
    v /= 10;
  } while (v);
  while (n) b->put8(static_cast<uint8_t>(d[--n]));
}

bool LpdTransport::start(ConstByteView job, const char* job_name) {
  if (state_ == TransportState::Connecting || state_ == TransportState::Sending || state_ == TransportState::Finishing) return false;
  socket_ = stack_.tcp_open();
  if (!socket_ || !socket_->connect(printer_, kPort)) {
    state_ = TransportState::Failed;
    return false;
  }
  job_ = job;
  uint32_t n = 0;
  for (const char* c = job_name; c && *c && n < 63; ++c) name_[n++] = (*c >= 0x21 && *c < 0x7f) ? *c : '_';
  name_[n] = 0;
  job_number_ = next_job_number_;
  next_job_number_ = next_job_number_ % 999 + 1;
  // The control file: host, user, job name, and which data file to print as raw (l) and delete afterwards (U, N).
  control_.clear();
  append_text(&control_, "H");
  append_text(&control_, host_);
  append_text(&control_, "\nP");
  append_text(&control_, user_);
  append_text(&control_, "\nJ");
  append_text(&control_, name_[0] ? name_ : "job");
  append_text(&control_, "\nldfA");
  append_number(&control_, job_number_ / 100);
  append_number(&control_, job_number_ / 10 % 10);
  append_number(&control_, job_number_ % 10);
  append_text(&control_, host_);
  append_text(&control_, "\nUdfA");
  append_number(&control_, job_number_ / 100);
  append_number(&control_, job_number_ / 10 % 10);
  append_number(&control_, job_number_ % 10);
  append_text(&control_, host_);
  append_text(&control_, "\nN");
  append_text(&control_, name_[0] ? name_ : "job");
  append_text(&control_, "\n");
  step_ = 0;
  awaiting_ack_ = false;
  offset_ = 0;
  sent_ = 0;
  deadline_ms_ = stack_.now_ms() + kConnectTimeoutMs;
  state_ = TransportState::Connecting;
  return true;
}

// The command bytes of the current step (not the control/data file contents themselves).
bool LpdTransport::build_segment() {
  segment_.clear();
  auto file_name = [&](char kind) {
    append_text(&segment_, " ");
    append_text(&segment_, kind == 'c' ? "cfA" : "dfA");
    append_number(&segment_, job_number_ / 100);
    append_number(&segment_, job_number_ / 10 % 10);
    append_number(&segment_, job_number_ % 10);
    append_text(&segment_, host_);
    append_text(&segment_, "\n");
  };
  switch (step_) {
    case 0:
      append_text(&segment_, "\x02");
      append_text(&segment_, queue_);
      append_text(&segment_, "\n");
      break;
    case 1:
      append_text(&segment_, "\x02");
      append_number(&segment_, control_.size());
      file_name('c');
      break;
    case 3:
      append_text(&segment_, "\x03");
      append_number(&segment_, job_.size());
      file_name('d');
      break;
    default:
      break;  // steps 2 and 4 send the file itself
  }
  return true;
}

void LpdTransport::poll() {
  if (!socket_) return;
  const uint64_t now = stack_.now_ms();
  if (state_ == TransportState::Connecting) {
    if (socket_->failed() || now > deadline_ms_) {
      socket_->abort();
      state_ = TransportState::Failed;
    } else if (socket_->is_connected()) {
      state_ = TransportState::Sending;
      build_segment();
      offset_ = 0;
      deadline_ms_ = now + kStallTimeoutMs;
    }
    return;
  }
  if (state_ != TransportState::Sending && state_ != TransportState::Finishing) return;
  if (socket_->failed() || now > deadline_ms_) {
    socket_->abort();
    state_ = TransportState::Failed;
    return;
  }
  for (int guard = 0; guard < 16; ++guard) {
    if (awaiting_ack_) {
      uint8_t ack;
      if (socket_->receive(ByteView(&ack, 1)) == 0) return;
      if (ack != 0) {  // the daemon refused: wrong queue, no room
        socket_->abort();
        state_ = TransportState::Failed;
        return;
      }
      awaiting_ack_ = false;
      deadline_ms_ = now + kStallTimeoutMs;
      if (++step_ == 5) {
        socket_->close();
        state_ = TransportState::Done;
        return;
      }
      build_segment();
      offset_ = 0;
    }
    // What this step sends: its command, or the file followed by a zero byte.
    ConstByteView body;
    bool file_step = step_ == 2 || step_ == 4;
    if (file_step) body = step_ == 2 ? control_.view() : job_;
    else body = segment_.view();
    const uint64_t total = body.size() + (file_step ? 1 : 0);
    while (offset_ < total) {
      uint32_t n;
      if (offset_ < body.size()) {
        n = socket_->send(body.subview(offset_));
      } else {
        const uint8_t zero = 0;
        n = socket_->send(ConstByteView(&zero, 1));
      }
      if (n == 0) return;
      offset_ += n;
      if (step_ == 4 && offset_ <= body.size()) sent_ += n;
      deadline_ms_ = now + kStallTimeoutMs;
    }
    awaiting_ack_ = true;
    state_ = step_ == 4 ? TransportState::Finishing : TransportState::Sending;
  }
}

void LpdTransport::cancel() {
  if (socket_) socket_->abort();
  socket_ = SharedPointer<ITcpSocket>();
  state_ = TransportState::Idle;
}

}  // namespace hardware
