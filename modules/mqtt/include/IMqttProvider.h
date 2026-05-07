#pragma once
#ifndef ESPRACK_IMQTT_PROVIDER_H
#define ESPRACK_IMQTT_PROVIDER_H

// Public contract for MqttSettingsService. Mirrors ITelegramProvider —
// subscriber-typed handle, idempotent registry, internal API delegated
// from the handle. Consumers hold ITelegramProvider* / IMqttProvider*,
// implementations evolve without rippling.

#include <messaging/IMessagingProvider.h>
#include "MqttTypes.h"
#include "MqttSubscription.h"

class IMqttProvider : public ESPRack::IMessagingProvider {
 public:
  ~IMqttProvider() override = default;

  // Claim a subscription slot. The serviceName drives the UI display
  // and (today) is the implicit logging tag. Idempotent by name —
  // re-subscribing keeps stats but updates cfg fields, so a service
  // can adjust its prefix / QoS / rate limit between firmware
  // versions without losing accumulated counters.
  virtual MqttSubscription subscribe(
      const char* serviceName,
      const MqttSubscriptionConfig& cfg = {}) = 0;

  // ── Internal API used by MqttSubscription handles. ───────────────
  //   Public on the interface because the handle is a non-friend
  //   value type, but consumers should NEVER call directly — use
  //   the handle methods which delegate here.
  virtual ESPRack::MessagingSendId
      doPublish(uint32_t subId,
                const String& relativeTopic,
                const String& payload,
                const MqttPublishOptions& opt) = 0;

  virtual void doSubscribeTopic(uint32_t subId,
                                  const String& relativeTopic,
                                  MqttSubscriptionMessageHandler handler) = 0;

  virtual MqttSubscriptionStats statsForSubscription(uint32_t subId) const = 0;
  virtual bool subscriptionEnabled(uint32_t subId) const = 0;
  virtual void setSubscriptionEnabled(uint32_t subId, bool on) = 0;
  virtual void releaseSubscription(uint32_t subId) = 0;
  virtual const char* subscriptionName(uint32_t subId) const = 0;

  // Operator-curated subscription list — populated by the Attach
  // action in the MQTT tab's Status form. Persists across reboots.
  // Consumer services pull this to render "what the operator wants
  // me to watch" plus their own captured values; example use in
  // LightStateService.cpp's "Attached values" table.
  virtual const std::vector<String>& attachedTopics() const = 0;
};

#endif  // ESPRACK_IMQTT_PROVIDER_H
