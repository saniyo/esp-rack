#include <WireguardService.h>
#include <WebManager.h>

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

// The ciniml/WireGuard-ESP32-Arduino library is consumed via PIO
// lib_deps (added in the demo's platformio.ini). The header lives
// at the lib's src/WireGuard-ESP32.h — we depend on it always being
// present when this module is built. (If a consumer doesn't want
// WG, they just don't install WireGuardModule.)
#if __has_include(<WireGuard-ESP32.h>)
  #include <WireGuard-ESP32.h>
  #define HAVE_WIREGUARD_LIB 1
#else
  #define HAVE_WIREGUARD_LIB 0
#endif

namespace {
// One process-wide WireGuard instance — the ciniml lib uses static
// LwIP state internally so multiple instances would collide. We
// keep ours here at file scope so both up() and down() can reach
// it without exposing the third-party type in the header.
#if HAVE_WIREGUARD_LIB
  WireGuard g_wg;
#endif
}

// ===== Persistence =====

void WireguardSettings::readConfig(WireguardSettings& s,
                                    JsonObject& root) {
  // priv_key is the only secret in this config — pub_key, peer
  // pubkey, endpoint, and tunnel IP are all server-disclosed public
  // data. SecretsVault encrypts priv_key at rest because buildForm()
  // declares it as addSecretField (auto-discovery detects FieldType::
  // SECRET, not the field name — earlier comment in this file was
  // wrong, fixed here). On-disk value lands as `ENC:...base64...`.
  root["priv_key"]       = s.priv_key;
  root["pub_key"]        = s.pub_key;
  root["server_pub_key"] = s.server_pub_key;
  root["endpoint"]       = s.endpoint;
  root["assigned_ip"]    = s.assigned_ip;
}

StateUpdateResult WireguardSettings::update(JsonObject& root,
                                              WireguardSettings& s) {
  bool ch = false;

  // priv_key is the sensitive field. SecretsVault dual-parses the
  // same way cert-manager's PEM blobs are handled: adopt only if
  // the incoming JSON has a non-empty value. At boot we MUST take
  // the decrypted value off disk. On every form POST the field
  // comes through as the same plaintext (AF::R round-trip) which
  // is fine — empty/missing wouldn't clobber. Without this guard,
  // a form save with a redacted/null priv_key field would wipe
  // our keypair and break the tunnel until next factory boot.
  if (root.containsKey("priv_key")) {
    JsonVariant v = root["priv_key"];
    if (!v.isNull()) {
      String incoming = v.as<String>();
      if (incoming.length() > 0 && incoming != s.priv_key) {
        s.priv_key = incoming;
        ch = true;
      }
    }
  }

  ch |= FormBuilder::updateValue(root, "pub_key",        s.pub_key);
  ch |= FormBuilder::updateValue(root, "server_pub_key", s.server_pub_key);
  ch |= FormBuilder::updateValue(root, "endpoint",       s.endpoint);
  ch |= FormBuilder::updateValue(root, "assigned_ip",    s.assigned_ip);
  return ch ? StateUpdateResult::CHANGED : StateUpdateResult::UNCHANGED;
}

// ===== Form schema =====

