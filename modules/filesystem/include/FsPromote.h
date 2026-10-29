#pragma once
#ifndef ESPRACK_FS_PROMOTE_H
#define ESPRACK_FS_PROMOTE_H

// An upload's promotion, <target>.partial -> <target>, one flash operation
// per call (a gate slot each), so that a restart or a power loss between
// calls never leaves the old file and the new one both gone:
//   - a volume that renames over a file in one atomic step (LittleFS): one
//     rename; readers see the old file until then;
//   - else (FAT refuses a rename onto a file): the old one steps aside as
//     <target>.partial.old, the new one is renamed in, the old one removed.
//     At boot restoreOld() puts the old one back if the new one never
//     arrived, else removes it.
// A hash mismatch keeps the upload as <target>.corrupt (a stale one is
// replaced); the target is untouched.
//
// Arduino-free (host-tested). Io supplies:
//   using Str;  bool replaces();  bool exists(path);  bool remove(path)
//   FsResult rename(from, to)    (FAT: fails onto an existing file)

#include <stdint.h>

#include "FsWriteQueue.h"  // FsResult

template <class Io>
struct FsPromote {
  using Str = typename Io::Str;
  enum class Next : uint8_t { More, Done, Failed };

  static Str partialOf(const Str& target) { return target + ".partial"; }
  static Str oldOf(const Str& target) { return target + ".partial.old"; }
  static Str corruptOf(const Str& target) { return target + ".corrupt"; }

  // at: where it is (0 at the start; kept by the caller between calls).
  // err: why it failed (a hash mismatch: OK, the caller knows).
  static Next step(Io& io, uint8_t& at, const Str& target, bool hashOk, FsResult& err) {
    const Str partial = partialOf(target);
    switch (at) {
      case 0:
        if (!hashOk) {
          const Str bad = corruptOf(target);
          if (!io.replaces() && io.exists(bad)) {
            (void)io.remove(bad);
            at = 1;
            return Next::More;
          }
          (void)io.rename(partial, bad);
          return Next::Failed;
        }
        if (io.replaces() || !io.exists(target)) return finish(io.rename(partial, target), err);
        // Aside first. A stale one from an earlier try is the older copy:
        // removed (a call of its own), then this step again.
        if (io.exists(oldOf(target))) {
          (void)io.remove(oldOf(target));
          return Next::More;
        }
        if (!ok(io.rename(target, oldOf(target)), err)) return Next::Failed;
        at = 2;
        return Next::More;
      case 1:
        (void)io.rename(partial, corruptOf(target));
        return Next::Failed;
      case 2:
        if (!ok(io.rename(partial, target), err)) {
          (void)io.rename(oldOf(target), target);  // the old one back
          return Next::Failed;
        }
        at = 3;
        return Next::More;
      default:
        (void)io.remove(oldOf(target));  // else at boot: the target is there
        return Next::Done;
    }
  }

  // At boot, for each <target>.partial.old: the promotion did not finish.
  // true: something was done.
  static bool restoreOld(Io& io, const Str& old) {
    const unsigned n = static_cast<unsigned>(oldOf(Str()).length());
    const unsigned len = static_cast<unsigned>(old.length());
    if (len <= n) return false;
    Str target;
    for (unsigned k = 0; k < len - n; ++k) target += old[k];
    if (io.exists(target)) return io.remove(old);
    FsResult err = FsResult::OK;
    return ok(io.rename(old, target), err);
  }

 private:
  static bool ok(FsResult r, FsResult& err) {
    if (r == FsResult::OK) return true;
    err = r;
    return false;
  }
  static Next finish(FsResult r, FsResult& err) { return ok(r, err) ? Next::Done : Next::Failed; }
};

#endif  // ESPRACK_FS_PROMOTE_H
