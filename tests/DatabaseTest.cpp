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

// Host-side tests for database access: the pieces every driver uses (URLs, SQL placeholders, number text, result sets, the manager) and the PostgreSQL
// driver, which talks to a pretend server written here from the protocol description: the logins (clear text, MD5 and SCRAM-SHA-256, with the RFC 7677
// example for SCRAM), simple and extended queries, parameters of every kind, errors, transactions, and the ways a connection can break.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "Crypto/Crypto.hpp"
#include "Database/ConnectionOptions.hpp"
#include "Database/DatabaseManager.hpp"
#include "Database/NumberText.hpp"
#include "Database/Postgres/PostgresAuth.hpp"
#include "Database/Postgres/PostgresConnection.hpp"
#include "Database/Postgres/PostgresDriver.hpp"
#include "Database/SqlText.hpp"
#include "Database/TextResultSet.hpp"
#include "Stream/StreamBase.hpp"
#include "FakePostgres.hpp"

using namespace database;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// The pieces every driver uses

static void test_numbers() {
  TestAllocator a;
  {
    int64_t i = 0;
    CHECK(parse_int64("42", &i) && i == 42 && parse_int64("-9223372036854775808", &i) && i == INT64_MIN && parse_int64("+7", &i) && i == 7);
    CHECK(!parse_int64("", &i) && !parse_int64("-", &i) && !parse_int64("1.5", &i) && !parse_int64("9223372036854775808", &i) && !parse_int64("12a", &i));
    double d = 0;
    CHECK(parse_double("1.5", &d) && d == 1.5 && parse_double("-0.25", &d) && d == -0.25 && parse_double("1e3", &d) && d == 1000 && parse_double("  7 ", &d) && d == 7);
    CHECK(parse_double("NaN", &d) && d != d && parse_double("Infinity", &d) && d > 1e308 && parse_double("-Infinity", &d) && d < -1e308);
    CHECK(!parse_double("", &d) && !parse_double("1.2.3", &d) && !parse_double("e5", &d) && !parse_double("1e", &d) && !parse_double("abc", &d));
    String s(a);
    CHECK(append_double(&s, 1.5) && s.view().equals("1.5"));
    s.clear();
    append_double(&s, 100);
    CHECK(s.view().equals("100"));
    s.clear();
    append_double(&s, -0.001);
    CHECK(s.view().equals("-0.001"));
    s.clear();
    append_double(&s, 0.1);
    CHECK(parse_double(s.view(), &d) && d > 0.0999999999999 && d < 0.1000000000001);
    s.clear();
    append_double(&s, 1e25);
    CHECK(s.view().find('E') != StringView::npos && parse_double(s.view(), &d) && d > 0.99999999999e25 && d < 1.00000000001e25);
    s.clear();
    append_double(&s, 0.0 / 0.0 * 0 + __builtin_nan(""));
    CHECK(s.view().equals("NaN"));
    s.clear();
    append_double(&s, 1234567.891);
    CHECK(parse_double(s.view(), &d) && d > 1234567.8909999 && d < 1234567.8910001);
  }
  CHECK(a.live == 0);
}

static void test_url() {
  TestAllocator a;
  {
    ConnectionOptions o(a);
    DbError e(a);
    CHECK(parse_database_url("postgres://app:s3cret@db.example.com:5433/shop?sslmode=require&connect_timeout=5&application_name=billing", &o, &e));
    CHECK(o.driver.view().equals("postgres") && o.user.view().equals("app") && o.password.view().equals("s3cret") && o.host.view().equals("db.example.com") && o.port == 5433);
    CHECK(o.database.view().equals("shop") && o.tls == TlsMode::Require && o.connect_timeout_ms == 5000 && o.application_name.view().equals("billing"));

    ConnectionOptions p(a);
    CHECK(parse_database_url("PostgreSQL://localhost", &p) && p.driver.view().equals("postgres") && p.host.view().equals("localhost") && p.port == 0 && p.user.empty() && p.database.empty());
    CHECK(p.tls == TlsMode::Prefer && p.connect_timeout_ms == ConnectionOptions::kDefaultConnectTimeoutMs);

    ConnectionOptions q(a);
    CHECK(parse_database_url("postgres://us%40er:p%3Ass%2Fw@h/db%20name?sslmode=disable&max_result_bytes=4096", &q) && q.user.view().equals("us@er") && q.password.view().equals("p:ss/w"));
    CHECK(q.database.view().equals("db name") && q.tls == TlsMode::Disable && q.max_result_bytes == 4096);
    ConnectionOptions r(a);
    CHECK(parse_database_url("postgres://u@h?sslmode=verify-full", &r) && r.tls == TlsMode::Require && r.password.empty() && r.user.view().equals("u"));
    ConnectionOptions s(a);
    CHECK(parse_database_url("postgres://u:@h:5432/", &s) && s.password.empty() && s.port == 5432 && s.database.empty());

    const char* bad[] = {"localhost/db", "://host", "postgres://", "postgres://:5432/db", "postgres://h:0/db", "postgres://h:70000/db", "postgres://h:abc/db", "postgres://u%zz@h/db",
                         "postgres://h/db%2", "postgres://h/db?sslmode=maybe", "postgres://h/db?connect_timeout=x", "postgres://h/db?max_result_bytes=1"};
    for (const char* url : bad) {
      ConnectionOptions t(a);
      DbError err(a);
      CHECK(!parse_database_url(url, &t, &err) && err.status == DbStatus::InvalidArgument && !err.message.empty());
      if (err.message.empty()) printf("   (%s)\n", url);
    }
  }
  CHECK(a.live == 0);
}

