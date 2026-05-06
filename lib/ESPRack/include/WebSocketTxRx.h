#ifndef WebSocketTxRx_h
#define WebSocketTxRx_h

#include <Arduino.h>
#include <Ticker.h>
#include <StatefulService.h>
#include <ESPAsyncWebServer.h> // нова бібліотека
#include <ArduinoJson.h>
#include <map>
#include <queue>
#include <functional>
#include <SecurityManager.h>   // Якщо потрібно

// Підключаємо заголовок із сучасного форку
#include <AsyncWebSocket.h>

// --------------------------------------------------------------------------------------
// 1) Клас-прокладка "AsyncWebSocketBasicMessage", який імітує стару назву,
//    але всередині використовує новий конструктор "AsyncWebSocketMessage(...)"
//    і НЕ звертається до _WSbuffer безпосередньо (бо воно приватне).
// --------------------------------------------------------------------------------------
class AsyncWebSocketBasicMessage : public AsyncWebSocketMessage {
public:
    AsyncWebSocketBasicMessage(const char* data, size_t size, uint8_t opcode, bool mask)
        : AsyncWebSocketMessage(
              // створимо shared_ptr<vector<uint8_t>>
              createBuffer(data, size),
              opcode,
              mask)
    {
        // Тіло конструктора порожнє — бо все зробили у createBuffer().
        // Головне, що _WSbuffer встановився в базовому класі.
    }

private:
    // Статична функція, що створює та заповнює shared_ptr<std::vector<uint8_t>>
    static AsyncWebSocketSharedBuffer createBuffer(const char* data, size_t size) {
        auto buffer = std::make_shared<std::vector<uint8_t>>(size);
        if (buffer && data && size > 0) {
            // Копіюємо байти у спільний вектор
            memcpy(buffer->data(), data, size);
        }
        return buffer;
    }
};

// ---------------------
// Дефолтні макроси
// ---------------------
#define WEB_SOCKET_CLIENT_ID_MSG_SIZE 128
#define WEB_SOCKET_ORIGIN "ws"
#define WEB_SOCKET_ORIGIN_CLIENT_ID_PREFIX "ws:"

#ifndef TRANSMIT_INTERVAL_MS
#define TRANSMIT_INTERVAL_MS 100 // Інтервал надсилання повідомлень
#endif

#ifndef WS_MAX_QUEUED_MESSAGES
#define WS_MAX_QUEUED_MESSAGES 20
#endif

#ifndef PONG_TIMEOUT_MS
#define PONG_TIMEOUT_MS 30000    // 30с таймаут неактивності клієнта
#endif

#ifndef PING_INTERVAL_SEC
#define PING_INTERVAL_SEC 10     // раз на 10с
#endif

#ifndef DEFAULT_BUFFER_SIZE
#define DEFAULT_BUFFER_SIZE 4096
#endif

// Приклад ліміту відправлень на секунду
#ifndef MAX_TX_MESSAGES_PER_SECOND
#define MAX_TX_MESSAGES_PER_SECOND 5
#endif

//--------------------------------------------------------------------------
// Базовий клас WebSocketConnector<T> з підтримкою Ping-Pong
//--------------------------------------------------------------------------
template <class T>
class WebSocketConnector {
protected:
    StatefulService<T>* _statefulService;
    AsyncWebServer*     _server;

    AsyncWebSocket      _webSocket;

    size_t              _bufferSize;

    std::map<uint32_t, size_t> _clientMessageCounts;
    std::map<uint32_t, std::queue<AsyncWebSocketMessage*>> _clientMessageQueues;
    std::map<uint32_t, unsigned long> _lastPongTime;

    Ticker _pingTicker;

