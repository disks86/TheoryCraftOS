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
#pragma once
// WebVTT (W3C "WebVTT: The Web Video Text Tracks Format"): the file parser (header, NOTE / STYLE / REGION blocks, cues with their timings and settings), the
// cue text parser (<c> <i> <b> <u> <v> <lang> <ruby> <rt>, timestamps, character references), and the cue layout that puts the cues showing at one moment
// on a video (line / position / size / align / vertical / regions, snap-to-lines with overlap avoidance), as a list of rectangles and pieces of text.
#include "Allocators/IAllocator.hpp"
#include "ObjectContainer/List.hpp"
#include "Utility/ArrayView.hpp"
#include "Utility/String.hpp"
#include "Utility/StringView.hpp"
#include "Utility/UniquePointer.hpp"

namespace media {

enum class VttVertical : uint8_t { Horizontal, GrowingLeft /* "rl" */, GrowingRight /* "lr" */ };
enum class VttLineAlign : uint8_t { Start, Center, End };
enum class VttPositionAlign : uint8_t { LineLeft, Center, LineRight, Auto };
enum class VttAlign : uint8_t { Start, Center, End, Left, Right };

// A region of the video that cues are rolled up in.
struct VttRegion {
  explicit VttRegion(allocators::IAllocator& allocator) : id(allocator) {}
  String id;
  double width = 100;                      // percent of the video
  uint32_t lines = 3;
  double anchor_x = 0, anchor_y = 100;     // the point of the region (percent) ...
  double viewport_x = 0, viewport_y = 100; // ... that sits at this point of the video (percent)
  bool scroll_up = false;
};

struct VttCue {
  explicit VttCue(allocators::IAllocator& allocator) : id(allocator), region_id(allocator), text(allocator), region(allocator) {}
  String id;
  String region_id;                        // the region setting of a parsed cue
  String text;                             // the cue text, as written (tags and all)
  double start = 0, end = 0;               // seconds
  bool pause_on_exit = false;
  VttVertical vertical = VttVertical::Horizontal;
  bool snap_to_lines = true;
  bool line_auto = true;
  double line = 0;                         // a line number, or percent when !snap_to_lines
  VttLineAlign line_align = VttLineAlign::Start;
  bool position_auto = true;
  double position = 50;                    // percent
  VttPositionAlign position_align = VttPositionAlign::Auto;
  double size = 100;                       // percent
  VttAlign align = VttAlign::Center;
  bool has_region = false;                 // for layout: the region the cue is in (filled in by whoever knows the regions)
  VttRegion region;
};

struct VttFile {
  explicit VttFile(allocators::IAllocator& allocator) : cues(allocator), regions(allocator), styles(allocator) {}
  List<UniquePointer<VttCue>> cues;
  List<UniquePointer<VttRegion>> regions;
  List<String> styles;                     // the text of the STYLE blocks
};

// Parses a WebVTT file (a leading byte order mark, any line ending). False if it does not start with "WEBVTT" (or memory ran out). Cues that are
// malformed are left out, as the standard says; the rest are in file order.
bool parse_webvtt(allocators::IAllocator& allocator, StringView text, VttFile* out);

// The cue settings of one cue ("line:10% position:20%,line-left align:start") applied to `cue`; settings that are not valid are ignored.
void apply_vtt_settings(StringView settings, VttCue* cue);

// A WebVTT timestamp ([hh:]mm:ss.ttt, the whole of `text`) in seconds.
bool parse_vtt_timestamp(StringView text, double* seconds);
// "hh:mm:ss.ttt" (hours always written: [hh:]mm:ss.ttt with hh when `seconds` is an hour or more, else mm:ss.ttt).
void format_vtt_timestamp(double seconds, String* out);

// The cue text as HTML for getCueAsHTML(): <c.x> -> <span class="x">, <i> <b> <u> <ruby> <rt> as they are, <v a> -> <span title="a">, <lang x> -> <span lang="x">,
// timestamps -> <span data-timestamp="seconds"></span>. The text is escaped. False if memory ran out.
bool vtt_cue_text_to_html(StringView cue_text, String* out);
// The text of the cue with the tags taken out and character references decoded.
bool vtt_cue_text_to_plain(StringView cue_text, String* out);

// One piece of text shown or one rectangle filled, from layout_vtt_cues().
struct VttDrawOp {
  explicit VttDrawOp(allocators::IAllocator& allocator) : text(allocator) {}
  enum Flags : uint8_t { Bold = 1, Italic = 2, Underline = 4 };
  int32_t x = 0, y = 0;
  uint32_t width = 0, height = 0;
  uint32_t background = 0;                 // 0xAARRGGBB; the rectangle (x, y, width, height) is filled with it when the alpha is not 0
  String text;                             // not empty: drawn at (x, y) in `color`
  uint32_t color = 0xFFFFFFFFu;
  uint8_t flags = 0;
};

constexpr uint32_t kVttCellWidth = 8, kVttCellHeight = 16;   // the font the cues are drawn in

// Lays out the cues that are showing on a video of `width` x `height` pixels. `bottom_inset` is the height at the bottom that must stay free (a control bar).
// The cues are placed in the order given. False if memory ran out.
bool layout_vtt_cues(allocators::IAllocator& allocator, ArrayView<const VttCue* const> cues, uint32_t width, uint32_t height, uint32_t bottom_inset,
                     List<UniquePointer<VttDrawOp>>* out);

}  // namespace media