static bool translated(TestAllocator& a, const char* sql, PlaceholderStyle style, const char* expected, uint32_t markers) {
  String out(a);
  uint32_t count = 99;
  const bool ok = translate_placeholders(sql, style, &out, &count) && out.view().equals(expected) && count == markers;
  if (!ok) printf("   \"%s\" became \"%s\" with %u markers\n", sql, out.c_str(), count);
  return ok;
}

static void test_sql_text() {
  TestAllocator a;
  {
    const PlaceholderStyle n = PlaceholderStyle::Numbered, q = PlaceholderStyle::Question;
    CHECK(translated(a, "SELECT * FROM t WHERE a = ? AND b = ?", n, "SELECT * FROM t WHERE a = $1 AND b = $2", 2));
    CHECK(translated(a, "SELECT * FROM t WHERE a = ? AND b = ?", q, "SELECT * FROM t WHERE a = ? AND b = ?", 2));
    CHECK(translated(a, "SELECT 'is it?' , \"what?\", ?", n, "SELECT 'is it?' , \"what?\", $1", 1));
    CHECK(translated(a, "SELECT 'it''s ?', ?", n, "SELECT 'it''s ?', $1", 1));
    CHECK(translated(a, "SELECT E'a\\'?', ?", n, "SELECT E'a\\'?', $1", 1));
    CHECK(translated(a, "SELECT $$ ? $$, ?", n, "SELECT $$ ? $$, $1", 1));
    CHECK(translated(a, "SELECT $tag$ ? $tag$, ?", n, "SELECT $tag$ ? $tag$, $1", 1));
    CHECK(translated(a, "SELECT 1 -- why?\n, ?", n, "SELECT 1 -- why?\n, $1", 1));
    CHECK(translated(a, "SELECT /* a /* nested ? */ ? */ ?", n, "SELECT /* a /* nested ? */ ? */ $1", 1));
    CHECK(translated(a, "SELECT data ?? 'key', ?", n, "SELECT data ? 'key', $1", 1));
    CHECK(translated(a, "SELECT $1, ?", n, "SELECT $1, $1", 1));   // (a numbered marker is the caller's own)
    CHECK(translated(a, "SELECT 1", n, "SELECT 1", 0));
    CHECK(translated(a, "", n, "", 0));
    CHECK(translated(a, "SELECT 'unterminated ?", n, "SELECT 'unterminated ?", 0));
  }
  CHECK(a.live == 0);
}

