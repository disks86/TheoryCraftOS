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

// A pretend PostgreSQL server for the host tests, written from the protocol description: it takes the client's bytes (feed()) and produces the
// bytes the server would answer (out). Logins (trust, clear text, MD5, SCRAM-SHA-256), simple and extended queries with canned answers, errors,
// transactions, and ways of misbehaving (silent, dropping the connection, speaking nonsense).
#pragma once
#include <string.h>
#include <string>
#include <vector>
#include "Crypto/Crypto.hpp"

typedef std::string Bytes;

inline Bytes be32(uint32_t v) { return Bytes{char(v >> 24), char(v >> 16), char(v >> 8), char(v)}; }
inline Bytes be16(uint32_t v) { return Bytes{char(v >> 8), char(v)}; }
inline Bytes cstr(const std::string& s) { return s + std::string(1, '\0'); }
inline Bytes message(char type, const Bytes& body) { return Bytes(1, type) + be32(static_cast<uint32_t>(body.size() + 4)) + body; }

struct Column {
  std::string name;
  uint32_t oid;
};
struct Cell {
  bool null;
  std::string text;
};

struct Reply {
  bool error = false, empty = false;
  std::string code, text, detail, tag;
  std::vector<Column> columns;
  std::vector<std::vector<Cell>> rows;
};

struct FakeServer {
  enum class Auth { Trust, Cleartext, Md5, Scram, Kerberos } auth = Auth::Trust;
  std::string user = "app", password = "secret";
  bool silent = false;           // answers nothing
  bool drop = false;             // the connection ends instead of answering a query
  bool garbage = false;          // answers a query with a message nobody knows
  bool bad_server_proof = false; // SCRAM: the server's signature is wrong

  Bytes in, out;
  bool terminated = false;
  bool started = false;
  int ssl_requests = 0;
  std::string startup_user, startup_database, startup_app, startup_float_digits;
  std::vector<std::string> sql_log;
  std::vector<Cell> last_params;
  enum class State { Startup, Password, ScramFirst, ScramFinal, Ready } state = State::Startup;
  bool in_tx = false, tx_failed = false;
  // SCRAM
  std::string client_first_bare, server_first, nonce;
  // Extended protocol
  std::string parsed_sql;
  std::vector<Cell> bound;
  bool skipping = false;   // after an error: ignore messages until Sync
  uint32_t last_read_timeout = 0;   // what the client asked the stream for

  void ready() { out += message('Z', Bytes(1, in_tx ? (tx_failed ? 'E' : 'T') : 'I')); }
  void login_done() {
    out += message('R', be32(0));
    out += message('S', cstr("server_version") + cstr("16.2"));
    out += message('S', cstr("client_encoding") + cstr("UTF8"));
    out += message('K', be32(1234) + be32(5678));
    state = State::Ready;
    ready();
  }
  void error(const std::string& code, const std::string& text, const std::string& detail = "") {
    Bytes body = Bytes("S") + cstr("ERROR") + Bytes("C") + cstr(code) + Bytes("M") + cstr(text);
    if (!detail.empty()) body += Bytes("D") + cstr(detail);
    body += Bytes(1, '\0');
    out += message('E', body);
  }

