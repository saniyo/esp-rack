#pragma once
#ifndef ESPRACK_TIME_SYNC_H
#define ESPRACK_TIME_SYNC_H

// TimeSync — owns the wall clock (SES-LIB-002).
//
// Every time source (NTP, GPS, an external RTC, the operator) offers
// samples; TimeArbiter (time/TimeArbiter.h) decides whether one may step the clock
// and whether the clock is valid. TimeSync applies the verdicts, tracks
// validity, emits events, owns the libc TZ, and survives soft resets through
// an RTC_NOINIT hold record.
//
//   * SNTP: TimeSync provides the strong sntp_sync_time(). lwIP hands it the
//     server time instead of stepping the clock; loop() submits it as an Ntp
//     sample, so an implausible NTP time never reaches the clock. A disputed
//     one (NTP far from GPS+RTC) is still applied — NTP ranks first —
//     and reported through disputed() / TimeEvent::Disputed.
//   * Other sources call submit() from the loop task (a module's onLoop()).
//   * The operator sets the time with the admin action system.setTime
//     (?epoch=<s>[&ms=<ms>]); it outranks ExtRtc and RtcHold, never GPS/NTP.
//   * clockSane() is THE "can I trust time(nullptr)?" check for JWT exp,
//     TLS X.509 time and certificate expiry.
//
// Threading: readers (isValid, status, clockSane, utcMsAtMonoMs, getTimezone)
// are safe from any task. submit(), begin(), loop() run on the loop task;
// listeners are called only from loop().

#include <stddef.h>
#include <stdint.h>

#include <functional>

#include "time/TimeArbiter.h"

#ifndef ESPRACK_MIN_SANE_EPOCH
#define ESPRACK_MIN_SANE_EPOCH 1767225600LL  // 2026-01-01T00:00:00Z
#endif
#ifndef ESPRACK_DEFAULT_TZ
#define ESPRACK_DEFAULT_TZ "UTC0"
#endif
#ifndef ESPRACK_DEFAULT_TZ_LABEL
#define ESPRACK_DEFAULT_TZ_LABEL "Etc/UTC"
#endif
#ifndef ESPRACK_TIMESYNC_MAX_LISTENERS
#define ESPRACK_TIMESYNC_MAX_LISTENERS 6  // App uses one (manifest re-bake on TzChanged)
#endif

class WebManager;

namespace ESPRack {

enum class TimeEvent : uint8_t { BecameValid, BecameInvalid, Stepped, SourceChanged, Stale, Disputed, TzChanged };

// A clock step in ms, as listeners get it (SES-LIB-027). 64-bit: a wrong
// date a month or a year off is corrected in one step. A struct, so a
// listener still declared with `int32_t stepMs` fails to compile instead of
// silently wrapping a step longer than 24.8 days.
struct TimeStep {
  int64_t ms = 0;
};

struct TimeStatus {
  bool valid, stale, disputed;
  TimeSource source;          // the authority, else the source of the last accepted sample
  int64_t lastSyncUtcMs;      // UTC of the last accepted sample (0 = none)
  uint32_t lastSyncAgeS, uncertaintyMs;  // UINT32_MAX = never / unknown
  int64_t lastStepMs;
  uint16_t steps, rejects;
  uint8_t clockEpoch;         // +1 on every BecameValid
  TimeSource lastRejectSrc;
  SubmitResult lastReject;
  int64_t offsetMs[kTimeSourceCount];
  uint32_t seenAgeS[kTimeSourceCount];  // since the last sample of any verdict; UINT32_MAX = never
  uint8_t registeredMask;     // bit = 1 << TimeSource
};

class TimeSync {
 public:
  static TimeSync& instance();

  void begin();  // App::begin, first thing (loop task)
  void loop();   // App::loop, first thing

  void registerSource(TimeSource s);
  // Loop task only. Stamp TimeSample::sampleMonoMs with TimeSync::monoMs()
  // (never millis(): it wraps after 49.7 days). A sample whose stamp is in
  // the future or more than 60 s old is rejected as implausible.
  SubmitResult submit(const TimeSample& s);
  // Monotonic milliseconds since boot (esp_timer, 64-bit, never wraps).
  static uint64_t monoMs();
  // Operator set (system.setTime): any task; submitted on the next loop().
  bool queueOperatorTime(int64_t utcMs);
  void registerManifest(WebManager* web);    // mounts system.setTime

  bool isValid() const;
  TimeStatus status() const;
  uint32_t uncertaintyMs() const;
  int64_t utcMsAtMonoMs(uint64_t monoMs) const;  // the current clock's UTC at that mono ms

