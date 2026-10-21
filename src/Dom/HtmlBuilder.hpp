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

// Reads HTML into a document tree, the forgiving way browsers do: the <html>, <head> and <body> elements are made when the page has none, text and
// elements that belong in the body start it, end tags that were left out (of <p>, <li>, <td>, <tr>, <dt>, <dd>, <option> ...) are put in, unknown and
// misplaced tags are kept as they are, comments are kept, character references are decoded (to UTF-8), the first newline of <pre> and <textarea> is dropped
// and the content of <script>, <style>, <title>, <textarea> and a few others is read as text. Scripts are handed to an IScriptRunner as they are met: what
// they document.write is read in their place. Not done: foster parenting of table content, reconstruction of formatting elements, <frameset>, SVG and MathML
// namespaces (their elements are ordinary elements).
#pragma once
#include "Dom/Dom.hpp"

namespace dom {

// What runs the scripts of a page while it is read.
class IScriptRunner {
 public:
  // The text of the index-th <script src=...> of the page (counting them in the order the page names them; `src` as written); empty if it cannot be had.
  virtual StringView external(uint32_t index, StringView src) = 0;
  // Runs one script (`script` is its element, already in the tree). What it wrote with document.write is appended to *written.
  virtual void run(Node* script, StringView source, String* written) = 0;

 protected:
  IScriptRunner() = default;
  ~IScriptRunner() = default;
};

// Builds the page's tree under doc.root() (which should be empty). Scripts run when `scripts` is given. False if memory or the node limit ran out.
bool build_document(Document& doc, StringView html, IScriptRunner* scripts);

}  // namespace dom
