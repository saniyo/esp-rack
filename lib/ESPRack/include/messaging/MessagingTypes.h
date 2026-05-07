#pragma once
#ifndef ESPRACK_MESSAGING_TYPES_H
#define ESPRACK_MESSAGING_TYPES_H

// Cross-provider messaging primitives. Both MQTT and Telegram modules
// share the same conceptual shape — a transport with connection state,
// outbound queue, per-consumer subscriptions, and audit. The shared
// types here capture only what's truly common; transport-specific
// details (Telegram parse modes, MQTT QoS, etc.) stay in their
// respective provider headers.

#include <Arduino.h>
#include <stdint.h>

namespace ESPRack {

// Generic transport health. Consumers can render a uniform "is the
// outbound channel healthy?" indicator without knowing whether they're
// looking at an MQTT broker or a Telegram bot.
enum class MessagingConnectionState : uint8_t {
  Disabled,        // toggled off in settings
  Idle,            // configured but no recent activity
  Connecting,      // handshake / TLS / first send in flight
  Online,          // last operation succeeded
  Reconnecting,    // last attempt failed; retry scheduled
  Error,           // unrecoverable without operator intervention
};

inline const char* toString(MessagingConnectionState s) {
  switch (s) {
    case MessagingConnectionState::Disabled:     return "Disabled";
    case MessagingConnectionState::Idle:         return "Idle";
    case MessagingConnectionState::Connecting:   return "Connecting";
    case MessagingConnectionState::Online:       return "Online";
    case MessagingConnectionState::Reconnecting: return "Reconnecting";
    case MessagingConnectionState::Error:        return "Error";
  }
  return "?";
}

// Send-attempt identity. Returned by every outbound API so callers
// can correlate later with success/failure callbacks. Zero is reserved
// for "invalid / send was not accepted" (e.g. quota exceeded, queue
// full, transport disabled). Wraps to 1 instead of 0 on overflow so
// no valid id is ever zero.
using MessagingSendId = uint32_t;
inline constexpr MessagingSendId InvalidMessagingSendId = 0;

}  // namespace ESPRack

#endif  // ESPRACK_MESSAGING_TYPES_H
