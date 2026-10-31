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

#include "Hardware/Bluetooth/Smp.hpp"
#include "Kernel/Delay.hpp"

namespace hardware {

namespace {
constexpr uint8_t kPairingRequest = 0x01, kPairingResponse = 0x02, kPairingConfirm = 0x03, kPairingRandom = 0x04, kPairingFailed = 0x05, kEncryptionInformation = 0x06,
                  kCentralIdentification = 0x07, kIdentityInformation = 0x08, kIdentityAddressInformation = 0x09, kSigningInformation = 0x0A, kSecurityRequest = 0x0B,
                  kPairingPublicKey = 0x0C, kPairingDhKeyCheck = 0x0D;
constexpr uint8_t kReasonPasskeyFailed = 0x01, kReasonOobNotAvailable = 0x02, kReasonAuthRequirements = 0x03, kReasonConfirmFailed = 0x04, kReasonNotSupported = 0x05,
                  kReasonKeySize = 0x06, kReasonUnspecified = 0x08, kReasonInvalidParameters = 0x0A, kReasonDhKeyCheckFailed = 0x0B, kReasonNumericFailed = 0x0C;
constexpr uint8_t kAuthBonding = 0x01, kAuthMitm = 0x04, kAuthSc = 0x08;
constexpr uint8_t kDistEnc = 0x01, kDistId = 0x02, kDistSign = 0x04;
constexpr uint8_t kJw = 0, kNc = 1, kPkDisplay = 2, kPkInput = 3, kPkBoth = 4;       // (a table entry: Just Works, numeric comparison, initiator displays, initiator inputs, both input)

// [initiator's IO capability][responder's], capabilities in the order display only, display and yes/no, keyboard only, none, keyboard and display.
const uint8_t kLegacy[5][5] = {{kJw, kJw, kPkDisplay, kJw, kPkDisplay}, {kJw, kJw, kPkDisplay, kJw, kPkDisplay}, {kPkInput, kPkInput, kPkBoth, kJw, kPkInput},
                               {kJw, kJw, kJw, kJw, kJw}, {kPkInput, kPkInput, kPkDisplay, kJw, kPkDisplay}};
const uint8_t kSecure[5][5] = {{kJw, kJw, kPkDisplay, kJw, kPkDisplay}, {kJw, kNc, kPkDisplay, kJw, kNc}, {kPkInput, kPkInput, kPkBoth, kJw, kPkInput},
                               {kJw, kJw, kJw, kJw, kJw}, {kPkInput, kNc, kPkDisplay, kJw, kNc}};

void put_passkey(uint8_t out[16], uint32_t passkey) {
  for (int i = 0; i < 16; ++i) out[i] = 0;
  for (int i = 0; i < 4; ++i) out[i] = static_cast<uint8_t>(passkey >> (8 * i));
}

bool same(const uint8_t* a, const uint8_t* b, uint32_t n) {
  uint8_t diff = 0;
  for (uint32_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}
}

// ------------------------------------------------------------------ bonds

const SmBond* SmBondStore::find(const hci::BdAddr& address, uint8_t address_type) const {
  for (const SmBond& b : bonds_)
    if (b.used && ((b.address == address && b.address_type == address_type) || (b.has_identity && b.identity == address && b.identity_type == address_type))) return &b;
  if (address_type == hci::kAddrRandom)
    for (const SmBond& b : bonds_)
      if (b.used && b.has_irk && bt::sm_irk_matches(b.irk, address.b)) return &b;
  return nullptr;
}

bool SmBondStore::store(const SmBond& bond) {
  SmBond* slot = nullptr;
  for (SmBond& b : bonds_) {
    if (b.used && b.address == bond.address && b.address_type == bond.address_type) { slot = &b; break; }
    if (!b.used && !slot) slot = &b;
  }
  if (!slot) return false;
  *slot = bond;
  slot->used = true;
  return true;
}

bool SmBondStore::remove(const hci::BdAddr& address, uint8_t address_type) {
  for (SmBond& b : bonds_)
    if (b.used && b.address == address && b.address_type == address_type) { b.used = false; return true; }
  return false;
}

uint32_t SmBondStore::count() const {
  uint32_t n = 0;
  for (const SmBond& b : bonds_) n += b.used;
  return n;
}

// "BTBD", the number of records, then each: address (6), type, flags, key size, LTK (16), EDIV (2), Rand (8), IRK (16), identity address (6), identity type.
bool SmBondStore::serialize(ByteBuffer& out) const {
  out.clear();
  if (!out.put_tag("BTBD") || !out.put8(static_cast<uint8_t>(count()))) return false;
  for (const SmBond& b : bonds_) {
    if (!b.used) continue;
    const uint8_t flags = static_cast<uint8_t>((b.has_ltk ? 1 : 0) | (b.has_irk ? 2 : 0) | (b.authenticated ? 4 : 0) | (b.secure_connections ? 8 : 0) | (b.has_identity ? 16 : 0));
    if (!out.append(ConstByteView(b.address.b, 6)) || !out.put8(b.address_type) || !out.put8(flags) || !out.put8(b.key_size) || !out.append(ConstByteView(b.ltk, 16)) || !out.put16(b.ediv) ||
        !out.append(ConstByteView(b.rand, 8)) || !out.append(ConstByteView(b.irk, 16)) || !out.append(ConstByteView(b.identity.b, 6)) || !out.put8(b.identity_type))
      return false;
  }
  return true;
}

uint32_t SmBondStore::parse(ConstByteView data) {
  ByteReader in(data);
  const ConstByteView tag = in.take(4);
  const uint8_t count = in.u8();
  if (!in.ok() || tag[0] != 'B' || tag[1] != 'T' || tag[2] != 'B' || tag[3] != 'D') return 0;
  uint32_t taken = 0;
  for (uint32_t i = 0; i < count && i < kMaxBonds; ++i) {
    SmBond b;
    in.read_into(ByteView(b.address.b, 6));
    b.address_type = in.u8();
    const uint8_t flags = in.u8();
    b.key_size = in.u8();
    in.read_into(ByteView(b.ltk));
    b.ediv = in.le16();
    in.read_into(ByteView(b.rand));
    in.read_into(ByteView(b.irk));
    in.read_into(ByteView(b.identity.b, 6));
    b.identity_type = in.u8();
    if (!in.ok() || b.address_type > 1 || b.key_size < 7 || b.key_size > 16 || flags > 31) break;
    b.has_ltk = flags & 1;
    b.has_irk = flags & 2;
    b.authenticated = flags & 4;
    b.secure_connections = flags & 8;
    b.has_identity = flags & 16;
    b.used = true;
    if (store(b)) ++taken;
  }
  return taken;
}

// ------------------------------------------------------------------ the manager

SecurityManager::SecurityManager(HciController& controller, L2cap& l2cap, crypto::IRandom& random, SmBondStore& bonds)
    : controller_(controller), l2cap_(l2cap), random_(random), bonds_(bonds) {
  l2cap_.register_fixed(L2cap::kCidSmp, this);
  controller_.set_encryption_handler(&SecurityManager::on_encryption, this);
}

SecurityManager::~SecurityManager() { controller_.set_encryption_handler(nullptr, nullptr); }

void SecurityManager::send(uint8_t code, ConstByteView body) {
  uint8_t pdu[65];
  if (body.size() > 64) return;
  pdu[0] = code;
  for (uint64_t i = 0; i < body.size(); ++i) pdu[1 + i] = body[i];
  l2cap_.send_fixed(session_.link, L2cap::kCidSmp, ConstByteView(pdu, 1 + body.size()));
}

void SecurityManager::make_random(uint8_t out[16]) {
  if (!random_.fill(ByteView(out, 16))) for (int i = 0; i < 16; ++i) out[i] = static_cast<uint8_t>(i * 37 + 11);       // (never reached with a working source; the pairing would fail its own checks)
}

void SecurityManager::fail(uint8_t reason, bool tell_peer) {
  if (session_.state == State::Idle || session_.state == State::Done || session_.state == State::Failed) return;
  if (tell_peer) send(kPairingFailed, ConstByteView(&reason, 1));
  failure_ = reason;
  session_.state = State::Failed;
}

void SecurityManager::on_link(const HciLink& link, bool up) {
  if (!up && session_.state != State::Idle && session_.link == link.handle) fail(kReasonUnspecified, false);
}

SecurityManager::Method SecurityManager::choose_method(uint8_t peer_io, bool mitm, bool sc, bool* we_display) const {
  *we_display = false;
  if (!mitm || peer_io > 4 || config_.io_capability > 4) return Method::JustWorks;
  const uint8_t entry = (sc ? kSecure : kLegacy)[config_.io_capability][peer_io];
  switch (entry) {
    case kNc: return Method::Numeric;
    case kPkDisplay: *we_display = true; return Method::PasskeyDisplay;
    case kPkInput:
    case kPkBoth: return Method::PasskeyInput;
    default: return Method::JustWorks;
  }
}

SmResult SecurityManager::start_pairing(uint16_t link) {
  if (session_.state != State::Idle && session_.state != State::Done && session_.state != State::Failed) return SmResult::Busy;
  const HciLink* l = controller_.link(link);
  if (!l || !l->le || !l->central) return SmResult::NoLink;
  session_ = Session();
  session_.link = link;
  const hci::BdAddr& ours = controller_.info().address;
  for (int i = 0; i < 6; ++i) { session_.a[i] = ours.b[i]; session_.b[i] = l->address.b[i]; }
  session_.a[6] = hci::kAddrPublic;
  session_.b[6] = l->address_type;
  failure_ = 0;
  uint8_t auth = static_cast<uint8_t>((config_.bonding ? kAuthBonding : 0) | (config_.mitm ? kAuthMitm : 0) | (config_.secure_connections ? kAuthSc : 0));
  const uint8_t dist = static_cast<uint8_t>(kDistEnc | kDistId);
  const uint8_t request[7] = {kPairingRequest, config_.io_capability, 0, auth, config_.max_key_size, 0, dist};      // (we hand out no keys, we want the peripheral's)
  for (int i = 0; i < 7; ++i) session_.preq[i] = request[i];
  session_.state = State::WaitResponse;
  send(kPairingRequest, ConstByteView(request + 1, 6));
  return SmResult::Ok;
}

bool SecurityManager::pairing_finished(SmResult* result) {
  if (session_.state == State::Done) { *result = SmResult::Ok; session_.state = State::Idle; return true; }
  if (session_.state == State::Failed) { *result = SmResult::Failed; session_.state = State::Idle; return true; }
  return false;
}

void SecurityManager::abort_pairing() {
  if (session_.state != State::Idle && session_.state != State::Done && session_.state != State::Failed) fail(kReasonUnspecified, true);
}

SmResult SecurityManager::pair(uint16_t link, uint32_t timeout_ms) {
  const SmResult started = start_pairing(link);
  if (started != SmResult::Ok) return started;
  const uint64_t limit = uint64_t{timeout_ms} * 10;
  for (uint64_t round = 0; round < limit && session_.state != State::Done && session_.state != State::Failed; ++round)
    if (controller_.poll() == 0) delay_us(100);
  SmResult result = SmResult::Timeout;
  if (!pairing_finished(&result)) {
    abort_pairing();
    session_.state = State::Idle;
    return SmResult::Timeout;
  }
  return result;
}

void SecurityManager::passkey_entered(uint16_t link, uint32_t passkey) {
  if (session_.state != State::WaitUser || session_.link != link || !session_.awaiting_passkey) return;
  session_.awaiting_passkey = false;
  session_.passkey = passkey % 1000000;
  if (!session_.sc) put_passkey(session_.tk, session_.passkey);
  after_user();
}

void SecurityManager::confirm(uint16_t link, bool accepted) {
  if (session_.state != State::WaitUser || session_.link != link || !session_.awaiting_confirm) return;
  session_.awaiting_confirm = false;
  if (!accepted) { fail(kReasonNumericFailed, true); return; }
  after_user();
}

void SecurityManager::after_user() {
  if (session_.method == Method::Numeric) { sc_send_dhkey_check(); return; }
  if (session_.method == Method::PasskeyInput) {
    if (session_.sc) sc_passkey_round_start();
    else start_legacy_confirm();
  }
}

// ------------------------------------------------------------------ receiving

void SecurityManager::on_data(const HciLink& link, ConstByteView sdu) {
  if (sdu.empty()) return;
  const uint8_t code = sdu[0];
  const ConstByteView body = sdu.dropped(1);
  if (code == kSecurityRequest) {
    const SmBond* bond = bonds_.find(link.address, link.address_type);
    if (bond && bond->has_ltk && session_.state == State::Idle && !link.encrypted) controller_.start_encryption(link.handle, bond->rand, bond->ediv, bond->ltk);
    else if (!link.encrypted) { request_pending_ = true; request_link_ = link.handle; }
    return;
  }
  if (session_.state == State::Idle || session_.state == State::Done || session_.state == State::Failed || session_.link != link.handle) {
    if (code == kPairingRequest) {                                          // a peripheral does not pair with a central by asking it to
      session_.link = link.handle;
      const uint8_t reason = kReasonNotSupported;
      send(kPairingFailed, ConstByteView(&reason, 1));
    }
    return;
  }
  if (code == kPairingFailed) {
    failure_ = body.empty() ? kReasonUnspecified : body[0];
    session_.state = State::Failed;
    return;
  }
  switch (code) {
    case kPairingResponse:
      if (session_.state == State::WaitResponse && sdu.size() == 7) { for (int i = 0; i < 7; ++i) session_.pres[i] = sdu[i]; handle_pairing_response(body); return; }
      break;
    case kPairingConfirm:
      if (session_.state == State::WaitConfirm && body.size() == 16) { if (session_.sc) sc_confirm(body); else legacy_confirm(body); return; }
      break;
    case kPairingRandom:
      if (session_.state == State::WaitRandom && body.size() == 16) { if (session_.sc) sc_random(body); else legacy_random(body); return; }
      break;
    case kPairingPublicKey:
      if (session_.state == State::WaitPublicKey && session_.sc && body.size() == 64) { sc_public_key(body); return; }
      break;
    case kPairingDhKeyCheck:
      if (session_.state == State::WaitDhKeyCheck && body.size() == 16) { sc_dhkey_check(body); return; }
      break;
    case kEncryptionInformation:
    case kCentralIdentification:
    case kIdentityInformation:
    case kIdentityAddressInformation:
    case kSigningInformation:
      if (session_.state == State::WaitKeys) { key_distribution(code, body); return; }
      break;
    default:
      fail(kReasonNotSupported, true);
      return;
  }
  fail(kReasonUnspecified, true);                                            // a command that does not belong in this state, or of the wrong size
}

void SecurityManager::handle_pairing_response(ConstByteView body) {
  const uint8_t io = body[0], oob = body[1], auth = body[2], max_key = body[3], init_dist = body[4], resp_dist = body[5];
  if (oob != 0) { fail(kReasonOobNotAvailable, true); return; }
  session_.key_size = max_key < config_.max_key_size ? max_key : config_.max_key_size;
  if (session_.key_size < 7) { fail(kReasonKeySize, true); return; }
  session_.sc = config_.secure_connections && (auth & kAuthSc);
  const bool mitm = config_.mitm || (auth & kAuthMitm);
  session_.method = choose_method(io, mitm, session_.sc, &session_.we_display);
  if ((session_.method != Method::JustWorks) && !ui_) { fail(kReasonAuthRequirements, true); return; }
  session_.resp_dist = static_cast<uint8_t>(resp_dist & session_.preq[6]);
  if (session_.sc) session_.resp_dist = static_cast<uint8_t>(session_.resp_dist & ~kDistEnc);          // (the long term key comes from the key exchange)
  (void)init_dist;
  session_.keys_expected = static_cast<uint8_t>(((session_.resp_dist & kDistEnc) ? 2 : 0) + ((session_.resp_dist & kDistId) ? 2 : 0) + ((session_.resp_dist & kDistSign) ? 1 : 0));
  session_.bond = SmBond();
  if (session_.sc) {
    if (!bt::sm_generate_key_pair(random_, &session_.keys)) { fail(kReasonUnspecified, true); return; }
    send(kPairingPublicKey, ConstByteView(session_.keys.public_key, 64));
    session_.state = State::WaitPublicKey;
    return;
  }
  switch (session_.method) {
    case Method::JustWorks:
      start_legacy_confirm();
      break;
    case Method::PasskeyDisplay: {
      uint8_t r[16];
      make_random(r);
      session_.passkey = (r[0] | (uint32_t{r[1]} << 8) | (uint32_t{r[2]} << 16) | (uint32_t{r[3]} << 24)) % 1000000;
      put_passkey(session_.tk, session_.passkey);
      ui_->display_passkey(session_.link, session_.passkey);
      start_legacy_confirm();
      break;
    }
    case Method::PasskeyInput:
      session_.state = State::WaitUser;
      session_.awaiting_passkey = true;
      ui_->request_passkey(session_.link);
      break;
    default:
      fail(kReasonUnspecified, true);
  }
}

// ------------------------------------------------------------------ legacy pairing

void SecurityManager::start_legacy_confirm() {
  make_random(session_.na);                                                  // (Mrand)
  uint8_t confirm[16];
  if (!bt::sm_c1(session_.tk, session_.na, session_.preq, session_.pres, hci::kAddrPublic, session_.a, session_.b[6], session_.b, confirm)) { fail(kReasonUnspecified, true); return; }
  send(kPairingConfirm, ConstByteView(confirm, 16));
  session_.state = State::WaitConfirm;
}

void SecurityManager::legacy_confirm(ConstByteView body) {
  for (int i = 0; i < 16; ++i) session_.peer_confirm[i] = body[i];
  send(kPairingRandom, ConstByteView(session_.na, 16));
  session_.state = State::WaitRandom;
}

void SecurityManager::legacy_random(ConstByteView body) {
  for (int i = 0; i < 16; ++i) session_.nb[i] = body[i];                     // (Srand)
  uint8_t expected[16], stk[16];
  if (!bt::sm_c1(session_.tk, session_.nb, session_.preq, session_.pres, hci::kAddrPublic, session_.a, session_.b[6], session_.b, expected)) { fail(kReasonUnspecified, true); return; }
  if (!same(expected, session_.peer_confirm, 16)) { fail(kReasonConfirmFailed, true); return; }
  if (!bt::sm_s1(session_.tk, session_.nb, session_.na, stk)) { fail(kReasonUnspecified, true); return; }
  for (uint32_t i = session_.key_size; i < 16; ++i) stk[i] = 0;              // (the key is as long as the two sides agreed on)
  const uint8_t zero[8] = {};
  begin_encryption(stk, 0, zero);
}

// ------------------------------------------------------------------ Secure Connections

void SecurityManager::sc_public_key(ConstByteView body) {
  for (int i = 0; i < 64; ++i) session_.peer_public[i] = body[i];
  if (same(session_.peer_public, session_.keys.public_key, 64)) { fail(kReasonInvalidParameters, true); return; }          // (our own key reflected back)
  if (!bt::sm_dhkey(session_.keys.private_key, session_.peer_public, session_.dhkey)) { fail(kReasonInvalidParameters, true); return; }
  switch (session_.method) {
    case Method::PasskeyDisplay: {
      uint8_t r[16];
      make_random(r);
      session_.passkey = (r[0] | (uint32_t{r[1]} << 8) | (uint32_t{r[2]} << 16) | (uint32_t{r[3]} << 24)) % 1000000;
      ui_->display_passkey(session_.link, session_.passkey);
      session_.round = 0;
      sc_passkey_round_start();
      break;
    }
    case Method::PasskeyInput:
      session_.state = State::WaitUser;
      session_.awaiting_passkey = true;
      session_.round = 0;
      ui_->request_passkey(session_.link);
      break;
    default:
      session_.state = State::WaitConfirm;                                   // (Just Works and numeric comparison: the peripheral sends its confirm value first)
  }
}

void SecurityManager::sc_passkey_round_start() {
  make_random(session_.na);
  const uint8_t z = static_cast<uint8_t>(0x80 | ((session_.passkey >> session_.round) & 1));
  uint8_t confirm[16];
  if (!bt::sm_f4(session_.keys.public_key, session_.peer_public, session_.na, z, confirm)) { fail(kReasonUnspecified, true); return; }
  send(kPairingConfirm, ConstByteView(confirm, 16));
  session_.state = State::WaitConfirm;
}

void SecurityManager::sc_confirm(ConstByteView body) {
  for (int i = 0; i < 16; ++i) session_.peer_confirm[i] = body[i];
  if (session_.method != Method::PasskeyDisplay && session_.method != Method::PasskeyInput) make_random(session_.na);
  send(kPairingRandom, ConstByteView(session_.na, 16));
  session_.state = State::WaitRandom;
}

void SecurityManager::sc_random(ConstByteView body) {
  for (int i = 0; i < 16; ++i) session_.nb[i] = body[i];
  const bool passkey = session_.method == Method::PasskeyDisplay || session_.method == Method::PasskeyInput;
  const uint8_t z = passkey ? static_cast<uint8_t>(0x80 | ((session_.passkey >> session_.round) & 1)) : 0;
  uint8_t expected[16];
  if (!bt::sm_f4(session_.peer_public, session_.keys.public_key, session_.nb, z, expected)) { fail(kReasonUnspecified, true); return; }
  if (!same(expected, session_.peer_confirm, 16)) { fail(passkey ? kReasonPasskeyFailed : kReasonConfirmFailed, true); return; }
  if (passkey) {
    if (++session_.round < 20) { sc_passkey_round_start(); return; }
    sc_send_dhkey_check();
    return;
  }
  if (session_.method == Method::Numeric) {
    uint32_t value = 0;
    if (!bt::sm_g2(session_.keys.public_key, session_.peer_public, session_.na, session_.nb, &value)) { fail(kReasonUnspecified, true); return; }
    session_.state = State::WaitUser;
    session_.awaiting_confirm = true;
    ui_->confirm_numeric(session_.link, value);
    return;
  }
  sc_send_dhkey_check();
}

void SecurityManager::sc_send_dhkey_check() {
  if (!bt::sm_f5(session_.dhkey, session_.na, session_.nb, session_.a, session_.b, session_.mackey, session_.ltk)) { fail(kReasonUnspecified, true); return; }
  uint8_t cap[3], rb[16] = {}, check[16];
  io_cap(session_.preq, cap);
  if (session_.method == Method::PasskeyDisplay || session_.method == Method::PasskeyInput) put_passkey(rb, session_.passkey);
  if (!bt::sm_f6(session_.mackey, session_.na, session_.nb, rb, cap, session_.a, session_.b, check)) { fail(kReasonUnspecified, true); return; }
  send(kPairingDhKeyCheck, ConstByteView(check, 16));
  session_.state = State::WaitDhKeyCheck;
}

void SecurityManager::sc_dhkey_check(ConstByteView body) {
  uint8_t cap[3], ra[16] = {}, expected[16];
  io_cap(session_.pres, cap);
  if (session_.method == Method::PasskeyDisplay || session_.method == Method::PasskeyInput) put_passkey(ra, session_.passkey);
  if (!bt::sm_f6(session_.mackey, session_.nb, session_.na, ra, cap, session_.b, session_.a, expected)) { fail(kReasonUnspecified, true); return; }
  if (!same(expected, body.data(), 16)) { fail(kReasonDhKeyCheckFailed, true); return; }
  const uint8_t zero[8] = {};
  begin_encryption(session_.ltk, 0, zero);
}

// ------------------------------------------------------------------ encryption and keys

void SecurityManager::begin_encryption(const uint8_t ltk[16], uint16_t ediv, const uint8_t rand[8]) {
  session_.state = State::WaitEncryption;
  if (controller_.start_encryption(session_.link, rand, ediv, ltk) != HciResult::Ok) fail(kReasonUnspecified, false);
}

void SecurityManager::on_encryption(void* self, uint16_t handle, uint8_t status, bool enabled) {
  SecurityManager& sm = *static_cast<SecurityManager*>(self);
  if (sm.encrypting_ && sm.encrypt_link_ == handle) {
    sm.encrypt_done_ = true;
    sm.encrypt_ok_ = status == 0 && enabled;
  }
  Session& s = sm.session_;
  if (s.state != State::WaitEncryption || s.link != handle) return;
  if (status != 0 || !enabled) { sm.fail(kReasonUnspecified, false); return; }
  if (s.keys_expected == 0) sm.finish_ok();
  else s.state = State::WaitKeys;
}

void SecurityManager::key_distribution(uint8_t code, ConstByteView body) {
  switch (code) {
    case kEncryptionInformation:
      if (body.size() != 16) { fail(kReasonInvalidParameters, true); return; }
      for (int i = 0; i < 16; ++i) session_.bond.ltk[i] = body[i];
      session_.have_ltk = true;
      break;
    case kCentralIdentification:
      if (body.size() != 10) { fail(kReasonInvalidParameters, true); return; }
      session_.bond.ediv = static_cast<uint16_t>(body[0] | (body[1] << 8));
      for (int i = 0; i < 8; ++i) session_.bond.rand[i] = body[2 + i];
      session_.have_ediv = true;
      break;
    case kIdentityInformation:
      if (body.size() != 16) { fail(kReasonInvalidParameters, true); return; }
      for (int i = 0; i < 16; ++i) session_.bond.irk[i] = body[i];
      session_.have_irk = true;
      break;
    case kIdentityAddressInformation:
      if (body.size() != 7 || body[0] > 1) { fail(kReasonInvalidParameters, true); return; }
      session_.bond.identity_type = body[0];
      for (int i = 0; i < 6; ++i) session_.bond.identity.b[i] = body[1 + i];
      session_.bond.has_identity = true;
      break;
    default:
      break;                                                                  // (the signing key is not kept)
  }
  if (++session_.keys_seen >= session_.keys_expected) finish_ok();
}

void SecurityManager::finish_ok() {
  const HciLink* l = controller_.link(session_.link);
  SmBond bond = session_.bond;
  if (l) { bond.address = l->address; bond.address_type = l->address_type; }
  bond.used = true;
  bond.has_irk = session_.have_irk;
  bond.authenticated = session_.method != Method::JustWorks;
  bond.secure_connections = session_.sc;
  bond.key_size = session_.key_size;
  if (session_.sc) {
    for (int i = 0; i < 16; ++i) bond.ltk[i] = session_.ltk[i];
    bond.ediv = 0;
    for (int i = 0; i < 8; ++i) bond.rand[i] = 0;
    bond.has_ltk = true;
  } else {
    bond.has_ltk = session_.have_ltk && session_.have_ediv;
  }
  last_authenticated_ = bond.authenticated;
  last_secure_ = session_.sc;
  if (config_.bonding && bond.has_ltk && l) bonds_.store(bond);
  session_.state = State::Done;
}

SmResult SecurityManager::encrypt(uint16_t link, uint32_t timeout_ms) {
  const HciLink* l = controller_.link(link);
  if (!l) return SmResult::NoLink;
  if (l->encrypted) return SmResult::Ok;
  if (encrypting_) return SmResult::Busy;
  const SmBond* bond = bonds_.find(l->address, l->address_type);
  if (!bond || !bond->has_ltk) return SmResult::NoBond;
  encrypting_ = true;
  encrypt_done_ = encrypt_ok_ = false;
  encrypt_link_ = link;
  SmResult result = SmResult::Failed;
  if (controller_.start_encryption(link, bond->rand, bond->ediv, bond->ltk) == HciResult::Ok) {
    const uint64_t limit = uint64_t{timeout_ms} * 10;
    for (uint64_t round = 0; round < limit && !encrypt_done_; ++round)
      if (controller_.poll() == 0) delay_us(100);
    result = !encrypt_done_ ? SmResult::Timeout : encrypt_ok_ ? SmResult::Ok : SmResult::Failed;
    if (encrypt_ok_) { last_authenticated_ = bond->authenticated; last_secure_ = bond->secure_connections; }
  }
  encrypting_ = false;
  return result;
}

bool SecurityManager::att_security(void* self, uint16_t link, uint8_t error) {
  SecurityManager& sm = *static_cast<SecurityManager*>(self);
  const HciLink* l = sm.controller_.link(link);
  if (!l) return false;
  if (l->encrypted) return false;                                            // (already as secure as it gets without a different pairing)
  (void)error;
  const SmResult encrypted = sm.encrypt(link);
  if (encrypted == SmResult::Ok) return true;
  if (encrypted == SmResult::NoBond || encrypted == SmResult::Failed) return sm.pair(link) == SmResult::Ok;
  return false;
}

}  // namespace hardware
