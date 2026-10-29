# LIB-OTA-LOADER — firmware updates through a small loader (one app slot)

Status: **brief**, owner's idea (2026-10-29), queued after GW-UI-002. Nothing built yet.

## 1. Why

Two OTA slots need two copies of the app in flash. On 4 MB chips (ESP32, C3, C6) the
app no longer fits twice: `rs485-c3` is 2.76 MB today. With a small loader in its own
partition and one big app slot, the app can use ~70-75 % of the flash instead of ~45 %,
and a broken app never bricks the device: the loader stays.

The same scheme runs in the field elsewhere (Tasmota's "safeboot" on ESP32): proven,
not new.

## 2. Layout

Two partition schemes per chip. The board's `platformio.ini` picks one; the update API
is the same in both:

| Scheme | Partitions | Rollback | Downtime during an update |
|---|---|---|---|
| `dual` (today) | `ota_0` + `ota_1` | yes | none |
| `loader` (new) | `factory` (loader) + `ota_0` (app) | no | the whole download |

`loader` layout for 4 MB (sizes to be fixed after the first loader build):

| Partition | Size | Content |
|---|---|---|
| nvs | 20 KB | as today; also the handoff record (§4) |
| otadata | 8 KB | which partition boots |
| factory | ~0.7 MB (HTTP) / ~1.0 MB (HTTPS) | the loader |
| ota_0 | ~2.9-3.1 MB | the app |
| littlefs, coredump | the rest | as today |

The ROM bootloader boots `ota_0` when otadata says so and the image is valid, else
`factory`. So a half-written app always lands in the loader.

## 3. The loader

A separate small ESP-IDF project in esp-rack (`loader/`), not Arduino and not
AsyncWebServer, to stay small. It only:

- joins Wi-Fi (credentials from the handoff record or a shared NVS copy; AP fallback
  with the device's AP settings);
- downloads an image from a URL (pull), or takes one on a minimal upload page
  (push, admin password);
- writes it to `ota_0` in 4 KB sector steps (the core's `OtaWriter` logic, `FlashSteps`),
  hashing on the fly;
- checks size, chip ID and header, SHA-256 and the signature (§6), then sets the boot
  partition to `ota_0` and restarts;
- writes its result into NVS for the app to report.

It never writes its own partition. Updating the loader itself is a separate, rare,
deliberate step from the app (the app runs from `ota_0`, so it may rewrite `factory`; a
power cut then breaks the loader but not the app).

## 4. Handoff record (NVS, written by the app, read by the loader)

`{ magic, version, mode (pull|push), url, token, size, sha256[32], signature?,
target version, attempts allowed, wifi (ssid, pass) or "use shared", ap fallback,
admin hash }`, and the result written back: `{ outcome, from, to, attempts, bytes,
duration, last error }`. CRC-guarded, one record; `NvsRecord` style.

## 5. Flow

**Manual (upload page).** The page shows "update mode: restart into the loader". The app
writes a push handoff, sets the boot partition to `factory` and restarts; the loader
serves its upload page; afterwards it boots the new app.

**Mothership / auto-update (pull).**
1. The app learns about an update (version, URL, size, SHA-256, signature, a one-time
   download token) and checks that it fits `ota_0`, matches the chip and board, and is
   newer.
2. It asks the application whether now is safe (a consumer hook, e.g. the JK gateway:
   inverter at 0 A, battery idle) and keeps the mothership's maintenance window.
3. It writes a pull handoff, sets the boot partition to `factory` and restarts.
4. The loader connects and requests the image. **Until its first flash write nothing is
   broken.** No network, server down, 404, token refused, wrong header: it sets the boot
   back to `ota_0` and restarts into the old app, which reports the error.
5. From the first write on (point of no return), it retries with growing pauses
   (10 s, 30 s, 2 min, ...) until a whole verified image is in. After N failed attempts
   it waits for a manual upload. A power cut lands back in the loader (invalid
   `ota_0`) and it goes on.
6. Hash and signature good: boot `ota_0`, restart. The new app reads the result record
   and reports to the mothership. Optional: the loader posts short progress/status to
   the mothership with the same token.

## 6. Security

- The loader's upload page needs the admin password (hash in the handoff record or a
  shared NVS copy).
- Images are signed (app-level ECDSA, or Secure Boot V2 where it is on). With a
  signature, plain HTTP is enough for integrity and keeps the loader ~0.3 MB smaller
  than with TLS; HTTPS is an option per board.
- The download token is one-time and short-lived.

## 7. Framework changes

- `FirmwareUpdate` / `OtaWriter`: `scheme()` (dual | loader); in `loader` the upload
  page and auto-update hand off instead of writing.
- New `system/OtaLoader.h`: `bool enter(const Handoff&)`, `lastResult()`.
- mothership and auto-update: pull handoff; the "safe now?" consumer hook.
- Partition CSVs per chip for `loader`; build scripts to build and flash the loader.
- UI: the update page knows the scheme, shows the downtime warning and the last result.
- Same mechanism on every chip; the scheme is a board choice, not a feature branch.

## 8. Bench tests (mandatory before any field use)

- Power cut before the first write, mid-write, at activation.
- Wrong image (other chip, too big, bad hash, bad signature), wrong token, server down,
  Wi-Fi down, Wi-Fi changes between handoff and loader.
- Manual upload with a wrong password.
- The loader's own update with a power cut.
- An app that boots and fails: no automatic rollback in `loader`; the loader is entered
  again from the boot counter (BootGuard safe mode) or by a button held at boot.

## 9. Open questions for the owner

1. HTTP + signature (smaller loader) or HTTPS in the loader?
2. Which boards switch to `loader`: all 4 MB ones, or only those whose app no longer
   fits twice?
3. A boot-time button / BootGuard safe mode entering the loader: wanted?
4. Who signs images (key storage, the build step)?

Effort: about 2-4 days plus bench tests. The riskiest part is the loader itself: a bug
there cannot be fixed without a cable.
