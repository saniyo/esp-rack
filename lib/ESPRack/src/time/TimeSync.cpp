#include <time/TimeSync.h>

#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ProblemJson.h>
#include <WebManager.h>
#include <esp_attr.h>
#include <esp_rom_crc.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <ctype.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <atomic>

namespace ESPRack {

namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;  // arbiter + bookkeeping
StaticSemaphore_t s_tzMutexBuf;
SemaphoreHandle_t s_tzMutex = nullptr;               // TZ strings + setenv/tzset
std::atomic<bool> s_tzEvent{false};

constexpr uint32_t kOperatorAccuracyMs = 2000;
constexpr int64_t kUnexplainedStepMs = 1000;
constexpr uint64_t kHoldRefreshMs = 10000;
constexpr int64_t kHoldMaxAgeS = 7 * 86400;
constexpr int64_t kMaxSampleAgeMs = 60000;   // older stamps: wrong timebase (millis wrap) or stale
constexpr int64_t kMaxSampleFutureMs = 1000;

// Survives soft resets (not power loss): the clock that kept running plus
// what made it valid. 32 bytes of RTC memory.
struct HoldRecord {
  uint32_t magic;
  uint8_t ver;
  uint8_t lastSrc;
  uint16_t reserved;
  int64_t lastSyncUtcMs;
  int64_t lastValidUtcS;
  uint32_t accMs;
  uint32_t crc;
};
static_assert(sizeof(HoldRecord) == 32, "hold record layout");
constexpr uint32_t kHoldMagic = 0x31485354u;  // "TSH1"
RTC_NOINIT_ATTR HoldRecord s_hold;

uint32_t holdCrc(const HoldRecord& r) {
  return esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(&r), offsetof(HoldRecord, crc));
}

inline int64_t sysUtcMs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
}

inline void setSysUtcMs(int64_t ms) {
  struct timeval tv;
  tv.tv_sec = static_cast<time_t>(ms / 1000);
  tv.tv_usec = static_cast<suseconds_t>((ms % 1000) * 1000);
  settimeofday(&tv, nullptr);
}

inline int32_t clamp32(int64_t v) {
  return v > INT32_MAX ? INT32_MAX : v < INT32_MIN ? INT32_MIN : static_cast<int32_t>(v);
}

inline bool accepted(SubmitResult r) { return r == SubmitResult::Applied || r == SubmitResult::Confirmed; }

// "EET-2EEST,M3.5.0/3,M10.5.0/4", "<+03>-3", "UTC0": starts with a zone
// name, contains an offset digit, only the POSIX TZ alphabet.
bool looksLikePosixTz(const char* s) {
  if (!s || !(isalpha((unsigned char)s[0]) || s[0] == '<')) return false;
  bool digit = false;
  for (const char* p = s; *p; ++p) {
    const unsigned char c = (unsigned char)*p;
    if (isdigit(c)) digit = true;
    else if (!isalpha(c) && !strchr("<>+-,.:/", c)) return false;
  }
  return digit;
}

// Whole string must be an optionally signed decimal integer.
bool parseInt64(const char* s, int64_t& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  const long long v = strtoll(s, &end, 10);
  if (!end || *end != '\0') return false;
  out = static_cast<int64_t>(v);
  return true;
}

// epoch (s) + optional ms -> utc ms, or a problem slug.
const char* validateOperatorTime(bool hasEpoch, bool epochOk, int64_t epoch, bool hasMs, bool msOk, int64_t ms,
                                 int64_t& utcMs) {
  if (!hasEpoch) return "missing_epoch";
  if (!epochOk) return "bad_epoch";
  if (hasMs && (!msOk || ms < 0 || ms > 999)) return "bad_ms";
  if (!TimeSync::isPlausible(epoch)) return "implausible_time";
  utcMs = epoch * 1000 + (hasMs ? ms : 0);
  return nullptr;
}
}  // namespace

const char* timeSourceName(TimeSource s) {
  switch (s) {
    case TimeSource::RtcHold: return "rtc_hold";
    case TimeSource::Manual: return "manual";
    case TimeSource::ExtRtc: return "ext_rtc";
    case TimeSource::Gps: return "gps";
    case TimeSource::Ntp: return "ntp";
    default: return "none";
  }
}

