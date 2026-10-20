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

// The model of a spreadsheet: a grid of cells (columns A-AZ, rows 1-1000), each holding the text that was typed into it. Text that starts with '=' is a
// formula, the rest is a number when it reads as one and plain text otherwise. A formula is evaluated when its cell is shown (and the value is kept until
// something changes): numbers, "text", cell references (A1, $B$2), ranges (A1:C3, inside functions), the operators + - * / ^ & = <> < > <= >=, parentheses
// and the functions SUM AVERAGE MIN MAX COUNT IF ABS ROUND INT SQRT MOD POWER AND OR NOT LEN. A formula that goes wrong shows an error (#DIV/0!, #VALUE!,
// #REF!, #NAME?, #NUM!, #CYCLE! for cells that depend on each other, #ERR! for a formula that cannot be read).
//
// A sheet is saved as CSV: one line per row, the typed text of each cell (formulas included) in quotes where it needs them.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/String.hpp"

namespace spreadsheet {

constexpr uint32_t kColumns = 52, kRows = 1000;

enum class ValueKind : uint8_t { Empty, Number, Text, Error, Range };
enum class ErrorKind : uint8_t { None, DivideByZero, Value, Reference, Name, Number, Cycle, Syntax, Pending };   // (Pending: internal, a cell the formula needs is not worked out yet)

// The value of a cell or of part of a formula.
struct Value {
  explicit Value(allocators::IAllocator& allocator) : text(allocator) {}
  ValueKind kind = ValueKind::Empty;
  double number = 0;
  ErrorKind error = ErrorKind::None;
  String text;                            // Text
  uint32_t c0 = 0, r0 = 0, c1 = 0, r1 = 0;  // Range (inclusive corners, c0 <= c1 and r0 <= r1)
};

class Sheet {
 public:
  explicit Sheet(allocators::IAllocator& allocator);
  ~Sheet();
  Sheet(const Sheet&) = delete;
  Sheet& operator=(const Sheet&) = delete;

  // --- Contents. Columns and rows count from 0 (A1 is column 0, row 0).
  // Sets what is typed into a cell ("" empties it). False if the cell is outside the sheet or memory ran out.
  bool set_text(uint32_t column, uint32_t row, StringView typed);
  // What was typed into the cell ("" for an empty one).
  StringView text(uint32_t column, uint32_t row) const;
  bool is_formula(uint32_t column, uint32_t row) const { return text(column, row).size() > 1 && text(column, row)[0] == '='; }
  // How the cell reads: the value of a formula, a number tidied up, the text itself. `right` says it is a number (the usual place to align it on the right).
  // False only when memory ran out.
  bool display(uint32_t column, uint32_t row, String* out, bool* right = nullptr);
  // The cell's number: true if its value is one (a number, or a formula that gives one).
  bool number(uint32_t column, uint32_t row, double* out);
  // Empties every cell.
  void clear();
  // Empties the cells of a rectangle.
  void clear_range(uint32_t c0, uint32_t r0, uint32_t c1, uint32_t r1);
  // The columns and rows that have anything in them, counted from the first (0 for an empty sheet).
  void used_extent(uint32_t* columns, uint32_t* rows) const;
  // Counts up with every change.
  uint32_t generation() const { return generation_; }
  bool modified() const { return modified_; }
  void mark_saved() { modified_ = false; }

  // --- CSV. load_csv replaces the sheet (cells outside it are dropped; `dropped` says how many); save_csv appends nothing but the file's text.
  bool load_csv(StringView text, uint32_t* dropped = nullptr);
  bool save_csv(String* out) const;

  // --- Helpers
  // "A", "B", ... "Z", "AA", ... for a column.
  static void column_name(uint32_t column, String* out);
  // Reads "B7" or "$B$7" (all of `text`) into column and row. False if it is not a cell of the sheet.
  static bool parse_reference(StringView text, uint32_t* column, uint32_t* row);
  // Copies `formula` with its relative cell references moved by (columns, rows) - what pasting a copied formula somewhere else does. A reference that
  // would leave the sheet becomes #REF!. References with a $ on the column or the row keep that part.
  static bool shift_references(StringView formula, int32_t columns, int32_t rows, String* out);
  // The text of a number the way a cell shows it (10 significant digits, no trailing zeros).
  static void format_number(double value, String* out);
  // Reads a number from the whole of `text` (blanks around are fine). False if it is not one.
  static bool parse_number(StringView text, double* out);
  static const char* error_text(ErrorKind error);

 private:
  struct Cell {
    explicit Cell(allocators::IAllocator& allocator) : typed(allocator), text(allocator) {}
    String typed;
    // The kept value, good while `generation` is the sheet's.
    uint32_t generation = 0;
    bool evaluating = false;
    ValueKind kind = ValueKind::Empty;
    double number = 0;
    ErrorKind error = ErrorKind::None;
    String text;
  };
  friend class FormulaParser;

  Cell* cell(uint32_t column, uint32_t row) const { return column < kColumns && row < kRows ? cells_[row * kColumns + column] : nullptr; }
  Cell* make_cell(uint32_t column, uint32_t row);
  // The value of a cell as the formula that reads it needs it. A cell that is not worked out yet is queued (pending_) and the value says Pending; the driver
  // (make_valid) works those out first and runs the formula again, so no chain of cells is ever followed by recursion.
  void value_of(uint32_t column, uint32_t row, Value* out);
  bool is_valid(uint32_t column, uint32_t row) const;
  void queue(uint32_t column, uint32_t row);
  // Works out a cell and everything it depends on.
  bool make_valid(uint32_t column, uint32_t row);
  // Runs the cell's formula once. False if it needed cells that were not worked out (they are in pending_).
  bool evaluate(Cell* cell);
  void changed() { ++generation_; modified_ = true; }

  allocators::IAllocator& allocator_;
  Cell** cells_ = nullptr;
  List<uint32_t> pending_;   // cells (row * kColumns + column) a running formula is waiting for
  uint32_t generation_ = 1;
  bool modified_ = false;
};

}  // namespace spreadsheet
