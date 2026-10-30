# LIB-OTA-LOADER — firmware updates through a small loader (one app slot)

Status: **being built** (2026-10-30). Owner's idea (2026-10-29); the open questions
are decided (§9, changeable). Formats final (§4); partition tables, signing tool and
demo envs in; the loader (`loader/`), `system/OtaLoader.h` and the module hand-offs
being built. Bench tests (§8) before any field use.

## 1. Why

Two OTA slots need two copies of the app in flash. On 4 MB chips (ESP32, C3, C6) the
app no longer fits twice: `rs485-c3` is 2.76 MB today. With a small loader in its own
partition and one big app slot, the app gets 3 MB of 4 instead of 1.9, and a broken
app never bricks the device: the loader stays.

The same scheme runs in the field elsewhere (Tasmota's "safeboot" on ESP32): proven,
not new.

## 2. Layout

Two partition schemes. The board's `platformio.ini` picks one through its partition
table; the app finds the scheme at run time (`OtaLoader::scheme()`), so the same
firmware code runs in both and on every chip:

| Scheme | Partitions | Rollback | Downtime during an update |
|---|---|---|---|
| `dual` (as before) | `ota_0` + `ota_1` | yes | none (a restart) |
| `loader` | `factory` (the loader) + `ota_0` (the app) | no | the whole transfer |

Nothing switches by itself: a board stays `dual` until its env names a loader table.

The loader tables, `esp-rack/partitions/` — **one table per flash size, the same on
ESP32, C3, C6 and S3** (app partitions on 64 KB boundaries; checked with
`gen_esp32part.py --flash-size`):

| Table | nvs | otadata | factory (loader) | ota_0 (app) | spiffs (LittleFS) | coredump | Data kept from |
|---|---|---|---|---|---|---|---|
| `loader_4MB.csv` | 0x9000, 20 KB | 0xE000, 8 KB | 0x10000, 768 KB | 0xD0000, **3 MB** | 0x3D0000, 128 KB | 0x3F0000, 64 KB | `min_spiffs.csv` |
| `loader_8MB.csv` | same | same | 0x10000, 1 MB | 0x110000, **5.4 MB** | 0x670000, 1.5 MB | 0x7F0000 | `default_8MB.csv` |
| `loader_16MB.csv` | same | same | 0x10000, 1 MB | 0x110000, **11.5 MB** | 0xC90000, 3.4 MB | 0xFF0000 | `default_16MB.csv` |

- **Data kept from**: nvs, the file system and the core dump sit exactly where that
  dual table has them, so a board switched over (§10.4) keeps its settings and files.
- 4 MB: the app slot is `huge_app.csv`'s (3 MB) and the file system `min_spiffs.csv`'s
  (128 KB: config files; the web UI is in the app, `PROGMEM_WWW`). A consumer that
  needs more file system copies the table and moves the `ota_0`/`spiffs` boundary
  (64 KB steps).
- factory: 768 KB on 4 MB (a plain-HTTP loader); 1 MB on 8 and 16 MB, which leaves
  room for an HTTPS loader later without a new table (a new table needs a cable). To
  be confirmed by the first loader build of each chip: if one does not fit 768 KB,
  factory grows by 64 KB steps and `ota_0` shrinks by as much.
- No `phy_init` partition: the loader and the app keep the PHY data in the image (the
  IDF and Arduino default).
- The partition label of the loader is `loader`; code finds it by subtype (`factory`),
  never by label.

The second-stage bootloader boots what otadata selects; when that image is invalid (or
otadata blank) it falls back to `factory`. So a half-written app always lands in the
loader.

## 3. The loader

A separate small ESP-IDF project in esp-rack (`loader/`, project name
`esprack-loader`), not Arduino and not AsyncWebServer, to stay small. It only (details
in `loader/main/loader.h`):

- joins Wi-Fi (the handoff's station, else the one it keeps in IDF's storage; none:
  an AP `esprack-loader-XXXX`);
- downloads an image from a URL (pull), or takes one on a minimal upload page (push,
  admin password);
- writes it to `ota_0` in 4 KB sector steps (`FlashSteps`), hashing on the fly;
- checks header, chip ID, size, SHA-256 and the signature (§6), then sets `ota_0` to
  boot and restarts. The image's first 16 bytes (its magic) stay in RAM and go in
  last, once everything passed: `ota_0` never holds an unchecked image that boots,
  whatever stops the loader (a power cut included);
- writes a Result into NVS before every restart into the app.

It never writes its own partition. Updating the loader itself is a rare, deliberate
step (USB: `flash_loader.ps1 -LoaderOnly`; from the app later: the app runs from
`ota_0` and may rewrite `factory`, a power cut then breaks the loader, not the app).

## 4. Formats (final, version 1)

**Handoff and Result** — `lib/ESPRack/include/system/OtaHandoff.h` (Arduino- and
IDF-free; the app and the loader both build it; host-tested in
`test-host/test/test_ota_handoff`). NVS namespace `esprack_ota`, blobs `handoff` (app →
loader, 616 B) and `result` (loader → app, 152 B). Each blob is the struct as it lies
in memory (offsets pinned by `static_assert`), magic + version + CRC-32 over the bytes
before `crc`; another version is refused, never misread.

| Handoff | | Result | |
|---|---|---|---|
| `magic` 'OTH1', `version` 1 | | `magic` 'OTR1', `version` 1 | |
| `mode` | 0 none, 1 push (upload page), 2 pull (url) | `outcome` | 0 none, 1 ok, 2 failed before write (old app kept), 3 failed after write (the loader keeps trying), 4 aborted |
| `attemptsAllowed` | downloads after the first write before the page calls the update stuck (it goes on); 0: default | `from`, `to` [32] | versions |
| `url` [200], `token` [96] | pull: image URL, download token (sent on every retry) | `attempts` | tries |
| `size`, `sha256` [32] | size: the image's or the signed file's bytes (either is taken); hash over the image bytes; 0 / all zero: not given | `bytes`, `durationMs` | written, time in the loader |
| `sig` [72] + `sigLen` | DER ECDSA; 0: none (the loader looks for a trailer) | `lastError` [64] | "" none |
| `targetVersion` [32] | the version expected | `crc` | |
| `wifiSsid` [33], `wifiPass` [65], `useShared` | station, or the loader's stored one | | |
| `adminHash` [65] | hex SHA-256 of the upload page's password | | |
| `crc` | | | |

**Signed image** — one file: the image bytes, then a 128-byte trailer (little-endian):

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | magic `ESRKSIG1` |
| 8 | 4 | `imageLen`: the bytes before the trailer (= file length − 128) |
| 12 | 1 | `sigLen`: 1..72 (70-72 in practice) |
| 13 | 72 | the signature: ECDSA P-256 over SHA-256 of the image bytes, DER, zero-padded |
| 85 | 39 | zero |
| 124 | 4 | CRC-32 (IEEE, zlib's, `persist/Crc32.h`) of bytes 0..123 |

A file whose last 128 bytes are not such a trailer is an unsigned image (all of it is
the image); a trailer with the magic but a bad CRC, length or `sigLen` is refused. A
pull handoff carries the SHA-256 and the DER signature itself (the sidecar below).
Parser: `ota::parseTrailer` (OtaHandoff.h; the app and the loader); writer:
`tools/ota_sign.py` (byte-identical to `ota::encodeTrailer`, cross-checked).

**Sign sidecar** `firmware.signed.json`: `{file, image_size, chip, sha256, sig_der (hex),
key_id, dev_key}` — what a server puts into a pull handoff.

**Public key header** `loader/main/ota_pubkey.h` (generated by `ota_sign.py
export-pubkey`): `ESPRACK_OTA_KEY_DEV` (1: dev key), `ESPRACK_OTA_KEY_ID` (16 hex: the
first 8 bytes of SHA-256 over the point), `esprack_ota_pubkey[65]` (SEC1 uncompressed
`04|X|Y`) and `esprack_ota_pubkey_pem[]`. The committed file is a placeholder dev key
whose private half is gone.

## 5. Flow

**Manual (upload page).** In the loader scheme the update page offers "Restart into
update mode" (admin action `system.enterUpdateMode`). The app writes a push handoff,
sets `factory` to boot and restarts; the loader serves its upload page (admin
password); afterwards it boots the new app. No upload for 10 min: back to the old app
(Result "aborted").

**Mothership / auto-update (pull).**
1. The app learns about an update (version, URL, size, SHA-256, signature, a download
   token) and checks that it fits `ota_0`, matches the chip and board, and is
   newer.
2. It asks the application whether now is safe (`OtaLoader::setReadyCheck`, e.g. the
   JK gateway: inverter at 0 A, battery idle) and keeps the mothership's maintenance
   window.
3. It writes a pull handoff, sets `factory` to boot and restarts.
4. The loader connects and requests the image. **Until its first flash write nothing
   is broken.** No network, server down, 404, token refused, wrong header or chip: it
   sets `ota_0` to boot again and restarts into the old app, which reports the error
   (Result "failed before write").
5. From the first write on (point of no return), it retries with growing pauses (10 s,
   30 s, 2 min, then every 5 min) until a whole verified image is in. It never gives
   up: past the handoff's attempts its page calls the update stuck and it goes on
   every 5 min, so a server or network back after hours still gets the device its
   app; an upload on its page works all along. A power cut lands back in the loader
   (invalid `ota_0`) and it goes on.
6. Hash and signature good: boot `ota_0`, restart. The new app reads the Result once
   (`OtaLoader::lastResult()`) and reports it.

**Crash loop.** BootGuard counts crash boots; in the loader scheme a run of `limit + 2`
of them restarts into the loader's upload page (safe mode itself, at `limit`, also
offers "Restart into update mode"). The boot back from the loader is an intended one.

## 6. Security

- Images are signed: ECDSA P-256 over SHA-256 of the whole image (§4). With a
  signature, plain HTTP is enough for integrity and keeps the loader small; HTTPS is a
  later option (§9.1).
- **Keys** live outside every repository:
  - `ESPRACK_OTA_KEY` (or `--key`): a production key's path; it must exist (a missing
    one fails the build, it never falls back to a dev key);
  - else `%USERPROFILE%\.esprack\ota_sign_key.pem`: the production key (`ota_sign.py
    keygen`);
  - else `%USERPROFILE%\.esprack\ota_dev_key.pem`: the dev key, made on first use.
  A key file named `ota_dev_key.pem` is a dev key, every other one a production key.
