#include "MqttSubscription.h"
#include "IMqttProvider.h"

ESPRack::MessagingSendId MqttSubscription::publish(const String& relativeTopic,
                                                    const String& payload) {
  if (!valid()) return ESPRack::InvalidMessagingSendId;
  return provider_->doPublish(id_, relativeTopic, payload, {});
}

ESPRack::MessagingSendId MqttSubscription::publishTo(
    const String& relativeTopic,
    const String& payload,
    const MqttPublishOptions& opt) {
  if (!valid()) return ESPRack::InvalidMessagingSendId;
  return provider_->doPublish(id_, relativeTopic, payload, opt);
}

void MqttSubscription::onMessage(const String& relativeTopic,
                                  MqttSubscriptionMessageHandler handler) {
  if (!valid() || !handler) return;
  provider_->doSubscribeTopic(id_, relativeTopic, std::move(handler));
}

bool MqttSubscription::active() const {
  if (!valid()) return false;
  if (!provider_->subscriptionEnabled(id_)) return false;
  return provider_->connectionState() != ESPRack::MessagingConnectionState::Disabled;
}

MqttSubscriptionStats MqttSubscription::stats() const {
  if (!valid()) return {};
  return provider_->statsForSubscription(id_);
}

bool MqttSubscription::enabled() const {
  if (!valid()) return false;
  return provider_->subscriptionEnabled(id_);
}

void MqttSubscription::setEnabled(bool on) {
  if (!valid()) return;
  provider_->setSubscriptionEnabled(id_, on);
}

void MqttSubscription::unsubscribe() {
  if (!valid()) return;
  provider_->releaseSubscription(id_);
  id_       = 0;
  provider_ = nullptr;
}

const char* MqttSubscription::serviceName() const {
  if (!valid()) return "";
  return provider_->subscriptionName(id_);
}
