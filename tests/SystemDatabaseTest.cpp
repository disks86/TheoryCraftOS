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

// Host-side test of the system database: the SQL engine (tables, expressions, errors with their SQLSTATEs), the file it lives in (written whole,
// replaced safely, recovered), transactions between connections, the driver behind the DatabaseManager, and the settings kept in it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "Database/DatabaseManager.hpp"
#include "Database/Local/LocalDatabase.hpp"
#include "Database/Local/SystemSettings.hpp"
#include "FileSystem/Ram/RamFileSystem.hpp"

using namespace database;
using namespace database::local;

struct TestAllocator final : allocators::IAllocator {
  int live = 0;
  void* allocate(uint64_t size) override { ++live; return malloc(size); }
  void* allocate_aligned(uint64_t size, uint64_t) override { return allocate(size); }
  void deallocate(void* p) override { if (p) { --live; free(p); } }
};

static int g_failed = 0;
#define CHECK(...) do { if (!(__VA_ARGS__)) { printf("FAILED line %d: %s\n", __LINE__, #__VA_ARGS__); ++g_failed; } } while (0)

struct Db {
  TestAllocator& a;
  LocalConnection conn;
  UniquePointer<IResultSet> rows;
  explicit Db(TestAllocator& alloc, LocalDatabase& database) : a(alloc), conn(alloc, database) {}
  DbStatus run(StringView sql) { return conn.execute(sql, &rows); }
  // The first cell of the answer, or "<error>".
  bool is(StringView sql, StringView expected) {
    if (run(sql) != DbStatus::Ok || !rows || rows->row_count() == 0) return false;
    return rows->text(0, 0).equals(expected);
  }
  bool sqlstate(const char* expected) { return conn.last_error().sqlstate.view().equals(expected); }
};

