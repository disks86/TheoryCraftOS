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

#include <stdint.h>
#include <string.h>
#include <functional>
#include <vector>
#include "Hardware/AudioDevice/Intel/IntelHdaAudioDevice.hpp"

namespace hardware {

// A High Definition Audio controller with one codec, simulated at the register level for the driver: the reset handshakes, the command ring (CORB, read from the driver's memory) answered
// into the response ring (RIRB, written there), and the stream descriptors as plain registers; and the codec: a graphics card's HDMI / DisplayPort codec (converters 2, 4, 6 and pins
// 3, 5, 7, each pin able to take any converter) or, with `analog_pin`, one with a line out pin (node 8) as well. The codec keeps the state the driver sets so a test can look at it, and every
// verb it gets is logged. Written from the High Definition Audio specification, independently of the driver.
struct FakeHda final : IHdaRegisters {
  // the codec's nodes
  static constexpr uint32_t kRoot = 0, kFunctionGroup = 1;
  static constexpr uint32_t kConverters[3] = {2, 4, 6}, kPins[3] = {3, 5, 7}, kLineOutPin = 8;

  struct Verb {
    uint32_t nid, command;     // the 12 or 4 bit verb and its payload together (the low 20 bits of the CORB entry)
  };
  struct Pin {
    uint32_t sense = 0;        // what GET_PIN_SENSE answers: presence detect (bit 31), ELD valid (bit 30)
    uint32_t control = 0;      // SET_PIN_WIDGET_CONTROL
    uint32_t select = 0;       // SET_CONNECTION_SELECT
    uint32_t dip_index = 0;
    uint32_t dip_xmit = 0;
    std::vector<uint8_t> dip;  // the bytes written to the DIP buffer since the last index set
    uint32_t power = 3;        // D3 until the driver says otherwise
    uint32_t eapd = 0;
  };
  struct Converter {
    uint32_t stream = 0;       // SET_CHANNEL_STREAM_ID: stream tag in bits 7:4, channel in 3:0
    uint32_t format = 0;       // SET_CONVERTER_FORMAT
    uint32_t digital = 0;      // SET_DIGI_CONVERT_1
    uint32_t channel_count = 0;
    uint32_t power = 3;
  };

  // configuration
  bool analog_pin = false;            // add the line out pin (node 8)
  bool no_codec = false;              // STATESTS stays zero
  bool no_output_streams = false;     // GCAP says no output stream
  bool no_converters = false;         // the pins have no connections
  bool codec_ignores_rates = false;   // the converters cannot play 48 kHz 16 bit
  bool pin_config_none[3] = {false, false, false};     // the pin's connectivity is "no physical connection"
  std::function<uint32_t(uint32_t)> sense;             // the sense of a pin by index (0..2) if set, else Pin::sense

  // state
  Pin pins[3];
  Pin line_out;
  Converter converters[3];
  Converter line_dac;
  std::vector<Verb> log;
  uint8_t mem[0x400] = {};
  bool in_reset = true;
  uint32_t corb_rp = 0, rirb_wp = 0;
  bool corb_rp_reset = false;
  uint32_t responses = 0;
  uint32_t stream_resets = 0;

  uint32_t corb_entries() const { return mem[0x4E] & 3 ? ((mem[0x4E] & 3) == 1 ? 16u : 256u) : 2u; }
  uint32_t rirb_entries() const { return mem[0x5E] & 3 ? ((mem[0x5E] & 3) == 1 ? 16u : 256u) : 2u; }

  FakeHda() {
    mem[0x4E] = 0x40;       // CORBSIZE: 256 entries supported
    mem[0x5E] = 0x40;
  }

  // ---- the controller's registers
  uint32_t load(uint32_t reg, uint32_t bytes) const {
    uint32_t v = 0;
    for (uint32_t i = 0; i < bytes; ++i) v |= uint32_t{mem[reg + i]} << (8 * i);
    return v;
  }
  void store(uint32_t reg, uint32_t bytes, uint32_t v) {
    for (uint32_t i = 0; i < bytes; ++i) mem[reg + i] = static_cast<uint8_t>(v >> (8 * i));
  }
  uint8_t read8(uint32_t reg) override { return static_cast<uint8_t>(read(reg, 1)); }
  uint16_t read16(uint32_t reg) override { return static_cast<uint16_t>(read(reg, 2)); }
  uint32_t read32(uint32_t reg) override { return read(reg, 4); }
  void write8(uint32_t reg, uint8_t v) override { write(reg, 1, v); }
  void write16(uint32_t reg, uint16_t v) override { write(reg, 2, v); }
  void write32(uint32_t reg, uint32_t v) override { write(reg, 4, v); }

