#include "TelegramSettings.h"
#include <FormBuilder.h>   // still used: updateValue() in update parsers
#include <web/Form.h>

// ─── Persisted config (ConfigDelegate readConfig) ────────────────────
//
// What lives on disk: token, chat, topic, enabled, sendIntervalMs,
// parseMode, silentDefault. Runtime status (qSize, lastMessage, log)
// is rebuilt on boot — never written back.
void TelegramSettings::readConfig(TelegramSettings& s, JsonObject& root) {
  root["token"]            = s.botToken;
  root["chat"]             = s.chatId;
  root["topic"]            = s.topicId;
  // device_label intentionally NOT persisted — single-source-of-truth
  // identity, recomputed every boot from DeviceIdentity::canonical().
  root["ena"]              = s.enabled;
  root["interval_ms"]      = s.sendIntervalMs;
  root["parse_mode"]       = s.parseMode;
  root["silent_default"]   = s.silentDefault;
  root["allowed_chat_ids"] = s.allowedChatIds;
  root["admin_user_ids"]   = s.adminUserIds;
  root["cmd_poll_s"]       = s.cmdPollS;
}

// ─── REST update (form POST + FS load) ───────────────────────────────
//
// Accepts both flat JSON and the nested {"settings": {...}} envelope
// the dynamic form posts. Runtime fields like manualText flow through
// here too (the textarea on the Status tab posts via the same form),
// but ONLY config changes flip cfgChanged → true so unrelated
// runtime edits don't trigger a disk write.
StateUpdateResult TelegramSettings::upd(JsonObject& j, TelegramSettings& s) {
  bool cfgChanged = false;

  JsonObject src = j;
  if (j.containsKey("settings") && j["settings"].is<JsonObject>()) {
    src = j["settings"].as<JsonObject>();
  }

  cfgChanged |= FormBuilder::updateValue(src, "token",          s.botToken);
  cfgChanged |= FormBuilder::updateValue(src, "chat",           s.chatId);
  cfgChanged |= FormBuilder::updateValue(src, "topic",          s.topicId);
  // device_label deliberately ignored — display-only field; runtime
  // value is set in TelegramService::begin() from DeviceIdentity.
  cfgChanged |= FormBuilder::updateValue(src, "ena",            s.enabled);
  cfgChanged |= FormBuilder::updateValue(src, "interval_ms",    s.sendIntervalMs);
  cfgChanged |= FormBuilder::updateValue(src, "parse_mode",     s.parseMode);
  cfgChanged |= FormBuilder::updateValue(src, "silent_default", s.silentDefault);
  cfgChanged |= FormBuilder::updateValue(src, "allowed_chat_ids", s.allowedChatIds);
  cfgChanged |= FormBuilder::updateValue(src, "admin_user_ids", s.adminUserIds);
  cfgChanged |= FormBuilder::updateValue(src, "cmd_poll_s", s.cmdPollS);
  if (s.cmdPollS < 5) s.cmdPollS = 5;
  if (s.cmdPollS > 60) s.cmdPollS = 60;

  // Runtime composer text — saved into state so the form re-renders
  // with the same text after the operator triggered Send. NOT persisted.
  (void)FormBuilder::updateValue(src, "manualText", s.manualText);

  return cfgChanged ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

// ─── REST form schema ────────────────────────────────────────────────
//
// Two tabs:
//   * Status   — read-only telemetry + manual-send composer + log table
//                + a "Send" ActionField wired to telegram.sendManual.
//   * Settings — config editor: token / chat / topic / enabled /
//                interval / parse mode / silent default.
//
// Manual send is an ActionField (not a fake bool field with "1"/"0"
// hack) — clicking POSTs the current manualText through the registered
// telegram.sendManual handler, which enqueues + clears. The frontend's
// refetchForm decorator triggers a Status-tab GET right after the
// action so the operator sees the queued line in the log.
void TelegramSettings::buildForm(TelegramSettings& s, JsonObject& root) {
  // ── STATUS ────────────────────────────────────────────────────────
  web::Form sta(root, "status", "Telegram bot status");

  // Status avatar: success (green) when the service is enabled and the
  // last send succeeded — mirrors the NTP "Active/Inactive" pattern so
  // operators get the same at-a-glance health cue across feature tabs.
  // Reasoning matches connectionState() in TelegramService.cpp:
  //   Disabled         → info     (operator chose to mute)
  //   queue >0         → warning  (sending in progress, no result yet)
  //   last send fail   → error
  //   last send ok     → success
  //   no sends yet     → info     (idle but armed)
  const char* statusColor;
  if (!s.enabled) {
    statusColor = "info";
  } else if (s.qSize > 0) {
    statusColor = "warning";
  } else if (s.lastSendAt_s > 0) {
    statusColor = s.lastSendOk ? "success" : "error";
  } else {
    statusColor = "info";
  }
  sta.add(web::Text("status", s.statusLabel.c_str()).readOnly()
              .label("Status").icon("Cable").color(statusColor));
  sta.add(web::Number("qsize", (double)s.qSize).readOnly()
              .label("Queue depth").unit("msgs").format("0").icon("Inventory2"));

  if (s.lastMessage.length() > 0) {
    String lastLine;
    if (s.lastSendAt_s > 0) {
      lastLine = String(s.lastSendOk ? "[ok] " : "[fail] ");
      lastLine += String(s.lastSendAt_s) + "s · ";
    }
    lastLine += s.lastMessage;
    sta.add(web::Text("last", lastLine.c_str()).readOnly()
                .label("Last attempt").icon(s.lastSendOk ? "Check" : "ErrorOutline"));
  }

  // Composer card — textarea + Send action button. The action's
  // refetchForm() decorator pushes a Status GET right after firing so
  // the new log row lands without a manual reload.
  sta.add(web::Textarea("manualText", s.manualText)
              .label("Compose").placeholder("Type a message…").icon("Edit"));
  sta.add(web::Action("manualSend", "Send")
              .ref("telegram.sendManual").icon("Send").color("primary").refetchForm());

  // Chat log — TableField; rows are pushed inline below (see
  // WiFiSettingsService scan-results pattern). One column per
  // structured field; frontend renders timestamps / status badges
  // using formatters specified per column.
  sta.add(web::Table("log")
              .col("ts", "Time", "number", "0")
              .col("stage", "Stage", "text")
              .col("text", "Message", "text")
              .mode("replace").maxRows(TelegramSettings::LOG_MAX)
              .icon("History").label("Recent activity"));
  JsonArray logRows = sta.rowsOf("log");
  for (const auto& e : s.chatLog) {
    JsonObject row = logRows.createNestedObject();
    row["ts"]    = e.ts_s;
    row["stage"] = e.stage;
    row["text"]  = e.text;
  }

  // ── SETTINGS ──────────────────────────────────────────────────────
  web::Form set(root, "settings", "Bot configuration");

  set.add(web::Secret("token", s.botToken.c_str())
              .label("Bot Token").icon("Lock").placeholder("123456:ABC-…"));
  set.add(web::Text("chat", s.chatId.c_str())
              .label("Default Chat ID").icon("Forum").placeholder("-1001234567890 or @channelname"));
  set.add(web::Text("topic", s.topicId.c_str())
              .label("Default Topic ID").icon("Topic").placeholder("optional, numeric thread ID"));
  // Device label — read-only mirror of the canonical Device ID.
  // Prepended to every outgoing message as `[label] <text>` so chat
  // readers can distinguish devices sharing a single bot. Not editable:
  // single-source-of-truth identity, identical to cert CN / MQTT
  // clientId / mothership deviceId.
  set.add(web::Text("device_label", s.deviceLabel.c_str()).readOnly()
              .label("Device Label (auto)").icon("Badge"));
  set.add(web::Switch("ena", s.enabled).label("Enabled").icon("Sync"));

  set.add(web::Number("interval_ms", (double)s.sendIntervalMs)
              .label("Min send interval").min(50).max(60000).format("0")
              .unit("ms").icon("Timer")
              .placeholder("≥50ms; lower = faster but watch Telegram rate limit (~30 msg/s)"));

  set.add(web::Dropdown("parse_mode", s.parseMode)
              .option("None", 0).option("Markdown", 1)
              .option("MarkdownV2", 2).option("HTML", 3)
              .label("Default parse mode").icon("FormatPaint"));

  // SES-LIB-019: bot commands.

  set.add(web::Text("allowed_chat_ids", s.allowedChatIds.c_str())

              .label("Command chats (id, -group, group:topic)").placeholder("the default chat only"));

  set.add(web::Text("admin_user_ids", s.adminUserIds.c_str()).label("Admin user ids"));

  set.add(web::Number("cmd_poll_s", s.cmdPollS).min(5).max(60).format("0").unit("s").label("Command poll"));

  set.add(web::Switch("silent_default", s.silentDefault)
              .label("Silent by default").icon("NotificationsOff"));
}

// ─── WS push (live status updates) ──────────────────────────────────
//
// Mirrors what the Status form would emit on a fresh REST GET, but
// at WS push cadence. Only state visible on the Status tab is sent
// — config (token/chat/etc.) is REST-only and isn't echoed here.
//
// NOTE: previously this path also accepted token/chat/topic edits
// from the WS side, which let any subscribed client rewrite the bot
// token without going through the admin-protected REST POST. That
// authorization gap is closed in staUpd below — config keys are
// now ignored on the WS update path.
void TelegramSettings::staRead(TelegramSettings& s, JsonObject& root) {
  root["status"]     = s.statusLabel;
  root["qsize"]      = s.qSize;
  root["manualText"] = s.manualText;

  if (s.lastMessage.length() > 0) {
    String lastLine = String(s.lastSendOk ? "[ok] " : "[fail] ");
    if (s.lastSendAt_s > 0) lastLine += String(s.lastSendAt_s) + "s · ";
    lastLine += s.lastMessage;
    root["last"] = lastLine;
  }

  // log[] mirrors the Status form's TableField rows so DynamicStatus
  // can append-merge incoming pushes onto the rendered table.
  JsonArray logRows = root.createNestedArray("log");
  for (const auto& e : s.chatLog) {
    JsonObject row = logRows.createNestedObject();
    row["ts"]    = e.ts_s;
    row["stage"] = e.stage;
    row["text"]  = e.text;
  }
}

// ─── WS update (limited) ────────────────────────────────────────────
//
// Only runtime fields editable over WS — the manual-send composer
// text. Bot config (token / chat / topic / enabled / interval /
// parseMode / silent) is REST-only and goes through the admin auth
// predicate on /api/v1/telegram/form. Closing this leak was a primary
// goal of the Telegram refactor: a WS subscriber should never have
// been able to rewrite credentials.
StateUpdateResult TelegramSettings::staUpd(JsonObject& in, TelegramSettings& s) {
  (void)FormBuilder::updateValue(in, "manualText", s.manualText);
  // Always UNCHANGED — staUpd never mutates persisted state.
  return StateUpdateResult::UNCHANGED;
}
