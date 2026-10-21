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

// Media queries (Media Queries Levels 3 and 4): what a style sheet's @media rules, a <style> or <link>'s media attribute and window.matchMedia ask about
// the display. A query list is true when any of its queries is. Understood: the media types (all, screen, print, speech, and the old ones, which never
// apply), `not` and `only`, `and`, `or`, `not (...)` and nesting in parentheses, the features with a min- and max- prefix or a range
// ((400px <= width <= 800px), (width > 600px)), the boolean form ((color)), and the values: lengths (px, em, rem, pt, pc, in, cm, mm, q, vw, vh, vmin,
// vmax), numbers, ratios (16/9), resolutions (dpi, dpcm, dppx, x) and keywords. Features: width, height, aspect-ratio, orientation, resolution, color,
// color-index, monochrome, device-width, device-height, device-aspect-ratio, hover, any-hover, pointer, any-pointer, prefers-color-scheme,
// prefers-reduced-motion, prefers-contrast, prefers-reduced-data, prefers-reduced-transparency, forced-colors, inverted-colors, scripting, update,
// overflow-block, overflow-inline, scan, grid, display-mode, color-gamut, dynamic-range and the -webkit- pixel ratio. What the browser does not know is
// not true (and neither is the `not` of it, as the standard says).
#pragma once
#include "Utility/StringView.hpp"

namespace browser {

// What the display is like at the moment.
struct MediaEnvironment {
  int32_t width = 640, height = 480;      // the viewport, in CSS pixels
  int32_t screen_width = 0, screen_height = 0;   // the screen (0: the same as the viewport)
  bool dark = false;                      // prefers-color-scheme: dark
  bool reduced_motion = false;
  bool scripting = true;
  bool print = false;                     // the media type: print instead of screen
  int32_t resolution_dpi = 96;
  int32_t color_bits = 8;

  bool operator==(const MediaEnvironment& o) const {
    return width == o.width && height == o.height && screen_width == o.screen_width && screen_height == o.screen_height && dark == o.dark &&
           reduced_motion == o.reduced_motion && scripting == o.scripting && print == o.print && resolution_dpi == o.resolution_dpi && color_bits == o.color_bits;
  }
  bool operator!=(const MediaEnvironment& o) const { return !(*this == o); }
};

// Does the media query list (text after "@media", or a media attribute) apply? An empty list applies; a list that is not valid does not.
bool media_matches(StringView query, const MediaEnvironment& env);

// Is the text a valid media query list at all (for matchMedia's `media`, and tests)?
bool media_valid(StringView query);

}  // namespace browser
