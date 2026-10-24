#ifndef AdaptiveJson_h
#define AdaptiveJson_h

// sendAdaptiveJson — THE response path for any handler that emits a JSON
// document on the AsyncWebServer (async_tcp) task.
//
// Policy (per the "no oversized buffers" rule): build the tree into the
// SMALLEST doc that holds it — start at the caller's measured size, and
// if the tree overflows it, grow x2 and rebuild, up to the live heap
// ceiling. The serialized bytes go out wire-chunked by AsyncJsonResponse
// (ChunkPrint streams per TCP ack), so the only thing held contiguously
// is the ArduinoJson tree itself — never a hand-guessed worst-case slab.
//
// Why this matters on the no-PSRAM C3: the doc size doubles as the
// heap-pressure gate. An 8 KB constant for a 700 B form made the gate
// demand ~9 KB of contiguous heap and 503'd the tab under any concurrency.
// Sizing to the real payload keeps the gate reachable; the grow path
// covers the rare case where reality exceeds the measured hint.
//
//   sendAdaptiveJson(request, [this](JsonObject& o){ buildForm(o); }, 1024);
//
// `fill` MUST be safe to invoke more than once — it is re-run once per
// size attempt. State reads / registry walks / FS enumerations are all
// fine; anything with side effects is not.
//
// Returns true once a 2xx response is queued; false after shedding a
// retryable 503 problem+json (the UI's axios layer auto-retries it).

#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ProblemJson.h>
#include <esp_heap_caps.h>
#include <functional>

// The largest doc of `size` bytes the heap can give right now. With PSRAM
// and the malloc policy that sends blocks above ALWAYSINTERNAL there, a big
// doc is limited by the PSRAM block, not the internal one (gating on the
// internal block 503'd S3 pages that PSRAM could serve).
inline size_t adaptiveJsonCeiling(size_t size) {
#if defined(CONFIG_SPIRAM_USE_MALLOC) && defined(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL)
  if (size > CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL && ESP.getPsramSize() > 0) {
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
#endif
  return ESP.getMaxAllocHeap();
}

inline bool sendAdaptiveJson(AsyncWebServerRequest* request,
                             const std::function<void(JsonObject&)>& fill,
                             size_t startBytes = 1024) {
  // Headroom kept FREE above the doc so the response head String + TCP
  // bookkeeping never compete with the tree for the same block.
  static constexpr size_t kSlack    = 1024;
  // Smallest doc we will allocate — a floor for tiny payloads, never a
  // rejection of legitimately-small forms.
  static constexpr size_t kMinDoc   = 512;
  static constexpr int    kAttempts = 5;

  size_t want = startBytes < kMinDoc ? kMinDoc : startBytes;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    const size_t ceiling = adaptiveJsonCeiling(want);
    if (ceiling < kMinDoc + kSlack) break;  // genuinely too tight — shed
    size_t size = want;
    // Under pressure clamp DOWN to what fits; the check above guarantees
    // the clamped size stays >= kMinDoc.
    if (size + kSlack > ceiling) size = ceiling - kSlack;

    AsyncJsonResponse* response = new AsyncJsonResponse(false, size);
    JsonObject root = response->getRoot().to<JsonObject>();
    if (root.isNull()) {  // doc alloc lost the race despite the clamp
      delete response;
      break;
    }
    fill(root);
    if (!response->overflowed()) {
      response->setLength();
      request->send(response);
      return true;
    }
    // Tree didn't fit — grow and rebuild. Grow toward the ceiling, but do
    // NOT bail just because the power-of-2 step overshoots: use the
    // LARGEST doc the heap can actually serve (ceiling - slack). Without
    // this, a form whose pool need sits between size and 2*size — e.g.
    // the endpoints registry table: ~5.8 KB serialized but ~9 KB of
    // ArduinoJson pool — 503'd even with 13 KB free, because 2*6144
    // overshot the ceiling by a few bytes. Only give up when we're
    // already at the ceiling (no further progress possible).
    delete response;
    const size_t maxDoc = (ceiling > kSlack) ? (ceiling - kSlack) : 0;
    if (size >= maxDoc) break;             // already maxed for this heap
    size_t next = size * 2;
    if (next > maxDoc) next = maxDoc;      // cap to what fits, don't bail
    log_w("[adaptive.json] tree overflowed %u B doc — retrying at %u B",
          (unsigned)size, (unsigned)next);
    want = next;
  }
  sendProblem(request, 503, "heap_pressure",
              "No contiguous heap for the response right now", 1);
  return false;
}

#endif  // AdaptiveJson_h