  uint32_t read(uint32_t reg, uint32_t bytes) {
    if (reg == 0x00 && bytes == 2) return no_output_streams ? 0x0001u : 0x1001u;     // GCAP: one output stream, none in, 64 bit addresses
    if (reg == 0x0E) return in_reset || no_codec ? 0 : 1;                           // STATESTS: codec 0
    if (reg == 0x4A) return corb_rp | (corb_rp_reset ? 0x8000u : 0);                 // CORBRP
    if (reg == 0x58) return rirb_wp;                                                 // RIRBWP
    return load(reg, bytes);
  }
  void write(uint32_t reg, uint32_t bytes, uint32_t v) {
    if (reg == 0x08) {                                                               // GCTL: bit 0 is the controller reset (0 = held in reset)
      in_reset = !(v & 1);
      store(reg, bytes, v & 1);
      if (in_reset) {
        corb_rp = rirb_wp = 0;
        mem[0x4C] = mem[0x5C] = 0;
      }
      return;
    }
    if (reg == 0x4A) {                                                               // CORBRP: bit 15 resets the read pointer; it reads back set until it is cleared
      corb_rp_reset = v & 0x8000;
      if (corb_rp_reset) corb_rp = 0;
      return;
    }
    if (reg == 0x58) {                                                               // RIRBWP: bit 15 resets the write pointer
      if (v & 0x8000) rirb_wp = 0;
      return;
    }
    if (reg == 0x48) {                                                               // CORBWP: the commands up to it are run
      store(reg, bytes, v);
      run_commands(v & 0xFF);
      return;
    }
    if (reg == 0x5D) {                                                               // RIRBSTS: bits clear when a 1 is written
      mem[reg] = static_cast<uint8_t>(mem[reg] & ~v);
      return;
    }
    if (reg == 0x80 && bytes == 1) {                                                 // output stream 0's control: bit 0 is the reset handshake
      if ((v & 1) && !(mem[reg] & 1)) ++stream_resets;
      store(reg, bytes, v);
      return;
    }
    store(reg, bytes, v);
  }

  // The commands the driver put in the CORB, up to `write_pointer`
  void run_commands(uint32_t write_pointer) {
    const uint64_t corb = load(0x40, 4) | (uint64_t{load(0x44, 4)} << 32);
    const uint64_t rirb = load(0x50, 4) | (uint64_t{load(0x54, 4)} << 32);
    const uint32_t n = corb_entries(), m = rirb_entries();
    while (corb_rp != write_pointer) {
      corb_rp = (corb_rp + 1) % n;
      uint32_t entry;
      memcpy(&entry, reinterpret_cast<const void*>(corb + uint64_t{corb_rp} * 4), 4);
      const uint32_t codec = entry >> 28, nid = (entry >> 20) & 0xFF, command = entry & 0xFFFFF;
      uint32_t response = 0;
      if (codec == 0) response = codec_verb(nid, command);
      rirb_wp = (rirb_wp + 1) % m;
      uint32_t slot[2] = {response, codec};
      memcpy(reinterpret_cast<void*>(rirb + uint64_t{rirb_wp} * 8), slot, 8);
      mem[0x5D] |= 1;
      ++responses;
    }
  }

  // ---- the codec
  uint32_t pin_index(uint32_t nid) const { return nid == 3 ? 0 : nid == 5 ? 1 : nid == 7 ? 2 : 0xFF; }
  Pin* pin_of(uint32_t nid) {
    if (nid == kLineOutPin && analog_pin) return &line_out;
    const uint32_t i = pin_index(nid);
    return i == 0xFF ? nullptr : &pins[i];
  }
  Converter* converter_of(uint32_t nid) {
    if (nid == 8 + 1 && analog_pin) return &line_dac;      // the analog converter, node 9
    for (uint32_t i = 0; i < 3; ++i)
      if (kConverters[i] == nid) return &converters[i];
    return nullptr;
  }
  bool is_pin(uint32_t nid) const { return pin_index(nid) != 0xFF || (analog_pin && nid == kLineOutPin); }
  bool is_converter(uint32_t nid) const { return nid == 2 || nid == 4 || nid == 6 || (analog_pin && nid == 9); }
  uint32_t last_widget() const { return analog_pin ? 9 : 7; }

  std::vector<uint8_t> connection_list(uint32_t nid) const {
    if (!is_pin(nid) || no_converters) return {};
    if (nid == kLineOutPin) return {9};
    return {2, 4, 6};
  }

