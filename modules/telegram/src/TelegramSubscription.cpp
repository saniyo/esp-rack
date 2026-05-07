#include "TelegramSubscription.h"
#include "ITelegramProvider.h"

ESPRack::MessagingSendId TelegramSubscription::send(const String& text) {
  if (!valid()) return ESPRack::InvalidMessagingSendId;
  return provider_->doSend(id_, TelegramRecipient::defaultDest(), text, {});
}

ESPRack::MessagingSendId TelegramSubscription::sendTo(
    const TelegramRecipient& to,
    const String& text,
    const TelegramSendOptions& opt) {
  if (!valid()) return ESPRack::InvalidMessagingSendId;
  return provider_->doSend(id_, to, text, opt);
}

TelegramSubscriptionStats TelegramSubscription::stats() const {
  if (!valid()) return {};
  return provider_->statsForSubscription(id_);
}

bool TelegramSubscription::enabled() const {
  if (!valid()) return false;
  return provider_->subscriptionEnabled(id_);
}

void TelegramSubscription::setEnabled(bool on) {
  if (!valid()) return;
  provider_->setSubscriptionEnabled(id_, on);
}

void TelegramSubscription::unsubscribe() {
  if (!valid()) return;
  provider_->releaseSubscription(id_);
  id_       = 0;
  provider_ = nullptr;
}

const char* TelegramSubscription::serviceName() const {
  if (!valid()) return "";
  return provider_->subscriptionName(id_);
}