static void test_sql() {
  TestAllocator a;
  {
    LocalDatabase database(a, nullptr, "/none.db");
    CHECK(database.open() == DbStatus::Ok && !database.persistent());
    Db db(a, database);
    CHECK(db.run("CREATE TABLE people (id INTEGER PRIMARY KEY, name TEXT NOT NULL, age INTEGER, score REAL, admin BOOLEAN DEFAULT FALSE, note TEXT DEFAULT 'none')") == DbStatus::Ok);
    CHECK(db.rows->command().equals("CREATE TABLE") && db.rows->column_count() == 0);
    CHECK(db.run("CREATE TABLE people (id INTEGER)") == DbStatus::Server && db.sqlstate("42P07"));
    CHECK(db.run("CREATE TABLE IF NOT EXISTS people (id INTEGER)") == DbStatus::Ok);
    CHECK(db.run("INSERT INTO people (id, name, age, score) VALUES (1, 'Ann', 31, 4.5), (2, 'Bob', 25, NULL), (3, 'Cy', 40, 2.25)") == DbStatus::Ok);
    CHECK(db.rows->command().equals("INSERT 0 3") && db.rows->rows_affected() == 3);

    // Reading
    CHECK(db.run("SELECT * FROM people ORDER BY id") == DbStatus::Ok && db.rows->row_count() == 3 && db.rows->column_count() == 6);
    CHECK(db.rows->column_name(1).equals("name") && db.rows->column_type(0) == DbType::Integer && db.rows->column_type(3) == DbType::Real && db.rows->column_type(4) == DbType::Boolean);
    CHECK(db.rows->text(0, 1).equals("Ann") && db.rows->is_null(1, 3) && db.rows->text(0, 3).equals("4.5") && db.rows->text(2, 4).equals("false") && db.rows->text(0, 5).equals("none"));
    int64_t i = 0;
    bool b = true;
    CHECK(db.rows->get_int(2, 0, &i) && i == 3 && db.rows->get_bool(0, 4, &b) && !b);
    CHECK(db.is("SELECT name FROM people WHERE age > 30 AND score IS NOT NULL ORDER BY age DESC", "Cy"));
    CHECK(db.run("SELECT name FROM people WHERE age > 30 ORDER BY age DESC") == DbStatus::Ok && db.rows->row_count() == 2 && db.rows->text(1, 0).equals("Ann"));
    CHECK(db.run("SELECT name AS who, age + 1 AS next FROM people ORDER BY next LIMIT 2 OFFSET 1") == DbStatus::Ok && db.rows->row_count() == 2 && db.rows->text(0, 0).equals("Ann") &&
          db.rows->text(0, 1).equals("32") && db.rows->column_name(1).equals("next") && db.rows->text(1, 0).equals("Cy"));
    CHECK(db.run("SELECT name FROM people ORDER BY 1 DESC") == DbStatus::Ok && db.rows->text(0, 0).equals("Cy"));
    CHECK(db.is("SELECT name FROM people WHERE name LIKE 'b%'", "Bob") && db.is("SELECT name FROM people WHERE name NOT LIKE '%o%' AND name LIKE '_nn'", "Ann"));
    CHECK(db.is("SELECT COUNT(*) FROM people", "3") && db.is("SELECT COUNT(score) FROM people", "2") && db.is("SELECT SUM(age) FROM people", "96") && db.is("SELECT MIN(name) FROM people", "Ann") &&
          db.is("SELECT MAX(age) FROM people", "40") && db.is("SELECT AVG(age) FROM people WHERE age < 35", "28"));
    CHECK(db.run("SELECT COUNT(*), MAX(name) FROM people WHERE id > 9") == DbStatus::Ok && db.rows->row_count() == 1 && db.rows->text(0, 0).equals("0") && db.rows->is_null(0, 1));
    CHECK(db.is("SELECT 1 + 2 * 3", "7") && db.is("SELECT (1 + 2) * 3", "9") && db.is("SELECT 7 / 2", "3") && db.is("SELECT 7 % 4", "3") && db.is("SELECT 7 / 2.0", "3.5") && db.is("SELECT -5 + 2", "-3"));
    CHECK(db.is("SELECT 'a' || 'b' || 1", "ab1") && db.is("SELECT UPPER('abc')", "ABC") && db.is("SELECT LOWER('ABC')", "abc") && db.is("SELECT LENGTH('héllo')", "5") && db.is("SELECT ABS(-4)", "4") &&
          db.is("SELECT COALESCE(NULL, NULL, 'x')", "x") && db.is("SELECT IFNULL(NULL, 3)", "3") && db.is("SELECT 'it''s'", "it's"));
    CHECK(db.is("SELECT 1 < 2", "true") && db.rows->column_type(0) == DbType::Boolean && db.is("SELECT NULL = NULL", "") && db.rows->is_null(0, 0) && db.is("SELECT NULL IS NULL", "true"));
    CHECK(db.is("SELECT TRUE AND NULL", "") && db.is("SELECT FALSE AND NULL", "false") && db.is("SELECT TRUE OR NULL", "true") && db.is("SELECT NOT FALSE", "true"));
    CHECK(db.run("SELECT 1 /* c */ ; -- x\n SELECT 2") == DbStatus::Ok && db.rows->text(0, 0).equals("2"));

    // Parameters
    const DbParam params[] = {DbParam::from_text("Bob"), DbParam::from_int(25)};
    CHECK(db.conn.execute("SELECT id FROM people WHERE name = ? AND age = ?", ArrayView<const DbParam>(params, 2), &db.rows) == DbStatus::Ok && db.rows->text(0, 0).equals("2"));
    CHECK(db.conn.execute("SELECT id FROM people WHERE name = ?", &db.rows) == DbStatus::InvalidArgument);
    const DbParam odd[] = {DbParam::null(), DbParam::from_bool(true), DbParam::from_real(1.5), DbParam::from_text("x'y")};
    CHECK(db.conn.execute("SELECT ?, ?, ?, ?", ArrayView<const DbParam>(odd, 4), &db.rows) == DbStatus::Ok && db.rows->is_null(0, 0) && db.rows->text(0, 1).equals("1") && db.rows->text(0, 2).equals("1.5") &&
          db.rows->text(0, 3).equals("x'y"));

    // Changing
    CHECK(db.run("UPDATE people SET age = age + 1, note = 'old' WHERE age > 30") == DbStatus::Ok && db.rows->command().equals("UPDATE 2") && db.is("SELECT age FROM people WHERE id = 3", "41") &&
          db.is("SELECT note FROM people WHERE id = 1", "old"));
    CHECK(db.run("UPDATE people SET id = 2 WHERE id = 3") == DbStatus::Server && db.sqlstate("23505") && db.is("SELECT id FROM people WHERE name = 'Cy'", "3"));
    CHECK(db.run("DELETE FROM people WHERE score IS NULL") == DbStatus::Ok && db.rows->command().equals("DELETE 1") && db.is("SELECT COUNT(*) FROM people", "2"));
    CHECK(db.run("INSERT INTO people (id, name) VALUES (1, 'Dup')") == DbStatus::Server && db.sqlstate("23505") && db.conn.last_error().detail.view().contains("Key (id)=(1)"));
    CHECK(db.run("INSERT INTO people (id, name) VALUES (9, 'Nine'), (1, 'Dup')") == DbStatus::Server && db.is("SELECT COUNT(*) FROM people", "2"));   // all or nothing
    CHECK(db.run("INSERT OR IGNORE INTO people (id, name) VALUES (1, 'Dup')") == DbStatus::Ok && db.rows->command().equals("INSERT 0 0") && db.is("SELECT name FROM people WHERE id = 1", "Ann"));
    CHECK(db.run("INSERT OR REPLACE INTO people (id, name, age) VALUES (1, 'Anna', 5)") == DbStatus::Ok && db.is("SELECT name FROM people WHERE id = 1", "Anna") && db.is("SELECT COUNT(*) FROM people", "2"));
    CHECK(db.run("INSERT INTO people (id) VALUES (5)") == DbStatus::Server && db.sqlstate("23502"));
    CHECK(db.run("INSERT INTO people (id, name, age) VALUES (6, 'Six', 'seven')") == DbStatus::Server && db.sqlstate("22P02"));
    CHECK(db.run("INSERT INTO people (id, name, age, admin) VALUES (7, 'Sev', '8', 'yes')") == DbStatus::Ok && db.is("SELECT age FROM people WHERE id = 7", "8") && db.is("SELECT admin FROM people WHERE id = 7", "true"));
    CHECK(db.run("INSERT INTO people (id, name) VALUES (8)") == DbStatus::Server && db.sqlstate("42601"));

    // Errors
    CHECK(db.run("SELECT * FROM nothing") == DbStatus::Server && db.sqlstate("42P01") && db.conn.last_error().message.view().contains("nothing"));
    CHECK(db.run("SELECT bogus FROM people") == DbStatus::Server && db.sqlstate("42703"));
    CHECK(db.run("SELEKT 1") == DbStatus::Server && db.sqlstate("42601"));
    CHECK(db.run("SELECT 1 +") == DbStatus::Server && db.sqlstate("42601"));
    CHECK(db.run("SELECT 'open") == DbStatus::Server && db.sqlstate("42601"));
    CHECK(db.run("SELECT 1 / 0") == DbStatus::Server && db.sqlstate("22012"));
    CHECK(db.run("SELECT nosuch(1)") == DbStatus::Server && db.sqlstate("42883"));
    CHECK(db.run("SELECT name FROM people WHERE COUNT(*) > 1") == DbStatus::Server && db.sqlstate("42803"));
    CHECK(db.run("SELECT DISTINCT name FROM people") == DbStatus::Server && db.sqlstate("0A000"));
    CHECK(db.run("SELECT * FROM people LIMIT -1") == DbStatus::Server);
    CHECK(db.run("DROP TABLE nothing") == DbStatus::Server && db.run("DROP TABLE IF EXISTS nothing") == DbStatus::Ok);
    CHECK(db.run("") == DbStatus::Ok && db.rows && db.rows->row_count() == 0 && db.run(" ; ; ") == DbStatus::Ok);
    String deep(a);
    deep.append("SELECT ");
    for (int k = 0; k < 200; ++k) deep.append("(");
    deep.append("1");
    for (int k = 0; k < 200; ++k) deep.append(")");
    CHECK(db.conn.execute(deep.view(), &db.rows) == DbStatus::Server && db.sqlstate("54001"));

    // Several statements, and the catalog
    CHECK(db.run("CREATE TABLE t2 (a, b); INSERT INTO t2 VALUES (1, 'x'), (2.5, NULL); SELECT a, b FROM t2 ORDER BY a DESC") == DbStatus::Ok && db.rows->row_count() == 2 &&
          db.rows->text(0, 0).equals("2.5") && db.rows->text(1, 1).equals("x"));
    CHECK(db.run("SHOW TABLES") == DbStatus::Ok && db.rows->row_count() == 2 && db.rows->text(0, 0).equals("people") && db.rows->text(0, 1).equals("6") && db.rows->text(1, 0).equals("t2"));
    CHECK(db.run("DESCRIBE people") == DbStatus::Ok && db.rows->row_count() == 6 && db.rows->text(0, 3).equals("true") && db.rows->text(1, 2).equals("true") && db.rows->text(5, 4).equals("none") &&
          db.rows->text(4, 1).equals("boolean"));
    CHECK(db.run("DROP TABLE t2") == DbStatus::Ok && db.run("SELECT * FROM t2") == DbStatus::Server);
    CHECK(db.run("CREATE TABLE kv (a TEXT, b TEXT, v INTEGER, PRIMARY KEY (a, b))") == DbStatus::Ok && db.run("INSERT INTO kv VALUES ('x', 'y', 1), ('x', 'z', 2)") == DbStatus::Ok &&
          db.run("INSERT INTO kv VALUES ('x', 'y', 3)") == DbStatus::Server && db.sqlstate("23505") && db.run("INSERT INTO kv VALUES ('y', 'x', 3)") == DbStatus::Ok);
    ByteBuffer bytes(a);
    const uint8_t raw[] = {0xDE, 0xAD, 0x00, 0xBE};
    CHECK(db.run("CREATE TABLE blobs (id INTEGER, data BLOB)") == DbStatus::Ok);
    const DbParam blob[] = {DbParam::from_bytes(ConstByteView(raw, 4))};
    CHECK(db.conn.execute("INSERT INTO blobs VALUES (1, ?)", ArrayView<const DbParam>(blob, 1), nullptr) == DbStatus::Ok && db.run("SELECT data FROM blobs") == DbStatus::Ok && db.rows->column_type(0) == DbType::Bytes &&
          db.rows->text(0, 0).equals("\\xdead00be") && db.rows->get_bytes(0, 0, &bytes) && bytes.size() == 4 && bytes.view()[3] == 0xBE);
  }
  CHECK(a.live == 0);
}

