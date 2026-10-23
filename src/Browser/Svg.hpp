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

// SVG: an <svg> element (inline in a page, or a .svg file used as a picture) drawn onto a CanvasBitmap, which is how it is shown.
//
//   shapes      rect (also rounded), circle, ellipse, line, polyline, polygon, path (M L H V C S Q T A Z, absolute and relative), text and tspan, image-less
//   structure   g, svg inside svg, defs and use, symbol (through use), title / desc / metadata / style / script are skipped
//   painting    fill, stroke, stroke-width, -linecap, -linejoin, -miterlimit, -dasharray, -dashoffset, fill-rule, opacity, fill-opacity, stroke-opacity, color and
//               currentColor, linearGradient and radialGradient (objectBoundingBox or userSpaceOnUse, stops from href too), display none, visibility hidden
//   geometry    transform (matrix, translate, scale, rotate, skewX, skewY), viewBox with preserveAspectRatio (meet; "none" stretches), x / y of nested svg
//   styling     presentation attributes and the style attribute (style wins); style sheets are not applied
// (The HTML reader lowercases the names of tags and attributes, so lineargradient, viewbox ... are what it finds.)
#pragma once
#include "Browser/Canvas.hpp"
#include "Dom/Dom.hpp"
#include "Media/Image.hpp"

namespace browser {

// The size an <svg> asks for: its width and height attributes (pixels), else the shape of its viewBox at 300 wide, else 300 x 150.
void svg_natural_size(const dom::Node* svg, uint32_t* width, uint32_t* height, bool* shape_only = nullptr);
// Draws the content of the <svg> onto `bitmap` (blank, any size): the viewBox is fitted into the bitmap. False if memory ran out.
bool draw_svg(const dom::Node* svg, CanvasBitmap* bitmap);
// Does this file look like an SVG (an <svg> element within its first bytes)?
bool looks_like_svg(StringView text);
// Reads an SVG file and draws it as a picture of its natural size (at most 2048 x 2048). False if it is not one.
bool rasterize_svg(allocators::IAllocator& allocator, StringView text, media::Image* out);

}  // namespace browser
