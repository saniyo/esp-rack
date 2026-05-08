# Plan: Mothership / Fleet Management Subsystem for ESP-Rack

## Context

Сьогодні ESP-Rack має `AutoUpdateModule` який ходить **plain HTTP** на
жорстко зашитий update-сервер раз на N хвилин і перевіряє чи є нова
прошивка. Жодних device-identity механізмів, жодного зворотного каналу
(сервер не може ініціювати дію), жодного захисту PKI, жодного способу
дотягнутися до пристрою за NAT/CGNAT/firewall'ом.

Потрібна повноцінна "Tesla-style mothership" модель:

1. **Кожний пристрій має унікальний X.509 cert+key**, отриманий при
   першому boot через CSR (одноразовий bootstrap token з UI).
2. **Усі канали до сервера — mTLS HTTPS** через спільну інфру
   (`TLSContextService`).
3. **Періодичний check-in** до сервера за політикою; сервер повертає
   список команд (`update`, `openTunnel`, `renewCert`, custom).
4. **Прямий доступ навіть за gateway** — за командою сервера пристрій
   піднімає WireGuard-тунель до VPS, сервер отримує route до пристрою.
5. **Автоматичне cert rotation** — сервер ініціює renewCert до закінчення
   дії, пристрій робить новий CSR + atomic swap в зашифрованому storage.
6. **Server-side (Java + Next.js SSR)** будується ПІД ці device-API
   контракти, тому контракти мусять бути зафіксовані до старту server-side
   роботи.

**Що залишається БЕЗ змін** — використовуємо існуючі примітиви як є:
- `Module.h` — сам контракт (describe/onInstall/onBegin/onLoop) ідеально
  лягає під нові модулі без жодних правок
- `SecretsVault` (AES-128-CBC + eFuse-derived key) — вже шифрує
  text-поля у `ENC:` форматі; PEM-сертифікати це теж текст, тому
  жодного розширення під бінарь не треба
- `ConfigDelegate` + auto-discovery secret keys — те що робив SecretField
  refactor минулого тижня, працює на новій конфігурації cert.json
  з коробки
- `mqtt`, `telegram`, `ntp`, `wifi`, `presence` модулі — **зовсім не
  чіпаємо**. Mothership-канал — це окремий HTTPS до окремого сервера,
  ортогонально до telegram-bot чи mqtt-broker які лишаються для
  їхніх існуючих кейсів

**Що буде модифіковано** — лише:
- `AutoUpdate` (Phase 2): `WiFiClient` → `WiFiClientSecure` через
  TLSContextService; внутрішній polling-таймер видаляється, бо
  оновленням тепер керує Mothership через action-команди. `performUpdate()`
  залишається public API і викликається з MothershipService

Audit findings з phase 1 нижче в **Critical files**.

---

## Roadmap

### Phase 1 — PKI Foundation ✅ COMPLETE (2026-05-08)

End-to-end verified on ESP32-C6: bootstrap-token enrollment via
`POST /api/v1/enroll` against the Python mock mothership succeeds,
all four sensitive fields (cert / key / ca_bundle / recovery_token)
land ENC:-encrypted on disk through SecretsVault. Found-and-fixed
during testing: 4 KB → 8 KB ConfigDelegate buffer (encryption walk
expanded PEMs past pool capacity, dropped last assignment to JSON
null), readonly secret-form-POST guard against form re-saves
clobbering the loaded PEMs. Ready to layer Phase 2 mothership client
on top.

**Goal:** Пристрій має унікальний keypair + X.509 cert у зашифрованому
storage; будь-який модуль може отримати готовий `mbedtls_ssl_context*`
з вшитим cert через спільний сервіс.

**New modules:**
- **`TLSContextService`** (priority 10, just after `wifi`): власник
  mbedtls primitives — CA-bundle, device cert, device private key,
  server CN whitelist. Експонує `mbedtls_ssl_config*` для клієнтів +
  helpers `attachToWiFiClientSecure(WiFiClientSecure&)` /
  `buildHttpClient(host)`. Late-bind через `App::tls()`.
- **`CertManagerModule`** (priority 12, requires `tls`): cert lifecycle
  — `hasValidCert()`, `daysUntilExpiry()`, `rotate()`, `enroll(token)`,
  signals `onCertChanged` для пере-attach всіх відкритих TLS-клієнтів.