const char* submitResultName(SubmitResult r) {
  switch (r) {
    case SubmitResult::Applied: return "applied";
    case SubmitResult::Confirmed: return "confirmed";
    case SubmitResult::Pending: return "pending";
    case SubmitResult::RejectedImplausible: return "implausible";
    case SubmitResult::RejectedOutranked: return "outranked";
    case SubmitResult::RejectedUnconfirmed: return "unconfirmed";
  }
  return "?";
}

uint64_t TimeSync::monoMs() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

namespace {
TimeArbiterPolicy withSaneFloor(TimeArbiterPolicy p) {
  p.minSaneUtcS = ESPRACK_MIN_SANE_EPOCH;
  return p;
}
}  // namespace

TimeSync& TimeSync::instance() {
  static TimeSync s;
  return s;
}

TimeSync::TimeSync() : _arb(withSaneFloor(TimeArbiterPolicy{})) {
  s_tzMutex = xSemaphoreCreateMutexStatic(&s_tzMutexBuf);
  strlcpy(_tz, ESPRACK_DEFAULT_TZ, sizeof(_tz));
  strlcpy(_tzLabel, ESPRACK_DEFAULT_TZ_LABEL, sizeof(_tzLabel));
}

void TimeSync::begin() {
  if (_begun) return;
  _loopTask = xTaskGetCurrentTaskHandle();
  xSemaphoreTake(s_tzMutex, portMAX_DELAY);
  setenv("TZ", _tz, 1);
  tzset();
  xSemaphoreGive(s_tzMutex);

  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  restoreHoldAtBoot(now);
  _expectedBase = now.sysUtcMs - static_cast<int64_t>(now.monoMs);
  _hasBase = true;
  _begun = true;
}

void TimeSync::restoreHoldAtBoot(const TimeArbiter::Now& now) {
  const esp_reset_reason_t r = esp_reset_reason();
  const bool softReset = r == ESP_RST_SW || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                         r == ESP_RST_WDT;  // not DEEPSLEEP: the RC slow clock drifts far beyond 30 ppm
  const bool recordOk = s_hold.magic == kHoldMagic && s_hold.ver == 1 && s_hold.crc == holdCrc(s_hold) &&
                        s_hold.lastSrc < kTimeSourceCount;
  const int64_t nowS = now.sysUtcMs / 1000;
  if (!softReset || !recordOk || nowS < s_hold.lastValidUtcS || nowS >= s_hold.lastValidUtcS + kHoldMaxAgeS) {
    s_hold.magic = 0;
    return;
  }
  portENTER_CRITICAL(&s_mux);
  _arb.restoreHold(static_cast<TimeSource>(s_hold.lastSrc), s_hold.lastSyncUtcMs, s_hold.accMs, now);
  _lastAccSrc = TimeSource::RtcHold;
  _holdSrc = static_cast<TimeSource>(s_hold.lastSrc);
  _lastSyncUtcMs = s_hold.lastSyncUtcMs;
  _lastAccMs = s_hold.accMs;
  const bool ok = _arb.valid(now);
  const uint32_t u = _arb.uncertaintyMs(now);
  portEXIT_CRITICAL(&s_mux);
  log_i("[time] RtcHold restored (last source %s, uncertainty %u ms, %s)",
        timeSourceName(static_cast<TimeSource>(s_hold.lastSrc)), (unsigned)u, ok ? "valid" : "not valid");
}

void TimeSync::saveHold(int64_t nowUtcMs) {
  HoldRecord r{};
  r.magic = kHoldMagic;
  r.ver = 1;
  r.lastSrc = static_cast<uint8_t>(_holdSrc);
  r.lastSyncUtcMs = _lastSyncUtcMs;
  r.lastValidUtcS = nowUtcMs / 1000;
  r.accMs = _lastAccMs;
  r.crc = holdCrc(r);
  s_hold = r;
  _holdSavedMono = monoMs();
}

void TimeSync::flushHold() {
  if (_begun && isValid()) saveHold(sysUtcMs());
}