- **Security never gates function.** A loader built with a dev key checks signatures
  but installs an unsigned or badly signed image anyway, with a "dev signing key"
  warning in its log and page; the app shows the same warning (`OtaLoader::devKey()`:
  the loader's version carries `devkey`). A loader built with a production key
  refuses an image without a good signature: then signatures are mandatory.
- Losing the production key means a device with that loader takes new images only
  over USB: keep an offline backup. An encrypted key works with the `cryptography`
  package (`ESPRACK_OTA_KEY_PASS`).
- With a production key a pull needs the SHA-256 and the signature in the handoff (the
  server's `x-SHA256` / `x-Signature`, the mothership's `sha256` / `sig`, from
  `firmware.signed.json`): the loader checks them before its first write and refuses a
  pull without them (the old app stays; the app already refuses such a handoff). A
  signed file's trailer is seen only once the image is written, so with a production
  key it serves uploads only; with a dev key a pull may rely on it.
- The upload page needs the admin password (its SHA-256 comes in the handoff); without
  one the page warns. The download token goes with every retry of the loader (§5), so
  a server keeps it valid until the image is installed (the device reports the new
  version), not just for the first request; a token refused after the first write is
  retried like any other failure.
- Secure Boot V2 / flash encryption: not used by this scheme; they stay possible on
  top (then the bootloader also checks the image).

## 7. Framework parts

| Part | Where |
|---|---|
| Handoff, Result, trailer | `system/OtaHandoff.h` |
| The scheme, entering the loader, the ready check, the last result | `system/OtaLoader.h` |
| Crash-loop entry | `system/BootGuard.h` |
| Upload page / auto-update / mothership hand-offs | `modules/upload-firmware`, `modules/auto-update`, `modules/mothership` |
| The loader | `loader/` (ESP-IDF) |
| Loader tables | `partitions/loader_{4,8,16}MB.csv` |
| Signing | `tools/ota_sign.py`, `tools/pio_ota_sign.py` |
| First install over USB | `tools/flash_loader.ps1` |
| Demo envs | `esp-rack-light-demo`: `loader-esp32`, `loader-c3`, `loader-c6`, `loader-s3` |

A dual board behaves exactly as before; the loader parts stay out of its manifest.

## 8. Bench tests (mandatory before any field use)

- Power cut before the first write, mid-write, at activation.
- Wrong image (other chip, too big, bad hash, bad signature), wrong token, server down,
  Wi-Fi down, Wi-Fi changes between handoff and loader.
- Manual upload with a wrong password.
- The loader's own update with a power cut.
- An app that boots and fails: no automatic rollback in `loader`; the loader is entered
  again by BootGuard (crash loop) or the boot button.
- Each of ESP32, C3, C6, S3; 4, 8 and 16 MB tables; a dual board unchanged.

## 9. Decisions (2026-10-30, the lead on the owner's brief; changeable)

1. **Transport:** plain HTTP for pull, plus the app-level ECDSA P-256 signature over
   the whole image. HTTPS optional later, not now (the 8/16 MB tables leave the room).
2. **Platforms (owner's requirement):** the loader is a framework mechanism selectable
   on **every** chip (ESP32, C3, C6, S3) and flash size (4, 8, 16 MB), chosen per board
   by its partition table. The same code on all chips; no chip gets it later. Nothing
   switches automatically: existing boards stay dual until their env picks a loader
   table.
3. **Entering the loader:** (a) the admin action "Enter update mode"; (b) a pull
   handoff from auto-update / mothership; (c) optionally a button held at boot (a GPIO
   chosen at build time, none by default); (d) BootGuard: safe mode offers "Enter
   update mode"; an automatic entry only after `limit + 2` crash boots in a row.
4. **Signing key:** outside the repo (§6), the public key compiled into the loader (the
   generated header). No key: a dev key is made on first use and the loader and the
   app warn "dev signing key"; a configured production key makes signatures mandatory.

## 10. How to use

### 10.1 A board on the loader scheme

```ini
[env:my-board-loader]
extends = env:my-board
board_build.partitions = ../esp-rack/partitions/loader_16MB.csv   ; the board's flash size
extra_scripts =
    ${env.extra_scripts}
    ../esp-rack/tools/pio_ota_sign.py
custom_ota_sign = yes
```

`pio run -e my-board-loader` then also writes `firmware.signed.bin` and
`firmware.signed.json` next to `firmware.bin`. The app's maximum size is `ota_0`'s.

### 10.2 The loader for the chip

First `python esp-rack/tools/ota_sign.py export-pubkey`: it writes
`loader/main/ota_pubkey.h` from the key `ota_sign.py` signs with (making the dev key on
first use) and leaves the file untouched when the key is the same. Then build the
loader for the chip (PowerShell or cmd): `pio run -d esp-rack/loader -e esp32c3`
(envs `esp32`, `esp32c3`, `esp32c6`, `esp32s3`; one build per chip serves every flash
size). `flash_loader.ps1` finds `loader/.pio/build/<env>/firmware.bin` by itself.
`pio run -d esp-rack/loader -e <chip> -t upload` writes the loader alone into `factory`
(`loader/upload_factory.py`: no bootloader, table, otadata or app). A new
production key needs a new loader on the devices (USB, or a loader update).

Other `ota_sign.py` commands: `keygen` (a production key; never overwrites one),
`sign IMAGE [-o OUT] [--json PATH]`, `verify FILE [--pub key.pem | ota_pubkey.h]`,
`info` (which key, dev or production). It uses the `cryptography` package
(PlatformIO's penv has it), else a built-in pure-Python P-256.

### 10.3 First install (USB)

```powershell
..\esp-rack\tools\flash_loader.ps1 -Chip c3 -Port COM7            # env loader-c3 of this project
..\esp-rack\tools\flash_loader.ps1 -Chip s3 -PioEnv my-board-loader -Port COM5
```

It writes bootloader, partition table, otadata (boots the app; `-StartIn loader`
boots the loader), the loader into `factory` and the app into `ota_0`, with the offsets
taken from the env's `partitions.bin`. It refuses a dual table, an image for another
chip, a loader that is not `esprack-loader`, and an image larger than its partition.
`-EraseAll` erases the whole flash first; `-LoaderOnly` writes only the loader; `-DryRun`
checks and prints. After the first install `pio run -t upload` updates the app alone
over USB (PlatformIO writes it at `ota_0`; it never writes the loader).

### 10.4 Switching a board between dual and loader

Only over USB: a partition table is never rewritten over the air (a power cut there
bricks the board).

Dual → loader (e.g. a 16 MB S3 on `default_16MB.csv`):
1. Add a loader env (§10.1) with `loader_16MB.csv`; build it and the loader for S3.
2. `flash_loader.ps1 -Chip s3 -PioEnv <env> -Port COMx` (no `-EraseAll`).
3. NVS (Wi-Fi, users, module settings) and the file system stay: the table keeps their
   offsets. A board coming from a table with other data offsets (e.g. a C3 on
   `huge_app.csv`) loses its file system: back its config up first (or `-EraseAll`
   for a clean start).

Loader → dual: `pio run -e <the dual env> -t upload` over USB writes the dual table,
otadata and the app at `ota_0`; the loader is overwritten. Settings and files stay
when the data offsets match (the pairs in §2).

What the loader scheme gives up:
- **Rollback**: dual keeps the previous image in the other slot; in loader the old app
  is gone from the first write of the new one. A new app that fails is caught only by
  BootGuard's crash-loop entry or the boot button, then a manual or pull update.
- **Uptime during an update**: the device does its job again only once the whole image
  is in (dual: one restart).
- **Updates without the loader**: ArduinoOTA (`OTAModule`, espota) has no free slot to
  write into, so the module leaves it off there (its page says so;
  `OtaWriter::freeSlotSize()` is 0); updates go through the upload page, auto-update
  or the mothership.

## 11. Open points

- First loader build per chip: factory size (§2), loader size in the table above.
- The boot button (§9.3 c): pin and hold time per board.
- The flavor (`compute_flavor.py`) does not carry the scheme: an update server can
  give a dual board a loader-env build and the other way round. The app is the same
  code, so it works when the image fits the board's slot (checked before the
  transfer); a slot-size field in the update check would let the server choose.