static void test_file_and_transactions() {
  TestAllocator a;
  {
    filesystem::RamFileSystem files(a);
    {
      LocalDatabase database(a, &files, "/AppStore/System/system.db");
      CHECK(database.open() == DbStatus::Ok && database.persistent());
      filesystem::DirectoryEntry entry;
      CHECK(files.stat("/AppStore/System/system.db", &entry) == filesystem::Status::Ok && entry.size > 8 && files.stat("/AppStore/System/system.db.tmp", &entry) == filesystem::Status::NotFound);
      Db db(a, database);
      CHECK(db.run("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)") == DbStatus::Ok && db.run("INSERT INTO t VALUES (1, 'one'), (2, 'two')") == DbStatus::Ok);
      CHECK(db.run("INSERT INTO t VALUES (2, 'again')") == DbStatus::Server);

      // A transaction holds its changes back from the file, and rollback drops them.
      Db other(a, database);
      CHECK(db.conn.begin() == DbStatus::Ok && db.conn.in_transaction() && db.conn.begin() == DbStatus::Server && db.sqlstate("25001"));
      CHECK(db.run("INSERT INTO t VALUES (3, 'three')") == DbStatus::Ok && other.is("SELECT COUNT(*) FROM t", "3"));
      CHECK(other.run("INSERT INTO t VALUES (4, 'four')") == DbStatus::Server && other.sqlstate("55P03") && other.conn.begin() == DbStatus::Server);
      CHECK(db.conn.rollback() == DbStatus::Ok && !db.conn.in_transaction() && db.is("SELECT COUNT(*) FROM t", "2"));
      CHECK(other.run("INSERT INTO t VALUES (4, 'four')") == DbStatus::Ok);
      CHECK(db.run("BEGIN") == DbStatus::Ok && db.run("DELETE FROM t WHERE id = 4") == DbStatus::Ok && db.run("COMMIT") == DbStatus::Ok && !db.conn.in_transaction() && db.is("SELECT COUNT(*) FROM t", "2"));
      // A connection that goes away with a transaction open rolls it back.
      {
        Db third(a, database);
        CHECK(third.conn.begin() == DbStatus::Ok && third.run("DELETE FROM t") == DbStatus::Ok && third.is("SELECT COUNT(*) FROM t", "0"));
      }
      CHECK(db.is("SELECT COUNT(*) FROM t", "2"));
      // A statement that fails inside a transaction undoes itself only.
      CHECK(db.run("BEGIN") == DbStatus::Ok && db.run("INSERT INTO t VALUES (7, 'seven')") == DbStatus::Ok && db.run("INSERT INTO t VALUES (8, 'eight'), (7, 'dup')") == DbStatus::Server &&
            db.is("SELECT COUNT(*) FROM t", "3") && db.run("COMMIT") == DbStatus::Ok);
      db.conn.close();
      CHECK(!db.conn.is_open() && db.run("SELECT 1") == DbStatus::Closed);
    }
    // A new start reads the file back.
    {
      LocalDatabase database(a, &files, "/AppStore/System/system.db");
      CHECK(database.open() == DbStatus::Ok && database.table_count() == 1);
      Db db(a, database);
      CHECK(db.run("SELECT id, v FROM t ORDER BY id") == DbStatus::Ok && db.rows->row_count() == 3 && db.rows->text(2, 1).equals("seven") && db.rows->text(0, 1).equals("one"));
    }
    // The file replaced halfway (only the .tmp is there): it is used.
    CHECK(files.rename("/AppStore/System/system.db", "/AppStore/System/system.db.tmp") == filesystem::Status::Ok);
    {
      LocalDatabase database(a, &files, "/AppStore/System/system.db");
      CHECK(database.open() == DbStatus::Ok && database.table_count() == 1);
      filesystem::DirectoryEntry entry;
      CHECK(files.stat("/AppStore/System/system.db", &entry) == filesystem::Status::Ok && files.stat("/AppStore/System/system.db.tmp", &entry) == filesystem::Status::NotFound);
    }
    // A damaged file is set aside and a new database starts.
    {
      UniquePointer<filesystem::IFile> file;
      CHECK(files.open_file("/AppStore/System/system.db", filesystem::OpenMode::ReadWrite, &file) == filesystem::Status::Ok);
      const uint8_t junk[] = {1, 2, 3, 4};
      uint64_t wrote = 0;
      CHECK(file->write(10, ConstByteView(junk, 4), &wrote) == filesystem::Status::Ok);
    }
    {
      LocalDatabase database(a, &files, "/AppStore/System/system.db");
      CHECK(database.open() == DbStatus::Ok && database.table_count() == 0 && database.persistent() && database.problem().view().contains("damaged"));
      filesystem::DirectoryEntry entry;
      CHECK(files.stat("/AppStore/System/system.db.corrupt", &entry) == filesystem::Status::Ok);
    }
    // A short or empty file is damaged too.
    {
      UniquePointer<filesystem::IFile> file;
      CHECK(files.open_file("/AppStore/System/system.db", filesystem::OpenMode::CreateTruncate, &file) == filesystem::Status::Ok);
    }
    {
      LocalDatabase database(a, &files, "/AppStore/System/system.db");
      CHECK(database.open() == DbStatus::Ok && database.table_count() == 0);
    }
  }
  CHECK(a.live == 0);
}

