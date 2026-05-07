#pragma once
#ifndef ESPRACK_IMESSAGING_PROVIDER_H
#define ESPRACK_IMESSAGING_PROVIDER_H

// Bare-minimum cross-provider contract: anything that publishes to
// some outbound channel and accepts named subscriptions exposes this
// interface. Today's Telegram refactor is the first implementer; the
// MQTT module will adopt the same shape in a follow-up iteration so
// consumer code that fans out to "every available messenger" can
// iterate IMessagingProvider* uniformly.
//
// Subscription registration intentionally LIVES IN THE SUBCLASS —
// each transport's subscription type is parameterized differently
// (Telegram needs default chat/topic, MQTT needs default topic +
// QoS, etc.). The shared base only exposes telemetry: connection
// health + how many subscriptions are currently active.

#include "MessagingTypes.h"

namespace ESPRack {

class IMessagingProvider {
 public:
  virtual ~IMessagingProvider() = default;

  // Stable ID for UI / logging. Matches the Module::describe().id of
  // the wrapping module ("telegram", "mqtt").
  virtual const char* providerId() const = 0;

  // Current outbound-channel health.
  virtual MessagingConnectionState connectionState() const = 0;

  // Active subscription count — for cross-provider dashboards that
  // show "WiFi service has 1 telegram + 1 mqtt subscription".
  virtual size_t subscriberCount() const = 0;
};

}  // namespace ESPRack

#endif  // ESPRACK_IMESSAGING_PROVIDER_H
