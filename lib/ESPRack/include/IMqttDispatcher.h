#ifndef IMqttDispatcher_h
#define IMqttDispatcher_h

#include <AsyncMqttClient.h>
#include <functional>

using MqttMsgCb = std::function<void(char*, char*, AsyncMqttClientMessageProperties, size_t, size_t, size_t)>;
using MqttConnectCb = std::function<void(bool)>;
using MqttDisconnectCb = std::function<void(AsyncMqttClientDisconnectReason)>;

class IMqttDispatcher {
 public:
  virtual void addMessageCallback(MqttMsgCb cb) = 0;
  virtual void addConnectCallback(MqttConnectCb cb) = 0;
  virtual void addDisconnectCallback(MqttDisconnectCb cb) = 0;
  virtual AsyncMqttClient* getMqttClient() = 0;
};

#endif