void WireguardSettings::buildForm(WireguardSettings& s,
                                    JsonObject& root) {
  JsonArray st = FormBuilder::createForm(root, "status",
                                          "Tunnel status");

  String state_label = s.is_up ? "Up" : "Down";
  // PowerSettingsNew — clearer Up/Down semantic than VpnLock (which
  // implied "VPN with lock" — wrong concept for on-demand reverse-
  // access tunnel). Color flips to success when the tunnel is up.
  FormBuilder::addTextField(st, "state", AF::R, state_label.c_str(),
                            label("State"), icon("PowerSettingsNew"),
                            colorMap("Up:success,Down:default"));
  // Router — the tunnel IP is the device's address inside the WG
  // overlay network, semantically a network-routing identifier.
  FormBuilder::addTextField(st, "assigned_ip", AF::R,
                            s.assigned_ip.c_str(),
                            label("Tunnel IP"), icon("Router"));
  FormBuilder::addTextField(st, "endpoint", AF::R,
                            s.endpoint.c_str(),
                            label("Server endpoint"), icon("Cloud"));
  FormBuilder::addTextField(st, "pub_key", AF::R,
                            s.pub_key.c_str(),
                            label("Device public key"),
                            icon("Key"));
  FormBuilder::addNumberField(st, "tx_bytes", AF::R,
                              (double)s.tx_bytes, format("0"),
                              label("Tx bytes"), icon("Upload"));
  FormBuilder::addNumberField(st, "rx_bytes", AF::R,
                              (double)s.rx_bytes, format("0"),
                              label("Rx bytes"), icon("Download"));

  FormBuilder::addMessageField(st, "m_config_where",
      "Configuration lives on the mothership, not here. The operator "
      "sets the subnet, port, and public endpoint once at "
      "/mothership/wg on the server. The triplet (server pubkey + "
      "endpoint + this device's tunnel IP) is delivered to the "
      "device through the /api/v1/enroll response and on every "
      "openTunnel action — that's why the Server endpoint row "
      "above is empty until the first enrollment + openTunnel cycle "
      "completes against a properly configured mothership.",
      level("info"), icon("Settings"));
  FormBuilder::addMessageField(st, "m_lifecycle",
      "Tunnel is on-demand. It comes up only when an operator opens "
      "this device's UI panel on the mothership dashboard, and is "
      "torn down after idle timeout. No manual controls here — this "
      "tab is read-only for diagnostics. Use the 'Open UI' button "
      "on the device's panel page (mothership side) to bring it up.",
      level("info"), icon("Info"));

  // ── INTERNALS ──────────────────────────────────────────────────
  // The device's Curve25519 private key — generated locally on first
  // boot and never leaves the device. Marked as addSecretField so
  // SecretsVault's auto-discovery picks it up (it scans for
  // FieldType::SECRET in the form schema, not field-name patterns)
  // and encrypts the on-disk value in /config/wireguard.json as
  // ENC:...base64... Read-only — there's no operator-meaningful
  // reason to edit it; rotating means clearing it and rebooting
  // (the service regenerates a fresh pair on next ensureKeypair()
  // when the field is empty).
  JsonArray intl = FormBuilder::createForm(root, "internals",
                                            "Sensitive material (encrypted on disk)");
  FormBuilder::addMessageField(intl, "m_internals_warn",
      "The device's Curve25519 private key. Generated locally on "
      "first boot, never transmitted. Encrypted at rest by "
      "SecretsVault — eye icon reveals plaintext for debug. Do NOT "
      "modify by hand: edits via this UI are discarded on save "
      "(AF::R). To rotate the keypair, clear this field manually "
      "and reboot the device (next ensureKeypair() generates a fresh "
      "pair, and the next enrollment ships the new public half).",
      level("warning"), icon("Warning"));
  FormBuilder::addSecretField(intl, "priv_key", AF::R,
                              s.priv_key.c_str(),
                              label("Device private key (Curve25519)"),
                              icon("Key"));
}

void WireguardSettings::staRead(WireguardSettings& s,
                                  JsonObject& root) {
  root["state"]       = s.is_up ? "Up" : "Down";
  root["assigned_ip"] = s.assigned_ip;
  root["endpoint"]    = s.endpoint;
  root["pub_key"]     = s.pub_key;
  root["tx_bytes"]    = s.tx_bytes;
  root["rx_bytes"]    = s.rx_bytes;
}

StateUpdateResult WireguardSettings::staUpd(JsonObject& root,
                                              WireguardSettings& s) {
  return update(root, s);
}

// ===== Service =====

WireguardService::WireguardService(ConfigManager* cfgMgr)
    : StatefulService<WireguardSettings>(),
      _cfg(cfgMgr,
           "wireguard",
           WG_FILE,
           2048,
           this,
           WireguardSettings::readConfig,
           WireguardSettings::update,
           false /*autoSave*/,
           nullptr /*validator*/,
           WireguardSettings::buildForm /*formReader*/) {
  addUpdateHandler([this](const String& origin) {
    _cfg.saveIfChanged(origin);
    if (_feature) _feature->broadcastWs(origin);
  }, false);
}

void WireguardService::registerManifest(WebManager* web) {
  if (!web) return;
  WebFeatureSpec spec;
  spec.id         = "wireguard";
  spec.title      = "Tunnel";
  spec.component  = "DynamicSettings";
  spec.menu.label = "Tunnel";
  spec.menu.icon  = "Cast";
  spec.menu.order = 380;
  spec.menu.auth  = WebAuthLevel::Admin;
  spec.auth       = WebAuthLevel::Admin;
  spec.restRead   = WG_FORM_PATH;
  spec.restUpdate = WG_FORM_PATH;

  WebTabSpec statusTab;
  statusTab.key      = "status";
  statusTab.title    = "Status";
  statusTab.restPath = WG_FORM_PATH;
  statusTab.postable = false;
  statusTab.live     = true;
  spec.tabs.push_back(statusTab);

  WebTabSpec internalsTab;
  internalsTab.key      = "internals";
  internalsTab.title    = "Internals";
  internalsTab.restPath = WG_FORM_PATH;
  internalsTab.postable = false;     // priv_key is AF::R; never accept writes
  internalsTab.auth     = WebAuthLevel::Admin;
  internalsTab.order    = 90;
  spec.tabs.push_back(internalsTab);

  _feature = web->registerFeature<WireguardSettings>(
      std::move(spec), this,
      WireguardSettings::buildForm,  WireguardSettings::update,
      WireguardSettings::staRead,    WireguardSettings::staUpd,
      4096, 2048);
}

