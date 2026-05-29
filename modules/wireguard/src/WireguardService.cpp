#include <WireguardService.h>
#include <WebManager.h>
#include <HeapMonitor.h>

#include <Arduino.h>
#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/ecdh.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
// netif_list + per-netif MIB2 counters live in lwIP — the WG lib
// doesn't expose per-peer byte counters, but the underlying netif
// it created tracks ifInOctets / ifOutOctets which give the right
// numbers for a single-peer tunnel.
#include <lwip/netif.h>

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

  // Library-backend identifier. Tells the operator which tunnel
  // implementation is running — the static-allocation
  // WireGuard-ESPRack fork (default v0.2.0+) or whatever override a
  // consumer set via build_flags. Read-only diagnostic, not a config.
  FormBuilder::addTextField(st, "backend", AF::R,
                            WIREGUARD_BACKEND_NAME,
                            label("Library backend"),
                            icon("Memory"));

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
  // Heap-trace timeline around WG up. The handshake fires async on
  // the first packet exchanged with the peer (200-500 ms typical on
  // a clean LAN), so the immediate wg.up-post snapshot only captures
  // the begin()+addPeer() cost. These deferred snapshots reveal what
  // the actual ECDH + lwIP-glue traffic adds AFTER setup. Each
  // timestamp fires exactly once per up()-cycle; flags reset in down().
  if (_wgUpAt_ms != 0) {
    uint32_t since = millis() - _wgUpAt_ms;
    if (!_wgUpSnap200_done && since >= 200) {
      ESPRack::HeapMonitor::logSnapshot("wg.up+200ms");
      _wgUpSnap200_done = true;
    }
    if (!_wgUpSnap2s_done && since >= 2000) {
      ESPRack::HeapMonitor::logSnapshot("wg.up+2s");
      _wgUpSnap2s_done = true;
    }
    if (!_wgUpSnap10s_done && since >= 10000) {
      ESPRack::HeapMonitor::logSnapshot("wg.up+10s");
      _wgUpSnap10s_done = true;
    }
  }
  bool actually_up = g_wg.is_initialized();
  if (actually_up != _state.is_up) {
    update([actually_up](WireguardSettings& s) {
      s.is_up = actually_up;
      return actually_up ? StateUpdateResult::CHANGED
                          : StateUpdateResult::CHANGED;
    }, "wg.statusFlip");
  }
  // Counters — the lib doesn't expose per-peer tx/rx getters, but
  // lwIP's MIB2 maintains octets on every netif. The wg lib names
  // the netif "wg" (two-letter convention) — see wireguardif.c
  // ~line 1027. Walk netif_list once per loop iteration, lift the
  // counters into state. Throttled to once a second so the live
  // Tunnel-tab status push isn't doing this on every busy tick.
  static uint32_t s_lastCountersMs = 0;
  uint32_t nowMs = millis();
  if (actually_up && (nowMs - s_lastCountersMs >= 1000)) {
    s_lastCountersMs = nowMs;
    uint32_t tx = 0, rx = 0;
    for (struct netif* nif = netif_list; nif != nullptr; nif = nif->next) {
      if (nif->name[0] == 'w' && nif->name[1] == 'g') {
#if MIB2_STATS
        tx = nif->mib2_counters.ifoutoctets;
        rx = nif->mib2_counters.ifinoctets;
#endif
        break;
      }
    }
    if (tx != _state.tx_bytes || rx != _state.rx_bytes) {
      update([tx, rx](WireguardSettings& s) {
        s.tx_bytes = tx;
        s.rx_bytes = rx;
        return StateUpdateResult::CHANGED;
      }, "wg.counters");
    }
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
    log_e("[wg] up: no device private key (keypair "
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
    log_d("[wg] up: missing triplet srv=%u ep=%u ip=%u",
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
    log_d("[wg] up: endpoint missing :port (%s)",
                  ep.c_str());
    return false;
  }
  String host = ep.substring(0, colon);
  uint16_t port = (uint16_t)ep.substring(colon + 1).toInt();
  if (port == 0) {
    log_d("[wg] up: endpoint port unparsable (%s)",
                  ep.c_str());
    return false;
  }
  IPAddress devIp;
  if (!devIp.fromString(ip)) {
    log_d("[wg] up: assigned ip unparsable (%s)",
                  ip.c_str());
    return false;
  }
  // Snapshot heap before WG so we can see exactly how much memory the
  // tunnel consumes — and whether it fragments the largest free block.
  // Diagnoses the "web won't load while WG is up" pattern: if max_alloc
  // post-WG drops below ~16 KB AsyncWebServer can't grab a recv buffer.
  ESPRack::HeapMonitor::logSnapshot("wg.up-pre");
  // Idempotent — tear down first if already initialised, so the
  // triplet swap takes effect cleanly.
  if (g_wg.is_initialized()) {
    g_wg.end();
  }
  // ── Two-step bring-up (NOT the convenience 5-arg overload) ──
  //
  // The convenience `begin(localIP, priv, host, peerPub, port)` overload
  // in francescolavra's fork sets subnet=/32 and calls the internal
  // addPeer with allowedCount=0. Result on the wire: cryptokey routing
  // drops every inbound transport packet because no peer matches the
  // source IP, and outbound has no route for the /24 anyway. The lib
  // accepts the handshake (no AllowedIPs gate during handshake) so
  // /mothership/wg shows "latest handshake: Now" — but ping / HTTP
  // never works because the data plane is broken.
  //
  // Two-step API does it correctly:
  //   1. peerless `begin(localIP, subnet, gateway, priv, mtu)` —
  //      registers the netif with the WHOLE /24 mask so the device
  //      route table has "10.99.0.0/24 dev wg" and lwIP accepts
  //      packets destined for any address in that subnet as local.
  //   2. explicit `addPeer(host, port, pub, NULL, &allow, &mask, 1,
  //      keepalive)` with allowed=10.99.0.0/24 — peer can both send
  //      and receive packets for the whole tunnel subnet. keepalive=25
  //      sends a 32-byte packet every 25 s, which keeps NAT mappings
  //      alive on whatever box sits between the device and the
  //      mothership (typical corporate/dual-LAN setups need this).
  //
  // Subnet assumed /24 — matches the mothership wg_admin default and
  // the operator's choice from the SUBNET_CHOICES dropdown is always
  // /16..24, all of which work fine with the same allowed_mask layout
  // (server still routes whole /N, device only cares about the local
  // segment to reach the gateway).
  IPAddress subnet (255, 255, 255, 0);
  IPAddress gateway(devIp[0], devIp[1], devIp[2], 1);
  // 1420 = standard WG MTU on IPv4 (1500 - 80 overhead). Matches
  // wg-quick's `MTU = 1420` default; the lib's WIREGUARDIF_MTU
  // constant is the same value, kept here as a literal to avoid the
  // extra `#include <wireguardif.h>`.
  const uint16_t WG_MTU = 1420;
  bool ok = g_wg.begin(devIp, subnet, gateway,
                        _state.priv_key.c_str(), WG_MTU);
  if (!ok) {
    log_e("[wg] up: WireGuard.begin() failed");
    return false;
  }
  IPAddress allowAddr(devIp[0], devIp[1], devIp[2], 0);
  IPAddress allowMask(255, 255, 255, 0);
  ok = g_wg.addPeer(host.c_str(), port, srv.c_str(),
                     /*preshared=*/nullptr,
                     &allowAddr, &allowMask, /*allowedCount=*/1,
                     /*keep_alive=*/25);
  if (!ok) {
    log_e("[wg] up: WireGuard.addPeer() failed");
    g_wg.end();
    return false;
  }
  log_i("[wg] up: tunnel started, devIp=%s/24 peer=%s:%u "
                "allowed=%s/24 keepalive=25s",
                ip.c_str(), host.c_str(), (unsigned)port,
                allowAddr.toString().c_str());
  ESPRack::HeapMonitor::logSnapshot("wg.up-post");
  // Arm the deferred snapshots — loop() fires them at +200ms / +2s
  // / +10s. The actual ECDH handshake runs async on the first WG
  // packet, so capturing only the begin()+addPeer() cost here misses
  // the bulk of the heap impact.
  _wgUpAt_ms = millis();
  _wgUpSnap200_done = false;
  _wgUpSnap2s_done  = false;
  _wgUpSnap10s_done = false;
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
  log_d("[wg] up: lib not linked — STUB mode. Returning "
                  "false so caller surfaces the failure clearly.");
  return false;
#endif
}

void WireguardService::down() {
#if HAVE_WIREGUARD_LIB
  // Snapshot lib state before tearing down so the post-end log makes
  // the transition obvious. Helps spot library quirks where end()
  // returns OK but is_initialized() stays sticky — the loop()'s
  // actually_up sync would then re-flip our state to UP and the
  // operator's Close click looks like a no-op. If we ever see
  // "post-end is_initialized=true" it's the lib that's wrong, not
  // our state machine, and we'll need to either retry end() or stop
  // trusting the lib's view.
  bool was_initialised = g_wg.is_initialized();
  log_i("[wg.down] enter: lib.is_initialized=%d state.is_up=%d",
        (int)was_initialised, (int)_state.is_up);
  if (was_initialised) {
    ESPRack::HeapMonitor::logSnapshot("wg.down-pre");
    g_wg.end();
    log_i("[wg.down] post-end: lib.is_initialized=%d",
          (int)g_wg.is_initialized());
    ESPRack::HeapMonitor::logSnapshot("wg.down-post");
  }
  // Disarm the deferred-snapshot timeline so the next up() cycle
  // starts a fresh probe window.
  _wgUpAt_ms = 0;
  _wgUpSnap200_done = true;
  _wgUpSnap2s_done  = true;
  _wgUpSnap10s_done = true;
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
    log_i("[wg] keypair already present, pub=%s",
                  _state.pub_key.c_str());
    return;
  }
  String priv, pub;
  if (!generateKeypair(priv, pub)) {
    log_e("[wg] keypair generation FAILED — tunnel will "
                    "stay unavailable until next boot");
    return;
  }
  update([&priv, &pub](WireguardSettings& s) {
    s.priv_key = priv;
    s.pub_key  = pub;
    return StateUpdateResult::CHANGED;
  }, "wg.keygen");
  log_i("[wg] generated new Curve25519 keypair, pub=%s",
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
      log_e("[wg.keygen] drbg seed failed");
      break;
    }
    if (mbedtls_ecp_group_load(&kp.MBEDTLS_PRIVATE(grp),
                                 MBEDTLS_ECP_DP_CURVE25519) != 0) {
      log_e("[wg.keygen] curve25519 group load failed");
      break;
    }
    if (mbedtls_ecp_gen_keypair(&kp.MBEDTLS_PRIVATE(grp),
                                  &kp.MBEDTLS_PRIVATE(d),
                                  &kp.MBEDTLS_PRIVATE(Q),
                                  mbedtls_ctr_drbg_random,
                                  &drbg) != 0) {
      log_e("[wg.keygen] gen_keypair failed");
      break;
    }
    // Export private scalar (little-endian for x25519, but mbedtls
    // stores it big-endian internally). We need the wire format.
    if (mbedtls_mpi_write_binary_le(&kp.MBEDTLS_PRIVATE(d),
                                       priv_raw, 32) != 0) {
      log_e("[wg.keygen] priv export failed");
      break;
    }
    // The X coordinate of Q is the public key, little-endian.
    if (mbedtls_mpi_write_binary_le(
            &kp.MBEDTLS_PRIVATE(Q).MBEDTLS_PRIVATE(X),
            pub_raw, 32) != 0) {
      log_e("[wg.keygen] pub export failed");
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
