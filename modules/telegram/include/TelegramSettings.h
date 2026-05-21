#pragma once
#ifndef ESPRACK_TELEGRAM_SETTINGS_H
#define ESPRACK_TELEGRAM_SETTINGS_H

// TelegramSettings — persisted config + runtime status surface for the
// Telegram module. The struct itself is data-only; all serialization,
// FormBuilder shape, and update parsers live in TelegramSettings.cpp
// to keep header dependency footprint small.

#include <Arduino.h>
#include <vector>
#include <ArduinoJson.h>
#include <StatefulService.h>

struct TelegramLogEntry {
  uint32_t ts_s{0};      // seconds since boot when the line was logged
  bool     ok{false};    // true = sent OK, false = failed/queued
  String   stage;        // human-readable stage: "queued" / "ok" / "fail"
  String   text;         // actual message body
};

struct TelegramSettings {
  // ── Persisted config ─────────────────────────────────────────────────
  String        botToken;
  String        chatId;
  String        topicId;          // optional message_thread_id

  // Device-identity label, parallel to MQTT's clientId. Defaults to the
  // canonical Device ID (#{device_id} via SettingValue::format on first
  // boot when empty) so chat readers can tell devices apart when several
  // devices share one bot/chat. Prepended to every outgoing message body
  // as `[deviceLabel] <message>` — operator can override or clear via
  // Settings tab to opt out of the prefix (empty string skips prefix).
  String        deviceLabel;

  bool          enabled{false};
  // Minimum interval between outbound sends in milliseconds. Lower bound
  // keeps us under Telegram's ~30 msg/s per-bot ceiling; higher values
  // throttle when the bot is shared with chat that doesn't tolerate spam.
  uint32_t      sendIntervalMs{1000};
  // Default parse mode for outgoing messages — 0 None, 1 Markdown,
  // 2 MarkdownV2, 3 HTML. Affects how the bot interprets formatting in
  // payload text. Stored as int because FormBuilder's dropdown is
  // int-based (matches the rest of the codebase pattern).
  int           parseMode{1};
  bool          silentDefault{false};

  // ── Runtime status (not persisted) ───────────────────────────────────
  // Connection-state label shown verbatim in the Status form. Set by
  // TelegramService based on transport health: "Disabled", "Idle",
  // "Sending…", "Last send OK", "Error: <reason>".
  String        statusLabel{"Disabled"};
  uint32_t      qSize{0};
  String        lastMessage;       // text of the most recent attempt
  uint32_t      lastSendAt_s{0};   // seconds since boot
  bool          lastSendOk{false};

  // ── Manual-send composer (Status tab) ────────────────────────────────
  // The text input lives in state so it survives REST refetches; the
  // "Send" button is wired to the telegram.sendManual action endpoint
  // — clicking it POSTs the current manualText to the action URL,
  // which clears the field after enqueuing.
  String        manualText;

  // ── Rolling chat log (Status tab table) ──────────────────────────────
  // Bounded to LOG_MAX entries; oldest evicted on overflow. Surfaced
  // as a TableField in the Status form so each row's stage / time /
  // text are individually addressable. 30 chosen as the largest count
  // that comfortably fits the Status tab's 24KB REST envelope along
  // with the manualText composer + settings/subs sub-forms — at 50 the
  // serialized response was hitting the buffer ceiling once the log
  // saturated and the form would silently truncate, leaving the tab
  // stuck on "Loading Status…" indefinitely.
  static constexpr size_t LOG_MAX = 30;
  std::vector<TelegramLogEntry> chatLog;

  // ── Static glue for ConfigDelegate + WebFeatureEntry ─────────────────
  // Implementations in TelegramSettings.cpp.
  static void readConfig(TelegramSettings& s, JsonObject& root);
  static StateUpdateResult upd(JsonObject& j, TelegramSettings& s);
  static void buildForm(TelegramSettings& s, JsonObject& root);
  static void staRead(TelegramSettings& s, JsonObject& root);
  static StateUpdateResult staUpd(JsonObject& in, TelegramSettings& s);
};

#endif  // ESPRACK_TELEGRAM_SETTINGS_H
