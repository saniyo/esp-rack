#pragma once
#ifndef ESPRACK_LOCK_WAIT_H
#define ESPRACK_LOCK_WAIT_H

// Pure helpers for bounded waits (HGW-LIB-003: every wait on a lock or an
// event in a shared context has a timeout and an error path). No Arduino,
// no FreeRTOS: the host tests build them as they are.
//
//   msUntilDue    how long a periodic job may sleep (the loop's wait)
//   logThisFail   1st, 2nd, 4th, 8th... failure: log without a flood
//   PinTable      keeps an object alive while another task uses it
//
// Users: StatefulService (state lock timeout), WsManager (roster lock,
// client pins, ping/heartbeat deadlines).

#include <stddef.h>
#include <stdint.h>

// How long a StatefulService read()/update() waits for the state lock.
// The lock is held for a lambda's run only, so this is reached only when a
// lambda does slow work (or a task died holding it). Override with -D.
#ifndef ESPRACK_STATE_LOCK_TIMEOUT_MS
#define ESPRACK_STATE_LOCK_TIMEOUT_MS 2000
#endif

namespace ESPRack {

// Milliseconds from nowMs until lastMs + periodMs; 0 when due. Wrap-safe
// (millis() rolls over every 49.7 days).
inline uint32_t msUntilDue(uint32_t lastMs, uint32_t periodMs, uint32_t nowMs) {
  const uint32_t elapsed = nowMs - lastMs;
  return elapsed >= periodMs ? 0 : periodMs - elapsed;
}

// The n-th failure (n counted from 1) is logged when n is a power of two.
inline bool logThisFail(uint32_t n) { return n && (n & (n - 1)) == 0; }

// A table of objects one task may destroy while others use them (a web
// socket client: async_tcp frees it on disconnect while the loop task sends
// to it). A user pins an entry and gets its pointer only while the entry is
// open; the owner, about to free the object, closes the entry: no new pins,
// and it waits until the pins of OTHER tasks are dropped (its own are held
// further up its own stack, so it never waits for itself).
//
// Not locked: the caller holds one short lock (a spinlock) around every call.
// N <= 24 lets a caller wake a closer with one event-group bit per slot.
template <typename T, size_t N>
class PinTable {
 public:
  static constexpr size_t kSlots = N;
  static constexpr size_t kUsers = 2;  // tasks that may pin one entry at once

  // A new open entry; false when the table is full.
  bool add(uint32_t id, T* p) {
    if (!id || !p) return false;
    for (auto& s : _s) {
      if (s.id) continue;
      s = Slot{};
      s.id = id;
      s.p = p;
      return true;
    }
    return false;
  }

  // The object, pinned for `task`; nullptr when unknown, closing, or more
  // than kUsers tasks hold it already (the caller skips this use).
  T* pin(uint32_t id, const void* task) {
    Slot* s = find(id);
    if (!s || !s->p) return nullptr;
    for (auto& u : s->users) {
      if (u.n && u.task == task) {
        ++u.n;
        return s->p;
      }
    }
    for (auto& u : s->users) {
      if (u.n) continue;
      u.task = task;
      u.n = 1;
      return s->p;
    }
    return nullptr;
  }

  // Drops one pin of `task`. true when a closer waits and no other task's
  // pin is left: the caller wakes it (slot index in `slot`).
  bool unpin(uint32_t id, const void* task, int& slot) {
    Slot* s = find(id);
    if (!s) return false;
    for (auto& u : s->users) {
      if (u.n && u.task == task) {
        --u.n;
        break;
      }
    }
    slot = (int)(s - _s);
    return s->closer && !othersPinned(*s, s->closer);
  }

  // The open object without pinning it (valid while the caller's lock is held).
  T* peek(uint32_t id) const {
    const Slot* s = find(id);
    return s ? s->p : nullptr;
  }

  // Any entry for id, open or closing.
  bool has(uint32_t id) const { return find(id) != nullptr; }

  // `task` is about to free the object. -1: the entry is gone (no other
  // task held it); else the slot to wait on: while waiting(slot), then done(slot).
  int close(uint32_t id, const void* task) {
    Slot* s = find(id);
    if (!s) return -1;
    s->p = nullptr;
    if (!othersPinned(*s, task)) {
      *s = Slot{};
      return -1;
    }
    s->closer = task;
    return (int)(s - _s);
  }
  bool waiting(int slot) const {
    return slot >= 0 && (size_t)slot < N && _s[slot].closer && othersPinned(_s[slot], _s[slot].closer);
  }
  void done(int slot) {
    if (slot >= 0 && (size_t)slot < N) _s[slot] = Slot{};
  }

  // Ids of the open entries (at most cap); returns the count.
  size_t ids(uint32_t* out, size_t cap) const {
    size_t n = 0;
    for (const auto& s : _s) {
      if (s.id && s.p && n < cap) out[n++] = s.id;
    }
    return n;
  }

 private:
  struct User {
    const void* task = nullptr;
    uint16_t n = 0;
  };
  struct Slot {
    uint32_t id = 0;
    T* p = nullptr;               // null once closing
    const void* closer = nullptr; // the task waiting to free it
    User users[kUsers];
  };
  Slot _s[N];

  Slot* find(uint32_t id) {
    if (!id) return nullptr;
    for (auto& s : _s) {
      if (s.id == id) return &s;
    }
    return nullptr;
  }
  const Slot* find(uint32_t id) const { return const_cast<PinTable*>(this)->find(id); }
  static bool othersPinned(const Slot& s, const void* task) {
    for (const auto& u : s.users) {
      if (u.n && u.task != task) return true;
    }
    return false;
  }
};

}  // namespace ESPRack

#endif  // ESPRACK_LOCK_WAIT_H
