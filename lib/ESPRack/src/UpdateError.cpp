#include "UpdateError.h"

#include <ProblemJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

namespace ESPRack {

namespace {
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
TaskHandle_t s_owner = nullptr;
char s_reason[96];
}  // namespace

void setUpdateError(const char* reason) {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_mux);
  s_owner = me;
  strlcpy(s_reason, reason ? reason : "", sizeof(s_reason));
  portEXIT_CRITICAL(&s_mux);
}

void clearUpdateError() {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  portENTER_CRITICAL(&s_mux);
  if (s_owner == me) s_owner = nullptr;
  portEXIT_CRITICAL(&s_mux);
}

bool takeUpdateError(char* buf, size_t len) {
  TaskHandle_t me = xTaskGetCurrentTaskHandle();
  bool mine = false;
  portENTER_CRITICAL(&s_mux);
  if (s_owner == me && s_reason[0]) {
    mine = true;
    if (buf && len) strlcpy(buf, s_reason, len);
  }
  if (s_owner == me) s_owner = nullptr;
  portEXIT_CRITICAL(&s_mux);
  if (!mine && buf && len) buf[0] = '\0';
  return mine;
}

void sendUpdateRejected(AsyncWebServerRequest* request) {
  char why[sizeof(s_reason)];
  if (!takeUpdateError(why, sizeof(why))) {
    sendProblem(request, 400, "update_rejected", "The update was rejected");
    return;
  }
  // sendProblem doesn't escape: keep the reason JSON-safe.
  for (char* p = why; *p; ++p) {
    if (*p == '"' || *p == '\\') *p = '\'';
    else if ((unsigned char)*p < 0x20) *p = ' ';
  }
  sendProblem(request, 400, "update_rejected", why);
}

void writeUpdateRejected(JsonVariant out) {
  char why[sizeof(s_reason)];
  JsonObject err = out.to<JsonObject>();
  err["error"] = "update_rejected";
  if (takeUpdateError(why, sizeof(why))) err["detail"] = why;  // copied
}

}  // namespace ESPRack
