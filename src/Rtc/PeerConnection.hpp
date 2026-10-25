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

// One RTCPeerConnection (JSEP, RFC 9429): session descriptions (offer, answer, rollback), ICE, DTLS, SCTP data channels (RFC 8831) on one bundled transport. It is polled: poll()
// drives the network and the timers; what happens (candidates, states, data channel events and messages) comes out of take_event() as JSON objects, one per call, the form
// the script side reads. Data channels are named by a handle, the number this class gives.
#pragma once
#include "Rtc/Dtls.hpp"
#include "Rtc/Ice.hpp"
#include "Rtc/Sctp.hpp"
#include "Rtc/Srtp.hpp"
#include "Rtc/RtcAudio.hpp"
#include "Rtc/Sdp.hpp"

namespace rtc {

class PeerConnection {
 public:
  enum class Signaling : uint8_t { Stable, HaveLocalOffer, HaveRemoteOffer, HaveLocalPranswer, HaveRemotePranswer, Closed };
  enum class Connection : uint8_t { New, Connecting, Connected, Disconnected, Failed, Closed };
  static constexpr uint32_t kMaxChannels = 256, kMaxMessage = SctpAssociation::kMaxMessage, kMaxPendingBytes = 16 * 1024 * 1024;

  PeerConnection(allocators::IAllocator& allocator, IRtcNetwork& network, crypto::IRandom& random, const DtlsIdentity& identity);
  ~PeerConnection();
  PeerConnection(const PeerConnection&) = delete;
  PeerConnection& operator=(const PeerConnection&) = delete;

  // "stun:host[:port]" addresses (turn: and others are not used).
  void add_stun_server(StringView url);

  // JSEP. The error text is a DOMException name followed by ": " and a message.
  bool create_offer(String* sdp, String* error);
  bool create_answer(String* sdp, String* error);
  // `type`: "offer", "answer", "pranswer" or "rollback"; an empty sdp with an offer/answer type means the one create_offer()/create_answer() made last.
  bool set_local_description(StringView type, StringView sdp, String* error);
  bool set_remote_description(StringView type, StringView sdp, String* error);
  bool add_ice_candidate(StringView candidate, StringView mid, int32_t mline_index, String* error);   // "" ends the candidates
  // The local / remote description as it stands (the local one with the candidates gathered so far). False if there is none.
  bool local_description(String* type, String* sdp) const;
  bool remote_description(String* type, String* sdp) const;
  Signaling signaling_state() const { return signaling_; }
  Connection connection_state() const { return connection_; }
  IceState ice_state() const { return ice_.state(); }
  IceGathering gathering_state() const { return ice_.gathering(); }
  void restart_ice() { restart_ice_ = true; }
  void close();

  // Data channels. Returns the handle (> 0) or 0 (error text set).
  uint32_t create_channel(StringView label, StringView protocol, bool ordered, int32_t max_retransmits, int32_t max_lifetime, bool negotiated, int32_t id, String* error);
  // false: the channel is not open, or the message is too large / the queue is full (error text set).
  bool channel_send(uint32_t handle, bool binary, ConstByteView data, String* error);
  void channel_close(uint32_t handle);
  uint64_t channel_buffered(uint32_t handle) const;
  void channel_threshold(uint32_t handle, uint64_t threshold);
  bool channel_info(uint32_t handle, String* json) const;
  uint64_t max_message_size() const { return SctpAssociation::kMaxMessage; }

  // Audio. A transceiver is a media section: it keeps its index for the life of the connection. Directions: 0 sendrecv, 1 sendonly, 2 recvonly, 3 inactive.
  void set_devices(IRtcAudioDevices* devices) { devices_ = devices; }
  int32_t add_transceiver(StringView kind, int direction, String* error);
  // Gives the sender of a transceiver a source (its track); the direction follows (a receiving transceiver starts to send as well).
  bool set_sender(uint32_t index, IRtcAudioSource* source, StringView track_id, StringView stream_id, String* error);
  // addTrack(): a transceiver that can be reused or a new one. Returns its index, -1 (error text set).
  int32_t add_track(IRtcAudioSource* source, StringView track_id, StringView stream_id, String* error);
  bool remove_sender(uint32_t index, String* error);
  bool set_direction(uint32_t index, int direction, String* error);
  bool stop_transceiver(uint32_t index, String* error);
  bool set_sender_enabled(uint32_t index, bool enabled);
  // Plays what the receiver gets on the speaker (or stops).
  bool set_receiver_playing(uint32_t index, bool playing);
  void drop_source(IRtcAudioSource* source);   // the source is going away
  uint32_t transceiver_count() const;
  bool transceiver_info(uint32_t index, String* json) const;