**Crypto algo:** **ECDSA-P256** (зафіксовано). Цільовий fleet — ESP32,
ESP32-S2, ESP32-S3, ESP32-C3, ESP32-C6 (esp8266 виключно з підтримки).
ECDSA-P256 виграє на всіх осях:
- C3/C6 мають hw-acceleration ECC peripheral; на S2/S3/classic ECDSA-P256
  у software ~50-100ms (толерантно). RSA-2048 у software на C3/C6
  навпаки ~300-500ms (болісно при частих handshake).
- Менший cert (~400 B PEM vs RSA-2048 ~1.7 KB) — економія
  SecretsVault-ємності + heap при паралельних TLS-сесіях.
- Менша signature на wire (64 B vs 256 B) — економія байтів при check-in.

**Storage:** X.509 у вигляді PEM string (~400 B для ECDSA-P256) → PEM
ASCII вже text-safe → пишемо у `ENC:` поля SecretsVault як зараз для
tokens. Private key те саме (~250 B PEM EC PRIVATE KEY block).
Файл: `/config/cert.json` з полями `device_cert_pem`, `device_key_pem`,
`ca_bundle_pem`, `serial`, `not_after_ts`. Перші три — secret.

**Bootstrap flow:**
1. На first boot CertManager бачить що cert відсутній → переходить у
   `state=NeedsEnrollment`, відкриває UI tab "Enrollment"
2. Оператор вводить **bootstrap token** — **time-bound (зафіксовано)**:
   token має `valid_until` мітку (default 24 години з моменту видачі
   у server admin UI). Якщо оператор згенерував і не використав за
   добу — token недійсний, треба згенерувати новий. Менш ризиковано
   ніж single-use (single-use псується якщо мережа зірвалась посередині
   enrollment і operator думає що використав а насправді ні), але все
   ж обмежений у часі (single-use у "довічному" виконанні залишає
   ризик якщо token витік). Поле text-secret, persisted у тому ж
   `/config/cert.json` лише до моменту успішного enrollment, потім
   стирається.
3. CertManager генерує ECDSA P-256 keypair (mbedtls), будує CSR
   (CN = `device-<MAC>`), POST-ить на `/api/v1/enroll` з
   `Authorization: Bearer <bootstrapToken>`. Сервер перевіряє token,
   підписує CSR, повертає cert + CA bundle.
4. CertManager пише cert+key+CA → ConfigDelegate → SecretsVault шифрує →
   диск. State → `Ready`. Bootstrap token стирається (one-time).
5. При наступних boot — cert уже є, enrollment skip.

**Critical files to create / modify:**
- `lib/ESPRack/include/TLSContextService.h` (new)
- `lib/ESPRack/src/TLSContextService.cpp` (new)
- `modules/cert-manager/include/CertManagerService.h` (new)
- `modules/cert-manager/src/CertManagerService.cpp` (new)
- `modules/cert-manager/include/CertManagerModule.h` (new)
- `lib/ESPRack/include/App.h` — додати `ITLSProvider* tls()` +
  `setTls()` за патерном `mqtt()` / `telegram()`
- `factory_settings.ini` — додати `BOOTSTRAP_ENROLL_URL` (default:
  `https://mothership.local/api/v1/enroll`)
- `features.ini` — `FT_CERT_MANAGER=1` (новий gate)

**Reuse (з audit):**
- `ConfigDelegate<T>` + `SecretsVault` — `ENC:` prefix вже працює для
  тексту (PEM сертів), AES-128-CBC + eFuse-derived key, файл:
  `lib/ESPRack/include/SecretsVault.h:1-100`
- `Module.h` контракт (`describe`/`onInstall`/`onBegin`/`onLoop`):
  `lib/ESPRack/include/Module.h:110-132`
- App late-bind pattern: `lib/ESPRack/include/App.h:77-95`
  (`telegram()` / `mqtt()` getters)

**Acceptance:**
- Пристрій з порожнім cert.json після введення валідного bootstrap-token
  отримує cert, перезавантажується (опціонально), cert.json на диску
  має `ENC:` prefix для трьох secret полів.
- `app->tls()->buildHttpClient("...")` повертає робочий HTTPS-клієнт
  який в`mTLS-handshake` пред'являє device cert.