  // What a statement answers.
  Reply run(const std::string& sql, const std::vector<Cell>& params) {
    Reply r;
    if (sql.empty() || sql == ";") {
      r.empty = true;
      return r;
    }
    if (tx_failed && sql != "ROLLBACK" && sql != "COMMIT") {
      r.error = true;
      r.code = "25P02";
      r.text = "current transaction is aborted, commands ignored until end of transaction block";
      return r;
    }
    if (sql == "SELECT 1") {
      r.columns = {{"?column?", 23}};
      r.rows = {{{false, "1"}}};
      r.tag = "SELECT 1";
    } else if (sql == "SELECT id, name, data, ok, score FROM items") {
      r.columns = {{"id", 23}, {"name", 25}, {"data", 17}, {"ok", 16}, {"score", 701}};
      r.rows = {{{false, "1"}, {false, "alpha"}, {false, "\\x0001ff"}, {false, "t"}, {false, "1.5"}}, {{false, "2"}, {true, ""}, {true, ""}, {false, "f"}, {false, "-0.25"}}};
      r.tag = "SELECT 2";
    } else if (sql.rfind("SELECT $1", 0) == 0) {   // echoes the parameters it was given
      for (size_t i = 0; i < params.size(); ++i) r.columns.push_back({"p" + std::to_string(i + 1), 25});
      r.rows.push_back(params);
      r.tag = "SELECT 1";
    } else if (sql.rfind("INSERT", 0) == 0) {
      r.tag = "INSERT 0 1";
    } else if (sql.rfind("UPDATE", 0) == 0) {
      r.tag = "UPDATE 5";
    } else if (sql.rfind("CREATE TABLE", 0) == 0) {
      r.tag = "CREATE TABLE";
    } else if (sql == "BEGIN") {
      in_tx = true;
      r.tag = "BEGIN";
    } else if (sql == "COMMIT" || sql == "ROLLBACK") {
      in_tx = tx_failed = false;
      r.tag = sql == "COMMIT" ? "COMMIT" : "ROLLBACK";
    } else if (sql == "SELECT many") {
      r.columns = {{"s", 25}};
      for (int i = 0; i < 1000; ++i) r.rows.push_back({{false, std::string(100, 'x')}});
      r.tag = "SELECT 1000";
    } else if (sql == "SELECT nope") {
      r.error = true;
      r.code = "42703";
      r.text = "column \"nope\" does not exist";
      r.detail = "in the list of columns";
    } else {
      r.error = true;
      r.code = "42601";
      r.text = "syntax error at or near \"" + sql + "\"";
    }
    return r;
  }

  void emit(const Reply& r, bool extended) {
    if (r.error) {
      error(r.code, r.text, r.detail);
      if (in_tx) tx_failed = true;
      return;
    }
    if (r.empty) {
      out += message('I', "");
      return;
    }
    if (!r.columns.empty()) {
      Bytes body = be16(static_cast<uint32_t>(r.columns.size()));
      for (const Column& c : r.columns) body += cstr(c.name) + be32(0) + be16(0) + be32(c.oid) + be16(0xFFFF) + be32(0xFFFFFFFFu) + be16(0);
      out += message('T', body);
    } else if (extended) {
      out += message('n', "");
    }
    for (const auto& row : r.rows) {
      Bytes body = be16(static_cast<uint32_t>(row.size()));
      for (const Cell& c : row) body += c.null ? be32(0xFFFFFFFFu) : be32(static_cast<uint32_t>(c.text.size())) + c.text;
      out += message('D', body);
    }
    out += message('C', cstr(r.tag));
  }

  // --- Taking in what the client sends
  void feed(const uint8_t* data, size_t size) {
    in.append(reinterpret_cast<const char*>(data), size);
    for (;;) {
      if (state == State::Startup && !started) {
        if (silent || in.size() < 4) return;
        const uint32_t length = (uint8_t(in[0]) << 24) | (uint8_t(in[1]) << 16) | (uint8_t(in[2]) << 8) | uint8_t(in[3]);
        if (in.size() < length) return;
        if (length == 8 && in.size() >= 8 && uint8_t(in[4]) == 0x04 && uint8_t(in[5]) == 0xD2 && uint8_t(in[6]) == 0x16 && uint8_t(in[7]) == 0x2F) {   // SSLRequest: no, thanks
          ssl_requests++;
          out += "N";
          in.erase(0, 8);
          continue;
        }
        handle_startup(in.substr(4, length - 4));
        in.erase(0, length);
        continue;
      }
      if (in.size() < 5) return;
      const uint32_t length = (uint8_t(in[1]) << 24) | (uint8_t(in[2]) << 16) | (uint8_t(in[3]) << 8) | uint8_t(in[4]);
      if (in.size() < 1 + length) return;
      const char type = in[0];
      const std::string body = in.substr(5, length - 4);
      in.erase(0, 1 + length);
      handle(type, body);
    }
  }

  static std::string take_cstring(const std::string& s, size_t* at) {
    const size_t end = s.find('\0', *at);
    std::string v = s.substr(*at, end - *at);
    *at = end + 1;
    return v;
  }

