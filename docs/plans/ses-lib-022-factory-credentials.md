# SES-LIB-022 — Cached factory-credential detection

Status: **agreed** with the SES firmware session and the owner (2026-10-13). Spec: `esp-rack-ses-light`
`docs/design/ses-light-design.md` §8 SES-LIB-022. The owner approved this task in the
core queue.

## 1. Motivation

Consumers that drive actuators block automation while stock credentials are in use.
Today the only test is `authenticate("admin","admin")`, which costs PBKDF2-SHA256 with
2000 iterations (≈ 100–250 ms per call). It also walks the users without the service lock,
and it exists only under `FT_SECURITY`.

## 2. API

```cpp
// SecurityManager (always declared; NullSecurityManager -> false)
virtual bool usesFactoryCredentials() const { return false; }
```

`SecuritySettingsService` overrides it. The call is a load from a `std::atomic<bool>`:
no lock and no hashing, so it is safe from any task.

## 3. Behaviour

- **Per user, in the security updater** (`SecuritySettings::update`), synchronously:
  - A **new plaintext password**, before it is hashed, sets the user's flag to
    (`username == FACTORY_ADMIN_USERNAME && pw == FACTORY_ADMIN_PASSWORD`) or
    (`username == FACTORY_GUEST_USERNAME && pw == FACTORY_GUEST_PASSWORD`).
  - An **unchanged row**, sent with a blank password or as the same hash round-trip,
    keeps its previous flag.
  - **Seeded defaults** (no file) set both factory rows to true.
  - **Cold load** of our own file takes the per-user `factory` flag from the file.
- **Overall:** the flag is true when any user has it. The updater stores it into the
  service's atomic before it returns, so the HTTP reply to a password change already
  sees the new value.
- **Persistence:** each users[] row gets `"factory": true|false`, a non-secret flag, so a
  boot needs no re-hashing.
- **Old files without the flag:**
  - Each such factory-username row is checked **once**, by hashing the factory password
    with the stored salt (PBKDF2, at most 2 × ≈ 200 ms) while the config loads in
    `begin()`, before HTTP.
  - `begin()` then saves the file, so later boots don't hash.
- **Before the config is loaded** the atomic is `true` (fail-safe): a consumer checking
  before security's `begin()` sees "stock".
- **`FT_SECURITY=0`:** the base default `false`, as the spec says. There are no
  credentials at all in that build. Owner's decision: firmware must work fully with or
  without the security module, so a consumer doesn't block anything there. In SES,
  stock passwords are only a warning; blocking automation is an optional hardening, off
  by default.

## 4. Budgets and acceptance

- Budget ≤ 0.5 KB.
- [build] C3/C6/S3; the security module compiles with `FT_SECURITY=0` (checked in a
  probe env).
- [bench-user] Changing the admin password flips the flag, with a read under 1 ms.
- The admin UI round-trips users[] and may drop the unknown `factory` key. A round-trip
  row keeps its prior flag (not the payload's), so that's harmless.

## 5. Commit plan

1. `feat(core,security)`: the API, the per-user flag, persistence, migration; plus this
   plan.
2. `docs`.

## 6. Implementation notes

- **Storage.** The flag is two bools (`factoryAdmin`, `factoryGuest`) instead of a
  per-user map, because only the two factory usernames can be "factory". A
  `std::map<String,bool>` cost 1.5 KB more on C3.
- **Measured** (C3 light-demo): +620 B flash, +0 B static RAM (the atomic and the
  migration flag are in .bss with the others). That is 120 B over the 0.5 KB budget.
  - C6 2,606,261; S3 2,701,750; C3 bundle 2,599,298; `FT_SECURITY=0` C3 2,563,618;
  - all SUCCESS, 0 warnings.