static void test_result_set() {
  TestAllocator a;
  {
    TextResultSet rs(a);
    CHECK(rs.column_count() == 0 && rs.row_count() == 0 && rs.complete());
    CHECK(rs.add_column("id", DbType::Integer) && rs.add_column("Name", DbType::Text) && rs.add_column("data", DbType::Bytes) && rs.add_column("ok", DbType::Boolean) &&
          rs.add_column("score", DbType::Real));
    CHECK(rs.begin_row() && rs.add_cell("7") && rs.add_cell("alpha") && rs.add_cell("\\x0001ff") && rs.add_cell("t") && rs.add_cell("1.5"));
    CHECK(rs.begin_row() && rs.add_cell("-3") && rs.add_null() && rs.add_null() && rs.add_cell("false") && rs.add_cell("x"));
    CHECK(!rs.add_column("late", DbType::Text));
    rs.set_command("SELECT 2", 2);
    CHECK(rs.complete() && rs.column_count() == 5 && rs.row_count() == 2 && rs.rows_affected() == 2 && rs.command().equals("SELECT 2"));
    CHECK(rs.column_name(1).equals("Name") && rs.column_type(2) == DbType::Bytes && rs.find_column("NAME") == 1 && rs.find_column("nope") == -1 && rs.column_name(9).empty());
    CHECK(rs.text(0, 1).equals("alpha") && rs.text(1, 1).empty() && rs.is_null(1, 1) && !rs.is_null(0, 1) && rs.is_null(5, 0) && rs.is_null(0, 9));
    int64_t i = 0;
    CHECK(rs.get_int(0, 0, &i) && i == 7 && rs.get_int(1, 0, &i) && i == -3 && !rs.get_int(0, 1, &i) && !rs.get_int(1, 1, &i));
    CHECK(rs.get_int(0, 3, &i) && i == 1 && rs.get_int(1, 3, &i) && i == 0);   // booleans read as 0 and 1
    bool b = false;
    CHECK(rs.get_bool(0, 3, &b) && b && rs.get_bool(1, 3, &b) && !b && !rs.get_bool(0, 0, &b));
    double d = 0;
    CHECK(rs.get_real(0, 4, &d) && d == 1.5 && !rs.get_real(1, 4, &d) && rs.get_real(0, 0, &d) && d == 7);
    ByteBuffer bytes(a);
    CHECK(rs.get_bytes(0, 2, &bytes) && bytes.size() == 3 && bytes.view()[0] == 0 && bytes.view()[1] == 1 && bytes.view()[2] == 0xFF);
    CHECK(rs.get_bytes(0, 1, &bytes) && bytes.size() == 5 && bytes.view()[0] == 'a' && !rs.get_bytes(1, 2, &bytes));
    // A row with missing cells is incomplete.
    TextResultSet half(a);
    half.add_column("a", DbType::Text);
    half.add_column("b", DbType::Text);
    half.begin_row();
    half.add_cell("x");
    CHECK(!half.complete() && !half.begin_row());
    // No columns: just a count of what the statement did.
    TextResultSet none(a);
    none.set_command("INSERT 0 5", 5);
    CHECK(none.column_count() == 0 && none.row_count() == 0 && none.rows_affected() == 5 && !none.add_cell("x"));
  }
  CHECK(a.live == 0);
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// A pretend PostgreSQL server

// The client's end of a connection to the pretend server.
struct PairStream final : stream::StreamBase {
  explicit PairStream(FakeServer* s) : server(s) {}
  bool can_read() const override { return !closed_; }
  bool can_write() const override { return !closed_; }
  bool can_timeout() const override { return true; }
  stream::StreamStatus read(ByteView buffer, uint64_t* count) override {
    *count = 0;
    if (closed_) return stream::StreamStatus::Closed;
    if (position >= server->out.size()) {
      if (server->dropped) return stream::StreamStatus::Ok;   // the end of the connection
      return stream::StreamStatus::TimedOut;
    }
    const size_t n = std::min<size_t>(buffer.size(), server->out.size() - position);
    memcpy(buffer.data(), server->out.data() + position, n);
    position += n;
    *count = n;
    return stream::StreamStatus::Ok;
  }
  stream::StreamStatus write(ConstByteView data) override {
    if (closed_) return stream::StreamStatus::Closed;
    server->feed(data.data(), data.size());
    return stream::StreamStatus::Ok;
  }
  stream::StreamStatus close() override {
    closed_ = true;
    ++closes;
    return stream::StreamStatus::Ok;
  }
  stream::StreamStatus set_read_timeout(uint32_t ms) override { read_timeout = ms; server->last_read_timeout = ms; return stream::StreamStatus::Ok; }
  stream::StreamStatus set_write_timeout(uint32_t) override { return stream::StreamStatus::Ok; }
  bool is_shut() const { return closed_; }
  FakeServer* server;
  size_t position = 0;
  uint32_t read_timeout = 0;
  int closes = 0;
};

struct FixedRandom final : crypto::IRandom {
  bool fill(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(0x30 + i);
    return true;
  }
};

struct FakeTransport final : IDatabaseTransport {
  FakeServer* server = nullptr;
  DbStatus fail = DbStatus::Ok;
  std::string host;
  uint16_t port = 0;
  TlsMode tls = TlsMode::Prefer;
  Bytes probe_request;
  uint8_t probe_accept = 0;
  bool tls_available = false;
  allocators::IAllocator* allocator;
  PairStream* last = nullptr;
  explicit FakeTransport(allocators::IAllocator& a) : allocator(&a) {}
  DbStatus open(const Endpoint& e, const TlsProbe* probe, UniquePointer<stream::IStream>* out, bool* encrypted, DbError* error) override {
    host.assign(e.host.data(), e.host.size());
    port = e.port;
    tls = e.tls;
    if (probe) {
      probe_request.assign(reinterpret_cast<const char*>(probe->request.data()), probe->request.size());
      probe_accept = probe->accept;
    }
    if (fail != DbStatus::Ok) {
      if (error) error->set(fail, "the transport failed");
      return fail;
    }
    *encrypted = tls_available && e.tls != TlsMode::Disable;
    UniquePointer<PairStream> s = UniquePointer<PairStream>::make(*allocator, server);
    last = s.get();
    *out = UniquePointer<stream::IStream>(util::move(s));
    return DbStatus::Ok;
  }
};

// ------------------------------------------------------------------------------------------------------------------------------------------------------

static void test_scram_and_md5() {
  TestAllocator a;
  {
    // RFC 7677, section 3: the example exchange (user "user", password "pencil").
    postgres::ScramClient scram(a);
    String first(a), final_message(a);
    CHECK(scram.start("user", "pencil", "rOprNGfwEbeRWgbNEkqO", &first) && first.view().equals("n,,n=user,r=rOprNGfwEbeRWgbNEkqO"));
    CHECK(scram.handle_server_first("r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096", &final_message));
    CHECK(final_message.view().equals("c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ="));
    CHECK(scram.verify_server_final("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
    CHECK(!scram.verify_server_final("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G5=") && !scram.verify_server_final("e=invalid-proof") && !scram.verify_server_final(""));

    // Servers that do not follow the exchange are refused.
    postgres::ScramClient other(a);
    other.start("", "pw", "abcdef", &first);
    CHECK(first.view().equals("n,,n=,r=abcdef"));
    CHECK(!other.handle_server_first("r=XXXXXX123,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096", &final_message));   // does not continue our nonce
    CHECK(!other.handle_server_first("r=abcdef123,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=0", &final_message));
    CHECK(!other.handle_server_first("r=abcdef123,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=99999999", &final_message));
    CHECK(!other.handle_server_first("r=abcdef123,s=!!!,i=4096", &final_message));
    CHECK(!other.handle_server_first("r=abcdef123,i=4096", &final_message));
    CHECK(!other.handle_server_first("r=abcdef,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096", &final_message));   // (the nonce must grow)
    CHECK(!other.verify_server_final("v=AAAA"));                                                          // (before a successful exchange nothing verifies)
    CHECK(!other.start("", "pw", "", &first) && !other.start("", "pw", "a,b", &first));
    CHECK(other.start("a=b,c", "pw", "n", &first) && first.view().equals("n,,n=a=3Db=2Cc,r=n"));

    String md5(a);
    const uint8_t salt[4] = {1, 2, 3, 4};
    CHECK(postgres::md5_password("postgres", "secret", ConstByteView(salt, 4), &md5) && md5.view().equals("md5bb41a296aab6baccb36ff243a562abff"));

    String b64(a);
    CHECK(postgres::append_base64(&b64, ConstByteView(reinterpret_cast<const uint8_t*>("Man"), 3)) && b64.view().equals("TWFu"));
    b64.clear();
    postgres::append_base64(&b64, ConstByteView(reinterpret_cast<const uint8_t*>("Ma"), 2));
    CHECK(b64.view().equals("TWE="));
    b64.clear();
    postgres::append_base64(&b64, ConstByteView(reinterpret_cast<const uint8_t*>("M"), 1));
    CHECK(b64.view().equals("TQ=="));
  }
  CHECK(a.live == 0);
}

static ConnectionOptions options(TestAllocator& a, const char* password = "secret") {
  ConnectionOptions o(a);
  o.driver.assign("postgres");
  o.host.assign("db.test");
  o.user.assign("app");
  o.password.assign(password);
  o.database.assign("shop");
  return o;
}

static DbStatus log_in(TestAllocator& a, FakeServer& server, UniquePointer<IConnection>* db, DbError* error, const char* password = "secret", PairStream** stream = nullptr,
                       uint64_t max_result_bytes = ConnectionOptions::kDefaultMaxResultBytes) {
  FixedRandom random;
  ConnectionOptions o = options(a, password);
  o.max_result_bytes = max_result_bytes;
  UniquePointer<PairStream> s = UniquePointer<PairStream>::make(a, &server);
  if (stream) *stream = s.get();
  return postgres::PostgresConnection::login(a, o, UniquePointer<stream::IStream>(util::move(s)), false, &random, db, error);
}

static void test_logins() {
  TestAllocator a;
  {
    const FakeServer::Auth methods[] = {FakeServer::Auth::Trust, FakeServer::Auth::Cleartext, FakeServer::Auth::Md5, FakeServer::Auth::Scram};
    for (FakeServer::Auth method : methods) {
      FakeServer server;
      server.auth = method;
      if (method == FakeServer::Auth::Md5) { server.user = "postgres"; }
      UniquePointer<IConnection> db;
      DbError error(a);
      // The MD5 vector is for the user "postgres": log in as that user.
      FixedRandom random;
      ConnectionOptions o = options(a);
      if (method == FakeServer::Auth::Md5) o.user.assign("postgres");
      UniquePointer<PairStream> s = UniquePointer<PairStream>::make(a, &server);
      const DbStatus status = postgres::PostgresConnection::login(a, o, UniquePointer<stream::IStream>(util::move(s)), false, &random, &db, &error);
      CHECK(status == DbStatus::Ok && db && db->is_open() && db->server_version().equals("16.2") && !db->in_transaction() && !db->encrypted() && db->driver_name()[0] == 'p');
      CHECK(server.startup_user == (method == FakeServer::Auth::Md5 ? "postgres" : "app") && server.startup_database == "shop" && server.startup_app == "TheoryCraftOS" && server.startup_float_digits == "3");
      db.reset();
      CHECK(server.terminated);   // destroying the connection says goodbye
    }

    // A wrong password.
    {
      FakeServer server;
      server.auth = FakeServer::Auth::Cleartext;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error, "wrong") == DbStatus::AuthFailed && !db && error.status == DbStatus::AuthFailed && error.sqlstate.view().equals("28P01") && error.message.view().first(8).equals("password"));
    }
    {
      FakeServer server;
      server.auth = FakeServer::Auth::Scram;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error, "wrong") == DbStatus::AuthFailed && !db);
    }
    // A server that cannot prove it knows the password is not trusted.
    {
      FakeServer server;
      server.auth = FakeServer::Auth::Scram;
      server.bad_server_proof = true;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error) == DbStatus::AuthFailed && !db && error.message.view().find(StringView("prove")) != StringView::npos);
    }
    // Methods this client does not have, and no password for one that needs it.
    {
      FakeServer server;
      server.auth = FakeServer::Auth::Kerberos;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error) == DbStatus::NotSupported && !db && error.message.view().find(StringView("method 2")) != StringView::npos);
    }
    {
      FakeServer server;
      server.auth = FakeServer::Auth::Cleartext;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error, "") == DbStatus::AuthFailed && !db);
    }
    // SCRAM without a source of random numbers.
    {
      FakeServer server;
      server.auth = FakeServer::Auth::Scram;
      UniquePointer<IConnection> db;
      DbError error(a);
      ConnectionOptions o = options(a);
      UniquePointer<PairStream> s = UniquePointer<PairStream>::make(a, &server);
      CHECK(postgres::PostgresConnection::login(a, o, UniquePointer<stream::IStream>(util::move(s)), false, nullptr, &db, &error) == DbStatus::NotSupported && !db);
    }
    // A server that does not answer.
    {
      FakeServer server;
      server.silent = true;
      UniquePointer<IConnection> db;
      DbError error(a);
      PairStream* stream = nullptr;
      CHECK(log_in(a, server, &db, &error, "secret", &stream) == DbStatus::Timeout && !db && error.status == DbStatus::Timeout && server.last_read_timeout == ConnectionOptions::kDefaultConnectTimeoutMs);
    }
  }
  CHECK(a.live == 0);
}

