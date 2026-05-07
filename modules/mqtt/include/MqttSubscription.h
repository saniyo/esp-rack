#pragma once
#ifndef ESPRACK_MQTT_SUBSCRIPTION_H
#define ESPRACK_MQTT_SUBSCRIPTION_H

// Lightweight handle. Mirrors TelegramSubscription's semantics —
// id + back-pointer to the provider, all real state lives in the
// MQTT service registry keyed by id.

#include "MqttTypes.h"

class IMqttProvider;

class MqttSubscription {
 public:
  MqttSubscription() = default;
  MqttSubscription(uint32_t id, IMqttProvider* provider)
      : id_(id), provider_(provider) {}

  bool valid() const { return id_ != 0 && provider_ != nullptr; }

  // Outbound — publish to `defaultTopicPrefix + relativeTopic` with
  // subscription default QoS / retain. Returns SendId for tracing,
  // InvalidMessagingSendId on quota / mute / disconnected.
  ESPRack::MessagingSendId publish(const String& relativeTopic,
                                    const String& payload);

  // Outbound with explicit per-call options (overrides cfg defaults).
  ESPRack::MessagingSendId publishTo(const String& relativeTopic,
                                      const String& payload,
                                      const MqttPublishOptions& opt);

  // Inbound — register a handler for messages on
  // `defaultTopicPrefix + relativeTopic`. Wildcards (+, #) are honored
  // per MQTT spec. Caller can register multiple handlers on different
  // topics through the same subscription. Each handler is dispatched
  // for matching messages; non-matching delivered messages are
  // ignored by this subscription.
  //
  // The provider also tells the broker to subscribe to this topic so
  // delivery happens — duplicate broker-side subscriptions are
  // de-duped by AsyncMqttClient.
  void onMessage(const String& relativeTopic,
                 MqttSubscriptionMessageHandler handler);

  // Composite "ready to actually deliver" gate. Returns true only
  // when handle valid, mute flag on, and broker not Disabled.
  // Consumers gate cyclic publishes on this so they don't spam the
  // log with skip lines while broker is intentionally off.
  bool active() const;

  // Telemetry snapshot.
  MqttSubscriptionStats stats() const;

  bool enabled() const;
  void setEnabled(bool on);

  void unsubscribe();

  const char* serviceName() const;

 private:
  uint32_t        id_{0};
  IMqttProvider*  provider_{nullptr};
};

#endif  // ESPRACK_MQTT_SUBSCRIPTION_H
