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

// Turns a document tree into what the screen shows: the author style sheets (<style> elements, linked sheets, the caller's own) are read - @media rules
// judged by the display they are for - each element gets a Style from the defaults of its kind, the cascade and its style attribute, and the text and
// the boxes become the runs of an HtmlDocument that PageLayout fits into lines. The tree can be changed and rendered again at any time; computed_style()
// answers the same question for one element (getComputedStyle).
#pragma once
#include "Browser/Css.hpp"
#include "Browser/HtmlDocument.hpp"
#include "Browser/Media.hpp"
#include "Browser/ObjectTypes.hpp"
#include "Dom/Dom.hpp"

namespace browser {

// Where the text of linked style sheets comes from (the browser fetched them).
class IStyleSource {
 public:
  // The text of the sheet a <link rel=stylesheet> names; empty if there is none.
  virtual StringView linked(const dom::Node* link_element) = 0;

 protected:
  IStyleSource() = default;
  ~IStyleSource() = default;
};

struct RenderOptions {
  MediaEnvironment media;          // what the display is like: @media and media="" are judged by it
  StringView extra_css;            // a style sheet read before the page's own (the caller's)
  IStyleSource* styles = nullptr;  // linked style sheets (nullptr: none)
  IObjectSource* objects = nullptr;   // pictures, videos and canvases (nullptr: <img> shows its alternative text, <video> and <canvas> their fallback content)
};

// Reads the style sheets of the document, in the order they come, into `sheet`.
bool collect_style_sheets(dom::Document& doc, const RenderOptions& options, StyleSheet* sheet);

// Renders the whole page into `out` (replacing what it had). False if memory ran out.
bool render_document(dom::Document& doc, const RenderOptions& options, HtmlDocument* out);

// The title of the document: its first <title>, with white space collapsed.
void document_title(const dom::Document& doc, String* out);

// What an element looks like after the defaults of its kind, `sheet` and its style attribute, with the inheritance from its parents.
Style computed_style(const StyleSheet& sheet, const dom::Node* element);

}  // namespace browser