void TimeSync::registerSource(TimeSource s) {
  const uint8_t r = static_cast<uint8_t>(s);
  if (r == 0 || r >= kTimeSourceCount) return;
  portENTER_CRITICAL(&s_mux);
  _registeredMask |= static_cast<uint8_t>(1u << r);
  portEXIT_CRITICAL(&s_mux);
}

SubmitResult TimeSync::submit(const TimeSample& s) {
  if (!_begun || xTaskGetCurrentTaskHandle() != static_cast<TaskHandle_t>(_loopTask)) {
    log_e("[time] submit(%s) outside the loop task — ignored", timeSourceName(s.src));
    return SubmitResult::RejectedOutranked;
  }
  return submitInternal(s);
}

SubmitResult TimeSync::submitInternal(const TimeSample& s) {
  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  const uint8_t r = static_cast<uint8_t>(s.src);
  const int64_t age = static_cast<int64_t>(now.monoMs) - static_cast<int64_t>(s.sampleMonoMs);
  if (age < -kMaxSampleFutureMs || age > kMaxSampleAgeMs) {
    log_w("[time] %s sample rejected: stamp %lld ms old (use TimeSync::monoMs())", timeSourceName(s.src),
          (long long)age);
    portENTER_CRITICAL(&s_mux);
    ++_rejects;
    _lastRejectSrc = s.src;
    _lastReject = SubmitResult::RejectedImplausible;
    portEXIT_CRITICAL(&s_mux);
    return SubmitResult::RejectedImplausible;
  }

  portENTER_CRITICAL(&s_mux);
  const TimeArbiter::Verdict v = _arb.submit(s, now);
  if (r < kTimeSourceCount) {
    _seenS[r] = static_cast<uint32_t>(now.monoMs / 1000);
    _seenMask |= static_cast<uint8_t>(1u << r);
  }
  if (accepted(v.result)) {
    _lastAccSrc = s.src;
    _holdSrc = s.src;
    _lastAccMs = s.accuracyMs;
    _lastSyncUtcMs = v.stepClock ? v.setUtcMs : now.sysUtcMs;
  } else if (v.result != SubmitResult::Pending && v.result != SubmitResult::RejectedOutranked) {
    // outranked is the normal state of a lower source (GPS while NTP is
    // fresh): not a rejection worth counting
    ++_rejects;
    _lastRejectSrc = s.src;
    _lastReject = v.result;
  }
  if (v.stepClock) {
    _lastStepMs = v.offsetMs;
    ++_steps;
  }
  portEXIT_CRITICAL(&s_mux);

  if (v.stepClock) {
    // the verdict is for `now`; add what elapsed since
    setSysUtcMs(v.setUtcMs + static_cast<int64_t>(monoMs() - now.monoMs));
    _expectedBase = sysUtcMs() - static_cast<int64_t>(monoMs());
    _hasBase = true;
    log_i("[time] %s stepped the clock by %ld ms", timeSourceName(s.src), (long)v.offsetMs);
    // Stepped only for listeners that have seen the clock valid (_prevValid);
    // otherwise the step travels with the next loop's BecameValid.
    if (_prevValid && isValid()) {
      emit(TimeEvent::Stepped, v.offsetMs);
    } else {
      _pendingStepInfo = v.offsetMs;
    }
  } else if (v.result == SubmitResult::RejectedOutranked) {
    log_d("[time] %s sample outranked (offset %ld ms)", timeSourceName(s.src), (long)v.offsetMs);
  } else if (!accepted(v.result) && v.result != SubmitResult::Pending) {
    log_w("[time] %s sample rejected: %s (offset %ld ms)", timeSourceName(s.src), submitResultName(v.result),
          (long)v.offsetMs);
  }
  if (accepted(v.result)) saveHold(sysUtcMs());
  return v.result;
}

bool TimeSync::queueOperatorTime(int64_t utcMs) {
  if (!isPlausible(utcMs / 1000)) return false;
  const uint64_t mono = monoMs();
  portENTER_CRITICAL(&s_mux);
  _opUtcMs = utcMs;
  _opMono = mono;
  _opPending = true;
  portEXIT_CRITICAL(&s_mux);
  return true;
}

