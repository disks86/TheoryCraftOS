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

// Forms: what the elements <form>, <input>, <button>, <select>, <option>, <textarea>, <fieldset> and <output> hold and how they behave, apart from how they
// look (Renderer.cpp) or how a page reacts to them (Page.cpp).
//
// A control has two kinds of value, as in HTML: the one in the markup (the value attribute, the text of a <textarea>, the selected / checked attributes) and
// the one the user or a script gave it since. The second is kept in the node (Node::form_state says which controls have one; a changed text is in Node::data),
// so reset() can go back to the first, and the :checked, :focus, :valid ... selectors can see the state.
//
//   values       get_value() / set_value() as the `value` property reads and writes them (a number field is sanitized, a checkbox's is "on" without a value
//                attribute, a select's is its selected option's); is_checked() / set_checked() (radio buttons of a group clear each other); options of a select.
//   reset        reset_control() / reset_form(): back to what the markup says.
//   submission   form_entries() lists what a submit sends (disabled controls, unnamed ones, unchecked boxes and the buttons that were not pressed are left
//                out); encode_urlencoded() / encode_multipart() / encode_plain() make the three bodies of enctype.
//   validity     validity() says which constraints a control breaks (required, type email / url / number, pattern, minlength, maxlength, min, max, step);
//                validation_message() is what a browser tells the user. pattern is a small regular expression matcher (regex_match()) - the script engine has none.
#pragma once
#include "Dom/Dom.hpp"
#include "Dom/LegacyEncodings.hpp"

