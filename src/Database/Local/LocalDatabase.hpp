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

// The database that lives inside the system: tables kept in memory and written to one file, with the SQL of LocalSql.hpp, reached like any other
// database through the DatabaseManager (the driver "system", URL system://localhost/system). The OS and its programs keep their settings in it
// (see SystemSettings); the database query application can open it to look at the tables and change them.
//
//   LocalDatabase system_db(allocator, &files, "/AppStore/System/system.db");
//   system_db.open(&error);
//   LocalDriver driver(allocator);
//   driver.add_database("system", &system_db);
//   manager.add_driver(&driver);
//   manager.connect("system://localhost/system", &connection, &error);
#pragma once
#include "Database/Local/LocalCatalog.hpp"
#include "Database/Local/LocalSql.hpp"
#include "Database/IDatabaseDriver.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Kernel/Sync.hpp"

namespace database::local {

// The tables and the file they are saved in. Every statement that changes something is on disk when it returns (the file is written beside the old
// one, which is replaced only after that), unless a transaction is open: BEGIN holds the changes back until COMMIT, and ROLLBACK drops them. One
// transaction at a time: the others see its changes while it runs, and cannot change anything until it ends (their statements fail with 55P03).
// Statements are atomic: one that fails changes nothing. Safe to use from any thread.
class LocalDatabase {
 public:
  // `files` may be nullptr: the tables are then only in memory. The file system and `path` ("/AppStore/System/system.db") must stay valid.
  LocalDatabase(allocators::IAllocator& allocator, filesystem::IFileSystem* files, StringView path)
      : allocator_(allocator), files_(files), path_(allocator), catalog_(allocator), transaction_(allocator), problem_(allocator) {
    path_.assign(path);
  }
  LocalDatabase(const LocalDatabase&) = delete;
  LocalDatabase& operator=(const LocalDatabase&) = delete;

  // Reads the file, or makes a new one (with its folder). A file that cannot be used is renamed to "<path>.corrupt" and a new database starts. If the
  // file system cannot be written to, the database works in memory only: persistent() is false and problem() says why. Fails (not Ok) only when
  // memory runs out.
  DbStatus open(DbError* error = nullptr);
  // True while the tables are being kept in the file (false in memory-only mode, and after a write that failed, until one succeeds).
  bool persistent() const;
  StringView path() const { return path_.view(); }
  // The last thing that went wrong with the file ("" when all is well).
  String problem() const;

  // --- What a connection uses. `session` identifies the connection (the address of its object).
  DbStatus execute(const void* session, StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result, DbError* error);
  DbStatus begin(const void* session, DbError* error);
  DbStatus commit(const void* session, DbError* error);
  DbStatus rollback(const void* session, DbError* error);
  bool in_transaction(const void* session) const;
  // The connection is gone: its open transaction is rolled back.
  void end_session(const void* session);

  uint64_t table_count() const;
  // Writes the file now (a transaction in progress is not part of it).
  DbStatus flush(DbError* error = nullptr);

 private:
  DbStatus save_locked(DbError* error);
  DbStatus restore_locked(ConstByteView snapshot);
  bool read_file(StringView path, ByteBuffer* data, filesystem::Status* status);
  bool ensure_folder();
  void note_problem(StringView text);
  DbStatus locked_error(DbError* error);

  allocators::IAllocator& allocator_;
  filesystem::IFileSystem* files_;
  String path_;
  mutable kernel::Mutex mutex_;
  Catalog catalog_;
  const void* transaction_owner_ = nullptr;
  ByteBuffer transaction_;   // the tables as they were at BEGIN
  bool persistent_ = false;
  bool write_failed_ = false;   // the last attempt to write the file failed
  String problem_;
};

// One session with a LocalDatabase. Closing it (or destroying it) rolls back what it left open.
class LocalConnection final : public IConnection {
 public:
  LocalConnection(allocators::IAllocator& allocator, LocalDatabase& database) : allocator_(allocator), database_(database), error_(allocator), version_(allocator) {
    version_.assign("1.0");
  }
  ~LocalConnection() { close(); }

  const char* driver_name() const override { return "system"; }
  bool is_open() const override { return open_; }
  StringView server_version() const override { return version_.view(); }
  bool encrypted() const override { return false; }
  DbStatus execute(StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result) override;
  using IConnection::execute;
  DbStatus begin() override { return open_ ? database_.begin(this, &error_) : closed(); }
  DbStatus commit() override { return open_ ? database_.commit(this, &error_) : closed(); }
  DbStatus rollback() override { return open_ ? database_.rollback(this, &error_) : closed(); }
  bool in_transaction() const override { return open_ && database_.in_transaction(this); }
  DbStatus ping() override { return open_ ? DbStatus::Ok : closed(); }
  const DbError& last_error() const override { return error_; }
  void close() override {
    if (!open_) return;
    database_.end_session(this);
    open_ = false;
  }

 private:
  DbStatus closed() {
    error_.set(DbStatus::Closed, "the connection is closed");
    return DbStatus::Closed;
  }

  allocators::IAllocator& allocator_;
  LocalDatabase& database_;
  DbError error_;
  String version_;
  bool open_ = true;
};

// The driver for the databases that live inside the system. Each is known by a name, which the URL gives as its database: system://localhost/system
// (the host, port and login are not used; a URL without a database name opens the first). It needs no network.
class LocalDriver final : public IDatabaseDriver {
 public:
  explicit LocalDriver(allocators::IAllocator& allocator) : allocator_(allocator), databases_(allocator) {}

  // Makes a database available under `name` (not owned). False if the name is taken or memory ran out.
  bool add_database(StringView name, LocalDatabase* database);
  LocalDatabase* find_database(StringView name) const;

  const char* name() const override { return "system"; }
  bool handles_scheme(StringView scheme) const override { return scheme.equals_ignore_case("system"); }
  uint16_t default_port() const override { return 0; }
  bool needs_transport() const override { return false; }
  DbStatus connect(const ConnectionOptions& options, IDatabaseTransport& transport, UniquePointer<IConnection>* connection, DbError* error) override;

 private:
  struct Entry {
    String name;
    LocalDatabase* database;
  };
  allocators::IAllocator& allocator_;
  List<Entry> databases_;
};

}  // namespace database::local