  void handle_startup(const std::string& body) {
    started = true;
    size_t at = 4;   // after the protocol version
    while (at < body.size() && body[at] != 0) {
      const std::string key = take_cstring(body, &at), value = take_cstring(body, &at);
      if (key == "user") startup_user = value;
      else if (key == "database") startup_database = value;
      else if (key == "application_name") startup_app = value;
      else if (key == "extra_float_digits") startup_float_digits = value;
    }
    switch (auth) {
      case Auth::Trust: login_done(); break;
      case Auth::Cleartext: out += message('R', be32(3)); state = State::Password; break;
      case Auth::Md5: out += message('R', be32(5) + Bytes("\x01\x02\x03\x04", 4)); state = State::Password; break;
      case Auth::Scram: out += message('R', be32(10) + cstr("SCRAM-SHA-256") + Bytes(1, '\0')); state = State::ScramFirst; break;
      case Auth::Kerberos: out += message('R', be32(2)); break;
    }
  }

  void handle(char type, const std::string& body) {
    if (silent) return;
    if (type == 'X') {
      terminated = true;
      return;
    }
    switch (state) {
      case State::Password: {
        size_t at = 0;
        const std::string given = take_cstring(body, &at);
        const std::string expected = auth == Auth::Cleartext ? password : "md5bb41a296aab6baccb36ff243a562abff";
        if (type == 'p' && given == expected) {
          login_done();
        } else {
          error("28P01", "password authentication failed for user \"" + startup_user + "\"");
        }
        return;
      }
      case State::ScramFirst: return scram_first(body);
      case State::ScramFinal: return scram_final(body);
      default: break;
    }
    if (garbage) {
      out += message('?', "what");
      return;
    }
    if (type == 'Q') {
      size_t at = 0;
      const std::string sql = take_cstring(body, &at);
      sql_log.push_back(sql);
      if (drop) {
        dropped = true;
        return;
      }
      // Several statements: each answers in turn.
      size_t from = 0;
      for (;;) {
        const size_t semicolon = sql.find(';', from);
        std::string one = sql.substr(from, semicolon == std::string::npos ? std::string::npos : semicolon - from);
        while (!one.empty() && one[0] == ' ') one.erase(0, 1);
        if (one.empty() && semicolon == std::string::npos && from > 0) break;
        const Reply r = run(one, {});
        emit(r, false);
        if (r.error || semicolon == std::string::npos) break;
        from = semicolon + 1;
      }
      ready();
    } else if (type == 'P') {
      if (skipping) return;
      size_t at = 0;
      take_cstring(body, &at);
      parsed_sql = take_cstring(body, &at);
      sql_log.push_back(parsed_sql);
      pending_parse = true;
    } else if (type == 'B') {
      if (skipping) return;
      size_t at = 0;
      take_cstring(body, &at);
      take_cstring(body, &at);
      const uint32_t formats = (uint8_t(body[at]) << 8) | uint8_t(body[at + 1]);
      at += 2 + 2 * formats;
      const uint32_t count = (uint8_t(body[at]) << 8) | uint8_t(body[at + 1]);
      at += 2;
      bound.clear();
      for (uint32_t i = 0; i < count; ++i) {
        const int32_t length = int32_t((uint8_t(body[at]) << 24) | (uint8_t(body[at + 1]) << 16) | (uint8_t(body[at + 2]) << 8) | uint8_t(body[at + 3]));
        at += 4;
        if (length < 0) {
          bound.push_back({true, ""});
        } else {
          bound.push_back({false, body.substr(at, static_cast<size_t>(length))});
          at += static_cast<size_t>(length);
        }
      }
      last_params = bound;
      pending_bind = true;
    } else if (type == 'D' || type == 'E') {
      if (type == 'E') pending_execute = true;
    } else if (type == 'S') {   // Sync: everything the client sent is answered now
      if (drop) {
        dropped = true;
        return;
      }
      if (!skipping && pending_parse) {
        const Reply r = run(parsed_sql, bound);
        if (r.error) {
          emit(r, true);
        } else {
          out += message('1', "");
          out += message('2', "");
          emit(r, true);
        }
      }
      pending_parse = pending_bind = pending_execute = false;
      skipping = false;
      ready();
    }
  }
  bool pending_parse = false, pending_bind = false, pending_execute = false, dropped = false;