static void test_queries() {
  TestAllocator a;
  {
    FakeServer server;
    UniquePointer<IConnection> db;
    DbError error(a);
    PairStream* stream = nullptr;
    CHECK(log_in(a, server, &db, &error, "secret", &stream, 20000) == DbStatus::Ok);

    // A simple query, the answer typed by the server's type numbers.
    UniquePointer<IResultSet> rows;
    CHECK(db->execute("SELECT id, name, data, ok, score FROM items", &rows) == DbStatus::Ok && rows);
    CHECK(stream->read_timeout == ConnectionOptions::kDefaultQueryTimeoutMs && server.sql_log.back() == "SELECT id, name, data, ok, score FROM items");
    CHECK(rows->column_count() == 5 && rows->row_count() == 2 && rows->column_name(2).equals("data") && rows->column_type(0) == DbType::Integer && rows->column_type(1) == DbType::Text &&
          rows->column_type(2) == DbType::Bytes && rows->column_type(3) == DbType::Boolean && rows->column_type(4) == DbType::Real);
    int64_t id = 0;
    bool ok = false;
    double score = 0;
    ByteBuffer bytes(a);
    CHECK(rows->get_int(0, 0, &id) && id == 1 && rows->text(0, 1).equals("alpha") && rows->get_bool(0, 3, &ok) && ok && rows->get_real(0, 4, &score) && score == 1.5);
    CHECK(rows->get_bytes(0, 2, &bytes) && bytes.size() == 3 && bytes.view()[2] == 0xFF);
    CHECK(rows->is_null(1, 1) && rows->is_null(1, 2) && rows->get_real(1, 4, &score) && score == -0.25 && rows->get_bool(1, 3, &ok) && !ok);
    CHECK(rows->command().equals("SELECT 2") && rows->rows_affected() == 2 && rows->find_column("SCORE") == 4);

    // Statements without rows.
    CHECK(db->execute("INSERT INTO items VALUES (1)", &rows) == DbStatus::Ok && rows->column_count() == 0 && rows->row_count() == 0 && rows->rows_affected() == 1 && rows->command().equals("INSERT 0 1"));
    CHECK(db->execute("UPDATE items SET x = 1", &rows) == DbStatus::Ok && rows->rows_affected() == 5);
    CHECK(db->execute("CREATE TABLE t (a int)", &rows) == DbStatus::Ok && rows->rows_affected() == 0 && rows->command().equals("CREATE TABLE"));
    CHECK(db->execute("SELECT 1") == DbStatus::Ok);                                            // (the answer is not wanted)
    CHECK(db->execute("", &rows) == DbStatus::Ok && rows && rows->column_count() == 0);       // an empty statement
    CHECK(db->execute("SELECT 1; SELECT 1", &rows) == DbStatus::Ok && rows->row_count() == 1 && rows->text(0, 0).equals("1"));   // several: the last one's answer
    CHECK(db->ping() == DbStatus::Ok);

    // Parameters: sent as text through the extended protocol, whatever the value holds.
    const uint8_t blob[3] = {0, 0xAB, 0xFF};
    const DbParam params[] = {DbParam::null(), DbParam::from_bool(true), DbParam::from_int(-42), DbParam::from_real(0.5), DbParam::from_text("it's \"quoted\"; DROP TABLE x; --"),
                              DbParam::from_bytes(ConstByteView(blob, 3)), DbParam::from_bool(false)};
    CHECK(db->execute("SELECT $1 -- ?\n, ?, ?, ?, ?, ?, ?, ?", ArrayView<const DbParam>(), nullptr) == DbStatus::InvalidArgument);   // (markers but no values)
    server.sql_log.clear();
    CHECK(db->execute("SELECT ?, ?, ?, ?, ?, ?, ?", ArrayView<const DbParam>(params, 7), &rows) == DbStatus::Ok);
    CHECK(server.sql_log.size() == 1 && server.sql_log[0] == "SELECT $1, $2, $3, $4, $5, $6, $7");
    CHECK(rows->column_count() == 7 && rows->row_count() == 1 && rows->is_null(0, 0) && rows->text(0, 1).equals("t") && rows->text(0, 2).equals("-42") && rows->text(0, 3).equals("0.5"));
    CHECK(rows->text(0, 4).equals("it's \"quoted\"; DROP TABLE x; --") && rows->text(0, 5).equals("\\x00abff") && rows->text(0, 6).equals("f"));
    CHECK(server.last_params.size() == 7 && server.last_params[0].null);
    // Wrong counts, and text that cannot be sent.
    CHECK(db->execute("SELECT ?, ?", ArrayView<const DbParam>(params, 1), nullptr) == DbStatus::InvalidArgument && db->last_error().status == DbStatus::InvalidArgument &&
          db->last_error().message.view().find(StringView("2 parameter markers")) != StringView::npos);
    CHECK(db->execute("SELECT 1", ArrayView<const DbParam>(params, 1), nullptr) == DbStatus::InvalidArgument);
    const char zero_text[] = {'a', 0, 'b'};
    const DbParam bad[] = {DbParam::from_text(StringView(zero_text, 3))};
    CHECK(db->execute("SELECT ?", ArrayView<const DbParam>(bad, 1), nullptr) == DbStatus::InvalidArgument);
    CHECK(db->execute(StringView(zero_text, 3), nullptr) == DbStatus::InvalidArgument);
    CHECK(db->is_open());   // (all of these were the caller's mistakes: nothing went to the server)
    // ?? stays a question mark.
    server.sql_log.clear();
    CHECK(db->execute("SELECT data ?? 'k', ?", ArrayView<const DbParam>(params + 2, 1), nullptr) == DbStatus::Server);   // (the pretend server does not know that SQL)
    CHECK(server.sql_log.size() == 1 && server.sql_log[0] == "SELECT data ? 'k', $1");

    // Errors from the server keep the connection.
    CHECK(db->execute("SELECT nope", &rows) == DbStatus::Server && !rows && db->is_open());
    CHECK(db->last_error().status == DbStatus::Server && db->last_error().sqlstate.view().equals("42703") && db->last_error().message.view().equals("column \"nope\" does not exist") &&
          db->last_error().detail.view().equals("in the list of columns"));
    const DbParam one[] = {DbParam::from_int(1)};
    CHECK(db->execute("SELECT nope", ArrayView<const DbParam>(), &rows) == DbStatus::Server);
    CHECK(db->execute("garbage ?", ArrayView<const DbParam>(one, 1), &rows) == DbStatus::Server && db->last_error().sqlstate.view().equals("42601") && db->is_open());   // the extended form too
    CHECK(db->execute("SELECT 1", &rows) == DbStatus::Ok && db->last_error().status == DbStatus::Ok && rows->text(0, 0).equals("1"));

    // Transactions follow the server's own report.
    CHECK(!db->in_transaction() && db->begin() == DbStatus::Ok && db->in_transaction());
    CHECK(db->execute("SELECT nope") == DbStatus::Server && db->in_transaction());         // failed, and the server now refuses everything
    CHECK(db->execute("SELECT 1") == DbStatus::Server && db->last_error().sqlstate.view().equals("25P02"));
    CHECK(db->rollback() == DbStatus::Ok && !db->in_transaction() && db->execute("SELECT 1") == DbStatus::Ok);
    CHECK(db->begin() == DbStatus::Ok && db->execute("INSERT INTO items VALUES (2)") == DbStatus::Ok && db->commit() == DbStatus::Ok && !db->in_transaction());

    // Answers over the limit are refused, and the connection goes on.
    CHECK(db->execute("SELECT many", &rows) == DbStatus::TooLarge && !rows && db->is_open() && db->last_error().status == DbStatus::TooLarge);
    CHECK(db->execute("SELECT 1", &rows) == DbStatus::Ok);

    // Closing.
    db->close();
    CHECK(!db->is_open() && server.terminated && stream->is_shut());
    CHECK(db->execute("SELECT 1", &rows) == DbStatus::Closed && !rows && db->last_error().status == DbStatus::Closed);
    db->close();   // (twice is fine)
  }
  CHECK(a.live == 0);
}

