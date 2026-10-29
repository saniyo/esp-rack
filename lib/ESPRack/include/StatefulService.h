#ifndef StatefulService_h
#define StatefulService_h

#include <Arduino.h>
#include <ArduinoJson.h>

#include <list>
#include <functional>
#ifdef ESP32
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "UpdateError.h"
#endif

#include "LockWait.h"

#ifndef DEFAULT_BUFFER_SIZE
#define DEFAULT_BUFFER_SIZE 8192 //1024
#endif

enum class StateUpdateResult {
  CHANGED = 0,  // The update changed the state and propagation should take place if required
  UNCHANGED,    // The state was unchanged, propagation should not take place
  ERROR         // There was a problem updating the state, propagation should not take place
};

template <typename T>
using JsonStateUpdater = std::function<StateUpdateResult(JsonObject& root, T& settings)>;

template <typename T>
using JsonStateReader = std::function<void(T& settings, JsonObject& root)>;

typedef size_t update_handler_id_t;
typedef std::function<void(const String& originId)> StateUpdateCallback;

typedef struct StateUpdateHandlerInfo {
  static update_handler_id_t currentUpdatedHandlerId;
  update_handler_id_t _id;
  StateUpdateCallback _cb;
  bool _allowRemove;
  StateUpdateHandlerInfo(StateUpdateCallback cb, bool allowRemove) :
      _id(++currentUpdatedHandlerId), _cb(cb), _allowRemove(allowRemove){};
} StateUpdateHandlerInfo_t;

template <class T>
class StatefulService {
 public:
  template <typename... Args>
#ifdef ESP32
  StatefulService(Args&&... args) :
      _state(std::forward<Args>(args)...), _accessMutex(xSemaphoreCreateRecursiveMutex()) {
  }
#else
  StatefulService(Args&&... args) : _state(std::forward<Args>(args)...) {
  }
#endif

  update_handler_id_t addUpdateHandler(StateUpdateCallback cb, bool allowRemove = true) {
    if (!cb) {
      return 0;
    }
    StateUpdateHandlerInfo_t updateHandler(cb, allowRemove);
    _updateHandlers.push_back(updateHandler);
    return updateHandler._id;
  }

  void removeUpdateHandler(update_handler_id_t id) {
    for (auto i = _updateHandlers.begin(); i != _updateHandlers.end();) {
      if ((*i)._allowRemove && (*i)._id == id) {
        i = _updateHandlers.erase(i);
      } else {
        ++i;
      }
    }
  }

  // The state lock is taken with a timeout (ESPRACK_STATE_LOCK_TIMEOUT_MS,
  // see LockWait.h). Busy: update() changes nothing and returns ERROR with
  // the reason "state busy" (the REST paths answer 400 with it), read()
  // does not run the reader and returns false (a JSON reader's object stays
  // as the caller passed it; a feature's GETs answer 503 "state_busy").
  // Check both results. Logged on the 1st, 2nd, 4th... timeout.
  StateUpdateResult update(std::function<StateUpdateResult(T&)> stateUpdater, const String& originId) {
    if (!beginTransaction()) return busyResult();
    StateUpdateResult result = stateUpdater(_state);
    endTransaction();
    if (result == StateUpdateResult::CHANGED) {
      callUpdateHandlers(originId);
    }
    return result;
  }

  StateUpdateResult updateWithoutPropagation(std::function<StateUpdateResult(T&)> stateUpdater) {
    if (!beginTransaction()) return busyResult();
    StateUpdateResult result = stateUpdater(_state);
    endTransaction();
    return result;
  }

  StateUpdateResult update(JsonObject& jsonObject, JsonStateUpdater<T> stateUpdater, const String& originId) {
    if (!beginTransaction()) return busyResult();
    StateUpdateResult result = stateUpdater(jsonObject, _state);
    endTransaction();
    if (result == StateUpdateResult::CHANGED) {
      callUpdateHandlers(originId);
    }
    return result;
  }

  StateUpdateResult updateWithoutPropagation(JsonObject& jsonObject, JsonStateUpdater<T> stateUpdater) {
    if (!beginTransaction()) return busyResult();
    StateUpdateResult result = stateUpdater(jsonObject, _state);
    endTransaction();
    return result;
  }

  bool read(std::function<void(T&)> stateReader) {
    if (!beginTransaction()) return false;
    stateReader(_state);
    endTransaction();
    return true;
  }

  bool read(JsonObject& jsonObject, JsonStateReader<T> stateReader) {
    if (!beginTransaction()) return false;
    stateReader(_state, jsonObject);
    endTransaction();
    return true;
  }

  // Runs fn with the state lock held, so the reads inside it (the lock is
  // recursive) can't time out halfway: a response built from several reads
  // is whole or not built. false: busy, fn did not run. Keep fn short and
  // never send on the network inside it.
  bool withLock(const std::function<void()>& fn) {
    if (!beginTransaction()) return false;
    fn();
    endTransaction();
    return true;
  }

  // Lock timeouts since boot (diagnostics).
  uint32_t lockTimeouts() const { return _lockTimeouts; }

  void callUpdateHandlers(const String& originId) {
    for (const StateUpdateHandlerInfo_t& updateHandler : _updateHandlers) {
      updateHandler._cb(originId);
    }
  }

 protected:
  T _state;

  // false: the lock was busy for ESPRACK_STATE_LOCK_TIMEOUT_MS; the caller
  // must not touch _state nor call endTransaction().
  inline bool beginTransaction() {
#ifdef ESP32
    if (!_accessMutex) return true;
    if (xSemaphoreTakeRecursive(_accessMutex, pdMS_TO_TICKS(ESPRACK_STATE_LOCK_TIMEOUT_MS)) == pdTRUE) {
      return true;
    }
    // Plain counter: a lost increment when two tasks time out at once is harmless.
    const uint32_t n = ++_lockTimeouts;
    if (ESPRack::logThisFail(n)) {
      log_w("[state] %p: lock busy for %u ms (%u so far)", (void*)this,
            (unsigned)ESPRACK_STATE_LOCK_TIMEOUT_MS, (unsigned)n);
    }
    return false;
#else
    return true;
#endif
  }

  inline void endTransaction() {
#ifdef ESP32
    if (_accessMutex) xSemaphoreGiveRecursive(_accessMutex);
#endif
  }

  StateUpdateResult busyResult() {
#ifdef ESP32
    ESPRack::setUpdateError("state busy");
#endif
    return StateUpdateResult::ERROR;
  }

 private:
#ifdef ESP32
  SemaphoreHandle_t _accessMutex;
#endif
  std::list<StateUpdateHandlerInfo_t> _updateHandlers;
  uint32_t _lockTimeouts{0};
};

#endif  // end StatefulService_h