void TimeSync::onSntpTime(int64_t utcMs) {
  if (!_begun) {  // no framework yet: behave like the default sntp_sync_time()
    setSysUtcMs(utcMs);
    sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
    return;
  }
  const uint64_t mono = monoMs();
  portENTER_CRITICAL(&s_mux);
  _sntpUtcMs = utcMs;
  _sntpMono = mono;
  _sntpPending = true;
  portEXIT_CRITICAL(&s_mux);
}

void TimeSync::loop() {
  if (!_begun) return;
  const uint64_t mono = monoMs();
  const int64_t sys = sysUtcMs();

  // 1. Someone else stepped the clock (settimeofday outside TimeSync).
  const int64_t base = sys - static_cast<int64_t>(mono);
  const bool unexplained = _hasBase && llabs(base - _expectedBase) > kUnexplainedStepMs;
  const int32_t unexplainedMs = unexplained ? clamp32(base - _expectedBase) : 0;
  _expectedBase = base;  // before the listeners: a step they make updates it again
  _hasBase = true;
  if (unexplained) {
    log_w("[time] clock stepped by %ld ms outside TimeSync", (long)unexplainedMs);
    if (_prevValid) emit(TimeEvent::Stepped, unexplainedMs);
  }

  // 2. Samples handed over by other tasks.
  bool sntp = false, op = false;
  int64_t sntpUtc = 0, opUtc = 0;
  uint64_t sntpMono = 0, opMono = 0;
  portENTER_CRITICAL(&s_mux);
  if (_sntpPending) {
    sntp = true;
    sntpUtc = _sntpUtcMs;
    sntpMono = _sntpMono;
    _sntpPending = false;
  }
  if (_opPending) {
    op = true;
    opUtc = _opUtcMs;
    opMono = _opMono;
    _opPending = false;
  }
  portEXIT_CRITICAL(&s_mux);
  if (sntp) {
    const SubmitResult r =
        submitInternal(TimeSample{TimeSource::Ntp, sntpUtc, sntpMono, TimeArbiter::kExternalNtpAccuracyMs, 0});
    if (accepted(r)) sntp_set_sync_status(SNTP_SYNC_STATUS_COMPLETED);
  }
  if (op) {
    submitInternal(TimeSample{TimeSource::Manual, opUtc, opMono, kOperatorAccuracyMs, kTimeFlagOperator});
  }

  // 3. Edges.
  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  TimeStatus st;
  portENTER_CRITICAL(&s_mux);
  fillStatus(st, now);
  portEXIT_CRITICAL(&s_mux);
  if (st.valid != _prevValid) {
    _prevValid = st.valid;
    if (st.valid) {
      portENTER_CRITICAL(&s_mux);
      ++_clockEpoch;
      st.clockEpoch = _clockEpoch;
      portEXIT_CRITICAL(&s_mux);
      _everValid = true;
      log_i("[time] clock valid (source %s, uncertainty %u ms)", timeSourceName(st.source), (unsigned)st.uncertaintyMs);
      emitWith(TimeEvent::BecameValid, st, _pendingStepInfo);
    } else {
      log_w("[time] clock no longer valid (uncertainty %u ms)", (unsigned)st.uncertaintyMs);
      emitWith(TimeEvent::BecameInvalid, st, 0);
    }
  } else if (st.valid && _pendingStepInfo != 0) {
    emitWith(TimeEvent::Stepped, st, _pendingStepInfo);
  }
  _pendingStepInfo = 0;
  if (st.source != _prevSource) {
    _prevSource = st.source;
    emitWith(TimeEvent::SourceChanged, st, 0);
  }
  if (st.stale != _prevStale) {
    _prevStale = st.stale;
    if (st.stale) {
      log_w("[time] no NTP/GPS sync for %u h", (unsigned)(_arb.policy().staleAfterMs / 3600000UL));
      emitWith(TimeEvent::Stale, st, 0);
    }
  }
  if (st.disputed != _prevDisputed) {
    _prevDisputed = st.disputed;
    if (st.disputed) {
      log_w("[time] NTP disagrees with GPS+RTC");
      emitWith(TimeEvent::Disputed, st, 0);
    }
  }
  if (s_tzEvent.exchange(false)) emitWith(TimeEvent::TzChanged, st, 0);

  // 4. Keep the hold record's "last valid" fresh.
  if (st.valid && now.monoMs - _holdSavedMono >= kHoldRefreshMs) saveHold(now.sysUtcMs);
}

