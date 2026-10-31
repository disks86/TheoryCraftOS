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

// The security manager (Core spec vol 3 part H) in the central role over LE links: pairing that this side starts, encryption with keys already held, and the bonds that come out of it.
//
//   Pairing:      LE legacy (Just Works, passkey entry) and LE Secure Connections (Just Works, numeric comparison, passkey entry) with the P-256 key exchange. Out of band data is refused.
//                 The methods follow the table of IO capabilities; a side that asks for no man-in-the-middle protection gets Just Works, which encrypts but does not authenticate
//                 (the bond says so: `authenticated`).
//   User:         the passkey to show, the number to compare and the passkey to type are handed to an ISmUserInterface; the answers come back through passkey_entered() and confirm(),
//                 from inside the callback or later.
//   Bonds:        the long term key and identity key the peripheral hands out (or the one derived by Secure Connections) are kept in an SmBondStore, which serialises to bytes.
//   Peripheral:   a peripheral's pairing request is refused (pairing not supported), its security request is answered with encryption if a bond exists.
#pragma once
#include "Crypto/Random.hpp"
#include "Hardware/Bluetooth/Att.hpp"
#include "Hardware/Bluetooth/BtCrypto.hpp"
#include "Utility/ByteBuffer.hpp"

namespace hardware {

struct SmBond {
  bool used = false;
  hci::BdAddr address;
  uint8_t address_type = 0;
  bool has_ltk = false, has_irk = false, authenticated = false, secure_connections = false;
  uint8_t key_size = 16;
  uint8_t ltk[16] = {};
  uint16_t ediv = 0;
  uint8_t rand[8] = {};
  uint8_t irk[16] = {};
  hci::BdAddr identity;                 // the peripheral's identity address when it gave one (its link address is a private one)
  uint8_t identity_type = 0;
  bool has_identity = false;
};

class SmBondStore {
 public:
  static constexpr uint32_t kMaxBonds = 16;
  static constexpr uint32_t kHeaderBytes = 5, kRecordBytes = 58;      // the serialised form: "BTBD" and the count, then this many bytes a bond
  // The bond for a device: by identity or link address, or, failing that, by resolving a private address with each bond's IRK.
  const SmBond* find(const hci::BdAddr& address, uint8_t address_type) const;
  bool store(const SmBond& bond);       // replaces the bond of the same device; false if the table is full
  bool remove(const hci::BdAddr& address, uint8_t address_type);
  uint32_t count() const;
  const SmBond* at(uint32_t index) const { return index < kMaxBonds && bonds_[index].used ? &bonds_[index] : nullptr; }
  // The bonds as bytes (for a file) and back. parse() returns how many it took; a damaged or truncated record ends it.
  bool serialize(ByteBuffer& out) const;
  uint32_t parse(ConstByteView data);

 private:
  SmBond bonds_[kMaxBonds];
};

class ISmUserInterface {
 public:
  virtual void display_passkey(uint16_t link, uint32_t passkey) = 0;       // show it: the peripheral's user types it
  virtual void confirm_numeric(uint16_t link, uint32_t value) = 0;         // show it and ask whether both sides show the same; answer with confirm()
  virtual void request_passkey(uint16_t link) = 0;                         // ask for the passkey the peripheral shows; answer with passkey_entered()

 protected:
  ISmUserInterface() = default;
  ~ISmUserInterface() = default;
};

struct SmConfig {
  uint8_t io_capability = 3;            // 0 display only, 1 display and yes/no, 2 keyboard only, 3 none, 4 keyboard and display
  bool bonding = true, mitm = false, secure_connections = true;
  uint8_t max_key_size = 16;
};

enum class SmResult : uint8_t { Ok, Timeout, Failed, NoLink, Busy, NoBond };

class SecurityManager final : public IL2capFixedHandler {
 public:
  SecurityManager(HciController& controller, L2cap& l2cap, crypto::IRandom& random, SmBondStore& bonds);
  ~SecurityManager();
  SecurityManager(const SecurityManager&) = delete;
  SecurityManager& operator=(const SecurityManager&) = delete;

  void set_config(const SmConfig& config) { config_ = config; }
  void set_user_interface(ISmUserInterface* ui) { ui_ = ui; }
  // Pairing refused? A peripheral's security request starts pairing when true (else only encryption with a bond).
  void set_pair_on_request(bool on) { pair_on_request_ = on; }
  // A peripheral asked us for security (a Security Request) and there was nothing to encrypt with: the link, once; the caller decides whether to pair.
  bool take_security_request(uint16_t* link) { if (!request_pending_) return false; request_pending_ = false; *link = request_link_; return true; }

