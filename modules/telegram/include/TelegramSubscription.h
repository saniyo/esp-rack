#pragma once
#ifndef ESPRACK_TELEGRAM_SUBSCRIPTION_H
#define ESPRACK_TELEGRAM_SUBSCRIPTION_H

// Lightweight handle — value-type, copyable, cheap. Holds an id +
// back-pointer to the provider; all per-subscription state (counters,
// config, mute flag) lives in TelegramService's registry keyed by id.
//
// Lifetime: the handle stays valid as long as TelegramService stays
// alive — typical for App-lifetime singletons. If somebody destroys
// the service before consumers, sends through stale handles are no-ops
// (provider pointer guarded internally; lookup returns "not found"
// and the handle's send() returns InvalidMessagingSendId).
//
// Why not unique_ptr / shared_ptr?
//   * Value-type handles compose naturally with ConfigDelegate-style
//     fields where a service caches `_telegramSub` as a member.
//   * Embedded RAM budget — no heap traffic per subscription.
//   * Unsubscribe is explicit (.unsubscribe()) rather than RAII; for
//     long-lived services that's the more predictable lifecycle.

#include "TelegramTypes.h"

class ITelegramProvider;  // forward — full definition in ITelegramProvider.h

class TelegramSubscription {
 public:
  TelegramSubscription() = default;
  TelegramSubscription(uint32_t id, ITelegramProvider* provider)
      : id_(id), provider_(provider) {}

  // Empty handle: no subscription claimed yet, sends are no-ops.
  bool valid() const { return id_ != 0 && provider_ != nullptr; }

  // Send to the subscription's configured default destination. Returns
  // a SendId for correlation; InvalidMessagingSendId if quota exceeded
  // / queue full / bot disabled / handle invalid.
  ESPRack::MessagingSendId send(const String& text);

  // Send to an explicit destination — overrides the subscription's
  // defaults for THIS message only (e.g. WiFi service usually posts
  // to topic 12 but wants to escalate a critical alert into the
  // pinned admin chat).
  ESPRack::MessagingSendId sendTo(const TelegramRecipient& to,
                                  const String& text,
                                  const TelegramSendOptions& opt = {});

  // Per-subscription telemetry snapshot (sent / errors / dropped /
  // lastSentAt). Refreshed each call from the provider's registry.
  TelegramSubscriptionStats stats() const;

  // Mute toggle. When false, send() / sendTo() return InvalidMessagingSendId
  // and increment dropped count. Persisted-to-disk muting may be added
  // in a later iteration; for now the flag is RAM-only and resets on
  // boot.
  bool enabled() const;
  void setEnabled(bool on);

  // Release the slot in the provider's registry. Idempotent. After
  // calling, valid() returns false.
  void unsubscribe();

  // Read-back of the registered service name (set at subscribe time).
  // Used by UI to label the subscriptions table row.
  const char* serviceName() const;

 private:
  uint32_t id_{0};
  ITelegramProvider* provider_{nullptr};
};

#endif  // ESPRACK_TELEGRAM_SUBSCRIPTION_H