- Mock mothership endpoint підписує CSR і повертає валідний chain.

---

### Phase 2 — Mothership Client (HTTPS poll + command channel)

**Goal:** Один централізований модуль робить періодичний check-in з
сервером через `TLSContextService` і виконує отримані команди. Існуючий
`AutoUpdateModule` стає одним з виконавців — приймає `update`-команду
від мазершіпа замість самостійного polling.

**New module:**
- **`MothershipModule`** (priority 35, requires `tls`, `cert-manager`,
  `auto-update`): heartbeat loop + command dispatcher.

**Cadence policy (зафіксовано):** **Adaptive polling** (pure polling, не
long-poll). Default інтервал з `MOTHERSHIP_INTERVAL_MIN` (5 хв). Якщо у
відповіді `actions[]` непорожній — наступний check-in через 10 секунд
(можливо є batched команди ще). Інакше повертаємось до базового
інтервалу. Pure polling (не long-poll) обраний бо:
- Server-side значно простіший (один stateless POST handler замість
  тисяч open async-connections)
- На мобільних 4G / CGNAT long-poll connection часто рветься
  silently через "stale connect" — device чекає, провайдер уже зрізав
- Реактивність 5 хв worst-case + 10s burst після першої команди дає
  еквівалентний UX для 99% сценаріїв

**Protocol (device ↔ mothership):**
- `POST /api/v1/checkin` — adaptive cadence (5 хв default + 10s burst).
  Request: `{deviceId, fwVer, hwVer, uptimeSec, freeHeap, lastErrors[]}`
  Response: `{actions: [{type, params}], nextCheckInSec}`
- Action types:
  - `update` → `{url, version, sha256}` → передається у
    `AutoUpdateService::performUpdate()` (існуючий шлях, просто на
    HTTPS через TLS context)
  - `openTunnel` → `{peerPubKey, endpoint, allowedIps}` → передається
    у WireGuardModule (Phase 3, до тоді no-op)
  - `renewCert` → `{enrollUrl}` → CertManager.rotate() (Phase 4)
  - `setConfig` → `{key, value}` → допомога діагностиці в полі
  - `reboot` → ESP.restart()
  - `log` → `{level, msg}` → пише в WS-діагностику

**Existing AutoUpdate refactor:**
- Видалити з `AutoUpdateService` polling loop (line 174-202).
- Залишити `performUpdate(url)` як public API.
- `AutoUpdateModule` залишається як OTA executor; Mothership delegate-ить
  оновлення через `app->autoUpdate()->performUpdate()`.
- HTTP→HTTPS — `WiFiClient` (line 220) → `WiFiClientSecure` через
  `app->tls()->attachToClient(...)`.

**UI:**
- New tab "Mothership" — Status (last check-in, next check-in,
  cert expiry days, command log) + Settings (enrolled status, manual
  "Check now" action, server endpoint readonly з `BOOTSTRAP_*`).

**Critical files to create / modify:**
- `modules/mothership/include/MothershipService.h` (new)
- `modules/mothership/src/MothershipService.cpp` (new)
- `modules/mothership/include/MothershipModule.h` (new)
- `modules/auto-update/src/AutoUpdateService.cpp` — видалити внутрішній
  polling timer, додати public `performUpdate(url, sha256)` що приймає
  виклики з боку Mothership
- `modules/auto-update/include/AutoUpdateService.h:188` — клас
  AutoUpdateService stays, signature змінюється

**Reuse:**
- `IMqttProvider` patern для command-channel — те саме, але через
  HTTPS + CSR-signed mTLS instead of broker
- `WebActionSpec` + `WebFeatureSpec` для UI tab
- `httpUpdate` / `ESPhttpUpdate` бібліотека для виконання OTA (line 265
  в `AutoUpdateService.cpp`)

**Acceptance:**
- Пристрій з валідним cert робить check-in кожні N хв; mock-сервер
  бачить mTLS-handshake з його CN.
- Сервер відповідає `{actions: [{type:"update", params:{...}}]}`
  → пристрій завантажує новий firmware і перезавантажується.
- Якщо cert експірований → check-in fails з TLS-помилкою → пристрій
  входить у `state=NeedsRenewal` і чекає manual / boostrap-fallback.

---

### Phase 3 — WireGuard Tunnel (on-demand direct access)