  // Pairs with the peripheral on a link and waits for the outcome (the encryption on, the keys received). On Failed, failure_reason() is the code the peripheral or we sent.
  SmResult pair(uint16_t link, uint32_t timeout_ms = 60000);
  // The same without waiting: start_pairing() sends the request (Ok: it is under way), the controller's poll() carries it on, and pairing_finished() says when it ended (Ok or Failed, with
  // failure_reason()). Only one pairing at a time.
  SmResult start_pairing(uint16_t link);
  bool pairing_finished(SmResult* result);
  // A pairing that takes too long is given up (the peripheral is told).
  void abort_pairing();
  // Encrypts a link with the bond of its peripheral.
  SmResult encrypt(uint16_t link, uint32_t timeout_ms = 5000);
  uint8_t failure_reason() const { return failure_; }
  // The answers to the user interface's questions.
  void passkey_entered(uint16_t link, uint32_t passkey);
  void confirm(uint16_t link, bool accepted);
  bool pairing_active() const { return session_.state != State::Idle; }
  // Whether the last pairing (or the encryption it ended in) was authenticated (a method other than Just Works) and used Secure Connections.
  bool last_authenticated() const { return last_authenticated_; }
  bool last_secure_connections() const { return last_secure_; }

  // For AttClient::set_security_handler: encrypts with the bond if there is one, else pairs.
  static bool att_security(void* self, uint16_t link, uint8_t error);

  void on_data(const HciLink& link, ConstByteView sdu) override;
  void on_link(const HciLink& link, bool up) override;

 private:
  enum class State : uint8_t { Idle, WaitResponse, WaitPublicKey, WaitConfirm, WaitRandom, WaitUser, WaitDhKeyCheck, WaitEncryption, WaitKeys, Done, Failed };
  enum class Method : uint8_t { JustWorks, Numeric, PasskeyDisplay, PasskeyInput };
  struct Session {
    State state = State::Idle;
    uint16_t link = 0;
    bool le = true, sc = false, we_display = false, awaiting_confirm = false, awaiting_passkey = false, sent_our_confirm = false;
    Method method = Method::JustWorks;
    uint8_t preq[7] = {}, pres[7] = {};
    uint8_t key_size = 16, resp_dist = 0, keys_expected = 0, keys_seen = 0;
    uint8_t tk[16] = {}, na[16] = {}, nb[16] = {}, peer_confirm[16] = {};
    uint8_t a[7] = {}, b[7] = {};
    bt::SmKeyPair keys;
    uint8_t peer_public[64] = {}, dhkey[32] = {}, mackey[16] = {}, ltk[16] = {};
    uint32_t passkey = 0;
    uint32_t round = 0;
    SmBond bond;
    bool have_ltk = false, have_ediv = false, have_irk = false;
    uint8_t derived_ltk[16] = {};
  };
  static void on_encryption(void* self, uint16_t handle, uint8_t status, bool enabled);
  void send(uint8_t code, ConstByteView body);
  void fail(uint8_t reason, bool tell_peer);
  void finish_ok();
  Method choose_method(uint8_t peer_io, bool mitm, bool sc, bool* we_display) const;
  void handle_pairing_response(ConstByteView body);
  void start_legacy_confirm();
  void legacy_confirm(ConstByteView body);
  void legacy_random(ConstByteView body);
  void sc_public_key(ConstByteView body);
  void sc_begin_exchange();
  void sc_passkey_round_start();
  void sc_confirm(ConstByteView body);
  void sc_random(ConstByteView body);
  void sc_send_dhkey_check();
  void sc_dhkey_check(ConstByteView body);
  void begin_encryption(const uint8_t ltk[16], uint16_t ediv, const uint8_t rand[8]);
  void key_distribution(uint8_t code, ConstByteView body);
  void after_user();
  void io_cap(const uint8_t* pdu, uint8_t out[3]) const { out[0] = pdu[1]; out[1] = pdu[2]; out[2] = pdu[3]; }
  void make_random(uint8_t out[16]);

  HciController& controller_;
  L2cap& l2cap_;
  crypto::IRandom& random_;
  SmBondStore& bonds_;
  SmConfig config_;
  ISmUserInterface* ui_ = nullptr;
  bool pair_on_request_ = false, last_authenticated_ = false, last_secure_ = false, request_pending_ = false;
  uint16_t request_link_ = 0;
  Session session_;
  uint8_t failure_ = 0;
  // encryption of a link with a stored key (encrypt())
  bool encrypting_ = false, encrypt_done_ = false, encrypt_ok_ = false;
  uint16_t encrypt_link_ = 0;
};

}  // namespace hardware
