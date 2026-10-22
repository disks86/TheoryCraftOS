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

// Checking what a TLS server presents, apart from the connection: the Certificate message (the chain against the trust store, the host name or address,
// the dates, the key's suitability) and a signature made with the leaf's key (CertificateVerify, ServerKeyExchange). TlsSocket (TCP) and Tls13Client (QUIC)
// both use it.
#pragma once
#include "Network/Tls/TlsKeys.hpp"
#include "Network/Tls/TlsSocket.hpp"

namespace network::tls {

// Why a certificate was refused: the status for the program, the alert to send, and the kind of TLS error.
struct CertificateProblem {
  crypto::CertStatus status = crypto::CertStatus::Ok;
  uint8_t alert = 0;
  TlsError error = TlsError::None;
};

// Reads the body of a Certificate message (`tls13`: with the request context and the per-certificate extensions of TLS 1.3) and checks the chain as
// `config` says (config.verify_certificates false: only that the leaf can be read). `peer_ipv4` is the server's address, checked when there is no name.
// `suite_auth` is the kind of certificate a TLS 1.2 suite needs (Any for TLS 1.3). On success the leaf (DER) is in `*leaf_der`; otherwise `*problem` says why.
bool check_server_certificate(allocators::IAllocator& allocator, const TlsConfig& config, uint32_t peer_ipv4, ConstByteView body, bool tls13, SuiteAuth suite_auth, ByteBuffer* leaf_der,
                              CertificateProblem* problem);

// Is `signature` a good signature by the key of the certificate `leaf_der` over `content` with the TLS signature scheme `scheme`? `tls13` applies the rules of TLS 1.3
// (ECDSA schemes tied to their curves, no PKCS#1 v1.5).
bool verify_leaf_signature(ConstByteView leaf_der, uint16_t scheme, ConstByteView content, ConstByteView signature, bool tls13);

}  // namespace network::tls
