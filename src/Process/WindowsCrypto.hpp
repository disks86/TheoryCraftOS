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
#include "Process/Process.hpp"
#include "Process/Syscall.hpp"

// The cryptographic primitives of bcrypt.dll. The DLL keeps the state of providers, hashes and keys; the arithmetic is done here, by src/Crypto, one operation per call.
namespace process::windowsabi {

// NtTcosCrypto(PVOID arguments): `arguments` points to 14 numbers of 8 bytes, the first is the operation, the others are its parameters (pointers into the program's memory, lengths,
// small codes; see WindowsCrypto.cpp for each operation). Algorithms: 0 MD5, 1 SHA-1, 2 SHA-256, 3 SHA-384, 4 SHA-512, 5 SHA-224, 6 MD4.
//   1 hash or HMAC   2 PBKDF2   3 HKDF   4 AES (ECB, CBC, CFB-128)   5 AES-GCM   6 elliptic curves (P-256, P-384: keys, ECDH, ECDSA)   7 RSA signature verification
uint32_t nt_tcos_crypto(Process& p, const SyscallRequest& r);

}  // namespace process::windowsabi
