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

#include "Hardware/Printer/IppPrinter.hpp"

namespace hardware {

using namespace network;
using namespace network::ipp;

namespace {
constexpr uint64_t kRetryDelayMs = 1000;

uint32_t length_of(const char* s) {
  uint32_t n = 0;
  while (s && s[n]) ++n;
  return n;
}
bool same(const char* a, const char* b) {
  uint32_t i = 0;
  for (; a[i] && a[i] == b[i]; ++i) {}
  return a[i] == b[i];
}
bool starts_with(const char* s, const char* prefix) {
  uint32_t i = 0;
  for (; prefix[i]; ++i)
    if (s[i] != prefix[i]) return false;
  return true;
}
void copy_text(char* dst, uint32_t capacity, const char* src) {
  uint32_t n = 0;
  for (; src && src[n] && n + 1 < capacity; ++n) dst[n] = src[n];
  dst[n] = 0;
}

// printer-state-reasons keywords (RFC 8011 4.4.12, PWG 5100.1) mapped to PrinterError bits and prtAlertCode values.
struct Reason {
  const char* keyword;
  uint16_t error;
  uint32_t code;
};
constexpr Reason kReasons[] = {
    {"media-low", kErrorLowPaper, 12},
    {"media-empty", kErrorNoPaper, 13},
    {"media-needed", kErrorNoPaper, 13},
    {"toner-low", kErrorLowToner, 12},
    {"toner-empty", kErrorNoToner, 13},
    {"marker-supply-low", kErrorLowToner, 12},
    {"marker-supply-empty", kErrorNoToner, 13},
    {"marker-supply-missing", kErrorMarkerSupplyMissing, 9},
    {"marker-waste-almost-full", 0, 14},
    {"marker-waste-full", 0, 15},
    {"door-open", kErrorDoorOpen, 3},
    {"cover-open", kErrorDoorOpen, 3},
    {"interlock-open", kErrorDoorOpen, 5},
    {"media-jam", kErrorJammed, 8},
    {"offline", kErrorOffline, 22},
    {"paused", kErrorOffline, 22},
    {"shutdown", kErrorOffline, 22},
    {"input-tray-missing", kErrorInputTrayMissing, 9},
    {"output-tray-missing", kErrorOutputTrayMissing, 9},
    {"output-area-almost-full", kErrorOutputNearFull, 14},
    {"output-area-full", kErrorOutputFull, 15},
    {"opc-near-eol", 0, 10},
    {"opc-life-over", kErrorServiceRequested, 11},
    {"developer-low", kErrorLowToner, 12},
    {"developer-empty", kErrorNoToner, 13},
    {"other", 0, 1},
};

SupplyKind supply_kind(const char* type, bool* consumed) {
  *consumed = true;
  if (starts_with(type, "waste")) { *consumed = false; return SupplyKind::WasteToner; }
  if (starts_with(type, "toner")) return SupplyKind::Toner;
  if (starts_with(type, "ink")) return SupplyKind::Ink;
  if (same(type, "opc")) return SupplyKind::Drum;
  if (same(type, "fuser")) return SupplyKind::Fuser;
  if (starts_with(type, "staples")) return SupplyKind::Staples;
  if (starts_with(type, "developer")) return SupplyKind::Developer;
  if (starts_with(type, "fuser-oil") || same(type, "oil")) return SupplyKind::Oil;
  if (starts_with(type, "cleaner") || starts_with(type, "fuser-cleaning")) return SupplyKind::Cleaner;
  if (starts_with(type, "transfer")) return SupplyKind::Transfer;
  return SupplyKind::Other;
}
}  // namespace

bool IppPrinter::submit(ConstByteView job, const char* job_name) {
  if (count_ >= kQueueDepth || job.empty()) return false;
  UniquePointer<Job> entry = UniquePointer<Job>::make(allocator_, allocator_);
  if (!entry || !entry->data.append(job)) return false;
  copy_text(entry->name, sizeof entry->name, job_name);
  queue_[(head_index_ + count_) % kQueueDepth] = util::move(entry);
  ++count_;
  return true;
}

bool IppPrinter::request_report() {
  if (report_requested_ || op_ == Op::Report) return false;
  report_requested_ = true;
  return true;
}
bool IppPrinter::request_jobs() {
  if (jobs_requested_ || op_ == Op::Jobs) return false;
  jobs_requested_ = true;
  return true;
}
bool IppPrinter::cancel_job(uint32_t job_id) {
  if (cancel_requested_ || op_ == Op::Cancel || job_id == 0) return false;
  cancel_requested_ = true;
  cancel_id_ = job_id;
  return true;
}

void IppPrinter::cancel_all() {
  exchange_.cancel();
  op_ = Op::None;
  for (UniquePointer<Job>& j : queue_) j.reset();
  head_index_ = count_ = 0;
  report_requested_ = jobs_requested_ = cancel_requested_ = false;
}

bool IppPrinter::build_printer_uri(char* out, uint32_t capacity) const {
  char t[96];
  uint32_t n = 0;
  auto text = [&](const char* s) { for (; *s && n + 1 < sizeof t; ++s) t[n++] = *s; };
  auto number = [&](uint32_t v) {
    char digits[11];
    uint32_t k = 0;
    do { digits[k++] = static_cast<char>('0' + v % 10); v /= 10; } while (v);
    while (k && n + 1 < sizeof t) t[n++] = digits[--k];
  };
  text(exchange_.uses_tls() ? "ipps://" : "ipp://");
  for (int i = 0; i < 4; ++i) {
    number((address_.value() >> (24 - 8 * i)) & 0xff);
    if (i < 3) text(".");
  }
  text(":");
  number(port_);
  text(path_ && path_[0] ? path_ : "/");
  t[n] = 0;
  if (n + 1 > capacity) return false;
  copy_text(out, capacity, t);
  return true;
}

// Starts the exchange for `op`; on a failure to even start, the operation is finished at once as failed.
void IppPrinter::begin(Op op) {
  char uri[96];
  head_.clear();
  ++request_id_;
  bool ok = build_printer_uri(uri, sizeof uri);
  ConstByteView document;
  if (ok) {
    switch (op) {
      case Op::Print: {
        Job& job = *queue_[head_index_];
        ++job.attempts;
        RequestBuilder b(head_, kPrintJob, request_id_);
        ok = b.standard_operation_attributes() && b.add_string(kTagUri, "printer-uri", uri) && b.add_string(kTagName, "requesting-user-name", user_) &&
             b.add_string(kTagName, "job-name", job.name[0] ? job.name : "TheoryCraftOS") && b.add_string(kTagMimeType, "document-format", format_) &&
             b.finish();
        document = job.data.view();
        break;
      }
      case Op::Report: {
        RequestBuilder b(head_, kGetPrinterAttributes, request_id_);
        static const char* const kWanted[] = {"printer-state", "printer-state-reasons", "printer-state-message", "printer-make-and-model",
                                              "printer-device-id", "marker-names", "marker-types", "marker-levels", "marker-low-levels",
                                              "marker-high-levels", "printer-info"};
        ok = b.standard_operation_attributes() && b.add_string(kTagUri, "printer-uri", uri) && b.add_string(kTagKeyword, "requested-attributes", kWanted[0]);
        for (uint32_t i = 1; ok && i < sizeof kWanted / sizeof kWanted[0]; ++i) ok = b.add_more_string(kTagKeyword, kWanted[i]);
        ok = ok && b.finish();
        break;
      }
      case Op::Jobs: {
        RequestBuilder b(head_, kGetJobs, request_id_);
        ok = b.standard_operation_attributes() && b.add_string(kTagUri, "printer-uri", uri) && b.add_integer(kTagInteger, "limit", kMaxJobs) &&
             b.add_string(kTagKeyword, "requested-attributes", "job-id") && b.add_more_string(kTagKeyword, "job-state") &&
             b.add_more_string(kTagKeyword, "job-name") && b.add_more_string(kTagKeyword, "job-k-octets") &&
             b.add_string(kTagKeyword, "which-jobs", "not-completed") && b.finish();
        break;
      }
      case Op::Cancel: {
        RequestBuilder b(head_, kCancelJob, request_id_);
        ok = b.standard_operation_attributes() && b.add_string(kTagUri, "printer-uri", uri) && b.add_integer(kTagInteger, "job-id", static_cast<int32_t>(cancel_id_)) &&
             b.add_string(kTagName, "requesting-user-name", user_) && b.finish();
        break;
      }
      default:
        ok = false;
    }
  }
  op_ = op;
  if (!ok || !exchange_.start(address_, port_, path_, head_.view(), document)) {
    last_status_ = 0xffff;
    finish(op);
  }
}

void IppPrinter::finish(Op op) {
  op_ = Op::None;
  const bool transport_ok = exchange_.state() == IppExchangeState::Done && exchange_.http_status() == 200;
  Response* response = nullptr;
  UniquePointer<Response> holder;
  if (transport_ok) {
    holder = UniquePointer<Response>::make(allocator_);
    if (holder && decode_response(exchange_.body(), holder.get())) {
      response = holder.get();
      last_status_ = response->status;
    } else {
      last_status_ = 0xffff;
    }
  } else {
    last_status_ = 0xffff;
  }
  const bool ok = response && status_ok(response->status);
  switch (op) {
    case Op::Print: {
      Job& job = *queue_[head_index_];
      if (!ok && (!response || response->status == 0x0507 /* server-error-busy */ || response->status == 0x0505 /* temporary-error */ ) && job.attempts < 3) {
        retry_at_ms_ = stack_.now_ms() + kRetryDelayMs;  // the same job again, a little later
        return;
      }
      if (ok) {
        ++stats_.jobs_sent;
        stats_.bytes_sent += job.data.size();
        last_failed_ = false;
        const Attribute* id = response->find("job-id", kGroupJob);
        last_job_id_ = id ? static_cast<uint32_t>(id->integer) : 0;
      } else {
        ++stats_.jobs_failed;
        last_failed_ = true;
      }
      queue_[head_index_].reset();
      head_index_ = (head_index_ + 1) % kQueueDepth;
      --count_;
      break;
    }
    case Op::Report:
      if (ok) {
        fill_report(*response);
        have_report_ = true;
      }
      break;
    case Op::Jobs:
      job_count_ = 0;
      if (ok) fill_jobs(*response);
      break;
    case Op::Cancel:
      cancel_result_ = last_status_;
      break;
    default:
      break;
  }
  exchange_.cancel();
}

void IppPrinter::poll() {
  if (op_ != Op::None) {
    exchange_.poll();
    if (!exchange_.busy()) finish(op_);
    return;
  }
  const uint64_t now = stack_.now_ms();
  if (cancel_requested_) {
    cancel_requested_ = false;
    begin(Op::Cancel);
  } else if (report_requested_) {
    report_requested_ = false;
    begin(Op::Report);
  } else if (jobs_requested_) {
    jobs_requested_ = false;
    begin(Op::Jobs);
  } else if (count_ > 0 && now >= retry_at_ms_) {
    begin(Op::Print);
  }
}

void IppPrinter::fill_report(const Response& r) {
  PrinterReport out;
  out.valid = true;
  const Attribute* state = r.find("printer-state", kGroupPrinter);
  if (state) out.engine = state->integer == 3 ? PrinterEngineState::Idle : state->integer == 4 ? PrinterEngineState::Printing : PrinterEngineState::Other;
  if (const Attribute* a = r.find("printer-make-and-model", kGroupPrinter)) copy_text(out.description, sizeof out.description, a->text);
  else if (const Attribute* i = r.find("printer-info", kGroupPrinter)) copy_text(out.description, sizeof out.description, i->text);
  if (const Attribute* a = r.find("printer-state-message", kGroupPrinter)) copy_text(out.display, sizeof out.display, a->text);
  if (const Attribute* id = r.find("printer-device-id", kGroupPrinter)) {
    // "MFG:Brother;CMD:PJL,PCL;MDL:HL-2070N;CLS:PRINTER;SN:123;"
    for (const char* p = id->text; *p; ++p) {
      const bool at_key = p == id->text || p[-1] == ';';
      const uint32_t skip = at_key && starts_with(p, "SN:") ? 3 : at_key && starts_with(p, "SERN:") ? 5 : 0;
      if (!skip) continue;
      uint32_t n = 0;
      for (p += skip; *p && *p != ';' && n + 1 < sizeof out.serial; ++p) out.serial[n++] = *p;
      out.serial[n] = 0;
      break;
    }
  }
  // state reasons
  for (uint32_t i = 0, n = r.value_count("printer-state-reasons"); i < n; ++i) {
    const Attribute* a = r.find("printer-state-reasons", i, kGroupPrinter);
    if (!a || same(a->text, "none")) continue;
    char keyword[64];
    copy_text(keyword, sizeof keyword, a->text);
    AlertSeverity severity = AlertSeverity::Other;
    uint32_t length = length_of(keyword);
    auto strip = [&](const char* suffix, AlertSeverity s) {
      const uint32_t sl = length_of(suffix);
      if (length > sl && same(keyword + length - sl, suffix)) {
        keyword[length - sl] = 0;
        length -= sl;
        severity = s;
      }
    };
    strip("-error", AlertSeverity::Critical);
    strip("-warning", AlertSeverity::Warning);
    strip("-report", AlertSeverity::Other);
    uint32_t code = 1;
    for (const Reason& reason : kReasons)
      if (same(keyword, reason.keyword)) {
        out.errors = static_cast<uint16_t>(out.errors | reason.error);
        code = reason.code;
        break;
      }
    if (out.alert_count < PrinterReport::kMaxAlerts) {
      AlertInfo& alert = out.alerts[out.alert_count++];
      alert.severity = severity;
      alert.code = code;
      copy_text(alert.description, sizeof alert.description, a->text);
    }
  }
  // supplies: marker-names, -types and -levels are parallel lists
  const uint32_t markers = r.value_count("marker-levels");
  for (uint32_t i = 0; i < markers && out.supply_count < PrinterReport::kMaxSupplies; ++i) {
    SupplyInfo& s = out.supplies[out.supply_count++];
    if (const Attribute* a = r.find("marker-names", i, kGroupPrinter)) copy_text(s.description, sizeof s.description, a->text);
    if (const Attribute* a = r.find("marker-types", i, kGroupPrinter)) s.kind = supply_kind(a->text, &s.consumed);
    const Attribute* level = r.find("marker-levels", i, kGroupPrinter);
    const int32_t v = level ? level->integer : SupplyInfo::kLevelUnknown;
    s.level_percent = v >= 0 ? (v > 100 ? 100 : v) : v == -1 ? SupplyInfo::kLevelOther : v == -3 ? SupplyInfo::kLevelSomeRemaining : SupplyInfo::kLevelUnknown;
  }
  report_ = out;
}

void IppPrinter::fill_jobs(const Response& r) {
  job_count_ = 0;
  IppJobInfo* current = nullptr;
  for (uint32_t i = 0; i < r.count; ++i) {
    const Attribute& a = r.attributes[i];
    if (a.group != kGroupJob || a.index != 0) continue;
    if (same(a.name, "job-id")) {
      if (job_count_ >= kMaxJobs) { current = nullptr; continue; }
      current = &jobs_[job_count_++];
      *current = IppJobInfo();
      current->id = static_cast<uint32_t>(a.integer);
    } else if (!current) {
      continue;
    } else if (same(a.name, "job-state")) {
      current->state = static_cast<uint32_t>(a.integer);
    } else if (same(a.name, "job-k-octets")) {
      current->size_kb = static_cast<uint32_t>(a.integer);
    } else if (same(a.name, "job-name")) {
      copy_text(current->name, sizeof current->name, a.text);
    }
  }
}

}  // namespace hardware