**Goal:** Пристрій за NAT/firewall може отримати команду від мазершіпа
"відкрий тунель", після чого VPS бачить пристрій по wg-IP і може робити
direct HTTPS / SSH / debug-port.

**New module:**
- **`WireGuardModule`** (priority 25, requires `wifi`): WireGuard
  client. Не активний за замовчуванням; піднімається лише по команді
  `openTunnel` від мазершіпа з ad-hoc peer config.

**Lib:** `ciniml/WireGuard-ESP32` (perd-tested, ESP32-C6 supported).

**Endpoint (зафіксовано):** **Single VPS** — один сервер
`wg.mothership.example.com`, всі пристрої коннектяться туди. Ємність
~5000 пристроїв на одній VPS — для старту достатньо. Перехід на
multi-VPS / GeoDNS — окрема Phase коли буде потрібно (>5000 пристроїв
або географічна оптимізація).

**MTU (зафіксовано):** **1280** як hard-coded default. Працює навіть на
агресивних NAT/CGNAT/IPv6-transition мережах. Втрата ~10% throughput —
прийнятна для діагностичного тунелю (HTTP/SSH, не masive data). Для
gateway-пристроїв з відомою ISP-конфігурацією оператор може вручну
підняти до 1380 через UI override.

**Lifecycle:**
- Default: stopped.
- Mothership action `openTunnel`: ефемерний keypair (генерується щоразу
  для forward secrecy), receive endpoint + peer pubkey, allowed-IPs
  з командного payload-у. Тунель up.
