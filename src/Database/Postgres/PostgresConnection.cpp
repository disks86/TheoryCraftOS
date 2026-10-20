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

#include "Database/Postgres/PostgresConnection.hpp"
#include "Database/NumberText.hpp"
#include "Database/Postgres/PostgresAuth.hpp"
#include "Database/Postgres/PostgresProtocol.hpp"
#include "Database/SqlText.hpp"
#include "Stream/StreamHelpers.hpp"

namespace database::postgres {

DbType db_type_for_oid(uint32_t oid) {
  switch (oid) {
    case 16: return DbType::Boolean;
    case 17: return DbType::Bytes;
    case 20: case 21: case 23: case 26: return DbType::Integer;
    case 18: case 19: case 25: case 1042: case 1043: return DbType::Text;
    case 114: case 3802: return DbType::Json;
    case 700: case 701: return DbType::Real;
    case 1700: return DbType::Decimal;
    case 1082: return DbType::Date;
    case 1083: case 1266: return DbType::Time;
    case 1114: case 1184: return DbType::Timestamp;
    case 2950: return DbType::Uuid;
    default: return DbType::Unknown;
  }
}

namespace {
DbStatus from_stream(stream::StreamStatus s) {
  switch (s) {
    case stream::StreamStatus::Ok: return DbStatus::Ok;
    case stream::StreamStatus::TimedOut: return DbStatus::Timeout;
    case stream::StreamStatus::OutOfMemory: return DbStatus::OutOfMemory;
    case stream::StreamStatus::HostNotFound: return DbStatus::ResolveFailed;
    default: return DbStatus::ConnectionLost;
  }
}

// The number at the end of a command tag: "INSERT 0 5" and "UPDATE 3" give 5 and 3, "CREATE TABLE" gives 0.
uint64_t tag_count(StringView tag) {
  const uint64_t space = tag.rfind(' ');
  int64_t n = 0;
  if (space == StringView::npos || !parse_int64(tag.substr(space + 1), &n) || n < 0) return 0;
  return static_cast<uint64_t>(n);
}
}  // namespace

PostgresConnection::PostgresConnection(allocators::IAllocator& allocator, const ConnectionOptions& options, UniquePointer<stream::IStream> stream, bool encrypted)
    : allocator_(allocator),
      stream_(util::move(stream)),
      out_(allocator),
      body_(allocator),
      version_(allocator),
      error_(allocator),
      query_timeout_ms_(options.query_timeout_ms),
      max_result_bytes_(options.max_result_bytes),
      encrypted_(encrypted) {}

PostgresConnection::~PostgresConnection() { close(); }

void PostgresConnection::close() {
  if (!open_) return;
  open_ = false;
  if (stream_) {
    out_.clear();
    MessageWriter w(out_);
    w.begin('X');   // Terminate
    w.end();
    if (w.ok()) {
      stream_->set_write_timeout(1000);
      stream_->write(out_.view());   // (best effort: the server does not answer)
    }
    stream_->close();
  }
}

DbStatus PostgresConnection::broken(DbStatus status, StringView text) {
  error_.set(status, text);
  open_ = false;
  if (stream_) stream_->close();
  return status;
}

DbStatus PostgresConnection::send_pending() {
  const stream::StreamStatus s = stream_->write(out_.view());
  out_.clear();
  if (s != stream::StreamStatus::Ok) return broken(from_stream(s), "the connection to the server broke while sending");
  const stream::StreamStatus f = stream_->flush();
  if (f != stream::StreamStatus::Ok) return broken(from_stream(f), "the connection to the server broke while sending");
  return DbStatus::Ok;
}

DbStatus PostgresConnection::read_message(char* type) {
  uint8_t header[5];
  stream::StreamStatus s = stream::read_exactly(*stream_, ByteView(header, 5));
  if (s != stream::StreamStatus::Ok) {
    return broken(s == stream::StreamStatus::TimedOut ? DbStatus::Timeout : DbStatus::ConnectionLost,
                  s == stream::StreamStatus::TimedOut ? "the server did not answer in time" : "the server closed the connection");
  }
  const uint32_t length = (uint32_t{header[1]} << 24) | (uint32_t{header[2]} << 16) | (uint32_t{header[3]} << 8) | header[4];
  if (length < 4) return broken(DbStatus::Protocol, "the server sent a message with an impossible length");
  const uint64_t body = length - 4;
  // A row can be as big as the whole result may be; anything else is small.
  const uint64_t limit = (header[0] == 'D' ? max_result_bytes_ : 0) + kMaxControlMessage;
  if (body > limit) return broken(DbStatus::TooLarge, "the server sent a message larger than the limit");
  body_.clear();
  if (body) {
    uint64_t done = 0;
    uint8_t chunk[1024];
    while (done < body) {
      const uint64_t want = body - done < sizeof chunk ? body - done : sizeof chunk;
      s = stream::read_exactly(*stream_, ByteView(chunk, want));
      if (s != stream::StreamStatus::Ok) {
        return broken(s == stream::StreamStatus::TimedOut ? DbStatus::Timeout : DbStatus::ConnectionLost, "the server stopped in the middle of a message");
      }
      if (!body_.append(ConstByteView(chunk, want))) return broken(DbStatus::OutOfMemory, "out of memory");
      done += want;
    }
  }
  *type = static_cast<char>(header[0]);
  return DbStatus::Ok;
}

// An ErrorResponse: fields of a type byte and text until a zero type byte.
void PostgresConnection::read_error_fields(ConstByteView body) {
  MessageReader r(body);
  error_.clear();
  error_.status = DbStatus::Server;
  while (!r.at_end()) {
    const uint8_t code = r.get8();
    if (code == 0) break;
    const StringView value = r.get_cstring();
    if (!r.ok()) break;
    if (code == 'C') error_.sqlstate.assign(value);
    else if (code == 'M') error_.message.assign(value);
    else if (code == 'D') error_.detail.assign(value);
  }
  if (error_.message.empty()) error_.message.assign("the server reported an error");
}

DbStatus PostgresConnection::collect(UniquePointer<IResultSet>* result) {
  UniquePointer<TextResultSet> current, finished;
  bool too_large = false;
  bool server_error = false;
  error_.clear();
  for (;;) {
    char type;
    const DbStatus s = read_message(&type);
    if (s != DbStatus::Ok) return s;
    MessageReader r(body_.view());
    switch (type) {
      case 'T': {   // RowDescription
        const uint32_t count = r.get16();
        current = UniquePointer<TextResultSet>::make(allocator_, allocator_);
        if (!current) return broken(DbStatus::OutOfMemory, "out of memory");
        for (uint32_t i = 0; i < count && r.ok(); ++i) {
          const StringView name = r.get_cstring();
          r.get32();   // table
          r.get16();   // column number
          const uint32_t oid = r.get32();
          r.get16();   // size
          r.get32();   // modifier
          r.get16();   // format
          if (!r.ok() || !current->add_column(name, db_type_for_oid(oid))) return broken(DbStatus::Protocol, "a bad row description from the server");
        }
        if (!r.ok()) return broken(DbStatus::Protocol, "a bad row description from the server");
        break;
      }
      case 'D': {   // DataRow
        if (!current || too_large || server_error) break;
        const uint32_t count = r.get16();
        if (!r.ok() || count != current->column_count() || !current->begin_row()) return broken(DbStatus::Protocol, "a row that does not match its description");
        for (uint32_t i = 0; i < count; ++i) {
          const int32_t length = r.get_i32();
          if (!r.ok()) return broken(DbStatus::Protocol, "a truncated row");
          bool ok;
          if (length < 0) {
            ok = current->add_null();
          } else {
            const ConstByteView v = r.get_bytes(static_cast<uint64_t>(length));
            if (!r.ok()) return broken(DbStatus::Protocol, "a truncated row");
            ok = current->add_cell(StringView(v));
          }
          if (!ok) return broken(DbStatus::OutOfMemory, "out of memory");
        }
        if (current->bytes_used() > max_result_bytes_) {   // too much: keep reading (to stay in step with the server) and keep nothing
          too_large = true;
          current.reset();
          finished.reset();
        }
        break;
      }
      case 'C': {   // CommandComplete
        const StringView tag = r.get_cstring();
        if (!r.ok()) return broken(DbStatus::Protocol, "a bad command tag from the server");
        if (!too_large && !server_error) {
          if (!current) {
            current = UniquePointer<TextResultSet>::make(allocator_, allocator_);
            if (!current) return broken(DbStatus::OutOfMemory, "out of memory");
          }
          current->set_command(tag, tag_count(tag));
          finished = util::move(current);
        }
        current.reset();
        break;
      }
      case 'I': {   // EmptyQueryResponse
        if (!too_large && !server_error) {
          finished = UniquePointer<TextResultSet>::make(allocator_, allocator_);
          if (!finished) return broken(DbStatus::OutOfMemory, "out of memory");
        }
        break;
      }
      case 'E':
        read_error_fields(body_.view());
        server_error = true;
        current.reset();
        finished.reset();
        break;
      case 'S': {   // ParameterStatus
        const StringView name = r.get_cstring();
        const StringView value = r.get_cstring();
        if (r.ok() && name.equals("server_version")) version_.assign(value);
        break;
      }
      case 'Z':   // ReadyForQuery: the answer is complete
        tx_status_ = body_.size() ? static_cast<char>(body_.view()[0]) : 'I';
        if (server_error) return DbStatus::Server;
        if (too_large) {
          error_.set(DbStatus::TooLarge, "the answer is larger than the connection's limit");
          return DbStatus::TooLarge;
        }
        if (result) {
          if (finished) result->reset(), *result = UniquePointer<IResultSet>(util::move(finished));
          else *result = UniquePointer<IResultSet>();
        }
        return DbStatus::Ok;
      case '1': case '2': case '3': case 'n': case 't': case 's': case 'N': case 'A': case 'K':
        break;   // ParseComplete, BindComplete, CloseComplete, NoData, ParameterDescription, PortalSuspended, notices, notifications, key data
      case 'G': case 'H': case 'W':
        return broken(DbStatus::NotSupported, "COPY is not supported");
      default:
        return broken(DbStatus::Protocol, "the server sent an unknown message");
    }
  }
}

DbStatus PostgresConnection::encode_parameter(const DbParam& p, String* text, bool* is_null) {
  *is_null = false;
  text->clear();
  switch (p.kind) {
    case DbParam::Kind::Null: *is_null = true; return DbStatus::Ok;
    case DbParam::Kind::Boolean: return text->push_back(p.boolean ? 't' : 'f') ? DbStatus::Ok : DbStatus::OutOfMemory;
    case DbParam::Kind::Integer: return text->append_int(p.integer) ? DbStatus::Ok : DbStatus::OutOfMemory;
    case DbParam::Kind::Real: return append_double(text, p.real) ? DbStatus::Ok : DbStatus::OutOfMemory;
    case DbParam::Kind::Text:
      for (uint64_t i = 0; i < p.text.size(); ++i)
        if (p.text[i] == 0) return DbStatus::InvalidArgument;   // PostgreSQL text cannot hold a zero byte
      return text->append(p.text) ? DbStatus::Ok : DbStatus::OutOfMemory;
    case DbParam::Kind::Bytes: {   // the hex form of bytea
      static const char kHex[] = "0123456789abcdef";
      if (!text->append("\\x")) return DbStatus::OutOfMemory;
      for (uint64_t i = 0; i < p.bytes.size(); ++i)
        if (!text->push_back(kHex[p.bytes[i] >> 4]) || !text->push_back(kHex[p.bytes[i] & 15])) return DbStatus::OutOfMemory;
      return DbStatus::Ok;
    }
  }
  return DbStatus::InvalidArgument;
}

DbStatus PostgresConnection::execute(StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result) {
  if (result) result->reset();
  if (!open_) {
    error_.set(DbStatus::Closed, "the connection is closed");
    return DbStatus::Closed;
  }
  error_.clear();
  for (uint64_t i = 0; i < sql.size(); ++i) {
    if (sql[i] == 0) {
      error_.set(DbStatus::InvalidArgument, "SQL text cannot hold a zero byte");
      return DbStatus::InvalidArgument;
    }
  }
  String text(allocator_);
  uint32_t markers = 0;
  if (!translate_placeholders(sql, PlaceholderStyle::Numbered, &text, &markers)) {
    error_.set(DbStatus::OutOfMemory, "out of memory");
    return DbStatus::OutOfMemory;
  }
  if (markers != params.size()) {
    String why(allocator_);
    why.append("the SQL has ");
    why.append_uint(markers);
    why.append(" parameter markers (?) but ");
    why.append_uint(params.size());
    why.append(" values were given");
    error_.set(DbStatus::InvalidArgument, why.view());
    return DbStatus::InvalidArgument;
  }

  out_.clear();
  MessageWriter w(out_);
  if (params.empty()) {
    w.begin('Q');
    w.put_cstring(text.view());
    w.end();
  } else {
    w.begin('P');   // Parse: an unnamed statement, parameter types left to the server
    w.put_cstring("");
    w.put_cstring(text.view());
    w.put16(0);
    w.end();
    w.begin('B');   // Bind: an unnamed portal, every value as text, every column of the answer as text
    w.put_cstring("");
    w.put_cstring("");
    w.put16(0);
    w.put16(static_cast<uint32_t>(params.size()));
    String value(allocator_);
    for (uint64_t i = 0; i < params.size(); ++i) {
      bool is_null;
      const DbStatus e = encode_parameter(params[i], &value, &is_null);
      if (e != DbStatus::Ok) {
        out_.clear();
        error_.set(e, e == DbStatus::InvalidArgument ? "a text value cannot hold a zero byte" : "out of memory");
        return e;
      }
      if (is_null) {
        w.put32(0xFFFFFFFFu);
      } else {
        w.put32(static_cast<uint32_t>(value.size()));
        w.put_bytes(value.view().bytes());
      }
    }
    w.put16(0);
    w.end();
    w.begin('D');   // Describe the portal, to get the row description
    w.put8('P');
    w.put_cstring("");
    w.end();
    w.begin('E');   // Execute, all rows
    w.put_cstring("");
    w.put32(0);
    w.end();
    w.begin('S');   // Sync
    w.end();
  }
  if (!w.ok()) {
    out_.clear();
    error_.set(DbStatus::OutOfMemory, "out of memory");
    return DbStatus::OutOfMemory;
  }
  stream_->set_read_timeout(query_timeout_ms_);
  DbStatus s = send_pending();
  if (s != DbStatus::Ok) return s;
  return collect(result);
}

DbStatus PostgresConnection::startup(const ConnectionOptions& options, crypto::IRandom* random) {
  out_.clear();
  MessageWriter w(out_);
  w.begin(0);   // the startup message has no type byte
  w.put32(kProtocolVersion);
  auto pair = [&](const char* name, StringView value) {
    w.put_cstring(name);
    w.put_cstring(value);
  };
  pair("user", options.user.view());
  if (!options.database.empty()) pair("database", options.database.view());
  pair("client_encoding", "UTF8");
  pair("extra_float_digits", "3");   // doubles read back exactly, also from servers before 12
  pair("application_name", options.application_name.empty() ? StringView("TheoryCraftOS") : options.application_name.view());
  w.put8(0);
  w.end();
  if (!w.ok()) return broken(DbStatus::OutOfMemory, "out of memory");
  stream_->set_read_timeout(options.connect_timeout_ms);
  DbStatus s = send_pending();
  if (s != DbStatus::Ok) return s;
  return authenticate(options, random);
}

DbStatus PostgresConnection::authenticate(const ConnectionOptions& options, crypto::IRandom* random) {
  ScramClient scram(allocator_);
  String scratch(allocator_);
  for (;;) {
    char type;
    DbStatus s = read_message(&type);
    if (s != DbStatus::Ok) return s;
    MessageReader r(body_.view());
    if (type == 'E') {
      read_error_fields(body_.view());
      const bool login = error_.sqlstate.size() >= 2 && error_.sqlstate.view().first(2).equals("28");
      if (login) error_.status = DbStatus::AuthFailed;
      const DbStatus status = error_.status;
      open_ = false;
      stream_->close();
      return status;
    }
    if (type == 'S') {
      const StringView name = r.get_cstring();
      const StringView value = r.get_cstring();
      if (r.ok() && name.equals("server_version")) version_.assign(value);
      continue;
    }
    if (type == 'K' || type == 'N') continue;
    if (type == 'Z') {
      tx_status_ = body_.size() ? static_cast<char>(body_.view()[0]) : 'I';
      return DbStatus::Ok;
    }
    if (type != 'R') return broken(DbStatus::Protocol, "the server sent something unexpected while logging in");

    const uint32_t code = r.get32();
    if (!r.ok()) return broken(DbStatus::Protocol, "a bad authentication message");
    out_.clear();
    MessageWriter w(out_);
    switch (code) {
      case 0: continue;   // AuthenticationOk
      case 3:             // clear text
        if (options.password.empty()) return broken(DbStatus::AuthFailed, "the server asks for a password and none was given");
        w.begin('p');
        w.put_cstring(options.password.view());
        w.end();
        break;
      case 5: {           // MD5
        const ConstByteView salt = r.get_bytes(4);
        if (!r.ok()) return broken(DbStatus::Protocol, "a bad authentication message");
        if (options.password.empty()) return broken(DbStatus::AuthFailed, "the server asks for a password and none was given");
        if (!md5_password(options.user.view(), options.password.view(), salt, &scratch)) return broken(DbStatus::OutOfMemory, "out of memory");
        w.begin('p');
        w.put_cstring(scratch.view());
        w.end();
        break;
      }
      case 10: {          // SASL: the server lists the mechanisms it has
        bool scram_available = false;
        while (!r.at_end()) {
          const StringView mechanism = r.get_cstring();
          if (!r.ok() || mechanism.empty()) break;
          if (mechanism.equals("SCRAM-SHA-256")) scram_available = true;
        }
        if (!scram_available) return broken(DbStatus::NotSupported, "the server offers no password mechanism this client has (SCRAM-SHA-256)");
        if (options.password.empty()) return broken(DbStatus::AuthFailed, "the server asks for a password and none was given");
        if (!random) return broken(DbStatus::NotSupported, "SCRAM-SHA-256 needs a source of random numbers");
        uint8_t raw[18];
        if (!random->fill(ByteView(raw, sizeof raw))) return broken(DbStatus::NotSupported, "no random numbers are available");
        String nonce(allocator_), first(allocator_);
        if (!append_base64(&nonce, ConstByteView(raw, sizeof raw)) || !scram.start(StringView(), options.password.view(), nonce.view(), &first))
          return broken(DbStatus::OutOfMemory, "out of memory");
        w.begin('p');   // SASLInitialResponse: the mechanism, then the length and the bytes of the first message
        w.put_cstring("SCRAM-SHA-256");
        w.put32(static_cast<uint32_t>(first.size()));
        w.put_bytes(first.view().bytes());
        w.end();
        break;
      }
      case 11: {          // SASLContinue
        String final_message(allocator_);
        if (!scram.handle_server_first(StringView(r.rest()), &final_message)) return broken(DbStatus::AuthFailed, "the server's SCRAM reply is not valid");
        w.begin('p');
        w.put_bytes(final_message.view().bytes());
        w.end();
        break;
      }
      case 12:            // SASLFinal: the server proves it knew the password too
        if (!scram.verify_server_final(StringView(r.rest()))) return broken(DbStatus::AuthFailed, "the server could not prove that it knows the password");
        continue;
      default: {
        String why(allocator_);
        why.append("the server wants authentication method ");
        why.append_uint(code);
        why.append(", which is not supported");
        return broken(DbStatus::NotSupported, why.view());
      }
    }
    if (!w.ok()) return broken(DbStatus::OutOfMemory, "out of memory");
    s = send_pending();
    if (s != DbStatus::Ok) return s;
  }
}

DbStatus PostgresConnection::login(allocators::IAllocator& allocator, const ConnectionOptions& options, UniquePointer<stream::IStream> stream, bool encrypted,
                                   crypto::IRandom* random, UniquePointer<IConnection>* connection, DbError* error) {
  UniquePointer<PostgresConnection> c = UniquePointer<PostgresConnection>::make(allocator, allocator, options, util::move(stream), encrypted);
  if (!c) {
    if (error) error->set(DbStatus::OutOfMemory, "out of memory");
    return DbStatus::OutOfMemory;
  }
  const DbStatus s = c->startup(options, random);
  if (s != DbStatus::Ok) {
    if (error) {
      error->clear();
      error->status = c->error_.status;
      error->sqlstate.assign(c->error_.sqlstate.view());
      error->message.assign(c->error_.message.view());
      error->detail.assign(c->error_.detail.view());
    }
    return s;
  }
  *connection = UniquePointer<IConnection>(util::move(c));
  return DbStatus::Ok;
}

DbStatus PostgresConnection::connect(allocators::IAllocator& allocator, const ConnectionOptions& options, IDatabaseTransport& transport, crypto::IRandom* random,
                                     UniquePointer<IConnection>* connection, DbError* error) {
  static const uint8_t kSslRequest[8] = {0, 0, 0, 8, 0x04, 0xD2, 0x16, 0x2F};   // length 8, code 80877103
  Endpoint endpoint;
  endpoint.host = options.host.view();
  endpoint.port = options.port ? options.port : 5432;
  endpoint.timeout_ms = options.connect_timeout_ms;
  endpoint.tls = options.tls;
  const TlsProbe probe{ConstByteView(kSslRequest, sizeof kSslRequest), 'S'};
  UniquePointer<stream::IStream> stream;
  bool encrypted = false;
  const DbStatus s = transport.open(endpoint, &probe, &stream, &encrypted, error);
  if (s != DbStatus::Ok) return s;
  return login(allocator, options, util::move(stream), encrypted, random, connection, error);
}

}  // namespace database::postgres
