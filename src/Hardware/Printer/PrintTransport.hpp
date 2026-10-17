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
#include "Network/INetworkStack.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

enum class TransportState : uint8_t { Idle, Connecting, Sending, Finishing, Done, Failed };

// Moves one job's bytes to a printer over the network. Polled: the owner calls poll() while INetworkStack::poll() runs.
class IPrintTransport {
 public:
  // Starts delivering `job`, which must stay valid until the state is Done or Failed.
  virtual bool start(ConstByteView job, const char* job_name) = 0;
  virtual void poll() = 0;
  virtual TransportState state() const = 0;
  virtual uint64_t bytes_sent() const = 0;
  virtual void cancel() = 0;

 protected:
  IPrintTransport() = default;
  ~IPrintTransport() = default;
};

// "Raw" printing (HP JetDirect / AppSocket, TCP port 9100): connect and send the job; the printer takes it as it comes.
class RawSocketTransport final : public IPrintTransport {
 public:
  static constexpr uint16_t kPort = 9100;
  RawSocketTransport(network::INetworkStack& stack, network::Ipv4Address printer, uint16_t port = kPort) : stack_(stack), printer_(printer), port_(port) {}
  bool start(ConstByteView job, const char* job_name) override;
  void poll() override;
  TransportState state() const override { return state_; }
  uint64_t bytes_sent() const override { return sent_; }
  void cancel() override;

 private:
  network::INetworkStack& stack_;
  network::Ipv4Address printer_;
  uint16_t port_;
  SharedPointer<network::ITcpSocket> socket_;
  ConstByteView job_;
  uint64_t sent_ = 0;
  uint64_t deadline_ms_ = 0, last_progress_ms_ = 0;
  TransportState state_ = TransportState::Idle;
};

// The Line Printer Daemon protocol (RFC 1179, TCP port 515): a control file and the data file are handed to a named queue, each
// step acknowledged by the printer. Brother print servers call the pass-through queue "BINARY_P1".
class LpdTransport final : public IPrintTransport {
 public:
  static constexpr uint16_t kPort = 515;
  LpdTransport(network::INetworkStack& stack, network::Ipv4Address printer, allocators::IAllocator& allocator, const char* queue = "BINARY_P1",
               const char* host_name = "theorycraftos", const char* user = "user")
      : stack_(stack), printer_(printer), allocator_(allocator), queue_(queue), host_(host_name), user_(user), segment_(allocator), control_(allocator) {}
  bool start(ConstByteView job, const char* job_name) override;
  void poll() override;
  TransportState state() const override { return state_; }
  uint64_t bytes_sent() const override { return sent_; }
  void cancel() override;

 private:
  bool build_segment();  // the bytes of the current step's command
  network::INetworkStack& stack_;
  network::Ipv4Address printer_;
  allocators::IAllocator& allocator_;
  const char* queue_;
  const char* host_;
  const char* user_;
  ByteBuffer segment_, control_;
  SharedPointer<network::ITcpSocket> socket_;
  ConstByteView job_;
  char name_[64] = {};
  uint32_t step_ = 0;       // 0 receive-job command, 1 control file header, 2 control file, 3 data header, 4 data
  bool awaiting_ack_ = false;
  uint64_t offset_ = 0;     // within the current step's bytes
  uint64_t sent_ = 0;
  uint64_t deadline_ms_ = 0;
  uint32_t job_number_ = 0;
  TransportState state_ = TransportState::Idle;
  static uint32_t next_job_number_;
};

}  // namespace hardware
