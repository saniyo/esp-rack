#pragma once
#ifndef ESPRACK_FS_PATH_H
#define ESPRACK_FS_PATH_H

// The file manager's path rules, Arduino-free (host-tested; Str: String or
// std::string).
//   fsNormalise   "/a//b/./c/" -> "/a/b/c" ("/" stays): a path is checked
//                 (read-only prefixes) and used in the one form, so
//                 "/flash/./config" is "/flash/config". false for a relative
//                 path or a ".." segment (never resolved).
//   fsWithin      path is dir or lies under it (both normalised; "/" holds all).

template <class Str>
bool fsNormalise(const Str& in, Str& out) {
  const unsigned n = static_cast<unsigned>(in.length());
  if (!n || in[0] != '/') return false;
  Str o;
  unsigned i = 0;
  while (i < n) {
    while (i < n && in[i] == '/') ++i;  // empty segments
    const unsigned s = i;
    while (i < n && in[i] != '/') ++i;
    const unsigned len = i - s;
    if (!len) break;
    if (len == 1 && in[s] == '.') continue;
    if (len == 2 && in[s] == '.' && in[s + 1] == '.') return false;
    o += '/';
    for (unsigned k = s; k < i; ++k) o += in[k];
  }
  if (!o.length()) o += '/';
  out = o;
  return true;
}

template <class Str>
bool fsWithin(const Str& path, const Str& dir) {
  const unsigned d = static_cast<unsigned>(dir.length());
  if (d == 1 && dir[0] == '/') return true;
  if (path.length() < d) return false;
  for (unsigned k = 0; k < d; ++k) {
    if (path[k] != dir[k]) return false;
  }
  return path.length() == d || path[d] == '/';
}

#endif  // ESPRACK_FS_PATH_H
