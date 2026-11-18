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

// How the bytes of a Chrome EC host command reach the controller. Both transports kick the EC the same way (the command byte 0xDA to port
// 0x204, then poll the status there until the busy bits clear, then read the result byte at 0x200); they differ in how the packet buffer and
// the memory map are reached:
//   LpcTransport  the buffer is plain I/O at 0x800..0x8FF and the map plain I/O at 0x900 (or 0xE00 on Framework boards). NPCX and ITE ECs.
//   MecTransport  a Microchip MEC152x/172x: the 0x800..0x9FF window sits behind the EMI's address and data registers (ports 0x802..0x807);
//                 the map is in that window at EMI 0x100, or plain I/O when the board remaps it.
// Everything that comes back from the EC is untrusted: a response that is too long, has another struct version or a wrong checksum is
// refused, and waits are bounded by a count of polls (never by time alone), so a hung EC returns Timeout instead of hanging the caller.
//
// Not thread-safe: ChromeEc serializes the callers.
#pragma once
#include <stdint.h>
#include "Hardware/EmbeddedController/ChromeEc/ChromeEcProtocol.hpp"

namespace hardware {

// The machine's 8-bit I/O ports and a short delay, as the EC code sees them (the I/O instructions on the machine, a model in the tests).
class IChromeEcPorts {
 public:
  virtual uint8_t in8(uint16_t port) = 0;
  virtual void out8(uint16_t port, uint8_t value) = 0;
  virtual void delay_us(uint32_t us) = 0;

 protected:
  constexpr IChromeEcPorts() = default;
  ~IChromeEcPorts() = default;
};

class SystemChromeEcPorts final : public IChromeEcPorts {
 public:
  uint8_t in8(uint16_t port) override;
  void out8(uint16_t port, uint8_t value) override;
  void delay_us(uint32_t us) override;
};

enum class EcTransportKind : uint8_t { Lpc, Mec };

// What a transfer ended with (independent of the EC's own result code, which the response carries).
enum class EcTransferStatus : uint8_t {
  Ok,
  BusyBefore,        // the EC was still busy with something before the request was written
  Timeout,           // it did not finish the command within the poll budget
  RequestTooLarge,   // the request does not fit the packet buffer
  BadResponse,       // wrong struct version in the response header
  ResponseTooLarge,  // the data length the EC claims does not fit the caller's buffer (or the packet buffer)
  BadChecksum,       // the response's bytes do not sum to 0
};

const char* ec_transfer_status_name(EcTransferStatus status);

struct EcTransfer {
  EcTransferStatus status = EcTransferStatus::Timeout;
  uint16_t result = 0;          // the EC's EC_RES_* (from the response header, or from the result byte when the EC refused the packet)
  uint32_t data_bytes = 0;      // bytes of response data copied into the caller's buffer
};

class ChromeEcTransport {
 public:
  // The poll budget: kPollLimit polls kPollDelayUs apart (about one second, as cros_ec_lpc waits).
  static constexpr uint32_t kPollLimit = 20000, kPollDelayUs = 50;

  virtual EcTransportKind kind() const = 0;
  // Reads `out.size()` bytes of the memory map from `offset`; false when they are not inside the map.
  [[nodiscard]] bool read_memmap(uint32_t offset, ByteView out);
  // Sends `request` (a whole v3 packet: header and parameters) and copies the response's data into `data`.
  [[nodiscard]] EcTransfer transfer(ConstByteView request, ByteView data);
  uint32_t polls_used() const { return polls_used_; }   // (of the last transfer, for tests)

 protected:
  explicit ChromeEcTransport(IChromeEcPorts& ports) : ports_(ports) {}
  ~ChromeEcTransport() = default;
  // Bytes of the 0x800 packet buffer at `offset` (offset + size <= kHostPacketSize is checked by the caller).
  virtual void read_packet(uint32_t offset, ByteView out) = 0;
  virtual void write_packet(uint32_t offset, ConstByteView bytes) = 0;
  virtual void read_map(uint32_t offset, ByteView out) = 0;
  [[nodiscard]] bool wait_idle();

  IChromeEcPorts& ports_;
  uint32_t polls_used_ = 0;
};

class LpcTransport final : public ChromeEcTransport {
 public:
  LpcTransport(IChromeEcPorts& ports, uint16_t memmap_base) : ChromeEcTransport(ports), memmap_base_(memmap_base) {}
  EcTransportKind kind() const override { return EcTransportKind::Lpc; }
  uint16_t memmap_base() const { return memmap_base_; }

 private:
  void read_packet(uint32_t offset, ByteView out) override;
  void write_packet(uint32_t offset, ConstByteView bytes) override;
  void read_map(uint32_t offset, ByteView out) override;
  uint16_t memmap_base_;
};

// The MEC's EMI, on its own (detection reads the memory map through it before a transport exists).
class MecEmi {
 public:
  explicit MecEmi(IChromeEcPorts& ports, uint16_t base = chromeec::kPortMecEmi) : ports_(ports), base_(base) {}
  // `address` is the EMI address (port - 0x800); the access must stay below kMecWindowEnd - kMecWindowStart.
  [[nodiscard]] bool read(uint32_t address, ByteView out);
  [[nodiscard]] bool write(uint32_t address, ConstByteView bytes);

 private:
  void set_address(uint32_t address, uint8_t access);
  bool fits(uint32_t address, uint64_t size) const;
  IChromeEcPorts& ports_;
  uint16_t base_;
};

class MecTransport final : public ChromeEcTransport {
 public:
  // `plain_memmap_base` 0: the map is read through the EMI (EMI 0x100); otherwise plain I/O at that base.
  MecTransport(IChromeEcPorts& ports, uint16_t plain_memmap_base = 0) : ChromeEcTransport(ports), emi_(ports), plain_memmap_base_(plain_memmap_base) {}
  EcTransportKind kind() const override { return EcTransportKind::Mec; }
  uint16_t plain_memmap_base() const { return plain_memmap_base_; }

 private:
  void read_packet(uint32_t offset, ByteView out) override;
  void write_packet(uint32_t offset, ConstByteView bytes) override;
  void read_map(uint32_t offset, ByteView out) override;
  MecEmi emi_;
  uint16_t plain_memmap_base_;
};

}  // namespace hardware
