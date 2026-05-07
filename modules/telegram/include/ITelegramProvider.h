#pragma once
#ifndef ESPRACK_ITELEGRAM_PROVIDER_H
#define ESPRACK_ITELEGRAM_PROVIDER_H

// Public contract for TelegramService. Other modules' services hold
// `ITelegramProvider*` (typed, never the concrete TelegramService) so
// the implementation can evolve without rippling through every
// consumer that wanted to send messages.

#include <messaging/IMessagingProvider.h>
#include "TelegramTypes.h"
#include "TelegramSubscription.h"

class ITelegramProvider : public ESPRack::IMessagingProvider {
 public:
  ~ITelegramProvider() override = default;

  // Claim a subscription slot. The serviceName is used for UI display
  // ("Subscriptions" tab) AND as the implicit tag prefix when
  // cfg.tagPrefix is empty (so a service named "wifi" without an
  // explicit prefix gets "[wifi]" appended automatically). Idempotent
  // by name: re-subscribing with the same name returns a handle to
  // the existing record after merging cfg fields (so a service can
  // update its rate limit on reboot without losing accumulated stats).
  virtual TelegramSubscription subscribe(
      const char* serviceName,
      const TelegramSubscriptionConfig& cfg = {}) = 0;

  // ── Internal API used by TelegramSubscription handles. ───────────
  //   These are public on the interface because the handle is a
  //   non-friend value type, but consumers should NEVER call them
  //   directly — use the handle methods which delegate here.
  virtual ESPRack::MessagingSendId
      doSend(uint32_t subId,
             const TelegramRecipient& to,
             const String& text,
             const TelegramSendOptions& opt) = 0;

  virtual TelegramSubscriptionStats statsForSubscription(uint32_t subId) const = 0;
  virtual bool subscriptionEnabled(uint32_t subId) const = 0;
  virtual void setSubscriptionEnabled(uint32_t subId, bool on) = 0;
  virtual void releaseSubscription(uint32_t subId) = 0;
  virtual const char* subscriptionName(uint32_t subId) const = 0;
};

#endif  // ESPRACK_ITELEGRAM_PROVIDER_H
