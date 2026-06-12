#ifndef ProblemJson_h
#define ProblemJson_h

// RFC 9457 "Problem Details for HTTP APIs" — the ONE error body shape for
// every 4xx/5xx the device emits (api-standardization plan, phase C0).
//
//   { "status": 503, "title": "heap_pressure",
//     "detail": "…", "retryAfter": 2 }
//
// Contract:
//   * `title` is a short stable machine-readable slug (snake_case) —
//     clients branch on it; `detail` is the human sentence and may be
//     omitted. Both MUST be JSON-safe literals (no quotes/backslashes/
//     control chars): the helper does NOT escape, by design — the error
//     path must not allocate, because errors fire exactly when the heap
//     is at its worst. Everything is composed into one stack buffer.
//   * `retryAfter` (seconds) marks the error as transient; the web UI's
//     axios layer auto-retries any 503 carrying it.
//   * `type` is intentionally omitted → defaults to "about:blank" per
//     RFC 9457; we don't run an error-catalog URL space on-device.
//
// Content-Type is application/problem+json so generated clients (and the
// external server) can dispatch on it uniformly.

#include <ESPAsyncWebServer.h>
#include <stdio.h>

inline void sendProblem(AsyncWebServerRequest* request,
                        int status,
                        const char* title,
                        const char* detail = nullptr,
                        int retryAfterSec = -1) {
  char body[256];
  int n;
  if (detail && retryAfterSec >= 0) {
    n = snprintf(body, sizeof(body),
                 "{\"status\":%d,\"title\":\"%s\",\"detail\":\"%s\",\"retryAfter\":%d}",
                 status, title, detail, retryAfterSec);
  } else if (detail) {
    n = snprintf(body, sizeof(body),
                 "{\"status\":%d,\"title\":\"%s\",\"detail\":\"%s\"}",
                 status, title, detail);
  } else if (retryAfterSec >= 0) {
    n = snprintf(body, sizeof(body),
                 "{\"status\":%d,\"title\":\"%s\",\"retryAfter\":%d}",
                 status, title, retryAfterSec);
  } else {
    n = snprintf(body, sizeof(body),
                 "{\"status\":%d,\"title\":\"%s\"}", status, title);
  }
  if (n < 0 || n >= (int)sizeof(body)) {
    // Composed body would not fit — ship a minimal valid problem rather
    // than a truncated one. (Only possible with an oversized detail.)
    snprintf(body, sizeof(body), "{\"status\":%d,\"title\":\"%s\"}",
             status, title);
  }
  AsyncWebServerResponse* res =
      request->beginResponse(status, "application/problem+json", body);
  if (retryAfterSec >= 0) {
    char ra[12];
    snprintf(ra, sizeof(ra), "%d", retryAfterSec);
    res->addHeader("Retry-After", ra);  // HTTP-level mirror of the field
  }
  request->send(res);
}

#endif  // ProblemJson_h