  // --- SCRAM-SHA-256, the server's side
  static std::string b64(const uint8_t* d, size_t n) {
    static const char k[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
      const uint32_t t = (d[i] << 16) | ((i + 1 < n ? d[i + 1] : 0) << 8) | (i + 2 < n ? d[i + 2] : 0);
      s += k[(t >> 18) & 63];
      s += k[(t >> 12) & 63];
      s += i + 1 < n ? k[(t >> 6) & 63] : '=';
      s += i + 2 < n ? k[t & 63] : '=';
    }
    return s;
  }
  static void salted_password(const std::string& pw, const uint8_t* salt, size_t salt_size, uint32_t iterations, uint8_t out[32]) {
    uint8_t block[36];
    memcpy(block, salt, salt_size);
    block[salt_size + 0] = 0; block[salt_size + 1] = 0; block[salt_size + 2] = 0; block[salt_size + 3] = 1;
    uint8_t u[32], result[32];
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(reinterpret_cast<const uint8_t*>(pw.data()), pw.size()), ConstByteView(block, salt_size + 4), u);
    memcpy(result, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
      uint8_t next[32];
      crypto::hmac(crypto::HashKind::Sha256, ConstByteView(reinterpret_cast<const uint8_t*>(pw.data()), pw.size()), ConstByteView(u, 32), next);
      for (int k = 0; k < 32; ++k) { u[k] = next[k]; result[k] ^= next[k]; }
    }
    memcpy(out, result, 32);
  }
  void scram_first(const std::string& body) {
    size_t at = 0;
    const std::string mechanism = take_cstring(body, &at);
    const uint32_t length = (uint8_t(body[at]) << 24) | (uint8_t(body[at + 1]) << 16) | (uint8_t(body[at + 2]) << 8) | uint8_t(body[at + 3]);
    at += 4;
    const std::string first = body.substr(at, length);
    if (mechanism != "SCRAM-SHA-256" || first.rfind("n,,n=,r=", 0) != 0) {
      error("28000", "bad SCRAM start");
      return;
    }
    client_first_bare = first.substr(3);
    nonce = first.substr(8) + "SERVERNONCE";
    static const uint8_t salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    server_first = "r=" + nonce + ",s=" + b64(salt, 16) + ",i=4096";
    out += message('R', be32(11) + server_first);
    state = State::ScramFinal;
  }
  void scram_final(const std::string& body) {
    static const uint8_t salt[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const size_t proof_at = body.find(",p=");
    if (proof_at == std::string::npos) {
      error("28000", "bad SCRAM final");
      return;
    }
    const std::string without_proof = body.substr(0, proof_at), proof_b64 = body.substr(proof_at + 3);
    const std::string auth_message = client_first_bare + "," + server_first + "," + without_proof;
    uint8_t salted[32], client_key[32], stored[32], server_key[32];
    salted_password(password, salt, 16, 4096, salted);
    const std::string ck = "Client Key", sk = "Server Key";
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(salted, 32), ConstByteView(reinterpret_cast<const uint8_t*>(ck.data()), ck.size()), client_key);
    crypto::hash(crypto::HashKind::Sha256, ConstByteView(client_key, 32), stored);
    uint8_t signature[32];
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(stored, 32), ConstByteView(reinterpret_cast<const uint8_t*>(auth_message.data()), auth_message.size()), signature);
    uint8_t proof[32];
    for (int k = 0; k < 32; ++k) proof[k] = client_key[k] ^ signature[k];
    if (b64(proof, 32) != proof_b64) {
      error("28P01", "password authentication failed for user \"" + startup_user + "\"");
      return;
    }
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(salted, 32), ConstByteView(reinterpret_cast<const uint8_t*>(sk.data()), sk.size()), server_key);
    uint8_t server_signature[32];
    crypto::hmac(crypto::HashKind::Sha256, ConstByteView(server_key, 32), ConstByteView(reinterpret_cast<const uint8_t*>(auth_message.data()), auth_message.size()), server_signature);
    if (bad_server_proof) server_signature[0] ^= 1;
    out += message('R', be32(12) + "v=" + b64(server_signature, 32));
    login_done();
  }
};

