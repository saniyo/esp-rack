#pragma once
#ifndef ESPRACK_TELEGRAM_COMMAND_H
#define ESPRACK_TELEGRAM_COMMAND_H

// SES-LIB-019: parsing and gating of bot commands. Arduino-free and
// header-only, so the rules are host-tested.

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <string>

namespace tgcmd {

struct Parsed {
  bool ok = false;
  std::string command;  // lower case, no "/" and no "@bot"
  std::string args;     // the rest, trimmed
};

// "/On@SesBot 30" -> {"on", "30"}. Anything else: ok = false.
inline Parsed parse(const char* text) {
  Parsed p;
  if (!text || text[0] != '/') return p;
  const char* s = text + 1;
  const char* end = s;
  while (*end && *end != ' ' && *end != '@' && *end != '\n') ++end;
  if (end == s) return p;
  for (const char* c = s; c < end; ++c) p.command += static_cast<char>(tolower(static_cast<unsigned char>(*c)));
  while (*end && *end != ' ' && *end != '\n') ++end;  // skip "@bot"
  while (*end == ' ' || *end == '\n') ++end;
  p.args = end;
  while (!p.args.empty() && (p.args.back() == ' ' || p.args.back() == '\n')) p.args.pop_back();
  p.ok = true;
  return p;
}

namespace detail {
// Calls fn(entry, len) for each comma-separated, trimmed, non-empty entry.
template <typename Fn>
inline bool anyEntry(const char* list, Fn fn) {
  if (!list) return false;
  const char* p = list;
  while (*p) {
    while (*p == ' ' || *p == ',') ++p;
    const char* b = p;
    while (*p && *p != ',') ++p;
    const char* e = p;
    while (e > b && e[-1] == ' ') --e;
    if (e > b && fn(b, static_cast<size_t>(e - b))) return true;
  }
  return false;
}
}  // namespace detail

// Is id one of the comma-separated entries?
inline bool inList(const char* list, const char* id) {
  if (!id || !*id) return false;
  const size_t n = strlen(id);
  return detail::anyEntry(list, [&](const char* b, size_t len) { return len == n && strncmp(b, id, n) == 0; });
}

// May chatId (topic topicId, 0 = none) send commands? Entries: "123",
// "-100456" (a group), "-100789:12" (only topic 12 of that group). An empty
// list allows only fallbackChat (the bot's default chat).
inline bool chatAllowed(const char* list, const char* chatId, int32_t topicId, const char* fallbackChat) {
  if (!chatId || !*chatId) return false;
  const size_t n = strlen(chatId);
  bool any = false;
  const bool hit = detail::anyEntry(list, [&](const char* b, size_t len) {
    any = true;
    const char* colon = static_cast<const char*>(memchr(b, ':', len));
    const size_t idLen = colon ? static_cast<size_t>(colon - b) : len;
    if (idLen != n || strncmp(b, chatId, n) != 0) return false;
    if (!colon) return true;
    return atol(std::string(colon + 1, b + len).c_str()) == topicId;
  });
  if (any) return hit;
  return fallbackChat && strcmp(fallbackChat, chatId) == 0;
}

enum class Verdict : uint8_t { Ignore, AdminOnly, Run };

// chat not allowed or unknown command: Ignore; admin command from someone
// else: AdminOnly; otherwise Run.
inline Verdict decide(bool chatOk, bool known, bool adminCommand, bool fromAdmin) {
  if (!chatOk || !known) return Verdict::Ignore;
  if (adminCommand && !fromAdmin) return Verdict::AdminOnly;
  return Verdict::Run;
}

}  // namespace tgcmd

#endif  // ESPRACK_TELEGRAM_COMMAND_H
