#include "TelegramSettings.h"
#include <FormBuilder.h>

// ─── Persisted config (ConfigDelegate readConfig) ────────────────────
//
// What lives on disk: token, chat, topic, enabled, sendIntervalMs,
// parseMode, silentDefault. Runtime status (qSize, lastMessage, log)
// is rebuilt on boot — never written back.
void TelegramSettings::readConfig(TelegramSettings& s, JsonObject& root) {
  root["token"]            = s.botToken;
  root["chat"]             = s.chatId;
  root["topic"]            = s.topicId;
  root["ena"]              = s.enabled;
  root["interval_ms"]      = s.sendIntervalMs;
  root["parse_mode"]       = s.parseMode;
  root["silent_default"]   = s.silentDefault;
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
  cfgChanged |= FormBuilder::updateValue(src, "ena",            s.enabled);
  cfgChanged |= FormBuilder::updateValue(src, "interval_ms",    s.sendIntervalMs);
  cfgChanged |= FormBuilder::updateValue(src, "parse_mode",     s.parseMode);
  cfgChanged |= FormBuilder::updateValue(src, "silent_default", s.silentDefault);

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
  JsonArray sta = FormBuilder::createForm(root, "status", "Telegram bot status");

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
  FormBuilder::addTextField(sta, "status", AF::R, s.statusLabel.c_str(),
                            label("Status"), icon("Cable"), color(statusColor));
  FormBuilder::addNumberField(sta, "qsize", AF::R, (double)s.qSize,
                              label("Queue depth"), unit("msgs"), format("0"),
                              icon("Inventory2"));

  if (s.lastMessage.length() > 0) {
    String lastLine;
    if (s.lastSendAt_s > 0) {
      lastLine = String(s.lastSendOk ? "[ok] " : "[fail] ");
      lastLine += String(s.lastSendAt_s) + "s · ";
    }
    lastLine += s.lastMessage;
    FormBuilder::addTextField(sta, "last", AF::R, lastLine.c_str(),
                              label("Last attempt"),
                              icon(s.lastSendOk ? "Check" : "ErrorOutline"));
  }

  // Composer card — textarea + Send action button. The action's
  // refetchForm() decorator pushes a Status GET right after firing so
  // the new log row lands without a manual reload.
  FormBuilder::addTextareaField(sta, "manualText", AF::RW, s.manualText,
                                label("Compose"), placeholder("Type a message…"),
                                icon("Edit"));
  FormBuilder::addActionField(sta, "manualSend", "Send", AF::RW,
                              actionRef("telegram.sendManual"),
                              icon("Send"), color("primary"), refetchForm());

  // Chat log — TableField; rows are pushed inline below (see
  // WiFiSettingsService scan-results pattern). One column per
  // structured field; frontend renders timestamps / status badges
  // using formatters specified per column.
  JsonObject logTbl = FormBuilder::addTableField(
      sta, "log", AF::R,
      col("ts",    "Time",   "number", format("0")),
      col("stage", "Stage",  "text"),
      col("text",  "Message", "text"),
      tableMode("replace"), maxRows(TelegramSettings::LOG_MAX),
      icon("History"),
      label("Recent activity"));
  JsonArray logRows = logTbl["log"].as<JsonArray>();
  for (const auto& e : s.chatLog) {
    JsonObject row = logRows.createNestedObject();
    row["ts"]    = e.ts_s;
    row["stage"] = e.stage;
    row["text"]  = e.text;
  }

  // ── SETTINGS ──────────────────────────────────────────────────────
  JsonArray set = FormBuilder::createForm(root, "settings", "Bot configuration");

  FormBuilder::addSecretField(set, "token", AF::RW, s.botToken.c_str(),
                              label("Bot Token"), icon("Lock"),
                              placeholder("123456:ABC-…"));
  FormBuilder::addTextField(set, "chat", AF::RW, s.chatId.c_str(),
                            label("Default Chat ID"), icon("Forum"),
                            placeholder("-1001234567890 or @channelname"));
  FormBuilder::addTextField(set, "topic", AF::RW, s.topicId.c_str(),
                            label("Default Topic ID"), icon("Topic"),
                            placeholder("optional, numeric thread ID"));
  FormBuilder::addSwitchField(set, "ena", AF::RW, s.enabled,
                              label("Enabled"), icon("Sync"));

  FormBuilder::addNumberField(set, "interval_ms", AF::RW, (double)s.sendIntervalMs,
                              label("Min send interval"),
                              minVal(50), maxVal(60000), format("0"),
                              unit("ms"), icon("Timer"),
                              placeholder("≥50ms; lower = faster but watch Telegram rate limit (~30 msg/s)"));

  FormBuilder::addDropdownField(set, "parse_mode", AF::RW, s.parseMode,
                                opt("None",        0),
                                opt("Markdown",    1),
                                opt("MarkdownV2",  2),
                                opt("HTML",        3),
                                label("Default parse mode"), icon("FormatPaint"));

  FormBuilder::addSwitchField(set, "silent_default", AF::RW, s.silentDefault,
                              label("Silent by default"),
                              icon("NotificationsOff"));
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
// predicate on /rest/telegramForm. Closing this leak was a primary
// goal of the Telegram refactor: a WS subscriber should never have
// been able to rewrite credentials.
StateUpdateResult TelegramSettings::staUpd(JsonObject& in, TelegramSettings& s) {
  (void)FormBuilder::updateValue(in, "manualText", s.manualText);
  // Always UNCHANGED — staUpd never mutates persisted state.
  return StateUpdateResult::UNCHANGED;
}
