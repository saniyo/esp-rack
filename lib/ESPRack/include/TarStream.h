#ifndef TarStream_h
#define TarStream_h

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <algorithm>

// Streams a USTAR archive via successive fill() calls suitable for
// AsyncWebServer chunked responses. State is fully self-contained.
class TarStream {
 public:
  struct Entry {
    String archiveName;  // path inside archive (POSIX, no leading slash)
    String fsPath;       // full path on filesystem
    bool isDir;
    size_t size;
    time_t mtime;
  };

  TarStream(fs::FS* fs, std::vector<Entry> entries)
      : _fs(fs), _entries(std::move(entries)) {}

  ~TarStream() {
    if (_curFile) _curFile.close();
  }

  // Fill up to maxLen bytes; returns 0 when archive is complete.
  size_t fill(uint8_t* buf, size_t maxLen) {
    size_t written = 0;
    while (written < maxLen && _state != State::Done) {
      switch (_state) {
        case State::NextEntry:
          if (_entryIdx >= _entries.size()) {
            _state = State::EndBlocks;
            break;
          }
          if (!buildHeader(_entries[_entryIdx])) {
            _entryIdx++;  // skip unreadable
            break;
          }
          _hdrPos = 0;
          _state = State::Header;
          break;

        case State::Header: {
          size_t n = std::min(maxLen - written, (size_t)512 - _hdrPos);
          memcpy(buf + written, _hdr + _hdrPos, n);
          written += n;
          _hdrPos += n;
          if (_hdrPos >= 512) {
            const auto& e = _entries[_entryIdx];
            if (e.isDir || e.size == 0) {
              if (_curFile) _curFile.close();
              _entryIdx++;
              _state = State::NextEntry;
            } else {
              _dataPos = 0;
              _state = State::Data;
            }
          }
          break;
        }

        case State::Data: {
          const auto& e = _entries[_entryIdx];
          size_t n = std::min(maxLen - written, e.size - _dataPos);
          if (n > 0 && _curFile) {
            int rd = _curFile.read(buf + written, n);
            if (rd <= 0) {
              memset(buf + written, 0, n);  // pad on unexpected EOF
              rd = (int)n;
            }
            written += (size_t)rd;
            _dataPos += (size_t)rd;
          }
          if (_dataPos >= e.size) {
            if (_curFile) _curFile.close();
            _padRemaining = (512 - (e.size % 512)) % 512;
            _state = _padRemaining ? State::Padding : State::AfterEntry;
          }
          break;
        }

        case State::Padding: {
          size_t n = std::min(maxLen - written, _padRemaining);
          memset(buf + written, 0, n);
          written += n;
          _padRemaining -= n;
          if (_padRemaining == 0) _state = State::AfterEntry;
          break;
        }

        case State::AfterEntry:
          _entryIdx++;
          _state = State::NextEntry;
          break;

        case State::EndBlocks: {
          size_t n = std::min(maxLen - written, (size_t)1024 - _endBlocksWritten);
          memset(buf + written, 0, n);
          written += n;
          _endBlocksWritten += n;
          if (_endBlocksWritten >= 1024) _state = State::Done;
          break;
        }

        case State::Done:
          break;
      }
    }
    return written;
  }

 private:
  enum class State { NextEntry, Header, Data, Padding, AfterEntry, EndBlocks, Done };

  fs::FS* _fs;
  std::vector<Entry> _entries;
  size_t _entryIdx{0};
  State _state{State::NextEntry};

  uint8_t _hdr[512]{};
  size_t _hdrPos{0};
  size_t _dataPos{0};
  size_t _padRemaining{0};
  size_t _endBlocksWritten{0};
  File _curFile;

  bool buildHeader(const Entry& e) {
    memset(_hdr, 0, 512);

    String name = e.archiveName;
    while (name.startsWith("/")) name.remove(0, 1);
    if (e.isDir && !name.endsWith("/")) name += "/";
    if (name.length() > 100) name = name.substring(name.length() - 100);
    memcpy(_hdr, name.c_str(), name.length());

    snprintf(reinterpret_cast<char*>(_hdr + 100), 8, "%07o", e.isDir ? 0755u : 0644u);
    snprintf(reinterpret_cast<char*>(_hdr + 108), 8, "%07o", 0u);
    snprintf(reinterpret_cast<char*>(_hdr + 116), 8, "%07o", 0u);

    size_t sz = e.isDir ? 0 : e.size;
    snprintf(reinterpret_cast<char*>(_hdr + 124), 12, "%011o", (unsigned)sz);
    snprintf(reinterpret_cast<char*>(_hdr + 136), 12, "%011lo", (unsigned long)e.mtime);

    memset(_hdr + 148, ' ', 8);  // chksum field as spaces during sum
    _hdr[156] = e.isDir ? '5' : '0';

    memcpy(_hdr + 257, "ustar", 5);  // magic (without trailing space)
    _hdr[262] = 0;
    _hdr[263] = '0';
    _hdr[264] = '0';

    unsigned sum = 0;
    for (int i = 0; i < 512; i++) sum += _hdr[i];
    snprintf(reinterpret_cast<char*>(_hdr + 148), 8, "%06o", sum);
    _hdr[154] = '\0';
    _hdr[155] = ' ';

    if (!e.isDir && e.size > 0) {
      _curFile = _fs->open(e.fsPath, "r");
      if (!_curFile) return false;
    }
    return true;
  }
};

#endif  // TarStream_h