void TimeSync::emit(TimeEvent e, int32_t stepMs) {
  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  TimeStatus st;
  portENTER_CRITICAL(&s_mux);
  fillStatus(st, now);
  portEXIT_CRITICAL(&s_mux);
  emitWith(e, st, stepMs);
}

void TimeSync::emitWith(TimeEvent e, const TimeStatus& st, int32_t stepMs) {
  for (auto& l : _listeners) {
    if (!l) continue;
    const Listener f = l;  // a listener may unsubscribe (destroy its slot) while running
    f(e, st, stepMs);
  }
}

// Caller holds s_mux.
void TimeSync::fillStatus(TimeStatus& st, const TimeArbiter::Now& now) const {
  // valid also needs the system clock itself to be plausible: between an
  // accepted sample and its settimeofday() the clock still reads the old time
  st.valid = _arb.valid(now) && isPlausible(now.sysUtcMs / 1000);
  st.stale = _arb.stale(now);
  st.disputed = _arb.disputed();
  const TimeSource auth = _arb.authority(now);
  st.source = auth != TimeSource::None ? auth : _lastAccSrc;
  st.lastSyncUtcMs = _lastSyncUtcMs;
  st.lastSyncAgeS = (_lastSyncUtcMs > 0 && now.sysUtcMs >= _lastSyncUtcMs)
                        ? static_cast<uint32_t>((now.sysUtcMs - _lastSyncUtcMs) / 1000)
                        : UINT32_MAX;
  st.uncertaintyMs = _arb.uncertaintyMs(now);
  st.lastStepMs = _lastStepMs;
  st.steps = _steps;
  st.rejects = _rejects;
  st.clockEpoch = _clockEpoch;
  st.lastRejectSrc = _lastRejectSrc;
  st.lastReject = _lastReject;
  for (uint8_t i = 0; i < kTimeSourceCount; ++i) {
    st.offsetMs[i] = _arb.offsetMs(static_cast<TimeSource>(i));
    st.seenAgeS[i] = (_seenMask & (1u << i)) ? static_cast<uint32_t>(now.monoMs / 1000) - _seenS[i] : UINT32_MAX;
  }
  st.registeredMask = _registeredMask;
}

bool TimeSync::isValid() const {
  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  portENTER_CRITICAL(&s_mux);
  const bool v = _arb.valid(now);
  portEXIT_CRITICAL(&s_mux);
  return v && isPlausible(now.sysUtcMs / 1000);  // see fillStatus
}

uint32_t TimeSync::uncertaintyMs() const {
  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  portENTER_CRITICAL(&s_mux);
  const uint32_t u = _arb.uncertaintyMs(now);
  portEXIT_CRITICAL(&s_mux);
  return u;
}

TimeStatus TimeSync::status() const {
  const TimeArbiter::Now now{sysUtcMs(), monoMs()};
  TimeStatus st;
  portENTER_CRITICAL(&s_mux);
  fillStatus(st, now);
  portEXIT_CRITICAL(&s_mux);
  return st;
}

int64_t TimeSync::utcMsAtMonoMs(uint64_t m) const {
  const uint64_t mono = monoMs();
  return sysUtcMs() - (static_cast<int64_t>(mono) - static_cast<int64_t>(m));
}

bool TimeSync::clockSane() {
  TimeSync& t = instance();
  if (t.isValid()) return true;
  portENTER_CRITICAL(&s_mux);
  const uint8_t mask = t._registeredMask;
  const bool everValid = t._everValid;
  portEXIT_CRITICAL(&s_mux);
  const int64_t nowS = static_cast<int64_t>(time(nullptr));
  if (everValid && isPlausible(nowS)) return true;  // was valid this boot; the clock kept running
  return mask == 0 && nowS > minSaneUtc();
}