namespace dom {

enum FormStateBit : uint8_t {
  kFormValueDirty = 1,     // the value was changed (a text field's text is in Node::data)
  kFormCheckedDirty = 2,   // the checkedness / selectedness was changed (kFormChecked is it)
  kFormChecked = 4,
  kFormFocused = 8,
  kFormIndeterminate = 64, // a checkbox in neither state
  kFormModal = 32,         // a <dialog> opened with showModal()
  kFormFullscreen = 128,   // the element that is displayed fullscreen (the Fullscreen API)
  kFormEdited = 16,        // the user typed in it (that is what makes maxlength / minlength count)
};

enum class InputType : uint8_t {
  Text, Search, Tel, Url, Email, Password, Number, Range, Date, Time, DateTimeLocal, Month, Week, Color, Checkbox, Radio, File, Hidden, Image, Button, Submit, Reset
};
InputType input_type(const Node* input);   // from the type attribute (text when it is missing or not a type)
bool is_text_field(InputType type);        // shows text that is typed (all but the buttons, boxes, range, color, file and hidden)

// The elements a form lists as its controls (input, button, select, textarea, fieldset, output).
bool is_listed_control(const Node* n);
// Those whose value a submit sends (input, button, select, textarea).
bool is_submittable(const Node* n);
bool is_button(const Node* n);   // <button>, <input type=submit / reset / button / image>
bool is_submit_button(const Node* n);
bool is_reset_button(const Node* n);
// The form a control belongs to: the one the form attribute names, else the nearest <form> above it. nullptr if none.
Node* form_owner(const Node* control);
// The controls of a form, in tree order (those inside it, and those elsewhere that name it with their form attribute).
void form_controls(const Node* form, List<Node*>* out);
bool is_disabled(const Node* control);   // its own disabled attribute, or a disabled <fieldset> around it (outside its first <legend>)
bool is_read_only(const Node* control);
bool is_focusable(const Node* n);        // a control the user can step to: not disabled, hidden, or a file / color input

// --- Values
void get_value(const Node* control, String* out);
// Sets what the control holds; `sanitize` brings it to what the type allows (the typing of the user does not: "-" is on its way to a number).
bool set_value(Node* control, StringView value, bool sanitize = true);
void default_value(const Node* control, String* out);   // what the markup says
// For a checkbox or radio button, and for an <option> (its selectedness).
bool is_checked(const Node* n);
void set_checked(Node* n, bool checked);

void select_options(const Node* select, List<Node*>* out);   // the <option>s of a select (also those in an <optgroup>)
void option_value(const Node* option, String* out);          // its value attribute, else its text
void option_text(const Node* option, String* out);           // its text, white space collapsed
bool select_is_list(const Node* select);                     // multiple, or size above 1: shown as a list
int32_t selected_index(const Node* select);                  // -1 if none
void set_selected_index(Node* select, int32_t index);
void select_all_off(Node* select);

void reset_control(Node* control);
void reset_form(Node* form);

// --- Text selection (a text field or textarea; positions are bytes of the value)
// The selection and where the caret is, kept within the text. start <= end; the caret is one of them.
void get_selection(const Node* control, uint32_t* start, uint32_t* end, uint32_t* caret);
void set_selection(Node* control, uint32_t start, uint32_t end);   // the caret goes to the end of it
// Replaces the selection with `text` (inserts it at the caret if nothing is selected); the caret goes after it. The user typed it (maxlength counts).
bool replace_selection(Node* control, StringView text, bool user = true);
// Backspace (direction -1) and Delete (+1): removes the selection, else the character before / after the caret.
bool delete_selection(Node* control, int direction);
// Moves the caret by `delta` characters (INT32_MIN: to the start, INT32_MAX: to the end); `extend` keeps the other end of the selection where it was.
void move_caret(Node* control, int32_t delta, bool extend);

// --- Files (a file input holds what was chosen: name, type and bytes)
bool add_file(Node* input, StringView name, StringView type, ConstByteView bytes, bool replace);
uint32_t file_count(const Node* input);
bool file_at(const Node* input, uint32_t index, StringView* name, StringView* type, StringView* bytes);
void clear_files(Node* input);

// --- Submission
struct Entry {
  explicit Entry(allocators::IAllocator& a) : name(a), value(a), filename(a), content_type(a) {}
  String name, value;   // (a file: value is its bytes)
  bool file = false;
  String filename, content_type;
};
// What submitting `form` with `submitter` (a button, or nullptr) sends. False if memory ran out.
bool form_entries(const Node* form, const Node* submitter, List<Entry>* out);
bool encode_urlencoded(const List<Entry>& entries, String* out);
bool encode_urlencoded(const List<Entry>& entries, Encoding encoding, String* out);   // the bytes in that encoding ("&#NNN;" for what it cannot hold)                              // name=value&name=value, as application/x-www-form-urlencoded
bool encode_multipart(const List<Entry>& entries, StringView boundary, String* out);          // multipart/form-data
bool encode_plain(const List<Entry>& entries, String* out);
// The colors a color input steps through, and the one after (before) `current`.
void next_color(StringView current, int direction, String* out);
// Date and time fields: the value `steps` steps (days, months, weeks, minutes) away; false if the field is not one of those or the value is not valid.
bool step_date(Node* input, int32_t steps);
bool valid_date_text(InputType type, StringView text);                                   // text/plain
bool url_encode(StringView text, String* out);                                                // one component (space is "+")

// --- Validity
enum ValidityBit : uint32_t {
  kValueMissing = 1, kTypeMismatch = 2, kPatternMismatch = 4, kTooLong = 8, kTooShort = 16, kRangeUnderflow = 32, kRangeOverflow = 64, kStepMismatch = 128,
  kBadInput = 256, kCustomError = 512,
};
// Lets the page say which controls a script called setCustomValidity() on.
struct ValidityHooks {
  bool (*has_custom_error)(void* context, const Node* control) = nullptr;
  void* context = nullptr;
};
bool will_validate(const Node* control);   // is subject to constraints (not disabled, read-only, hidden, or a plain button)
uint32_t validity(const Node* control, const ValidityHooks* hooks = nullptr);
// What to tell the user about a control that breaks a constraint ("" if it breaks none).
bool validation_message(const Node* control, uint32_t flags, String* out);
// The first control of the form that breaks one (nullptr if the form is valid).
Node* first_invalid(const Node* form, const ValidityHooks* hooks = nullptr);

// --- Small tools
// Does the whole of `text` match the regular expression? (Literals, ., classes [a-z], \d \w \s and their capitals, groups, |, * + ? {n,m}, ^ and $. A
// pattern that cannot be read, or is too costly, matches.)
bool regex_match(StringView pattern, StringView text);
bool parse_number(StringView text, double* out);
void format_number(double value, String* out);
// Moves a number or range field up or down by `steps` of its step (false if it is not one).
bool step_value(Node* input, int32_t steps);

}  // namespace dom