void WireguardService::begin() {
  (void)_cfg.ensureLoaded();
  // Generate keypair on first boot if SecretsVault has nothing for
  // us. Otherwise the priv_key field is already populated by
  // ensureLoaded → SecretsVault decrypt.
  ensureKeypair();
}

void WireguardService::loop() {
  // Refresh the live state from the WG lib once per pass. We could
  // skip when down for efficiency, but the cost is negligible.
#if HAVE_WIREGUARD_LIB
  bool actually_up = g_wg.is_initialized();
  if (actually_up != _state.is_up) {
    update([actually_up](WireguardSettings& s) {
      s.is_up = actually_up;
      return actually_up ? StateUpdateResult::CHANGED
                          : StateUpdateResult::CHANGED;
    }, "wg.statusFlip");
  }
#endif
}

int32_t WireguardService::lastHandshakeAgeSec() const {
  if (!_state.is_up || _state.last_handshake_ms == 0) return INT32_MAX;
  uint32_t now = millis();
  if (now < _state.last_handshake_ms) return INT32_MAX;  // wrap
  return (int32_t)((now - _state.last_handshake_ms) / 1000);
}

bool WireguardService::up(const String& serverPublicKey,
                            const String& endpoint,
                            const String& assignedIp) {
  if (_state.priv_key.length() == 0) {
    Serial.println("[wg] up: no device private key (keypair "
                    "generation failed?)");
    return false;
  }
  String srv = serverPublicKey.length() > 0
                 ? serverPublicKey
                 : _state.server_pub_key;
  String ep  = endpoint.length() > 0
                 ? endpoint
                 : _state.endpoint;
  String ip  = assignedIp.length() > 0
                 ? assignedIp
                 : _state.assigned_ip;
  if (srv.length() == 0 || ep.length() == 0 || ip.length() == 0) {
    Serial.printf("[wg] up: missing triplet srv=%u ep=%u ip=%u\n",
                  (unsigned)srv.length(), (unsigned)ep.length(),
                  (unsigned)ip.length());
    return false;
  }

  // Persist the latest triplet so a reboot mid-session can short-
  // circuit (if the server commands openTunnel again it'll re-send
  // anyway, but having the values cached helps the UI show stale
  // state without an empty-string flash).
  update([&srv, &ep, &ip](WireguardSettings& s) {
    bool changed = (s.server_pub_key != srv) ||
                   (s.endpoint != ep) ||
                   (s.assigned_ip != ip);
    s.server_pub_key = srv;
    s.endpoint       = ep;
    s.assigned_ip    = ip;
    return changed ? StateUpdateResult::CHANGED
                    : StateUpdateResult::UNCHANGED;
  }, "wg.triplet");

#if HAVE_WIREGUARD_LIB
  // Parse "host:port" — ciniml WireGuard wants them separate.
  int colon = ep.lastIndexOf(':');
  if (colon <= 0 || colon >= (int)ep.length() - 1) {
    Serial.printf("[wg] up: endpoint missing :port (%s)\n",
                  ep.c_str());
    return false;
  }
  String host = ep.substring(0, colon);
  uint16_t port = (uint16_t)ep.substring(colon + 1).toInt();
  if (port == 0) {
    Serial.printf("[wg] up: endpoint port unparsable (%s)\n",
                  ep.c_str());
    return false;
  }
  IPAddress devIp;
  if (!devIp.fromString(ip)) {
    Serial.printf("[wg] up: assigned ip unparsable (%s)\n",
                  ip.c_str());
    return false;
  }
  // Idempotent — tear down first if already initialised, so the
  // triplet swap takes effect cleanly.
  if (g_wg.is_initialized()) {
    g_wg.end();
  }
  bool ok = g_wg.begin(
      devIp,
      _state.priv_key.c_str(),
      host.c_str(),
      srv.c_str(),
      (int)port);
  if (!ok) {
    Serial.println("[wg] up: WireGuard.begin() failed");
    return false;
  }
  Serial.printf("[wg] up: tunnel started, devIp=%s peer=%s:%u\n",
                ip.c_str(), host.c_str(), (unsigned)port);
  update([](WireguardSettings& s) {
    s.is_up = true;
    s.up_since_ms = millis();
    s.last_handshake_ms = millis();  // optimistic; refined by lib
    s.tx_bytes = 0;
    s.rx_bytes = 0;
    return StateUpdateResult::CHANGED;
  }, "wg.up");
  return true;
#else
  Serial.println("[wg] up: lib not linked — STUB mode. Returning "
                  "false so caller surfaces the failure clearly.");
  return false;
#endif
}

