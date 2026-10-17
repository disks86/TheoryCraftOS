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

#pragma once
#include "Hardware/Printer/PrinterTypes.hpp"
#include "Utility/ArrayView.hpp"

namespace hardware {

enum class PrinterState : uint8_t {
  Ready,   // idle, nothing queued
  Busy,    // a job is being sent or waits to be
  Error,   // the last job could not be delivered (cleared by the next job that is)
};

struct PrinterStatistics {
  uint64_t jobs_sent = 0;
  uint64_t jobs_failed = 0;
  uint64_t bytes_sent = 0;
};

// A printer, whatever it hangs off. Like the other devices it is polled and never blocks: submit() queues a finished job (the
// bytes PrintJob builds, in a language the printer reads) and poll() moves it along.
class IPrinter {
 public:
  virtual const char* name() const = 0;
  virtual bool initialize() = 0;
  virtual void shutdown() = 0;
  virtual const PrinterInfo& info() const = 0;
  virtual PrinterState state() const = 0;
  // Takes a copy of `job` and queues it. False if the queue is full or memory ran out.
  virtual bool submit(ConstByteView job, const char* job_name) = 0;
  virtual void poll() = 0;
  virtual uint32_t pending_jobs() const = 0;
  // Drops every queued job and the one in flight.
  virtual void cancel_all() = 0;
  virtual PrinterStatistics statistics() const = 0;

 protected:
  constexpr IPrinter() = default;
  ~IPrinter() = default;
};

}  // namespace hardware