static void test_broken_connections() {
  TestAllocator a;
  {
    // The server hangs up in the middle of a statement.
    {
      FakeServer server;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error) == DbStatus::Ok);
      server.drop = true;
      UniquePointer<IResultSet> rows;
      CHECK(db->execute("SELECT 1", &rows) == DbStatus::ConnectionLost && !db->is_open() && db->last_error().status == DbStatus::ConnectionLost);
      CHECK(db->execute("SELECT 1") == DbStatus::Closed);
    }
    // It stops answering: the statement times out, and since the server may still be at it the connection is given up.
    {
      FakeServer server;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error) == DbStatus::Ok);
      server.silent = true;
      CHECK(db->execute("SELECT 1") == DbStatus::Timeout && !db->is_open() && db->last_error().status == DbStatus::Timeout);
    }
    // It says something no PostgreSQL says.
    {
      FakeServer server;
      UniquePointer<IConnection> db;
      DbError error(a);
      CHECK(log_in(a, server, &db, &error) == DbStatus::Ok);
      server.garbage = true;
      CHECK(db->execute("SELECT 1") == DbStatus::Protocol && !db->is_open());
    }
    // A limit set in the options applies.
    {
      FakeServer server;
      UniquePointer<IConnection> db;
      DbError error(a);
      FixedRandom random;
      ConnectionOptions o = options(a);
      o.max_result_bytes = 2000;
      o.query_timeout_ms = 1234;
      UniquePointer<PairStream> s = UniquePointer<PairStream>::make(a, &server);
      PairStream* stream = s.get();
      CHECK(postgres::PostgresConnection::login(a, o, UniquePointer<stream::IStream>(util::move(s)), false, &random, &db, &error) == DbStatus::Ok);
      UniquePointer<IResultSet> rows;
      CHECK(db->execute("SELECT 1", &rows) == DbStatus::Ok && stream->read_timeout == 1234);
      CHECK(db->execute("SELECT many", &rows) == DbStatus::TooLarge);
    }
  }
  CHECK(a.live == 0);
}