- Idle timeout (config'ured, default 10 min без traffic) → тунель down.
- Operator override (UI Settings tab "Always on for diagnostics" toggle)
  → постійний тунель (cost: батарея, CPU).

**Critical files to create:**
- `modules/wireguard/include/WireGuardModule.h` (new)
- `modules/wireguard/src/WireGuardService.cpp` (new)
- `platformio.ini` — додати `lib_deps += ciniml/WireGuard-ESP32@^0.2.0`

**Reuse:**
- ConfigDelegate для збереження ефемерного wg-state на runtime
  (не персистити приватний key — генерувати щоразу при handshake)

**Acceptance:**
- Mothership-test: команда `openTunnel` → за 5-10 сек пристрій
  пінгується по wg-IP з VPS.
- Без command — `wg show` на VPS не бачить пристрій.

---

### Phase 4 — Cert Rotation (proactive + gray-zone recovery)

**Goal:** Cert не експіровується мовчки — мазершіп ініціює renewal
до 30 днів до expiry; пристрій робить новий CSR, atomic swap. Якщо
проактивне rotation чомусь не вдалося і cert експірувався (device був
оффлайн ≥ 30 днів) — пристрій падає в "gray zone" і чекає approval від
оператора, без необхідності повторного bootstrap-token-flow.

**Strategy (зафіксовано): Proactive primary + gray-zone reactive fallback.**

#### 4a. Proactive rotation (нормальний шлях, ~99% випадків)

- Mothership scheduler (cron, щодня) знаходить пристрої з
  `not_after - now < 30d` → ставить action `renewCert` у per-device
  command queue
- На наступному check-in device отримує цю action → виконує rotate flow
- Все відбувається доки старий cert ще валідний → mTLS-handshake під
  час процесу не падає → невидимо для оператора

**`CertManagerService::rotate()` flow:**
1. Generate новий ECDSA-P256 keypair (старий зберігаємо в RAM)
2. CSR з новим pubkey, тим самим CN
3. POST на `/api/v1/renew` через mTLS під СТАРИМ cert (mutual auth
   доводить що це той самий device — bootstrap-token не потрібен)
4. Server підписує, повертає новий cert chain
5. **Atomic verify-then-swap:**
   - Записати новий cert+key у `/config/cert.tmp.json`
   - Спробувати handshake з checkin endpoint використовуючи новий cert
   - Якщо ОК → перезаписати `/config/cert.json` з новим, видалити tmp
   - Якщо handshake fails → відкинути новий, лишити старий, повторити
     спробу на наступному check-in
6. Forward secrecy: старий приватний key стирається з RAM після
   успішного swap

#### 4b. Gray-zone recovery (fallback коли proactive fail)

**Сценарії коли потрапляє сюди:**
- Device був оффлайн понад 30 днів (Wi-Fi дім вимкнено, привезли
  з ремонту назад) → cert уже експірований до того як прийшла
  renewCert команда
- Cert був пошкоджений / стертий (flash corruption, factory reset
  з частковою втратою) → device не може більше предʼявити mTLS
- Device з якоїсь причини відкинув новий cert під час swap і потім
  старий теж пропав

**Recovery flow (новий endpoint, окремий від `/enroll`):**
1. Device при старті бачить що cert.json відсутній/мертвий
2. Замість того щоб вимагати від оператора bootstrap-token
   (як на first-enrollment), device пробує **recovery endpoint**:
   `POST /api/v1/recover` через server-side TLS only (CA pinned,
   client cert НЕ предʼявляється)
3. Body: `{deviceId: <MAC>, hwInfo: {...}, lastKnownSerial:
   "<old-cert-serial>", recoveryToken: "<persisted-secret>"}`
4. `recoveryToken` — це окремий persistent secret який device генерує
   при першому успішному enrollment (Phase 1) і зберігає у тому ж
   шифрованому cert.json. Він НЕ змінюється при rotate. Якщо втрачений
   разом з cert — оператор робить повний re-enroll з bootstrap-token
5. Server отримує запит → перевіряє що такий device ID існує в базі →
   ставить його у статус `gray_zone` із timestamp + remote IP +
   recoveryToken match
6. **Operator action:** в admin UI зʼявляється "Pending recovery"
   список з картками "device-XYZ хоче відновити cert, останнє active
   було N днів тому, IP: ..." з кнопками `Approve` / `Reject`
7. Оператор перевіряє що це справді той пристрій (наприклад зірочно
   звіряє IP з очікуваним gateway або просто довіряє recoveryToken)
   → Approve
8. На наступному `/recover` polling-call (device робить це раз на 60с
   у gray-zone state) device отримує `{approved: true,
   freshEnrollUrl: "..."}` і робить звичайний CSR-enroll знову

**Що device робить у gray-zone state:**
- Статус UI: "Recovery — pending operator approval"
- Жодного MQTT/Telegram/AutoUpdate (бо немає mTLS до сервера для
  отримання команд)
- Лише polling `/api/v1/recover` раз на 60с
- Wi-Fi+UI продовжують працювати — оператор може зайти на
  локальну адресу і вручну ввести bootstrap-token якщо хоче зробити
  full re-enrollment минаючи gray-zone

**Recovery vs Bootstrap (різниця):**

| Аспект | Bootstrap (first-time) | Recovery (lost-cert) |
|---|---|---|
| Endpoint | `/api/v1/enroll` | `/api/v1/recover` |
| Auth | One-time bootstrap token | recoveryToken persisted з попереднього enroll |
| TLS | Server-side TLS only | Server-side TLS only (mTLS неможливий — cert мертвий) |
| Operator role | Видає token заздалегідь | Approve у admin UI коли запит прилетів |
| Scenario | Новий пристрій з factory | Знаний пристрій з пошкодженим cert |

#### Critical files

- `modules/cert-manager/src/CertManagerService.cpp` — додати:
  - `rotate()` (proactive flow)
  - `enterGrayZone()` + `pollRecovery()` (recovery flow)
  - `verifyHandshakeWithCandidate(cert, key)` helper
  - `_recoveryToken` persistent field у cert.json
- `modules/mothership/src/MothershipService.cpp` — handler для
  `renewCert` action → виклик `app->cert()->rotate()`

#### Acceptance

- **Proactive:** Manual server-side trigger renewCert на пристрій з cert
  що валідний ще 60 днів → пристрій перезаписує cert+key, наступний
  check-in йде з новим CN-serial. Старий cert не пошкоджений якщо
  rotation посередині fails.
- **Gray-zone:** на тестовому стенді стираємо cert.json вручну → device
  при бутапі не виходить у requirement-bootstrap-token state, а заявляє
  про себе на `/recover`. У admin UI бачимо запис у "Pending recovery".
  Approve → device отримує новий cert + переходить у normal operation
  без operator-side ручного введення token-а.
- **Edge case:** recoveryToken пошкоджений → device fallback'ує на
  bootstrap-token UI (повний re-enrollment), як для нового пристрою.

---

### Phase 5 — Server-side API contracts (specification only, no code)

**Goal:** Зафіксувати OpenAPI / proto-spec до старту Java-роботи,
щоб device-side modules (Phases 1-4) не лежали без виконавця.

**Endpoints:**
- `POST /api/v1/enroll` — bootstrap CSR signing
- `POST /api/v1/checkin` — heartbeat + command pull (mTLS-only)
- `POST /api/v1/renew` — cert rotation (mTLS під старим cert)
- `GET  /api/v1/firmware/{deviceId}/{version}` — OTA download (mTLS)
- (server-side internal admin API — не пов'язано з device, окремо)

**Output:** `docs/mothership-api.md` — повна специфікація з прикладами
JSON request/response для кожного endpoint, error codes, retry policy.

**Server-side roadmap (out of scope for device repo, але треба щоб
плани узгоджувались):**
- Java + Spring Boot 3 (mTLS handler через bouncy-castle, CSR signing
  через PKCS#10)
- Postgres (devices, certs, command_queue, audit_log)
- Next.js SSR admin UI (operator: видавати bootstrap tokens, переглядати
  fleet, надсилати commands, viewing tunnels)

---

## Open questions (refine при початку кожної phase)

| Phase | Open question |
|-------|--------------|
| 4 | Recovery token — generate-once-on-first-enroll чи rotate з cert? Якщо rotate — ризик втрати разом з cert; якщо persist — постійне local secret що довго живе |
| 4 | Operator-approve у gray-zone — strict (manual click завжди) чи trust-on-first-IP (auto-approve якщо IP той самий що останній active check-in)? |
| 5 | Server-side: SQL row-locking при concurrent renewCert command-queue mutations |

---

## Verification flow (end-to-end)

Після всіх phase device-side:
1. Flash чисту прошивку → boot → UI показує "Enrollment required"
2. Оператор клацає "Generate token" в server admin UI → копіює → вводить в device UI
3. Device → mothership → cert + CA → reboot → "Mothership: Connected" зелений
4. Server admin UI → click device → "Trigger update" → device update OTA → reboot з новою версією
5. Server admin UI → click device → "Open tunnel" → SSH/HTTPS direct до device по wg-IP
6. 30 днів до cert expiry → server auto-rotation → device cert.json онов'юється без operator action
7. Power-cycle device → all of the above survives reboot (cert persists, tunnel re-stablished
   on next openTunnel command)

---

## Critical files (audit summary, file:line)

- `modules/auto-update/src/AutoUpdateService.cpp:174-265` — existing
  HTTP polling loop (видалити в Phase 2) + `performUpdate()` (зберегти)
- `modules/auto-update/AutoUpdateService.h:188` — class signature
  (`AutoUpdateService : public StatefulService<AutoUpdateSettings>`)
- `lib/ESPRack/include/Module.h:110-132` — Module contract
  (describe/onInstall/onBegin/onLoop/onShutdown)
- `lib/ESPRack/include/App.h:77-95` — late-bind pattern для
  IMqttProvider/ITelegramProvider — копіюємо для ITLSProvider
- `lib/ESPRack/include/SecretsVault.h:1-100` — AES-128-CBC + eFuse key
  (existing storage для PEM-strings)
- `lib/ESPRack/include/ConfigDelegate.h:19-60` — secret-key auto-discovery
  (Phase D — fields with `secret;` prefix у FormBuilder option auto-encrypt)
- `factory_settings.ini` — додати `BOOTSTRAP_ENROLL_URL`,
  `MOTHERSHIP_CHECKIN_URL`, `MOTHERSHIP_INTERVAL_MIN`
- `features.ini` — `FT_CERT_MANAGER=1`, `FT_MOTHERSHIP=1`,
  `FT_WIREGUARD=1` (gates щоб phases можна було відключити окремо)

---

## Sequencing rules (важливо)

- **Phases 1-2 фундаментальні** — без них Phases 3-4 не можуть існувати
- **Phase 3 (WireGuard) можна робити паралельно з Phase 4 (rotation)**
  — обидва покладаються лише на готову Phase 1+2
- **Phase 5 (server API) має бути зафіксована до того як начнеться
  Phase 2 кодинг** — інакше device-API і server-API розійдуться
- На кожній phase спочатку **mock server** (Python/Node одно-файловий
  тест-stub) → device тестується проти нього → потім pad'ити на Java-server