bool TimeSync::setTimezone(const char* posix, const char* ianaLabel) {
  if (!posix || strlen(posix) >= sizeof(_tz) || !looksLikePosixTz(posix)) {
    log_w("[time] rejected TZ '%s' (not a POSIX TZ string)", posix ? posix : "");
    return false;
  }
  const char* label = ianaLabel ? ianaLabel : "";
  xSemaphoreTake(s_tzMutex, portMAX_DELAY);
  const bool changed = strcmp(_tz, posix) != 0 || strncmp(_tzLabel, label, sizeof(_tzLabel) - 1) != 0;
  if (changed) {
    strlcpy(_tz, posix, sizeof(_tz));
    strlcpy(_tzLabel, label, sizeof(_tzLabel));
    ++_tzRev;
  }
  setenv("TZ", _tz, 1);
  tzset();
  xSemaphoreGive(s_tzMutex);
  if (changed) {
    log_i("[time] timezone %s (%s)", posix, label);
    s_tzEvent = true;
  }
  return true;
}

void TimeSync::getTimezone(char* posix, size_t posixLen, char* iana, size_t ianaLen) const {
  xSemaphoreTake(s_tzMutex, portMAX_DELAY);
  if (posix && posixLen) strlcpy(posix, _tz, posixLen);
  if (iana && ianaLen) strlcpy(iana, _tzLabel, ianaLen);
  xSemaphoreGive(s_tzMutex);
}

uint32_t TimeSync::tzRevision() const {
  xSemaphoreTake(s_tzMutex, portMAX_DELAY);
  const uint32_t r = _tzRev;
  xSemaphoreGive(s_tzMutex);
  return r;
}

int8_t TimeSync::subscribe(Listener l) {
  if (!l) return -1;
  for (int8_t i = 0; i < ESPRACK_TIMESYNC_MAX_LISTENERS; ++i) {
    if (!_listeners[i]) {
      _listeners[i] = std::move(l);
      return i;
    }
  }
  log_e("[time] no free listener slot (ESPRACK_TIMESYNC_MAX_LISTENERS=%d)", ESPRACK_TIMESYNC_MAX_LISTENERS);
  return -1;
}

void TimeSync::unsubscribe(int8_t id) {
  if (id >= 0 && id < ESPRACK_TIMESYNC_MAX_LISTENERS) _listeners[id] = nullptr;
}

void TimeSync::setPolicy(const TimeArbiterPolicy& p) {
  const TimeArbiterPolicy q = withSaneFloor(p);
  portENTER_CRITICAL(&s_mux);
  _arb.setPolicy(q);
  portEXIT_CRITICAL(&s_mux);
}

void TimeSync::registerManifest(WebManager* web) {
  if (!web) return;
  WebActionSpec spec;
  spec.id = "system.setTime";
  spec.title = "Set time";
  spec.icon = "AccessTime";
  spec.color = "primary";
  spec.method = "POST";
  spec.auth = WebAuthLevel::Admin;
  spec.confirm = "Set the device clock to the selected time?";
  // The reply comes before the arbiter's verdict (next loop()); the NTP tab
  // shows the result. NTP/GPS outrank the operator.
  spec.successMessage = "Time submitted";
  spec.handler = [](AsyncWebServerRequest* r) {
    int64_t epoch = 0, ms = 0, utcMs = 0;
    const bool hasEpoch = r->hasArg("epoch"), hasMs = r->hasArg("ms");
    const bool epochOk = hasEpoch && parseInt64(r->arg("epoch").c_str(), epoch);
    const bool msOk = hasMs && parseInt64(r->arg("ms").c_str(), ms);
    const char* err = validateOperatorTime(hasEpoch, epochOk, epoch, hasMs, msOk, ms, utcMs);
    if (err) {
      sendProblem(r, 400, err, "epoch = UTC seconds 2026..2099, ms = 0..999 (optional)");
      return;
    }
    TimeSync::instance().queueOperatorTime(utcMs);
    r->send(200, "application/json", "{\"queued\":true}");
  };
  web->registerAction(std::move(spec));
}

}  // namespace ESPRack

// lwIP's SNTP client calls this with the server time instead of stepping the
// clock itself (strong definition of IDF's weak default; the Arduino core
// doesn't provide one). TimeSync decides on the next loop().
extern "C" void sntp_sync_time(struct timeval* tv) {
  if (!tv) return;
  ESPRack::TimeSync::instance().onSntpTime(static_cast<int64_t>(tv->tv_sec) * 1000 + tv->tv_usec / 1000);
}
