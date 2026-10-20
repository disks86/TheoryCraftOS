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

#include "Database/Local/LocalDatabase.hpp"
#include "Utility/Move.hpp"

namespace database::local {

namespace {
using filesystem::OpenMode;
using filesystem::Status;

bool is_read_only(StringView sql) {
  StringView s = sql.trimmed();
  while (s.ends_with(';')) s = s.without_suffix(1).trimmed();
  if (s.contains(';')) return false;   // several statements: the later ones may write
  static const char* const kReaders[] = {"select", "show", "describe", "desc"};
  for (const char* word : kReaders) {
    const uint64_t n = StringView(word).size();
    if (s.size() < n || !s.first(n).equals_ignore_case(word)) continue;
    if (s.size() == n || s[n] == ' ' || s[n] == '\t' || s[n] == '\n' || s[n] == '\r' || s[n] == '*' || s[n] == '(') return true;
  }
  return false;
}

// BEGIN, COMMIT and ROLLBACK, which a connection takes for itself. 0: some other statement.
int transaction_word(StringView sql) {
  StringView s = sql.trimmed();
  while (s.ends_with(';')) s = s.without_suffix(1).trimmed();
  if (s.equals_ignore_case("begin") || s.equals_ignore_case("begin transaction") || s.equals_ignore_case("start transaction")) return 1;
  if (s.equals_ignore_case("commit") || s.equals_ignore_case("end") || s.equals_ignore_case("commit transaction")) return 2;
  if (s.equals_ignore_case("rollback") || s.equals_ignore_case("abort") || s.equals_ignore_case("rollback transaction")) return 3;
  return 0;
}

DbStatus fail(DbError* error, DbStatus status, const char* sqlstate, StringView message) {
  if (error) {
    error->set(status, message);
    error->sqlstate.assign(sqlstate);
  }
  return status;
}
}  // namespace

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// The file

bool LocalDatabase::read_file(StringView path, ByteBuffer* data, Status* status) {
  UniquePointer<filesystem::IFile> file;
  *status = files_->open_file(path, OpenMode::Read, &file);
  if (*status != Status::Ok) return false;
  data->clear();
  uint8_t chunk[4096];
  for (uint64_t offset = 0;;) {
    uint64_t got = 0;
    *status = file->read(offset, ByteView(chunk, sizeof chunk), &got);
    if (*status != Status::Ok) return false;
    if (got == 0) break;
    if (!data->append(ConstByteView(chunk, got))) {
      *status = Status::OutOfMemory;
      return false;
    }
    offset += got;
  }
  return true;
}

bool LocalDatabase::ensure_folder() {
  const StringView path = path_.view();
  const uint64_t slash = path.rfind('/');
  if (slash == StringView::npos || slash == 0) return true;   // directly in the root
  const StringView folder = path.first(slash);
  for (uint64_t i = 1; i <= folder.size(); ++i) {
    if (i < folder.size() && folder[i] != '/') continue;
    const Status s = files_->create_directory(folder.first(i));
    if (s != Status::Ok && s != Status::AlreadyExists) return false;
  }
  return true;
}

void LocalDatabase::note_problem(StringView text) {
  problem_.assign(text);
}

bool LocalDatabase::persistent() const {
  kernel::ScopedMutex lock(mutex_);
  return persistent_ && !write_failed_;
}

String LocalDatabase::problem() const {
  kernel::ScopedMutex lock(mutex_);
  String copy(allocator_);
  copy.assign(problem_.view());
  return copy;
}

DbStatus LocalDatabase::save_locked(DbError* error) {
  if (!files_ || !persistent_) return DbStatus::Ok;
  ByteBuffer bytes(allocator_);
  if (!catalog_.serialize(&bytes)) return fail(error, DbStatus::OutOfMemory, "53200", "out of memory");
  String temporary(allocator_);
  temporary.assign(path_.view());
  temporary.append(".tmp");
  auto failed = [&](const char* what, Status s) {
    String m(allocator_);
    m.assign(what);
    m.append(": ");
    m.append(filesystem::status_name(s));
    note_problem(m.view());
    files_->remove(temporary.view());
    write_failed_ = true;
    return fail(error, DbStatus::Server, "58030", m.view());
  };
  {
    UniquePointer<filesystem::IFile> file;
    Status s = files_->open_file(temporary.view(), OpenMode::CreateTruncate, &file);
    if (s != Status::Ok) return failed("could not write the database file", s);
    uint64_t done = 0;
    while (done < bytes.size()) {
      uint64_t wrote = 0;
      s = file->write(done, bytes.view().subview(done, bytes.size() - done), &wrote);
      if (s != Status::Ok || wrote == 0) return failed("could not write the database file", s == Status::Ok ? Status::IoError : s);
      done += wrote;
    }
    s = file->flush();
    if (s != Status::Ok) return failed("could not write the database file", s);
  }
  // The new file is complete; now it takes the place of the old one. (Should the power fail between the two steps, open() finds the .tmp file.)
  Status s = files_->remove(path_.view());
  if (s != Status::Ok && s != Status::NotFound) return failed("could not replace the database file", s);
  s = files_->rename(temporary.view(), path_.view());
  if (s != Status::Ok) return failed("could not replace the database file", s);
  files_->flush();
  write_failed_ = false;
  problem_.clear();
  return DbStatus::Ok;
}

DbStatus LocalDatabase::restore_locked(ConstByteView snapshot) {
  DbError ignored(allocator_);
  return catalog_.load(snapshot, &ignored);
}

DbStatus LocalDatabase::open(DbError* error) {
  kernel::ScopedMutex lock(mutex_);
  DbError local(allocator_);
  DbError* e = error ? error : &local;
  catalog_.clear();
  persistent_ = false;
  if (!files_) {
    note_problem("the database is kept in memory only");
    return DbStatus::Ok;
  }
  ByteBuffer data(allocator_);
  Status status;
  String temporary(allocator_);
  temporary.assign(path_.view());
  temporary.append(".tmp");
  bool loaded = false;
  if (read_file(path_.view(), &data, &status)) {
    loaded = catalog_.load(data.view(), e) == DbStatus::Ok;
    if (!loaded) {   // unusable: keep it for a look, and use the last complete copy if there is one
      String corrupt(allocator_);
      corrupt.assign(path_.view());
      corrupt.append(".corrupt");
      files_->remove(corrupt.view());
      files_->rename(path_.view(), corrupt.view());
      note_problem("the database file was damaged and was set aside");
    }
  } else if (status == Status::OutOfMemory) {
    return fail(error, DbStatus::OutOfMemory, "53200", "out of memory");
  }
  if (!loaded && read_file(temporary.view(), &data, &status)) loaded = catalog_.load(data.view(), e) == DbStatus::Ok;
  if (!ensure_folder()) {
    note_problem("the folder for the database could not be made; the database is kept in memory only");
    return DbStatus::Ok;
  }
  persistent_ = true;
  if (!loaded) catalog_.clear();
  // A fresh start, or a recovered copy: (re)write the file now so that it exists and is whole.
  UniquePointer<filesystem::IFile> probe;
  if (!loaded || files_->open_file(path_.view(), OpenMode::Read, &probe) != Status::Ok) {
    probe.reset();
    DbError write_error(allocator_);
    const bool aside = !problem_.empty();
    if (save_locked(&write_error) == DbStatus::Ok && aside) note_problem("the database file was damaged and was set aside");
  }
  return DbStatus::Ok;
}

DbStatus LocalDatabase::flush(DbError* error) {
  kernel::ScopedMutex lock(mutex_);
  return save_locked(error);
}

uint64_t LocalDatabase::table_count() const {
  kernel::ScopedMutex lock(mutex_);
  return catalog_.table_count();
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Statements and transactions

DbStatus LocalDatabase::locked_error(DbError* error) {
  return fail(error, DbStatus::Server, "55P03", "the database is locked by a transaction of another connection");
}

DbStatus LocalDatabase::execute(const void* session, StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result, DbError* error) {
  switch (transaction_word(sql)) {
    case 1: return params.empty() ? begin(session, error) : fail(error, DbStatus::InvalidArgument, "08P01", "BEGIN takes no parameters");
    case 2: return commit(session, error);
    case 3: return rollback(session, error);
    default: break;
  }
  kernel::ScopedMutex lock(mutex_);
  const bool writes = !is_read_only(sql);
  if (writes && transaction_owner_ && transaction_owner_ != session) return locked_error(error);
  ByteBuffer before(allocator_);
  if (writes && !catalog_.serialize(&before)) return fail(error, DbStatus::OutOfMemory, "53200", "out of memory");
  UniquePointer<TextResultSet> rows;
  bool changed = false;
  const DbStatus status = run_sql(catalog_, allocator_, sql, params, &rows, &changed, error);
  if (status != DbStatus::Ok) {
    if (writes) restore_locked(before.view());
    return status;
  }
  if (changed && !transaction_owner_) {
    DbError ignored(allocator_);
    save_locked(&ignored);   // if the file cannot be written the change stays in memory, problem() says so, and the next change tries again
  }
  if (result) *result = UniquePointer<IResultSet>(util::move(rows));
  return DbStatus::Ok;
}

DbStatus LocalDatabase::begin(const void* session, DbError* error) {
  kernel::ScopedMutex lock(mutex_);
  if (transaction_owner_ == session) return fail(error, DbStatus::Server, "25001", "a transaction is already in progress");
  if (transaction_owner_) return locked_error(error);
  if (!catalog_.serialize(&transaction_)) return fail(error, DbStatus::OutOfMemory, "53200", "out of memory");
  transaction_owner_ = session;
  return DbStatus::Ok;
}

DbStatus LocalDatabase::commit(const void* session, DbError* error) {
  kernel::ScopedMutex lock(mutex_);
  if (transaction_owner_ != session) {
    if (transaction_owner_) return locked_error(error);
    return DbStatus::Ok;   // nothing to commit (PostgreSQL only warns)
  }
  DbError ignored(allocator_);
  save_locked(&ignored);
  transaction_owner_ = nullptr;
  transaction_.clear();
  return DbStatus::Ok;
}

DbStatus LocalDatabase::rollback(const void* session, DbError* error) {
  kernel::ScopedMutex lock(mutex_);
  if (transaction_owner_ != session) {
    if (transaction_owner_) return locked_error(error);
    return DbStatus::Ok;
  }
  const DbStatus restored = restore_locked(transaction_.view());
  transaction_owner_ = nullptr;
  transaction_.clear();
  return restored == DbStatus::Ok ? DbStatus::Ok : fail(error, DbStatus::Server, "XX000", "the tables could not be restored");
}

bool LocalDatabase::in_transaction(const void* session) const {
  kernel::ScopedMutex lock(mutex_);
  return transaction_owner_ == session;
}

void LocalDatabase::end_session(const void* session) {
  kernel::ScopedMutex lock(mutex_);
  if (transaction_owner_ != session) return;
  restore_locked(transaction_.view());
  transaction_owner_ = nullptr;
  transaction_.clear();
}

// ------------------------------------------------------------------------------------------------------------------------------------------------------
// Connection and driver

DbStatus LocalConnection::execute(StringView sql, ArrayView<const DbParam> params, UniquePointer<IResultSet>* result) {
  if (!open_) return closed();
  error_.clear();
  UniquePointer<IResultSet> rows;
  const DbStatus status = database_.execute(this, sql, params, &rows, &error_);
  if (status != DbStatus::Ok) return status;
  if (result) *result = util::move(rows);
  return DbStatus::Ok;
}

bool LocalDriver::add_database(StringView name, LocalDatabase* database) {
  if (!database || name.empty() || find_database(name)) return false;
  Entry entry{String(allocator_), database};
  return entry.name.assign(name) && databases_.push_back(util::move(entry));
}

LocalDatabase* LocalDriver::find_database(StringView name) const {
  for (uint64_t i = 0; i < databases_.count(); ++i)
    if (databases_[i].name.view().equals_ignore_case(name)) return databases_[i].database;
  return nullptr;
}

DbStatus LocalDriver::connect(const ConnectionOptions& options, IDatabaseTransport&, UniquePointer<IConnection>* connection, DbError* error) {
  LocalDatabase* database = options.database.empty() ? (databases_.count() ? databases_[0].database : nullptr) : find_database(options.database.view());
  if (!database) {
    if (error) {
      error->set(DbStatus::InvalidArgument, "there is no such database in the system");
      error->detail.assign(options.database.view());
    }
    return DbStatus::InvalidArgument;
  }
  *connection = UniquePointer<IConnection>(UniquePointer<LocalConnection>::make(allocator_, allocator_, *database));
  if (!*connection) return fail(error, DbStatus::OutOfMemory, "53200", "out of memory");
  return DbStatus::Ok;
}

}  // namespace database::local
