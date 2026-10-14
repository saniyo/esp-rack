#pragma once
#ifndef ESPRACK_PERSIST_NVS_RECORD_H
#define ESPRACK_PERSIST_NVS_RECORD_H

// NvsRecord<T> (SES-LIB-008): one struct in one NVS blob, CRC-checked and
// versioned. For checkpoints and counters, which JSON config files don't
// suit (a ConfigManager save is three flash operations).
//
//   struct Counters { uint32_t starts; uint32_t onMinutes; };  // trivially copyable
//   NvsRecord<Counters> rec("dio", "relay1", /*schema=*/1);
//   Counters c{};
//   rec.load(c);   // Ok / Migrated / Missing (fresh device) / Corrupt / NoStore
//   rec.store(c);  // Ok / Unchanged / Skipped (PowerGuard) / Failed / NoStore
//
// Blob: {magic 'NVR1', schema u16, len u16, gen u32, crc32} + payload. NVS
// replaces an item atomically, so a power cut during store() leaves the old
// value or the new one. A store equal to the last one loaded or stored
// writes nothing. Zero-initialise T (padding counts). Use a record from one
// task only.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <memory>
#include <new>
#include <type_traits>

#include "persist/Crc32.h"
#include "power/PowerGuard.h"

namespace ESPRack {

enum class RecStatus : uint8_t {
  Ok,        // loaded
  Migrated,  // loaded from an older schema by the migrate callback
  Missing,   // nothing stored yet: a fresh device, not a loss
  Corrupt,   // magic / length / CRC mismatch, or an old schema nobody migrated
  NoStore    // NVS unavailable
};

enum class StoreStatus : uint8_t {
  Ok,
  Unchanged,  // the same payload is already stored: nothing written
  Skipped,    // PowerGuard paused writes
  Failed,     // the NVS write failed
  NoStore     // NVS unavailable
};
inline bool stored(StoreStatus s) { return s == StoreStatus::Ok || s == StoreStatus::Unchanged; }

namespace nvsrec {

struct Header {
  uint32_t magic;
  uint16_t schema;
  uint16_t len;
  uint32_t gen;
  uint32_t crc;  // over magic..gen and the payload
};
static_assert(sizeof(Header) == 16, "nvsrec::Header layout");
constexpr uint32_t kMagic = 0x3152564Eu;  // bytes 'N','V','R','1'

enum class Verdict : uint8_t { Ok, OtherSchema, Corrupt };
// out: sizeof(Header) + len bytes.
void encode(uint8_t* out, uint16_t schema, uint32_t gen, const void* payload, uint16_t len);
// Ok: schema and len match. OtherSchema: a valid blob of another schema
// (hdr.len payload bytes follow the header).
Verdict decode(const uint8_t* blob, size_t size, uint16_t schema, uint16_t len, Header& hdr);

enum class Io : uint8_t { Ok, Missing, TooBig, NoStore, Failed };
// NVS I/O (target: NvsIo.cpp). size: the stored blob's size, also on TooBig.
Io get(const char* ns, const char* key, uint8_t* buf, size_t cap, size_t& size);
Io set(const char* ns, const char* key, const uint8_t* buf, size_t size);

}  // namespace nvsrec

template <typename T>
class NvsRecord {
  static_assert(std::is_trivially_copyable<T>::value, "NvsRecord<T>: T must be trivially copyable");
  static_assert(sizeof(T) <= 0xFFFF, "NvsRecord<T>: T too large");

 public:
  // fromSchema: the stored schema; payload/len: its bytes. Fill out, or
  // return false (the load is then Corrupt).
  using Migrate = bool (*)(uint16_t fromSchema, const uint8_t* payload, size_t len, T& out);

  // ns and key: NVS names (<= 15 chars), static strings.
  NvsRecord(const char* ns, const char* key, uint16_t schema) : _ns(ns), _key(key), _schema(schema) {}

  RecStatus load(T& out, Migrate migrate = nullptr) {
    size_t size = 0;
    const uint8_t* blob = _buf;
    std::unique_ptr<uint8_t[]> big;  // an older, larger schema: boot time only
    nvsrec::Io io = nvsrec::get(_ns, _key, _buf, sizeof(_buf), size);
    if (io == nvsrec::Io::TooBig) {
      big.reset(new (std::nothrow) uint8_t[size]);
      if (!big) return RecStatus::NoStore;
      io = nvsrec::get(_ns, _key, big.get(), size, size);
      blob = big.get();
    }
    if (io == nvsrec::Io::Missing) return RecStatus::Missing;
    if (io != nvsrec::Io::Ok) return RecStatus::NoStore;
    nvsrec::Header h;
    const nvsrec::Verdict v = nvsrec::decode(blob, size, _schema, static_cast<uint16_t>(sizeof(T)), h);
    if (v == nvsrec::Verdict::Corrupt) return RecStatus::Corrupt;
    _gen = h.gen;
    const uint8_t* payload = blob + sizeof(nvsrec::Header);
    if (v == nvsrec::Verdict::OtherSchema) {
      if (!migrate || !migrate(h.schema, payload, h.len, out)) return RecStatus::Corrupt;
      _hasLast = false;  // the next store() writes the current schema
      return RecStatus::Migrated;
    }
    memcpy(&out, payload, sizeof(T));
    _lastCrc = crc32Ieee(payload, sizeof(T));
    _hasLast = true;
    return RecStatus::Ok;
  }

  StoreStatus store(const T& v) {
    const uint32_t c = crc32Ieee(&v, sizeof(T));
    if (_hasLast && c == _lastCrc) return StoreStatus::Unchanged;
    if (!PowerGuard::writesAllowed()) {
      ++_skips;
      return StoreStatus::Skipped;
    }
    nvsrec::encode(_buf, _schema, _gen + 1, &v, static_cast<uint16_t>(sizeof(T)));
    const nvsrec::Io io = nvsrec::set(_ns, _key, _buf, sizeof(_buf));
    if (io != nvsrec::Io::Ok) return io == nvsrec::Io::NoStore ? StoreStatus::NoStore : StoreStatus::Failed;
    ++_gen;
    ++_writes;
    _lastCrc = c;
    _hasLast = true;
    return StoreStatus::Ok;
  }

  uint32_t generation() const { return _gen; }  // of the blob last loaded or stored
  uint32_t writes() const { return _writes; }   // blobs written since boot
  uint32_t skips() const { return _skips; }     // stores PowerGuard paused

 private:
  const char* _ns;
  const char* _key;
  uint16_t _schema;
  uint32_t _gen = 0;
  uint32_t _writes = 0;
  uint32_t _skips = 0;
  uint32_t _lastCrc = 0;
  bool _hasLast = false;
  uint8_t _buf[sizeof(nvsrec::Header) + sizeof(T)];  // no heap per store
};

}  // namespace ESPRack

#endif  // ESPRACK_PERSIST_NVS_RECORD_H
