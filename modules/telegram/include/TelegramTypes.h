#pragma once
#ifndef ESPRACK_TELEGRAM_TYPES_H
#define ESPRACK_TELEGRAM_TYPES_H

#include <Arduino.h>
#include <messaging/MessagingTypes.h>

// Telegram-specific message routing. Empty fields fall back to
// whatever the subscription was configured with (subscription's
// defaultChatId / defaultTopicId), and from there to the bot's
// global defaults (TelegramSettings.chatId / topicId).
struct TelegramRecipient {
  String chatId;        // numeric ID or @channel
  String topicId;       // optional message_thread_id (forum thread)

  static TelegramRecipient toChat(const String& chat) {
    TelegramRecipient r; r.chatId = chat; return r;
  }
  static TelegramRecipient toThread(const String& chat, const String& topic) {
    TelegramRecipient r; r.chatId = chat; r.topicId = topic; return r;
  }
  static TelegramRecipient defaultDest() { return {}; }
};

// Per-message rendering / delivery options. -1 / unset means "fall
// back to subscription default → bot default" — same precedence as
// recipient fields.
struct TelegramSendOptions {
  int  parseMode{-1};   // -1 use defaults; 0=None, 1=Markdown, 2=MarkdownV2, 3=HTML
  bool silent{false};   // OR-ed with subscription/bot silentDefault
};

// Per-subscription configuration. Each consuming service hands one
// of these to TelegramService::subscribe() to claim a slot in the
// outbound channel. The provider keeps a record per subscription
// (counters, mute flag, last-sent timestamp) keyed by the returned
// id, and routes the consumer's sends through that record's defaults.
struct TelegramSubscriptionConfig {
  // Optional override — empty means "use bot's configured default
  // chat" (TelegramSettings.chatId). Set when this service should
  // post into a different chat than other subscriptions on this bot.
  String defaultChatId;

  // Optional override — empty means "use bot's configured default
  // topic" (TelegramSettings.topicId). Per-subscription topic is
  // the primary way to route different services into different
  // forum threads of the same chat: WiFi alerts → topic 12, OTA
  // updates → topic 34, sensor data → topic 56.
  String defaultTopicId;

  // Soft per-subscription rate limit: after this many sends inside
  // a one-minute sliding window, further sends are dropped (counted
  // in stats.dropped). 0 = unlimited (still subject to the bot's
  // global send interval).
  uint32_t maxMessagesPerMinute{60};

  // Prepended to every outbound text. Useful for letting humans tell
  // which service is screaming at them without each subscriber having
  // to remember to add its own tag.
  String tagPrefix;
};

// Public per-subscription telemetry — surfaced in the Telegram UI's
// Subscriptions tab so the operator can see which services are
// chatty / quiet / over-quota at a glance.
struct TelegramSubscriptionStats {
  uint32_t sent{0};         // successful API ack
  uint32_t errors{0};       // attempted but transport / API failed
  uint32_t dropped{0};      // not attempted: quota / queue full / disabled
  uint32_t lastSentAt_s{0}; // seconds since boot (0 = never)
};

#endif  // ESPRACK_TELEGRAM_TYPES_H
