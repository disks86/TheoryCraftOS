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

// The SQL of the system database: a small, portable subset, enough for settings and for the tables programs keep for themselves.
//
//   CREATE TABLE [IF NOT EXISTS] t (col TYPE [PRIMARY KEY] [NOT NULL] [DEFAULT literal], ..., [PRIMARY KEY (col, ...)])
//       TYPE: INTEGER (INT, BIGINT), BOOLEAN, REAL (FLOAT, DOUBLE, NUMERIC), TEXT (VARCHAR, CHAR), BLOB (BYTEA); none keeps values as they come
//   DROP TABLE [IF EXISTS] t
//   INSERT [OR REPLACE | OR IGNORE] INTO t [(col, ...)] VALUES (expr, ...) [, (expr, ...)]
//   SELECT expr [AS name], ... | * [FROM t] [WHERE expr] [ORDER BY expr [ASC | DESC], ...] [LIMIT n] [OFFSET n]
//   UPDATE t SET col = expr, ... [WHERE expr]
//   DELETE FROM t [WHERE expr]
//   SHOW TABLES        DESCRIBE t        (and BEGIN, COMMIT, ROLLBACK on a connection)
//
// Expressions: numbers, 'text' (a quote inside is written twice), NULL, TRUE, FALSE, columns, ? parameters, + - * / %, ||, comparisons, IS [NOT] NULL,
// [NOT] LIKE (% and _), AND, OR, NOT, parentheses; the functions COALESCE (IFNULL), LOWER, UPPER, LENGTH, ABS; and COUNT(*), COUNT(x), SUM, AVG,
// MIN, MAX, which make the whole answer one row. There are no joins, GROUP BY or subqueries. Several statements may be separated by ';'.
#pragma once
#include "Database/Local/LocalCatalog.hpp"
#include "Database/TextResultSet.hpp"

namespace database::local {

// Runs the statements of `sql` against `catalog` one after the other and stops at the first that fails (what the earlier ones did stays done: a
// caller that wants all or nothing keeps a copy to go back to). `result` receives the answer of the last statement; *changed says whether any
// statement could have changed the tables. A failure sets `error` (a PostgreSQL SQLSTATE, a message) and returns Server.
DbStatus run_sql(Catalog& catalog, allocators::IAllocator& allocator, StringView sql, ArrayView<const DbParam> params, UniquePointer<TextResultSet>* result, bool* changed,
                 DbError* error);

}  // namespace database::local
