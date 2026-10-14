#pragma once
#ifndef ESPRACK_TELEGRAM_SERVICE_H
#define ESPRACK_TELEGRAM_SERVICE_H

// TelegramService — bot transport + subscription provider. Implements
// ITelegramProvider so internal services interact through subscription
// handles rather than reaching into the service directly. Outbound
// queue, REST/WS surface, and the telegram.sendManual action endpoint
// for the Status-tab composer all live here.

#include "TelegramSettings.h"
#include "ITelegramProvider.h"
#include <ConfigManager.h>
#include <ConfigDelegate.h>
#include <UniversalTelegramBot.h>
#include <WiFiClientSecure.h>
#include <WebFeatureDelegate.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <vector>

// Form/state paths are derived by the registry (C1): /api/v1/telegram/form (GET)
// and /api/v1/telegram/state (GET+POST). WS path stays explicit (/ws/*).
#define TEL_WS_PATH      "/ws/telegramStatus"
#define TEL_FILE         "/config/telegram.json"
#define TEL_QUEUE_DEPTH  20

class WebManager;

// FreeRTOS queue payload — value-type, no heap allocs per message.
struct TelegramQueuedMessage {
  uint32_t subId{0};            // 0 = manual / legacy enqueue (no rate-limit accounting)
  String   chatOverride;
  String   topicOverride;
  String   tokenOverride;
  String   text;
  int      parseMode{-1};
  bool     silent{false};
};

// Internal subscription record — provider-side state for each handle
// returned by subscribe(). Lives in TelegramService::_subs vector;
// indexed by id (1-based; 0 = invalid id).
struct TelegramSubscriptionRecord {
  uint32_t                       id{0};
  String                         name;
  TelegramSubscriptionConfig     cfg;
  TelegramSubscriptionStats      stats;
  bool                           enabled{true};
  // Sliding-window rate limiter — windowStart_s is the unix-ish-epoch
  // (millis()/1000) when the current minute window opened; windowSent
  // is the count inside it. Reset when the second wraps past +60.
  uint32_t                       windowStart_s{0};
  uint32_t                       windowSent{0};
};

class TelegramService : public StatefulService<TelegramSettings>,
                        public ITelegramProvider {
 public:
  explicit TelegramService(ConfigManager* cfgMgr);
  ~TelegramService() override;

  void registerManifest(WebManager* web);
  void begin();
  // Runs command handlers on the loop task (SES-LIB-019).
  void loop();

  bool doOnCommand(uint32_t subId, const char* cmd, const char* help, TelegramCommandHandler h,
                   bool adminOnly) override;

  // ── Legacy direct-enqueue (to be deprecated) ────────────────────
  void enqueueDefault(const char* text);
  void enqueueMessage(const String& chatId,
                      const String& topicId,
                      const String& text,
                      const TelegramSendOptions& opt = {},
                      const String& tokenOverride = String());

  // Triggered by telegram.sendManual action.
  void sendManualNow();

  // ── ITelegramProvider implementation ────────────────────────────
  TelegramSubscription subscribe(const char* serviceName,
                                 const TelegramSubscriptionConfig& cfg) override;

  ESPRack::MessagingSendId doSend(uint32_t subId,
                                  const TelegramRecipient& to,
                                  const String& text,
                                  const TelegramSendOptions& opt) override;
  TelegramSubscriptionStats statsForSubscription(uint32_t subId) const override;
  bool subscriptionEnabled(uint32_t subId) const override;
  void setSubscriptionEnabled(uint32_t subId, bool on) override;
  void releaseSubscription(uint32_t subId) override;
  const char* subscriptionName(uint32_t subId) const override;

  // ── IMessagingProvider implementation ───────────────────────────
  const char* providerId() const override { return "telegram"; }
  ESPRack::MessagingConnectionState connectionState() const override;
  size_t subscriberCount() const override;

  // Read-only iteration over subscriptions for UI rendering.
  // The callback gets a const-ref so the subscriber table can be
  // built without exposing the internal vector. Order = registration
  // order (also UI display order).
  template <typename Fn>
  void forEachSubscription(Fn cb) const {
    for (const auto& s : _subs) if (s.id != 0) cb(s);
  }

 private:
  ConfigDelegate<TelegramSettings>      _cfg;
  WebFeatureEntry<TelegramSettings>*    _feature{nullptr};

  WiFiClientSecure                      _cli;
  UniversalTelegramBot                  _bot;
  String                                _botTokenInBot;

  QueueHandle_t                         _q{nullptr};
  TaskHandle_t                          _task{nullptr};

  std::vector<TelegramSubscriptionRecord> _subs;
  uint32_t                              _nextSubId{1};
  uint32_t                              _lastSendOkAt_s{0};

  static void taskTrampoline(void*);
  void runWorker();
  void startTask();

  // SES-LIB-019 commands.
  struct TgCommand {
    uint32_t subId;
    String cmd;
    String help;
    TelegramCommandHandler h;
    bool adminOnly;
  };
  static constexpr size_t kMaxCommands = 16;
  std::vector<TgCommand> _cmds;            // guarded by _cmdMx
  SemaphoreHandle_t _cmdMx{nullptr};
  uint32_t _cmdsRev{0};                    // +1 per registration
  uint32_t _cmdsPublished{0};              // the rev setMyCommands last sent
  QueueHandle_t _inQ{nullptr};             // TelegramInbound*, TelTask -> loop
  long long _updOffset{0};                 // 0 = backlog not skipped yet
  uint32_t _lastPollMs{0};
  struct AdminNag {
    String fromId;
    uint32_t at;
  };
  AdminNag _nags[8];                       // "admin only" at most once a minute per user
  uint32_t _cmdIgnored{0}, _cmdDropped{0};
  void pollCommands(const String& token);
  void publishCommands(const String& token);
  bool postJson(const String& token, const char* method, const String& payload);

  // Locate a subscription record by id; nullptr if missing/invalid.
  TelegramSubscriptionRecord*       findSub(uint32_t id);
  const TelegramSubscriptionRecord* findSub(uint32_t id) const;

  // Sliding-window quota check + bookkeeping. Returns true if a new
  // send fits the budget and updates windowSent; false if the
  // subscription has hit its per-minute cap.
  bool checkAndConsumeQuota(TelegramSubscriptionRecord& rec);

  // Apply tag prefix + subscription-default routing. Mutates `to` and
  // `text` in place to the values that should actually go on the wire.
  void applyDefaults(const TelegramSubscriptionRecord& rec,
                     TelegramRecipient& to,
                     String& text);

  bool sendDirect(const String& token, const String& chatId,
                  const String& topicId, const String& text,
                  int parseMode, bool silent);
  bool sendViaUtb(const String& token, const String& chatId,
                  const String& topicId, const String& text,
                  int parseMode);
  void ensureBotToken(const String& token);

  void logLine(bool ok, const char* stage, const String& text);
  void updateStatusLabel();
  static const char* parseModeString(int code);
};

#endif  // ESPRACK_TELEGRAM_SERVICE_H
