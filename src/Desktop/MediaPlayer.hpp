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

// The media player: a window that plays sound and video files. File > Open (Ctrl+O) picks a file in the shared FileDialog; it is
// recognised by what it contains (not by its name) and starts playing. The file is not read into memory: sound and video are read from it and
// decoded a little at a time while they play (only the pictures that are decoded whole - GIF, animated PNG and WebP, a still picture - are read in full). Sound files (WAV, AIFF, AU, FLAC, Ogg Vorbis, MP3, AAC, M4A, MIDI) get an
// AudioPlayerControl - play/pause, stop, seek, volume and a level meter - and video files (AVI, MP4 / MOV with H.264, GIF, animated PNG and
// WebP, Y4M) get a VideoPlayerControl with the picture over the same kind of bar. Problems (no audio output, a file that cannot be decoded)
// appear in the notice dialog. The machine's sound mixer, video codecs and clock are handed over with set_services(); without them
// the player says what is missing instead of playing. Closing the window stops playback and lets go of the file.
#pragma once
#include "Allocators/IAllocator.hpp"
#include "Audio/AudioMixer.hpp"
#include "Desktop/FileByteSource.hpp"
#include "Desktop/FileDialog.hpp"
#include "FileSystem/IFileSystem.hpp"
#include "Gui/Controls/AudioPlayerControl.hpp"
#include "Gui/Controls/Label.hpp"
#include "Gui/Controls/Menu.hpp"
#include "Gui/Controls/Panel.hpp"
#include "Gui/Controls/VideoPlayerControl.hpp"
#include "Gui/NoticeDialog.hpp"
#include "Gui/Windows/Frame.hpp"
#include "Hardware/Clock/IClock.hpp"
#include "Hardware/VideoCodec/SystemVideoCodecs.hpp"
#include "Media/AudioStreaming.hpp"
#include "Midi/MidiStream.hpp"
#include "Desktop/Playlist.hpp"
#include "Utility/UniquePointer.hpp"
#include "Video/VideoPlayer.hpp"

namespace desktop {

class MediaPlayer : public gui::Frame {
 public:
  explicit MediaPlayer(allocators::IAllocator& allocator);
  ~MediaPlayer();

  // The machine's sound mixer (nullptr: no sound), video codecs and clock (nullptr: no video). They must outlive the player. Call once, before use.
  void set_services(audio::AudioMixer* mixer, hardware::SystemVideoCodecs* codecs, const hardware::IClock* clock);
  // The machine's MIDI output hardware (nullptr: none). MIDI files are played on it when it is available() as the song is opened - the notes are
  // sent to it as the song plays - and otherwise by the software synthesizer. It must outlive the player.
  void set_midi_output(midi::IMidiOutput* output) { midi_output_ = output; }
  void set_file_system(filesystem::IFileSystem* fs) { fs_ = fs; }
  void set_file_dialog(FileDialog* dialog) { dialog_ = dialog; }
  void set_notices(gui::NoticeDialog* notices) { notices_ = notices; }

  // Reads the file at `path`, works out what it is and starts playing it. False (with a notice) if that fails; what was open stays closed.
  // A file that is not in the playlist replaces it with the media files of its folder (by name), so that next / previous have somewhere to go.
  bool open_file(StringView path);
  // The next / previous track of the playlist (the next button, the media keys). Previous goes back to the start of a track that has played for more than a few seconds first.
  // False when there is no such track (the end or start of the list, nothing open), or it could not be opened.
  bool next_track();
  bool previous_track();
  Playlist& playlist() { return playlist_; }
  // Called often (by the desktop): goes on to the next track when the one playing has played through.
  void pump();
  // File > Open: the dialog, then open_file().
  void ask_open();
  // Stops and forgets the open file.
  void close_media();

  enum class Kind : uint8_t { None, Audio, Video };
  Kind kind() const { return kind_; }
  StringView path() const { return path_.view(); }
  // Whether the name looks like something this player opens.
  static bool is_media_name(StringView name);

  gui::Label& status_label() { return status_; }
  gui::MenuBar& menu_bar() { return menu_; }
  gui::AudioPlayerControl* audio_control() { return audio_control_.get(); }
  gui::VideoPlayerControl* video_control() { return video_control_.get(); }
  audio::AudioPlayer* audio_player() { return audio_player_.get(); }
  video::VideoPlayer* video_player() { return video_player_.get(); }

  // A sound or video file dragged here from a file explorer is opened (just like File > Open).
  bool accepts_drop(StringView text, int32_t, int32_t, const gui::Control*) const override { return fs_ && is_media_name(text); }
  void on_drop(StringView text, int32_t, int32_t, const gui::Control*) override { open_file(text); }
  bool on_key(const gui::KeyArgs& args) override;

 private:
  static void on_open_menu(void* self, gui::Control&);
  static void on_close_file(void* self, gui::Control&);
  static void on_exit(void* self, gui::Control&);
  static void on_picked(void* self, gui::Control&);
  static void on_window_closed(void* self, gui::Control&);
  void notify(gui::NoticeKind kind, StringView text) {
    if (!gui::NoticeDialog::post(notices_, manager(), kind, text)) status_.set_text(text);
  }
  bool open_source();
  bool play_path(StringView path);
  void load_folder(StringView path);
  bool play_index(uint64_t index);
  int64_t position_ms();
  static void on_next_menu(void* self, gui::Control&);
  static void on_previous_menu(void* self, gui::Control&);

  gui::MenuBar menu_;
  gui::Panel status_bar_;
  gui::Label status_;
  filesystem::IFileSystem* fs_ = nullptr;
  FileDialog* dialog_ = nullptr;
  gui::NoticeDialog* notices_ = nullptr;
  UniquePointer<FileByteSource> file_;        // the open file (the players read from it while they play)
  UniquePointer<audio::IPcmSource> sound_;    // a sound file, decoded as it plays
  UniquePointer<audio::AudioPlayer> audio_player_;
  UniquePointer<video::VideoPlayer> video_player_;
  UniquePointer<gui::AudioPlayerControl> audio_control_;
  UniquePointer<gui::VideoPlayerControl> video_control_;
  midi::IMidiOutput* midi_output_ = nullptr;
  bool playing_midi_ = false, playing_midi_hardware_ = false;
  String path_;
  Playlist playlist_;
  int64_t last_position_ms_ = 0;
  bool was_playing_ = false;
  Kind kind_ = Kind::None;
};

}  // namespace desktop