static void test_manager() {
  TestAllocator a;
  {
    FakeServer server;
    FakeTransport transport(a);
    transport.server = &server;
    FixedRandom random;
    postgres::PostgresDriver driver(a, &random);
    CHECK(driver.handles_scheme("postgres") && driver.handles_scheme("postgresql") && !driver.handles_scheme("mysql") && driver.default_port() == 5432 && StringView(driver.name()).equals("postgres"));
    DatabaseManager databases(a, &transport);
    CHECK(databases.add_driver(&driver) && !databases.add_driver(&driver) && databases.driver_count() == 1 && databases.driver(0) == &driver && databases.find_driver("postgresql") == &driver);
    CHECK(!databases.add_driver(nullptr) && databases.find_driver("oracle") == nullptr);

    UniquePointer<IConnection> db;
    DbError error(a);
    server.auth = FakeServer::Auth::Cleartext;
    CHECK(databases.connect("postgres://app:secret@db.test:6000/shop?sslmode=require", &db, &error) == DbStatus::Ok && db && db->is_open());
    CHECK(transport.host == "db.test" && transport.port == 6000 && transport.tls == TlsMode::Require && transport.probe_accept == 'S');
    CHECK(transport.probe_request == Bytes("\0\0\0\x08\x04\xD2\x16\x2F", 8));   // the SSLRequest
    CHECK(server.startup_database == "shop");
    UniquePointer<IResultSet> rows;
    CHECK(db->execute("SELECT 1", &rows) == DbStatus::Ok);
    db.reset();

    // The usual port when the URL has none, TLS reported by the transport.
    transport.tls_available = true;
    server = FakeServer();
    CHECK(databases.connect("postgresql://app@db.test/shop", &db, &error) == DbStatus::Ok && transport.port == 5432 && db->encrypted() && transport.tls == TlsMode::Prefer);
    db.reset();

    // Things that go wrong before a connection exists.
    CHECK(databases.connect("mysql://u@h/db", &db, &error) == DbStatus::UnknownDriver && error.status == DbStatus::UnknownDriver && error.detail.view().equals("mysql"));
    CHECK(databases.connect("not a url", &db, &error) == DbStatus::InvalidArgument && error.status == DbStatus::InvalidArgument);
    CHECK(databases.connect("postgres://h/db", &db, &error) == DbStatus::InvalidArgument && error.message.view().find(StringView("user")) != StringView::npos);   // PostgreSQL needs a user
    transport.fail = DbStatus::ConnectFailed;
    CHECK(databases.connect("postgres://app@h/db", &db, &error) == DbStatus::ConnectFailed && !db && error.status == DbStatus::ConnectFailed);
    transport.fail = DbStatus::Ok;
    CHECK(databases.connect("postgres://app@h/db", &db) == DbStatus::Ok);   // (no error object needed)
    db.reset();
    databases.set_transport(nullptr);
    CHECK(databases.connect("postgres://app@h/db", &db, &error) == DbStatus::ConnectFailed && error.message.view().find(StringView("network")) != StringView::npos);
  }
  CHECK(a.live == 0);
}

int main() {
  test_numbers();
  test_url();
  test_sql_text();
  test_result_set();
  test_scram_and_md5();
  test_logins();
  test_queries();
  test_broken_connections();
  test_manager();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("database tests passed\n");
  return 0;
}