void WireguardService::down() {
#if HAVE_WIREGUARD_LIB
  if (g_wg.is_initialized()) {
    g_wg.end();
    Serial.println("[wg] down: tunnel stopped");
  }
#endif
  update([](WireguardSettings& s) {
    bool was = s.is_up;
    s.is_up = false;
    s.up_since_ms = 0;
    s.last_handshake_ms = 0;
    return was ? StateUpdateResult::CHANGED
                : StateUpdateResult::UNCHANGED;
  }, "wg.down");
}

// ===== Keypair management =====

void WireguardService::ensureKeypair() {
  if (_state.priv_key.length() > 0 && _state.pub_key.length() > 0) {
    Serial.printf("[wg] keypair already present, pub=%s\n",
                  _state.pub_key.c_str());
    return;
  }
  String priv, pub;
  if (!generateKeypair(priv, pub)) {
    Serial.println("[wg] keypair generation FAILED — tunnel will "
                    "stay unavailable until next boot");
    return;
  }
  update([&priv, &pub](WireguardSettings& s) {
    s.priv_key = priv;
    s.pub_key  = pub;
    return StateUpdateResult::CHANGED;
  }, "wg.keygen");
  Serial.printf("[wg] generated new Curve25519 keypair, pub=%s\n",
                pub.c_str());
}

bool WireguardService::generateKeypair(String& outPrivB64,
                                        String& outPubB64) {
  // mbedtls Curve25519 — WG uses the same primitive.
  // Path:
  //   1. mbedtls_ecp_gen_keypair on MBEDTLS_ECP_DP_CURVE25519
  //   2. clamp the private scalar per RFC 7748 §5 (mbedtls already
  //      does this for x25519 internally during gen_keypair, but
  //      we re-export the raw bytes which need to be the *clamped*
  //      form so they round-trip through any consumer)
  //   3. base64-encode 32 priv bytes + 32 pub bytes
  mbedtls_ecp_keypair kp;
  mbedtls_ecp_keypair_init(&kp);
  mbedtls_entropy_context entropy;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_context drbg;
  mbedtls_ctr_drbg_init(&drbg);

  bool ok = false;
  uint8_t priv_raw[32] = {0};
  uint8_t pub_raw [32] = {0};
  do {
    const char* pers = "wg.keygen";
    if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char*)pers,
                                strlen(pers)) != 0) {
      Serial.println("[wg.keygen] drbg seed failed");
      break;
    }
    if (mbedtls_ecp_group_load(&kp.MBEDTLS_PRIVATE(grp),
                                 MBEDTLS_ECP_DP_CURVE25519) != 0) {
      Serial.println("[wg.keygen] curve25519 group load failed");
      break;
    }
    if (mbedtls_ecp_gen_keypair(&kp.MBEDTLS_PRIVATE(grp),
                                  &kp.MBEDTLS_PRIVATE(d),
                                  &kp.MBEDTLS_PRIVATE(Q),
                                  mbedtls_ctr_drbg_random,
                                  &drbg) != 0) {
      Serial.println("[wg.keygen] gen_keypair failed");
      break;
    }
    // Export private scalar (little-endian for x25519, but mbedtls
    // stores it big-endian internally). We need the wire format.
    if (mbedtls_mpi_write_binary_le(&kp.MBEDTLS_PRIVATE(d),
                                       priv_raw, 32) != 0) {
      Serial.println("[wg.keygen] priv export failed");
      break;
    }
    // The X coordinate of Q is the public key, little-endian.
    if (mbedtls_mpi_write_binary_le(
            &kp.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
            pub_raw, 32) != 0) {
      Serial.println("[wg.keygen] pub export failed");
      break;
    }
    ok = true;
  } while (false);

  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  mbedtls_ecp_keypair_free(&kp);

  if (!ok) return false;
  outPrivB64 = base64Encode(priv_raw, 32);
  outPubB64  = base64Encode(pub_raw,  32);
  // Wipe raw material on return.
  memset(priv_raw, 0, sizeof(priv_raw));
  return true;
}

String WireguardService::base64Encode(const uint8_t* data,
                                        size_t len) {
  // WG public/private keys are 32 bytes → 44 chars b64 (with '=').
  unsigned char out[64];
  size_t olen = 0;
  int rc = mbedtls_base64_encode(out, sizeof(out), &olen, data, len);
  if (rc != 0 || olen == 0) return String();
  return String((const char*)out, olen);
}
