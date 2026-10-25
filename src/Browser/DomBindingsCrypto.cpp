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

// The natives under crypto.getRandomValues() and crypto.subtle (the algorithms' plumbing - key objects, formats, padding schemes - is JavaScript in WebPrelude.cpp):
// random bytes, PBKDF2, HKDF, AES (CBC, CTR, GCM, one block), elliptic curves (P-256 and P-384 keys, ECDH, ECDSA), X25519, Ed25519 verification, and the modular power
// RSA is made of. All of them take and give Uint8Arrays. Timing is as documented in Crypto/*.hpp: RSA's private power branches on the bits of the exponent.
#include "Browser/DomBindingsInternal.hpp"
#include "Browser/WebRandom.hpp"
#include "Crypto/Aead.hpp"
#include "Crypto/BigInt.hpp"
#include "Crypto/Crypto.hpp"
#include "Crypto/Ecc.hpp"
#include "Crypto/Random.hpp"
#include "Kernel/Sync.hpp"
#include "Script/TypedArray.hpp"
#include "Utility/ByteBuffer.hpp"
#include "Utility/ByteText.hpp"

namespace browser {

namespace {
kernel::Mutex g_random_lock;
crypto::IRandom* g_random = nullptr;
}  // namespace

void set_system_random(crypto::IRandom* random) {
  kernel::ScopedMutex lock(g_random_lock);
  g_random = random;
}

bool system_random(ByteView out) {
  kernel::ScopedMutex lock(g_random_lock);
  if (g_random) return g_random->fill(out);
  // (nothing was given: the CPU's own generator, if it has one)
  static crypto::CpuEntropySource cpu;
  static crypto::DrbgRandom drbg(cpu);
  if (!drbg.ready() && (!crypto::CpuEntropySource::available() || !drbg.initialize())) return false;
  return drbg.fill(out);
}

namespace bind {

namespace {

#define NATIVE(name) bool name(Vm& vm, const Value& self, const Value* args, uint32_t argc, Value* result)

class SystemRandom final : public crypto::IRandom {
 public:
  bool fill(ByteView out) override { return system_random(out); }
};

// The bytes of argument i (null or undefined: none).
bool bytes_arg(Vm& vm, const Value* args, uint32_t argc, uint32_t i, ByteView* out) {
  const Value v = arg_at(args, argc, i);
  if (v.is_nullish()) {
    *out = ByteView();
    return true;
  }
  ArrayView<uint8_t> view;
  if (!script::typed_array_bytes(v, &view)) return vm.throw_error(ErrorKind::TypeError, "A crypto native needs ArrayBuffers or views of them");
  *out = ByteView(view.data(), view.size());
  return true;
}

bool number_arg(Vm& vm, const Value* args, uint32_t argc, uint32_t i, double* out) { return vm.to_number(arg_at(args, argc, i), out); }

Value bytes_value(DomBindings::Impl& c, ConstByteView bytes) { return Value::make_object(make_byte_array(c, bytes_as_text(bytes))); }

ConstByteView const_view(ByteView v) { return ConstByteView(v.data(), v.size()); }

bool hash_kind(Vm& vm, double d, crypto::HashKind* kind) {
  switch (static_cast<int>(d)) {
    case 1: *kind = crypto::HashKind::Sha1; return true;
    case 2: *kind = crypto::HashKind::Sha256; return true;
    case 3: *kind = crypto::HashKind::Sha384; return true;
    case 4: *kind = crypto::HashKind::Sha512; return true;
  }
  return vm.throw_error(ErrorKind::TypeError, "Unrecognized hash algorithm");
}

// __crypto_random(n): n unpredictable bytes (at most 65536).
NATIVE(crypto_random) {
  (void)self;
  double n;
  if (!number_arg(vm, args, argc, 0, &n)) return false;
  if (!(n >= 0 && n <= 65536)) return vm.throw_error(ErrorKind::RangeError, "Too many random bytes asked for");
  ByteBuffer buffer(vm.allocator());
  if (!buffer.put_zeros(static_cast<uint64_t>(n))) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  if (n > 0 && !system_random(buffer.writable_view())) return vm.throw_error(ErrorKind::Error, "No source of random numbers is available");
  *result = bytes_value(impl(vm), buffer.view());
  return true;
}

// __crypto_pbkdf2(hash, password, salt, iterations, length)
NATIVE(crypto_pbkdf2) {
  (void)self;
  double kind_n, iterations, length;
  ByteView password, salt;
  crypto::HashKind kind;
  if (!number_arg(vm, args, argc, 0, &kind_n) || !hash_kind(vm, kind_n, &kind) || !bytes_arg(vm, args, argc, 1, &password) || !bytes_arg(vm, args, argc, 2, &salt) ||
      !number_arg(vm, args, argc, 3, &iterations) || !number_arg(vm, args, argc, 4, &length))
    return false;
  if (!(iterations >= 1 && iterations <= 20000000) || !(length >= 0 && length <= 1048576)) return vm.throw_error(ErrorKind::RangeError, "PBKDF2 parameters out of range");
  const uint32_t hlen = crypto::hash_size(kind);
  ByteBuffer out(vm.allocator()), message(vm.allocator());
  if (!out.put_zeros(static_cast<uint64_t>(length)) || !message.put_zeros(salt.size() + 4)) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  ByteView m = message.writable_view();
  if (salt.size()) m.subview(0, salt.size()).copy_from(const_view(salt));
  ByteView o = out.writable_view();
  const ConstByteView key = const_view(password);
  for (uint32_t block = 1, done = 0; done < o.size(); ++block) {
    m[salt.size()] = static_cast<uint8_t>(block >> 24);
    m[salt.size() + 1] = static_cast<uint8_t>(block >> 16);
    m[salt.size() + 2] = static_cast<uint8_t>(block >> 8);
    m[salt.size() + 3] = static_cast<uint8_t>(block);
    uint8_t u[crypto::kMaxHashSize], t[crypto::kMaxHashSize];
    crypto::hmac(kind, key, const_view(m), u);
    for (uint32_t i = 0; i < hlen; ++i) t[i] = u[i];
    for (uint32_t it = 1; it < static_cast<uint32_t>(iterations); ++it) {
      uint8_t next[crypto::kMaxHashSize];
      crypto::hmac(kind, key, ConstByteView(u, hlen), next);
      for (uint32_t i = 0; i < hlen; ++i) {
        u[i] = next[i];
        t[i] ^= next[i];
      }
    }
    for (uint32_t i = 0; i < hlen && done < o.size(); ++i, ++done) o[done] = t[i];
  }
  *result = bytes_value(impl(vm), out.view());
  return true;
}

// __crypto_hkdf(hash, ikm, salt, info, length)
NATIVE(crypto_hkdf) {
  (void)self;
  double kind_n, length;
  ByteView ikm, salt, info;
  crypto::HashKind kind;
  if (!number_arg(vm, args, argc, 0, &kind_n) || !hash_kind(vm, kind_n, &kind) || !bytes_arg(vm, args, argc, 1, &ikm) || !bytes_arg(vm, args, argc, 2, &salt) ||
      !bytes_arg(vm, args, argc, 3, &info) || !number_arg(vm, args, argc, 4, &length))
    return false;
  if (!(length >= 0 && length <= 255.0 * crypto::hash_size(kind))) return vm.throw_error(ErrorKind::RangeError, "HKDF length out of range");
  uint8_t prk[crypto::kMaxHashSize];
  const uint8_t zero_salt[crypto::kMaxHashSize] = {};
  crypto::hkdf_extract(kind, salt.size() ? const_view(salt) : ConstByteView(zero_salt, crypto::hash_size(kind)), const_view(ikm), prk);
  ByteBuffer out(vm.allocator());
  if (!out.put_zeros(static_cast<uint64_t>(length))) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  if (!crypto::hkdf_expand(kind, ConstByteView(prk, crypto::hash_size(kind)), const_view(info), out.writable_view())) return vm.throw_error(ErrorKind::RangeError, "HKDF length out of range");
  *result = bytes_value(impl(vm), out.view());
  return true;
}

// __crypto_aes(mode, decrypt, key, iv, data, aad, counter_bits): mode 1 CBC over whole blocks without padding, 2 CTR, 3 GCM (12 byte iv; sealing appends the 16 byte
// tag, opening takes it off and gives null when it is wrong), 4 one block (ECB). The key has 16, 24 (not GCM) or 32 bytes.
NATIVE(crypto_aes) {
  (void)self;
  double mode, counter_bits = 128;
  ByteView key, iv, data, aad;
  if (!number_arg(vm, args, argc, 0, &mode) || !bytes_arg(vm, args, argc, 2, &key) || !bytes_arg(vm, args, argc, 3, &iv) || !bytes_arg(vm, args, argc, 4, &data) ||
      !bytes_arg(vm, args, argc, 5, &aad))
    return false;
  const bool decrypt = arg_at(args, argc, 1).is_bool() && arg_at(args, argc, 1).u.boolean;
  if (argc > 6 && !number_arg(vm, args, argc, 6, &counter_bits)) return false;
  ByteBuffer out(vm.allocator());
  const int m = static_cast<int>(mode);
  if (m == 3) {
    crypto::AesGcm gcm;
    if (iv.size() != crypto::kAeadNonceSize || !gcm.set_key(const_view(key))) return vm.throw_error(ErrorKind::TypeError, "Bad AES-GCM key or iv");
    if (!decrypt) {
      if (!out.put_zeros(data.size() + crypto::kAeadTagSize)) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
      ByteView o = out.writable_view();
      if (data.size()) o.subview(0, data.size()).copy_from(const_view(data));
      gcm.seal(iv.data(), const_view(aad), o.subview(0, data.size()), &o[data.size()]);
    } else {
      if (data.size() < crypto::kAeadTagSize) {
        *result = Value::make_null();
        return true;
      }
      const uint64_t n = data.size() - crypto::kAeadTagSize;
      if (!out.put_zeros(n)) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
      ByteView o = out.writable_view();
      if (n) o.copy_from(ConstByteView(data.data(), n));
      if (!gcm.open(iv.data(), const_view(aad), o, &data[n])) {
        *result = Value::make_null();
        return true;
      }
    }
    *result = bytes_value(impl(vm), out.view());
    return true;
  }
  crypto::Aes aes;
  if (!aes.set_key(const_view(key))) return vm.throw_error(ErrorKind::TypeError, "Bad AES key length");
  if (!out.put_zeros(data.size())) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  ByteView o = out.writable_view();
  if (data.size()) o.copy_from(const_view(data));
  if (m == 1) {
    if (iv.size() != 16 || data.size() % 16 != 0) return vm.throw_error(ErrorKind::TypeError, "Bad AES-CBC iv or length");
    aes.cbc(iv.data(), o, decrypt);
  } else if (m == 4) {
    if (data.size() != 16) return vm.throw_error(ErrorKind::TypeError, "AES block must be 16 bytes");
    uint8_t block[16];
    if (decrypt) aes.decrypt_block(data.data(), block);
    else aes.encrypt_block(data.data(), block);
    o.copy_from(ConstByteView(block, 16));
  } else if (m == 2) {
    if (iv.size() != 16 || !(counter_bits >= 1 && counter_bits <= 128)) return vm.throw_error(ErrorKind::TypeError, "Bad AES-CTR counter");
    uint8_t counter[16];
    for (uint32_t i = 0; i < 16; ++i) counter[i] = iv[i];
    const uint32_t bits = static_cast<uint32_t>(counter_bits);
    for (uint64_t pos = 0; pos < o.size(); pos += 16) {
      uint8_t stream[16];
      aes.encrypt_block(counter, stream);
      for (uint64_t i = 0; i < 16 && pos + i < o.size(); ++i) o[pos + i] ^= stream[i];
      // the low `bits` bits of the block count up and wrap around
      uint32_t carry = 1;
      for (int32_t i = 15; i >= 0 && carry; --i) {
        const uint32_t below = static_cast<uint32_t>(15 - i) * 8;   // counter bits in the bytes after this one
        if (below >= bits) break;
        const uint32_t here = bits - below >= 8 ? 8 : bits - below;
        const uint32_t mask = here == 8 ? 0xFFu : (1u << here) - 1;
        const uint32_t sum = (counter[i] & mask) + carry;
        counter[i] = static_cast<uint8_t>((counter[i] & ~mask) | (sum & mask));
        carry = sum > mask ? 1 : 0;
      }
    }
  } else {
    return vm.throw_error(ErrorKind::TypeError, "Unknown AES mode");
  }
  *result = bytes_value(impl(vm), out.view());
  return true;
}

bool curve_of(Vm& vm, double d, crypto::EcCurve* curve) {
  if (d == 256) {
    *curve = crypto::EcCurve::P256;
    return true;
  }
  if (d == 384) {
    *curve = crypto::EcCurve::P384;
    return true;
  }
  return vm.throw_error(ErrorKind::TypeError, "Unknown curve");
}

// __crypto_ec(op, curve_bits, a, b, c): op 1 new private scalar; 2 public point of private a; 3 ECDH of private a with peer point b (the X coordinate; null if the
// point is not on the curve); 4 ECDSA signature (r | s) with private a over digest b; 5 verify point a, digest b, signature c (r | s) -> bool; 6 is a a valid point.
NATIVE(crypto_ec) {
  (void)self;
  double op, bits;
  ByteView a, b, c;
  crypto::EcCurve curve;
  if (!number_arg(vm, args, argc, 0, &op) || !number_arg(vm, args, argc, 1, &bits) || !curve_of(vm, bits, &curve) || !bytes_arg(vm, args, argc, 2, &a) ||
      !bytes_arg(vm, args, argc, 3, &b) || !bytes_arg(vm, args, argc, 4, &c))
    return false;
  const uint32_t size = crypto::ec_field_size(curve);
  DomBindings::Impl& ctx = impl(vm);
  SystemRandom random;
  uint8_t out[crypto::kMaxEcPoint];
  *result = Value::make_null();
  switch (static_cast<int>(op)) {
    case 1:
      if (!crypto::ec_generate_private(curve, random, out)) return vm.throw_error(ErrorKind::Error, "Could not make a key");
      *result = bytes_value(ctx, ConstByteView(out, size));
      return true;
    case 2:
      if (a.size() == size && crypto::ec_public_key(curve, a.data(), out)) *result = bytes_value(ctx, ConstByteView(out, crypto::ec_point_size(curve)));
      return true;
    case 3:
      if (a.size() == size && crypto::ec_ecdh(curve, a.data(), const_view(b), out)) *result = bytes_value(ctx, ConstByteView(out, size));
      return true;
    case 4:
      if (a.size() == size && crypto::ecdsa_sign(curve, a.data(), b.data(), static_cast<uint32_t>(b.size()), random, out)) *result = bytes_value(ctx, ConstByteView(out, 2 * size));
      return true;
    case 5:
      *result = Value::make_bool(c.size() == 2 * size && crypto::ecdsa_verify(curve, const_view(a), b.data(), static_cast<uint32_t>(b.size()), ConstByteView(c.data(), size),
                                                                             ConstByteView(c.data() + size, size)));
      return true;
    case 6:
      *result = Value::make_bool(crypto::ec_point_valid(curve, const_view(a)));
      return true;
  }
  return vm.throw_error(ErrorKind::TypeError, "Unknown curve operation");
}

// __crypto_x25519(op, a, b, c): op 1 public key of private a; 2 shared secret of private a and peer b (null for a bad peer); 3 Ed25519 verify: public key a, message b,
// signature c.
NATIVE(crypto_x25519) {
  (void)self;
  double op;
  ByteView a, b, c;
  if (!number_arg(vm, args, argc, 0, &op) || !bytes_arg(vm, args, argc, 1, &a) || !bytes_arg(vm, args, argc, 2, &b) || !bytes_arg(vm, args, argc, 3, &c)) return false;
  uint8_t out[32];
  if (static_cast<int>(op) == 3) {
    *result = Value::make_bool(a.size() == 32 && c.size() == 64 && crypto::ed25519_verify(a.data(), const_view(b), c.data()));
    return true;
  }
  if (a.size() != 32) return vm.throw_error(ErrorKind::TypeError, "X25519 keys are 32 bytes");
  if (static_cast<int>(op) == 1) {
    crypto::x25519_public_key(a.data(), out);
  } else if (b.size() != 32 || !crypto::x25519_shared_secret(a.data(), b.data(), out)) {
    *result = Value::make_null();
    return true;
  }
  *result = bytes_value(impl(vm), ConstByteView(out, 32));
  return true;
}

// __crypto_rsa_modpow(modulus, exponent, data): data ^ exponent mod modulus as exactly as many bytes as the modulus has; null if data is not below the modulus (or the
// modulus is not usable).
NATIVE(crypto_rsa_modpow) {
  (void)self;
  ByteView modulus, exponent, data;
  if (!bytes_arg(vm, args, argc, 0, &modulus) || !bytes_arg(vm, args, argc, 1, &exponent) || !bytes_arg(vm, args, argc, 2, &data)) return false;
  static crypto::MontRsa mont;   // (a big object: it is not on the stack, and pages share it, one at a time)
  static kernel::Mutex lock;
  kernel::ScopedMutex guard(lock);
  crypto::MontRsa::Num x, xm, rm, r;
  *result = Value::make_null();
  if (!mont.init(const_view(modulus)) || !mont.load(const_view(data), &x) || !mont.below_modulus(x)) return true;
  mont.to_mont(x, &xm);
  mont.pow(xm, const_view(exponent), &rm);
  mont.from_mont(rm, &r);
  ByteBuffer out(vm.allocator());
  if (!out.put_zeros(mont.byte_size()) || !mont.store(r, out.writable_view())) return vm.throw_error(ErrorKind::RangeError, "Out of memory");
  *result = bytes_value(impl(vm), out.view());
  return true;
}

}  // namespace

bool install_crypto(DomBindings::Impl& c) {
  Vm& vm = c.vm;
  JsObject* g = vm.global();
  vm.define_native(g, "__crypto_random", crypto_random, 1);
  vm.define_native(g, "__crypto_pbkdf2", crypto_pbkdf2, 5);
  vm.define_native(g, "__crypto_hkdf", crypto_hkdf, 5);
  vm.define_native(g, "__crypto_aes", crypto_aes, 7);
  vm.define_native(g, "__crypto_ec", crypto_ec, 5);
  vm.define_native(g, "__crypto_x25519", crypto_x25519, 4);
  vm.define_native(g, "__crypto_rsa_modpow", crypto_rsa_modpow, 3);
  return !vm.halted();
}

}  // namespace bind
}  // namespace browser
