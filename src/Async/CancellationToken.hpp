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

namespace async {

// Cooperative cancellation, like C#'s CancellationTokenSource/Token. The source owns
// the flag and must outlive every token made from it. cancel() may be called from
// any thread; a task that was passed the token notices it at its next wait.
//
//   async::CancellationSource stop;
//   auto read = stream.read_async(buffer, stop.token());
//   ... stop.cancel();   // the read finishes with StreamStatus::Cancelled
class CancellationToken {
 public:
  constexpr CancellationToken() = default;  // a token that is never cancelled
  bool cancelled() const { return flag_ && __atomic_load_n(flag_, __ATOMIC_ACQUIRE); }
  bool can_be_cancelled() const { return flag_ != nullptr; }

 private:
  friend class CancellationSource;
  explicit constexpr CancellationToken(const bool* flag) : flag_(flag) {}
  const bool* flag_ = nullptr;
};

class CancellationSource {
 public:
  CancellationSource() = default;
  CancellationSource(const CancellationSource&) = delete;
  CancellationSource& operator=(const CancellationSource&) = delete;

  CancellationToken token() const { return CancellationToken(&flag_); }
  void cancel() { __atomic_store_n(&flag_, true, __ATOMIC_RELEASE); }
  bool cancelled() const { return __atomic_load_n(&flag_, __ATOMIC_ACQUIRE); }
  void reset() { __atomic_store_n(&flag_, false, __ATOMIC_RELEASE); }

 private:
  bool flag_ = false;
};

}  // namespace async
