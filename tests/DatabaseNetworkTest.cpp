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

// Host-side test of database access over the OS's own network stack: a DatabaseManager with the PostgreSQL driver and the NetworkTransport connects
// to a pretend PostgreSQL server (tests/FakePostgres.hpp) running on a second stack, the two joined by the fake wire of the network harness. So
// the whole path is real: name lookup, TCP, the stream, the driver, the manager; also an answer of a hundred kilobytes in many segments.
#include <stdio.h>
#include "Database/DatabaseManager.hpp"
#include "Database/Net/NetworkTransport.hpp"
#include "Database/Postgres/PostgresDriver.hpp"
#include "FakePostgres.hpp"
#include "NetworkHarness.hpp"

using namespace database;

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct FixedRandom final : crypto::IRandom {
  bool fill(ByteView out) override {
    for (uint64_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(0x41 + i);
    return true;
  }
};

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  {
    Lan lan;
    lan.configure_static();
    FakeServer fake;
    fake.auth = FakeServer::Auth::Scram;
    fake.user = "app";
    fake.password = "secret";
    Server server(lan, 5432);
    size_t fed = 0, sent = 0;
    server.app = [&](Server& s) {
      if (!s.conn) return;
      if (s.received.size() > fed) {
        fake.feed(reinterpret_cast<const uint8_t*>(s.received.data()) + fed, s.received.size() - fed);
        fed = s.received.size();
      }
      while (sent < fake.out.size()) {
        const uint32_t k = s.conn->send(ConstByteView(reinterpret_cast<const uint8_t*>(fake.out.data()) + sent, fake.out.size() - sent));
        if (!k) break;
        sent += k;
      }
    };

    // A new server for the next connection (the harness serves one connection at a time).
    auto renew = [&](FakeServer::Auth auth) {
      fake = FakeServer();
      fake.auth = auth;
      fed = sent = 0;
      server.received.clear();
      server.conn = SharedPointer<ITcpSocket>();
    };
    FixedRandom random;
    TestAllocator& a = lan.alloc;
    network::SingleStackNetwork network(lan.a);
    NetworkTransport transport(network, a);
    postgres::PostgresDriver driver(a, &random);
    DatabaseManager databases(a, &transport);
    databases.add_driver(&driver);

    UniquePointer<IConnection> db;
    DbError error(a);
    // No TLS configuration: a connection that must be encrypted cannot be made, and the server is not even asked.
    CHECK(databases.connect("postgres://app:secret@10.0.0.2/shop?sslmode=require", &db, &error) == DbStatus::TlsFailed && !db && fake.ssl_requests == 0);

    // A plain connection over TCP, logging in with SCRAM-SHA-256.
    renew(FakeServer::Auth::Scram);
    const DbStatus connected = databases.connect("postgres://app:secret@10.0.0.2/shop?sslmode=disable", &db, &error);
    if (connected != DbStatus::Ok) printf("   connect failed: %s: %s\n", db_status_name(connected), error.message.c_str());
    CHECK(connected == DbStatus::Ok && db && db->is_open() && !db->encrypted());
    CHECK(db && db->server_version().equals("16.2") && fake.startup_database == "shop" && fake.ssl_requests == 0);
    UniquePointer<IResultSet> rows;
    CHECK(db->execute("SELECT id, name, data, ok, score FROM items", &rows) == DbStatus::Ok && rows->row_count() == 2 && rows->text(0, 1).equals("alpha"));
    const DbParam params[] = {DbParam::from_text("hello"), DbParam::from_int(7)};
    CHECK(db->execute("SELECT ?, ?", ArrayView<const DbParam>(params, 2), &rows) == DbStatus::Ok && rows->text(0, 0).equals("hello") && rows->text(0, 1).equals("7"));
    // An answer of about 110 kilobytes arrives in many TCP segments.
    CHECK(db->execute("SELECT many", &rows) == DbStatus::Ok && rows->row_count() == 1000 && rows->text(999, 0).size() == 100 && rows->text(500, 0)[99] == 'x');
    CHECK(db->execute("SELECT nope", &rows) == DbStatus::Server && db->last_error().sqlstate.view().equals("42703"));
    CHECK(db->ping() == DbStatus::Ok);
    db.reset();
    CHECK(lan.run_until([&] { return fake.terminated; }, 2000));   // the goodbye reached the server

    // A wrong password.
    renew(FakeServer::Auth::Cleartext);
    CHECK(databases.connect("postgres://app:wrong@10.0.0.2/shop?sslmode=disable", &db, &error) == DbStatus::AuthFailed && !db && error.sqlstate.view().equals("28P01"));

    // With a TLS configuration the server is asked first (it says no): fine when TLS is preferred, a failure when it is required.
    network::TlsConfig tls;
    transport.set_tls(&tls);
    renew(FakeServer::Auth::Trust);
    CHECK(databases.connect("postgres://app:secret@10.0.0.2/shop?sslmode=prefer", &db, &error) == DbStatus::Ok && fake.ssl_requests == 1 && !db->encrypted());
    CHECK(db->execute("SELECT 1") == DbStatus::Ok);
    db.reset();
    renew(FakeServer::Auth::Trust);
    CHECK(databases.connect("postgres://app:secret@10.0.0.2/shop?sslmode=require", &db, &error) == DbStatus::TlsFailed && !db && fake.ssl_requests == 1);

    renew(FakeServer::Auth::Trust);
    // Nobody listens there: the connection is refused (or never answered).
    CHECK(databases.connect("postgres://app:secret@10.0.0.2:5999/shop?sslmode=disable&connect_timeout=2", &db, &error) != DbStatus::Ok && !db);
    // A name nobody can look up (the transport reports it like any server that cannot be reached).
    CHECK(databases.connect("postgres://app:secret@no-such-host.invalid/shop?connect_timeout=1", &db, &error) == DbStatus::ConnectFailed);
    lan.step(50);
  }
  CHECK(g_live == 0);
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("database network tests passed\n");
  return 0;
}