  uint32_t codec_verb(uint32_t nid, uint32_t command) {
    log.push_back({nid, command});
    const uint32_t verb12 = command >> 8, payload8 = command & 0xFF, verb4 = command >> 16, payload16 = command & 0xFFFF;
    if (verb12 == 0xF00) return parameter(nid, payload8);
    if (verb12 == 0xF02) {                                                           // GET_CONNECTION_LIST: four entries from the index
      const std::vector<uint8_t> list = connection_list(nid);
      uint32_t word = 0;
      for (uint32_t i = 0; i < 4; ++i)
        if (payload8 + i < list.size()) word |= uint32_t{list[payload8 + i]} << (8 * i);
      return word;
    }
    if (verb12 == 0xF1C) {                                                           // GET_CONFIG_DEFAULT: connectivity (31:30), device type (23:20)
      if (nid == kLineOutPin && analog_pin) return 0u << 30 | 0u << 20;              // jack, line out
      const uint32_t i = pin_index(nid);
      if (i == 0xFF) return 0;
      return (pin_config_none[i] ? 1u << 30 : 0u) | (5u << 20);                      // digital other out
    }
    if (verb12 == 0xF09) {                                                           // GET_PIN_SENSE
      Pin* p = pin_of(nid);
      if (!p) return 0;
      const uint32_t i = pin_index(nid);
      return i != 0xFF && sense ? sense(i) : p->sense;
    }
    if (verb12 == 0x705) {                                                           // SET_POWER_STATE
      if (Pin* p = pin_of(nid)) p->power = payload8;
      if (Converter* c = converter_of(nid)) c->power = payload8;
      return 0;
    }
    if (verb12 == 0x701) {
      if (Pin* p = pin_of(nid)) p->select = payload8;
      return 0;
    }
    if (verb12 == 0x707) {
      if (Pin* p = pin_of(nid)) p->control = payload8;
      return 0;
    }
    if (verb12 == 0x70C) {
      if (Pin* p = pin_of(nid)) p->eapd = payload8;
      return 0;
    }
    if (verb12 == 0x706) {
      if (Converter* c = converter_of(nid)) c->stream = payload8;
      return 0;
    }
    if (verb12 == 0x70D) {
      if (Converter* c = converter_of(nid)) c->digital = payload8;
      return 0;
    }
    if (verb12 == 0x72D) {
      if (Converter* c = converter_of(nid)) c->channel_count = payload8;
      return 0;
    }
    if (verb4 == 0x2) {
      if (Converter* c = converter_of(nid)) c->format = payload16;
      return 0;
    }
    if (verb12 == 0x730) {
      if (Pin* p = pin_of(nid)) {
        p->dip_index = payload8;
        p->dip.clear();
      }
      return 0;
    }
    if (verb12 == 0x731) {
      if (Pin* p = pin_of(nid)) p->dip.push_back(static_cast<uint8_t>(payload8));
      return 0;
    }
    if (verb12 == 0x732) {
      if (Pin* p = pin_of(nid)) p->dip_xmit = payload8;
      return 0;
    }
    return 0;      // amplifier gains and the like: accepted
  }

  uint32_t parameter(uint32_t nid, uint32_t id) {
    if (nid == kRoot) {
      if (id == 0x00) return 0x1002AA01;                                             // vendor and device
      if (id == 0x04) return (kFunctionGroup << 16) | 1;                             // one function group, node 1
      return 0;
    }
    if (nid == kFunctionGroup) {
      if (id == 0x04) return (2u << 16) | (last_widget() - 1);                       // widgets from node 2
      if (id == 0x05) return 1;                                                      // audio function group
      if (id == 0x0A) return codec_ignores_rates ? 0x00000002u : (1u << 6) | (1u << 17) | 0x7F;    // PCM rates: 48 kHz; sizes: 16 bit
      if (id == 0x0D || id == 0x12) return 0;                                        // no amplifiers at the function group
      return 0;
    }
    if (is_converter(nid)) {
      if (id == 0x09) return (0u << 20) | (1u << 9);                                 // audio output converter, digital
      return 0;
    }
    if (is_pin(nid)) {
      if (id == 0x09) return (4u << 20) | (1u << 9) | (1u << 8);                     // pin complex, digital, with a connection list
      if (id == 0x0C) {                                                              // pin capabilities
        if (nid == kLineOutPin) return 1u << 4;                                      // output
        const uint32_t i = pin_index(nid);
        return (1u << 4) | (i == 2 ? (1u << 24) : (1u << 7));                        // output; DisplayPort for pin 7, HDMI for the others
      }
      if (id == 0x0E) return static_cast<uint32_t>(connection_list(nid).size());     // connection list length
      return 0;
    }
    return 0;
  }
};

}  // namespace hardware