  static int64_t minSaneUtc() { return ESPRACK_MIN_SANE_EPOCH; }
  static bool isPlausible(int64_t utcS) {
    return utcS >= ESPRACK_MIN_SANE_EPOCH && utcS < TimeArbiter::kMaxSaneUtcS;
  }
  // "Can time(nullptr) be trusted?" — valid now; or valid earlier this boot
  // (the clock keeps running, so a lapse of the 10-min uncertainty bound
  // doesn't revert to 1970 semantics); or no time source is registered and
  // the clock is past ESPRACK_MIN_SANE_EPOCH. Read time(nullptr) AFTER this
  // check, not before.
  static bool clockSane();

  // false: not shaped like a POSIX TZ string (name, offset, optional rules)
  // or too long. The check is syntactic; libc decides the meaning.
  bool setTimezone(const char* posix, const char* ianaLabel);
  void getTimezone(char* posix, size_t posixLen, char* iana, size_t ianaLen) const;
  uint32_t tzRevision() const;

  // step: the Stepped amount (also with BecameValid when the clock was
  // stepped while invalid), else 0.
  using Listener = std::function<void(TimeEvent, const TimeStatus&, TimeStep step)>;
  // Loop task only (e.g. in onBegin); a listener may unsubscribe itself.
  int8_t subscribe(Listener l);  // -1 when full
  void unsubscribe(int8_t id);
  // minSaneUtcS is always ESPRACK_MIN_SANE_EPOCH (the one plausibility floor).
  void setPolicy(const TimeArbiterPolicy& p);

  // lwIP's SNTP result (tcpip thread) — called by sntp_sync_time().
  void onSntpTime(int64_t utcMs);
  // Refresh the RTC hold record now (App calls it on shutdown).
  void flushHold();

 private:
  TimeSync();
  TimeSync(const TimeSync&) = delete;
  TimeSync& operator=(const TimeSync&) = delete;

  SubmitResult submitInternal(const TimeSample& s);
  void emit(TimeEvent e, int64_t stepMs);
  void emitWith(TimeEvent e, const TimeStatus& st, int64_t stepMs);
  void fillStatus(TimeStatus& st, const TimeArbiter::Now& now) const;  // caller holds the lock
  void saveHold(int64_t nowUtcMs);
  void restoreHoldAtBoot(const TimeArbiter::Now& now);

  TimeArbiter _arb;
  void* _loopTask = nullptr;
  bool _begun = false;

  // pending samples from other tasks (under the mux)
  bool _sntpPending = false, _opPending = false;
  int64_t _sntpUtcMs = 0, _opUtcMs = 0;
  uint64_t _sntpMono = 0, _opMono = 0;

  // bookkeeping (under the mux for cross-task readers)
  TimeSource _lastAccSrc = TimeSource::None;  // RtcHold after a restored hold
  TimeSource _holdSrc = TimeSource::None;     // what originally synced the held clock
  uint32_t _lastAccMs = 0;
  int64_t _lastSyncUtcMs = 0;
  int64_t _lastStepMs = 0;
  uint16_t _steps = 0, _rejects = 0;
  uint8_t _clockEpoch = 0;
  TimeSource _lastRejectSrc = TimeSource::None;
  SubmitResult _lastReject = SubmitResult::Applied;
  uint32_t _seenS[kTimeSourceCount] = {};  // mono seconds of the last sample per source
  uint8_t _seenMask = 0;
  uint8_t _registeredMask = 0;

  // loop-only edge detection (stale starts true: no Stale event at boot)
  bool _prevValid = false, _prevStale = true, _prevDisputed = false;
  bool _everValid = false;  // valid at least once this boot (clockSane)
  TimeSource _prevSource = TimeSource::None;
  int64_t _pendingStepInfo = 0;  // step reported with the next BecameValid
  bool _hasBase = false;
  int64_t _expectedBase = 0;  // sysUtcMs - monoMs after our last step
  uint64_t _holdSavedMono = 0;

  // TZ
  char _tz[48];       // longest IANA footer is ~36 chars
  char _tzLabel[40];  // longest IANA name is 32
  uint32_t _tzRev = 0;

  Listener _listeners[ESPRACK_TIMESYNC_MAX_LISTENERS];
};

const char* timeSourceName(TimeSource s);
const char* submitResultName(SubmitResult r);

}  // namespace ESPRack

#endif  // ESPRACK_TIME_SYNC_H