    unsigned long _lastPingTimestamp = 0;
    float _avgRttMillis = 0.0f;
    float _alpha        = 0.3f;
    bool  _hasFirstRtt  = false;

public:
    // ------------------------------------------------
    // Конструктори з/без SecurityManager
    // ------------------------------------------------
    WebSocketConnector(StatefulService<T>* statefulService,
                       AsyncWebServer*     server,
                       const char*         webSocketPath,
                       SecurityManager*    securityManager,
                       AuthenticationPredicate authPred,
                       size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : _statefulService(statefulService)
      , _server(server)
      , _webSocket(webSocketPath)
      , _bufferSize(bufferSize)
    {
        // Можливо: _webSocket.setFilter( securityManager->filterRequest(authPred) );

        _webSocket.onEvent(std::bind(&WebSocketConnector::onWSEventInternal,
                                     this,
                                     std::placeholders::_1,
                                     std::placeholders::_2,
                                     std::placeholders::_3,
                                     std::placeholders::_4,
                                     std::placeholders::_5,
                                     std::placeholders::_6));
        _server->addHandler(&_webSocket);

        // Заборонити звичайний GET на цей шлях
        _server->on(webSocketPath, HTTP_GET, std::bind(&WebSocketConnector::forbidden, this, std::placeholders::_1));
    }

    // Спрощений конструктор
    WebSocketConnector(StatefulService<T>* statefulService,
                       AsyncWebServer*     server,
                       const char*         webSocketPath,
                       size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : _statefulService(statefulService)
      , _server(server)
      , _webSocket(webSocketPath)
      , _bufferSize(bufferSize)
    {
        _webSocket.onEvent(std::bind(&WebSocketConnector::onWSEventInternal,
                                     this,
                                     std::placeholders::_1,
                                     std::placeholders::_2,
                                     std::placeholders::_3,
                                     std::placeholders::_4,
                                     std::placeholders::_5,
                                     std::placeholders::_6));
        _server->addHandler(&_webSocket);
    }

    virtual ~WebSocketConnector() {
        _pingTicker.detach();
    }

    // ------------------------------------------------
    // Ping-Pong
    // ------------------------------------------------
    static void staticPingCallback(WebSocketConnector<T>* instance) {
        instance->onPingCheck();
    }

    void beginPingPong(unsigned int pingIntervalSec = PING_INTERVAL_SEC) {
        _pingTicker.attach((float)pingIntervalSec, staticPingCallback, this);
    }

    void onWSEventInternal(AsyncWebSocket* server,
                           AsyncWebSocketClient* client,
                           AwsEventType type,
                           void* arg,
                           uint8_t* data,
                           size_t len)
    {
        uint32_t clientId = client->id();

        if (type == WS_EVT_CONNECT) {
            // setCloseClientOnQueueFull(false) -> не рвемо з'єднання
            client->setCloseClientOnQueueFull(false);
            _lastPongTime[clientId] = millis();
        }
        else if (type == WS_EVT_DISCONNECT || type == WS_EVT_ERROR) {
            decrementMessageCount(clientId);
            clearMessageQueue(clientId);
            _lastPongTime.erase(clientId);
        }
        else if (type == WS_EVT_PONG) {
            _lastPongTime[clientId] = millis();
            processMessageQueue(clientId);

            if (_lastPingTimestamp > 0) {
                unsigned long rtt = millis() - _lastPingTimestamp;
                updateRtt(rtt);
            }
        }

        onWSEvent(server, client, type, arg, data, len);
    }

    virtual void onWSEvent(AsyncWebSocket* server,
                           AsyncWebSocketClient* client,
                           AwsEventType type,
                           void* arg,
                           uint8_t* data,
                           size_t len) = 0;

    void forbidden(AsyncWebServerRequest* request) {
        request->send(403);
    }

    void pingAllClients() {
        _lastPingTimestamp = millis();
        _webSocket.pingAll();
    }

    void checkInactiveClients() {
        unsigned long now = millis();
        for (auto it = _lastPongTime.begin(); it != _lastPongTime.end();) {
            if (now - it->second > PONG_TIMEOUT_MS) {
                AsyncWebSocketClient* c = _webSocket.client(it->first);
                if (c) c->close();
                it = _lastPongTime.erase(it);
            } else {
                ++it;
            }
        }
    }

protected:
    void onPingCheck() {
        pingAllClients();
        checkInactiveClients();
    }

    void updateRtt(unsigned long rttMillis) {
        if (!_hasFirstRtt) {
            _avgRttMillis = rttMillis;
            _hasFirstRtt = true;
        } else {
            _avgRttMillis = _alpha * rttMillis + (1.0f - _alpha) * _avgRttMillis;
        }
    }

    void incrementMessageCount(uint32_t clientId) {
        _clientMessageCounts[clientId]++;
    }

    void decrementMessageCount(uint32_t clientId) {
        auto it = _clientMessageCounts.find(clientId);
        if (it != _clientMessageCounts.end() && it->second > 0) {
            it->second--;
        }
    }

    void clearMessageQueue(uint32_t clientId) {
        auto it = _clientMessageQueues.find(clientId);
        if (it != _clientMessageQueues.end()) {
            while (!it->second.empty()) {
                delete it->second.front();
                it->second.pop();
            }
            _clientMessageQueues.erase(it);
        }
    }

    void enqueueMessage(uint32_t clientId, AsyncWebSocketMessage* message) {
        if (_clientMessageQueues[clientId].size() < WS_MAX_QUEUED_MESSAGES) {
            _clientMessageQueues[clientId].push(message);
            incrementMessageCount(clientId);
        } else {
            // Якщо черга переповнена — знищимо повідомлення
            delete message;
        }
    }

    void processMessageQueue(uint32_t clientId) {
        AsyncWebSocketClient* c = _webSocket.client(clientId);
        if (!c) return;

        while (!_clientMessageQueues[clientId].empty() && c->canSend()) {
            AsyncWebSocketMessage* msg = _clientMessageQueues[clientId].front();
            _clientMessageQueues[clientId].pop();

            if (msg->send(c->client())) {
                decrementMessageCount(clientId);
                delete msg;
            } else {
                // Якщо не вдалося, повертаємо назад в чергу
                _clientMessageQueues[clientId].push(msg);
                break;
            }
        }
    }

    String makeClientId(AsyncWebSocketClient* client) {
        return String(WEB_SOCKET_ORIGIN_CLIENT_ID_PREFIX) + String(client->id());
    }

public:
    float getAverageRTT() const {
        return _avgRttMillis;
    }
};

// --------------------------------------------------------------------------
// Клас WebSocketTx<T> — відправник
// --------------------------------------------------------------------------
template <class T>
class WebSocketTx : virtual public WebSocketConnector<T> {
public:
    WebSocketTx(JsonStateReader<T>   stateReader,
                StatefulService<T>*  statefulService,
                AsyncWebServer*     server,
                const char*         webSocketPath,
                SecurityManager*    securityManager,
                AuthenticationPredicate authPred,
                size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebSocketConnector<T>(statefulService,
                              server,
                              webSocketPath,
                              securityManager,
                              authPred,
                              bufferSize),
        _stateReader(stateReader),
        lastTransmitTime(0),
        messagesSentThisSecond(0),
        lastSecondMark(0),
        transmitInterval(TRANSMIT_INTERVAL_MS)
    {
        this->_statefulService->addUpdateHandler(
            [&](const String& originId) { scheduleTransmitData(originId); },
            false
        );
    }

    // Без SecurityManager
    WebSocketTx(JsonStateReader<T>   stateReader,
                StatefulService<T>*  statefulService,
                AsyncWebServer*     server,
                const char*         webSocketPath,
                size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebSocketConnector<T>(statefulService,
                              server,
                              webSocketPath,
                              bufferSize),
        _stateReader(stateReader),
        lastTransmitTime(0),
        messagesSentThisSecond(0),
        lastSecondMark(0),
        transmitInterval(TRANSMIT_INTERVAL_MS)
    {
        this->_statefulService->addUpdateHandler(
            [&](const String& originId) { scheduleTransmitData(originId); },
            false
        );
    }

    void scheduleTransmitData(const String& originId) {
        unsigned long now = millis();

        if (now - lastSecondMark >= 1000) {
            lastSecondMark = now;
            messagesSentThisSecond = 0;
        }
        if (messagesSentThisSecond >= MAX_TX_MESSAGES_PER_SECOND) {
            return;
        }
        if (now - lastTransmitTime >= transmitInterval) {
            transmitData(nullptr, originId);
            lastTransmitTime = now;
            messagesSentThisSecond++;
        }
    }

protected:
    void onWSEvent(AsyncWebSocket*     server,
                   AsyncWebSocketClient* client,
                   AwsEventType         type,
                   void*                arg,
                   uint8_t*             data,
                   size_t               len) override
    {
        if (type == WS_EVT_CONNECT) {
            transmitId(client);
            transmitData(client, WEB_SOCKET_ORIGIN);
        }
    }

private:
    JsonStateReader<T> _stateReader;

    unsigned long lastTransmitTime;
    unsigned long transmitInterval;

    unsigned int messagesSentThisSecond;
    unsigned long lastSecondMark;

    void transmitId(AsyncWebSocketClient* client) {
        uint32_t cid = client->id();

        DynamicJsonDocument doc(WEB_SOCKET_CLIENT_ID_MSG_SIZE);
        doc["type"] = "id";
        doc["id"]   = this->makeClientId(client);

        size_t len = measureJson(doc);

        AsyncWebSocketMessageBuffer* buffer = this->_webSocket.makeBuffer(len);
        if (!buffer) return;

        serializeJson(doc, (char*)buffer->get(), len + 1);

        if (client->canSend()) {
            client->text(buffer);
            this->incrementMessageCount(cid);
        } else {
            // Тепер використовуємо "AsyncWebSocketBasicMessage"
            this->enqueueMessage(
                cid,
                new AsyncWebSocketBasicMessage((const char*)buffer->get(), len, WS_TEXT, false)
            );
        }
    }

    void transmitData(AsyncWebSocketClient* client, const String& originId) {
        uint32_t cid = client ? client->id() : 0;

        DynamicJsonDocument doc(this->_bufferSize);
        JsonObject root = doc.to<JsonObject>();
        root["type"]      = "p";
        root["origin_id"] = originId;
        JsonObject p      = root.createNestedObject("p");

        this->_statefulService->read(p, _stateReader);

        size_t len = measureJson(doc);
        AsyncWebSocketMessageBuffer* buffer = this->_webSocket.makeBuffer(len);
        if (!buffer) return;

        serializeJson(doc, (char*)buffer->get(), len + 1);

        if (client) {
            if (client->canSend()) {
                client->text(buffer);
                this->incrementMessageCount(cid);
            } else {
                this->enqueueMessage(
                    cid,
                    new AsyncWebSocketBasicMessage((const char*)buffer->get(), len, WS_TEXT, false)
                );
            }
        } else {
            // Для всіх
            this->_webSocket.textAll(buffer);
            for (auto& kv : this->_clientMessageCounts) {
                this->incrementMessageCount(kv.first);
            }
        }
    }
};

// --------------------------------------------------------------------------
// Клас WebSocketRx<T> — приймач
// --------------------------------------------------------------------------
template <class T>
class WebSocketRx : virtual public WebSocketConnector<T> {
public:
    WebSocketRx(JsonStateUpdater<T>   stateUpdater,
                StatefulService<T>*   statefulService,
                AsyncWebServer*      server,
                const char*          webSocketPath,
                SecurityManager*     securityManager,
                AuthenticationPredicate authPred,
                size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebSocketConnector<T>(statefulService,
                              server,
                              webSocketPath,
                              securityManager,
                              authPred,
                              bufferSize),
        _stateUpdater(stateUpdater)
    {}

    // Без SecurityManager
    WebSocketRx(JsonStateUpdater<T>   stateUpdater,
                StatefulService<T>*   statefulService,
                AsyncWebServer*      server,
                const char*          webSocketPath,
                size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebSocketConnector<T>(statefulService,
                              server,
                              webSocketPath,
                              bufferSize),
        _stateUpdater(stateUpdater)
    {}

protected:
    void onWSEvent(AsyncWebSocket*     server,
                   AsyncWebSocketClient* client,
                   AwsEventType         type,
                   void*                arg,
                   uint8_t*             data,
                   size_t               len) override
    {
        if (type == WS_EVT_DATA) {
            AwsFrameInfo* info = (AwsFrameInfo*)arg;
            if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
                DynamicJsonDocument doc(this->_bufferSize);
                DeserializationError err = deserializeJson(doc, (char*)data);
                if (!err && doc.is<JsonObject>()) {
                    JsonObject jsonObject = doc.as<JsonObject>();
                    this->_statefulService->update(
                        jsonObject,
                        _stateUpdater,
                        this->makeClientId(client)
                    );
                }
            }
        }
    }

private:
    JsonStateUpdater<T> _stateUpdater;
};

// --------------------------------------------------------------------------
// Клас WebSocketTxRx<T> — комбайн Tx + Rx
// --------------------------------------------------------------------------
template <class T>
class WebSocketTxRx : public WebSocketTx<T>, public WebSocketRx<T> {
public:
    WebSocketTxRx(JsonStateReader<T>   stateReader,
                  JsonStateUpdater<T>  stateUpdater,
                  StatefulService<T>*  statefulService,
                  AsyncWebServer*     server,
                  const char*         webSocketPath,
                  SecurityManager*    securityManager,
                  AuthenticationPredicate authPred = AuthenticationPredicates::IS_ADMIN,
                  size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebSocketConnector<T>(statefulService,
                              server,
                              webSocketPath,
                              securityManager,
                              authPred,
                              bufferSize),
        WebSocketTx<T>(stateReader,
                       statefulService,
                       server,
                       webSocketPath,
                       securityManager,
                       authPred,
                       bufferSize),
        WebSocketRx<T>(stateUpdater,
                       statefulService,
                       server,
                       webSocketPath,
                       securityManager,
                       authPred,
                       bufferSize)
    {}

    // Без SecurityManager
    WebSocketTxRx(JsonStateReader<T>   stateReader,
                  JsonStateUpdater<T>  stateUpdater,
                  StatefulService<T>*  statefulService,
                  AsyncWebServer*     server,
                  const char*         webSocketPath,
                  size_t bufferSize = DEFAULT_BUFFER_SIZE)
      : WebSocketConnector<T>(statefulService, server, webSocketPath, bufferSize),
        WebSocketTx<T>(stateReader, statefulService, server, webSocketPath, bufferSize),
        WebSocketRx<T>(stateUpdater, statefulService, server, webSocketPath, bufferSize)
    {}

protected:
    void onWSEvent(AsyncWebSocket*     server,
                   AsyncWebSocketClient* client,
                   AwsEventType         type,
                   void*                arg,
                   uint8_t*             data,
                   size_t               len) override
    {
        // Спочатку Rx
        WebSocketRx<T>::onWSEvent(server, client, type, arg, data, len);
        // Потім Tx
        WebSocketTx<T>::onWSEvent(server, client, type, arg, data, len);
    }
};

#endif // WebSocketTxRx_h
