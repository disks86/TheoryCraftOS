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
#include "Crypto/Random.hpp"
#include "Database/ConnectionOptions.hpp"
#include "Database/IConnection.hpp"
#include "Database/IDatabaseTransport.hpp"
#include "Database/TextResultSet.hpp"
#include "Utility/ByteBuffer.hpp"

namespace database::postgres {

// A session with a PostgreSQL server (protocol 3.0). Logs in with a password the server asks for in any of the ways PostgreSQL has had since
// 7.4: clear text, MD5 and SCRAM-SHA-256 (not the -PLUS variant with channel binding; Kerberos, GSSAPI, SSPI and client certificates are not
// supported). A statement without parameters goes to the server as a simple query; one with parameters uses the extended protocol with the
// values sent as text, so they never become part of the SQL. Answers come back as text (TextResultSet); `bytea` columns are read in their hex form.
// COPY is not supported. Statements are limited by the options' query timeout and results by their size limit; a timeout closes the connection
// (the server may still be working on the statement).
class PostgresConnection final : public IConnection {
 public:
  // Connects through `transport` and logs in. `random` is needed for SCRAM-SHA-256 (it gives the client nonce).
  static DbStatus connect(allocators::IAllocator& allocator, const ConnectionOptions& options, IDatabaseTransport& transport, crypto::IRandom* random,
                          UniquePointer<IConnection>* connection, DbError* error);
  // Logs in over a stream that is already open (and TLS already started if `encrypted`).
  static DbStatus login(allocators::IAllocator& allocator, const ConnectionOptions& options, UniquePointer<stream::IStream> stream, bool encrypted,
                        crypto::IRandom* random, UniquePointer<IConnection>* connection, DbError* error);

  PostgresConnection(allocators::IAllocator& allocator, const ConnectionOptions& options, UniquePointer<stream::IStream> stream, bool encrypted);
  ~PostgresConnection();
  PostgresConnection(const PostgresConnection&) = delete;
  PostgresConnection& operator=(const PostgresConnection&) = delete;

  const char* driver_name() const override { return "postgres"; }
  bool is_open() const override { return open_; }
  StringView server_version() const override { return version_.view(); }
  bool encrypted() const override { return encrypted_; }
  using IConnection::execute;
  DbStatus execute(StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result) override;
  DbStatus begin() override { return execute("BEGIN"); }
  DbStatus commit() override { return execute("COMMIT"); }
  DbStatus rollback() override { return execute("ROLLBACK"); }
  bool in_transaction() const override { return open_ && tx_status_ != 'I'; }
  DbStatus ping() override { return execute("SELECT 1"); }
  const DbError& last_error() const override { return error_; }
  void close() override;

 private:
  DbStatus authenticate(const ConnectionOptions& options, crypto::IRandom* random);
  DbStatus startup(const ConnectionOptions& options, crypto::IRandom* random);
  // Collects the server's answers up to ReadyForQuery.
  DbStatus collect(UniquePointer<IResultSet>* result);
  DbStatus send_pending();
  DbStatus read_message(char* type);
  // Notes a failure of the conversation itself: the connection cannot be used any more.
  DbStatus broken(DbStatus status, StringView text);
  void read_error_fields(ConstByteView body);
  DbStatus encode_parameter(const DbParam& param, String* text, bool* is_null);

  static constexpr uint64_t kMaxControlMessage = 1u << 20;

  allocators::IAllocator& allocator_;
  UniquePointer<stream::IStream> stream_;
  ByteBuffer out_, body_;
  String version_;
  DbError error_;
  uint32_t query_timeout_ms_;
  uint64_t max_result_bytes_;
  bool open_ = true;
  bool encrypted_;
  char tx_status_ = 'I';   // 'I' idle, 'T' in a transaction, 'E' in a failed transaction
};

}  // namespace database::postgres