  void poll();
  bool take_event(String* json);
  // The statistics (RTCStatsReport entries) as a JSON array.
  void stats(String* json);

 private:
  struct Channel;
  struct Mline {
    explicit Mline(allocators::IAllocator& a) : kind(a), mid(a), formats(a), track_id(a), stream_id(a), remote_track_id(a), remote_stream_id(a), remote_direction(a) {}
    String kind, mid, formats;   // formats: the payload types of a section we reject, to answer it with
    bool rejected = false;
    // audio (kind "audio")
    uint8_t desired = 2, negotiated = 3;   // directions: what the script wants, what the descriptions agreed (inactive until they have)
    bool agreed = false, stopped = false, ever_sent = false, enabled = true, announced = false, remote_sends = false, unmuted = false, playing = false, first_packet = true;
    int32_t payload = -1;                  // 0 PCMU or 8 PCMA once chosen
    uint32_t ssrc = 0, remote_ssrc = 0;
    String track_id, stream_id, remote_track_id, remote_stream_id, remote_direction;
    IRtcAudioSource* source = nullptr;
    UniquePointer<IRtcAudioSink> sink;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    int16_t stage[160] = {};   // the samples of the frame being gathered
    uint32_t staged = 0;
    uint64_t packets_sent = 0, octets_sent = 0, last_report_ms = 0;
    // receive
    bool have_seq = false;
    uint16_t expected_seq = 0;
    uint64_t packets_received = 0, octets_received = 0, packets_lost = 0, last_arrival_ms = 0;
    uint32_t last_rtp_timestamp = 0, jitter = 0, last_sr = 0;
    uint64_t last_sr_ms = 0;
  };
  struct Description {
    explicit Description(allocators::IAllocator& a) : type(a), sdp(a) {}
    String type, sdp;
  };

  void event(StringView json);
  void set_signaling(Signaling s);
  void update_connection();
  bool write_sdp(bool offer, String* sdp) const;
  bool read_sdp(StringView text, bool offer, String* error);
  Channel* find_channel(uint32_t handle) const;
  Channel* find_by_sid(int32_t sid) const;
  void announce(Channel& c);
  void open_channels();
  void flush(Channel& c);
  void deliver(UniquePointer<SctpMessage> m);
  void handle_dcep(UniquePointer<SctpMessage> m);
  void close_channel_now(Channel& c, bool error);
  void pump_transports();
  void pump_media(uint64_t now);
  void handle_rtp(ConstByteView datagram, uint64_t now);
  void handle_rtcp(ConstByteView datagram, uint64_t now);
  void send_reports(uint64_t now);
  Mline* audio_at(uint32_t index) const;
  void start_srtp();
  void note_track(Mline& m, uint32_t index);
  uint32_t random32();
  void media_event(StringView type, uint32_t index);
  void start_dtls();
  int32_t allocate_sid();
  void random_text(uint32_t n, String* out);
  void apply_descriptions();
  bool candidate_line(const IceCandidate& c, String* out) const;

  allocators::IAllocator& allocator_;
  IRtcNetwork& net_;
  crypto::IRandom& random_;
  const DtlsIdentity& identity_;
  IceAgent ice_;
  DtlsTransport dtls_;
  SctpAssociation sctp_;
  List<String> stun_servers_;
  String ufrag_, pwd_, remote_ufrag_, remote_pwd_, remote_fingerprint_, session_id_, local_setup_, remote_setup_;
  bool local_offerer_ = false, applied_ = false;
  Signaling signaling_ = Signaling::Stable;
  Connection connection_ = Connection::New;
  List<UniquePointer<Mline>> mlines_;
  UniquePointer<Description> local_, remote_, pending_local_, pending_remote_, last_created_;
  List<String> candidates_;                // local candidate lines, in the order found
  List<UniquePointer<Channel>> channels_;
  IRtcAudioDevices* devices_ = nullptr;
  Srtp srtp_send_, srtp_recv_;
  bool srtp_ready_ = false;
  String cname_;
  List<String> events_;
  uint32_t next_handle_ = 1, version_ = 0;
  bool gathering_started_ = false, dtls_started_ = false, sctp_started_ = false, dtls_client_ = false, role_known_ = false, restart_ice_ = false, eoc_sent_ = false, closed_ = false;
  bool negotiation_needed_ = false;
  IceGathering reported_gathering_ = IceGathering::New;
  IceState reported_ice_ = IceState::New;
  Connection reported_connection_ = Connection::New;
  uint64_t closing_since_ = 0;
};

}  // namespace rtc