static void test_driver_and_settings() {
  TestAllocator a;
  {
    filesystem::RamFileSystem files(a);
    LocalDatabase database(a, &files, "/AppStore/System/system.db");
    CHECK(database.open() == DbStatus::Ok);
    LocalDriver driver(a);
    CHECK(driver.add_database("system", &database) && !driver.add_database("SYSTEM", &database));
    DatabaseManager manager(a);   // no network at all
    CHECK(manager.add_driver(&driver) && manager.find_driver("system") == &driver);
    UniquePointer<IConnection> connection;
    DbError error(a);
    CHECK(manager.connect("system://localhost/system", &connection, &error) == DbStatus::Ok && connection && connection->is_open() && !connection->encrypted() &&
          connection->driver_name()[0] == 's');
    UniquePointer<IResultSet> rows;
    CHECK(connection->execute("CREATE TABLE notes (id INTEGER, body TEXT)") == DbStatus::Ok && connection->execute("INSERT INTO notes VALUES (1, 'hello')") == DbStatus::Ok &&
          connection->execute("SELECT body FROM notes", &rows) == DbStatus::Ok && rows->text(0, 0).equals("hello"));
    CHECK(connection->execute("SELECT nope FROM notes") == DbStatus::Server && connection->last_error().sqlstate.view().equals("42703") && connection->ping() == DbStatus::Ok);
    UniquePointer<IConnection> second;
    CHECK(manager.connect("system://anything:1/", &second, &error) == DbStatus::Ok);   // no database name: the first one
    CHECK(manager.connect("system://localhost/other", &second, &error) == DbStatus::InvalidArgument && !second);
    CHECK(manager.connect("postgres://localhost/db", &second, &error) == DbStatus::UnknownDriver);

    // Settings
    {
      SystemSettings settings(a, database);
      CHECK(settings.ready() && !settings.has("desktop", "theme") && settings.get_int("desktop", "gap", 7) == 7 && settings.get_bool("desktop", "dark", true));
      CHECK(settings.set_text("desktop", "theme", "midnight") && settings.set_int("desktop", "gap", 12) && settings.set_bool("desktop", "dark", false) && settings.set_real("audio", "gain", 0.75));
      String text(a);
      CHECK(settings.get_text("desktop", "theme", &text) && text.view().equals("midnight") && settings.get_int("desktop", "gap", 0) == 12 && !settings.get_bool("desktop", "dark", true) &&
            settings.get_real("audio", "gain", 0) == 0.75);
      CHECK(settings.set_text("desktop", "theme", "dawn") && settings.get_text("desktop", "theme", &text) && text.view().equals("dawn"));   // replaced, not added
      CHECK(settings.get_int("desktop", "theme", 99) == 99 && settings.set_text("desktop", "it's", "a 'quote'") && settings.get_text("desktop", "it's", &text) && text.view().equals("a 'quote'"));
      CHECK(settings.remove("desktop", "gap") && !settings.has("desktop", "gap") && settings.remove_scope("desktop") == 3 && !settings.has("desktop", "theme") && settings.has("audio", "gain"));
      // The settings are rows in a table anyone can query.
      CHECK(connection->execute("SELECT scope, name, value FROM settings", &rows) == DbStatus::Ok && rows->row_count() == 1 && rows->text(0, 1).equals("gain"));
    }
    connection.reset();
    second.reset();
  }
  // Settings survive a restart.
  {
    filesystem::RamFileSystem files(a);
    {
      LocalDatabase database(a, &files, "/AppStore/System/system.db");
      database.open();
      SystemSettings settings(a, database);
      CHECK(settings.set_int("boot", "count", 5));
    }
    LocalDatabase database(a, &files, "/AppStore/System/system.db");
    database.open();
    SystemSettings settings(a, database);
    CHECK(settings.get_int("boot", "count", 0) == 5 && settings.set_int("boot", "count", 6) && settings.get_int("boot", "count", 0) == 6);
  }
  // A file system that cannot be written to: the database works in memory.
  {
    filesystem::RamFileSystem files(a, 64);   // room for nothing
    LocalDatabase database(a, &files, "/AppStore/System/system.db");
    CHECK(database.open() == DbStatus::Ok);
    SystemSettings settings(a, database);
    CHECK(settings.set_int("a", "b", 1) && settings.get_int("a", "b", 0) == 1 && !database.persistent() && !database.problem().empty());
  }
  CHECK(a.live == 0);
}

int main() {
  setvbuf(stdout, nullptr, _IONBF, 0);
  test_sql();
  test_file_and_transactions();
  test_driver_and_settings();
  if (g_failed) {
    printf("%d check(s) failed\n", g_failed);
    return 1;
  }
  printf("system database tests passed\n");
  return 0;
}
