#pragma once
#ifndef ESPRACK_MQTT_TYPES_H
#define ESPRACK_MQTT_TYPES_H

#include <Arduino.h>
#include <functional>
#include <messaging/MessagingTypes.h>

// Per-publish options. -1 / unset = "fall back to subscription default
// → broker default", same precedence as Telegram's recipient/options.
struct MqttPublishOptions {
  int  qos{-1};       // -1 use defaults; else 0 / 1 / 2
  int  retain{-1};    // -1 use defaults; else 0 (false) / 1 (true)
};

// Per-subscription configuration. Each consuming service hands one
// to MqttSettingsService::subscribe() — the provider keeps a record
// (counters, mute flag, last-published timestamp) and routes the
// consumer's publish/subscribeTopic calls through these defaults.
struct MqttSubscriptionConfig {
  // Concatenated with the relativeTopic argument of publish()/
  // subscribeTopic() to form the full MQTT topic. Empty = no prefix
  // (consumer always uses absolute topics). The primary mechanism
  // for routing different services into different parts of the topic
  // tree: WiFi service prefix "home/devices/wifi/", Sensor service
  // prefix "home/sensors/temperature/" — services don't have to know
  // each other's namespace.
  //
  // Trailing '/' is recommended but not required — the provider will
  // join with relativeTopic verbatim, so "home/wifi" + "status" →
  // "home/wifistatus" (probably wrong); pass "home/wifi/" for clean
  // joins.
  String   defaultTopicPrefix;

  // QoS for outbound publishes when the publish() call doesn't
  // specify. 0 = at most once (fire and forget), 1 = at least once
  // (broker acks), 2 = exactly once (handshake — slowest, rarely
  // needed for sensor data).
  uint8_t  defaultQos{0};

  // Retain flag for outbound publishes when the publish() call
  // doesn't specify. true = broker keeps the latest message and
  // sends it to new subscribers immediately. Useful for "current
  // state" topics (status, config snapshot); never set for event
  // streams (logs, metrics).
  bool     defaultRetain{false};

  // Soft per-subscription rate limit on the publish path. After
  // this many publishes inside a one-minute sliding window, further
  // calls return InvalidMessagingSendId and increment dropped count.
  // 0 = unlimited (still subject to AsyncMqttClient's own internal
  // backpressure if any).
  uint32_t maxPublishesPerMinute{0};
};

// Inbound message handler signature. `topic` is the FULL topic the
// broker delivered (already includes any prefix that was on the
// subscribeTopic() call). `payload` is the byte content as String —
// for binary payloads with embedded NULs, prefer raw access through
// the legacy IMqttDispatcher::addMessageCallback path until the
// subscription model grows a binary handler variant.
using MqttSubscriptionMessageHandler =
    std::function<void(const String& topic, const String& payload)>;

// Public per-subscription telemetry — surfaced in the MQTT UI's
// Subscriptions tab so the operator can see which services are
// publishing / receiving / dropping.
struct MqttSubscriptionStats {
  uint32_t published{0};       // outbound publishes successfully accepted by AsyncMqttClient
  uint32_t publishErrors{0};   // outbound publishes the client refused (queue full / disconnected)
  uint32_t publishDropped{0};  // outbound publishes blocked client-side (quota / mute / handle invalid)
  uint32_t received{0};        // inbound messages dispatched to this subscription's handlers
  uint32_t lastPublishAt_s{0}; // seconds since boot (0 = never)
  uint32_t lastReceiveAt_s{0};
};

// MQTT topic-pattern matcher. Returns true if `topic` matches
// `pattern` per MQTT v3.1.1 §4.7 wildcard rules:
//   * '+' matches exactly one topic level (no slashes)
//   * '#' matches zero or more remaining levels and MUST be the
//     last character of the pattern
//   * exact string match otherwise
// Used by the dispatcher to decide which subscriber handlers fire
// for an incoming message — the broker may deliver under a
// wildcard subscription, so we re-match locally to feed the right
// handler.
bool mqttTopicMatches(const String& pattern, const String& topic);

#endif  // ESPRACK_MQTT_TYPES_H
