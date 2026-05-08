#pragma once
#ifndef ESPRACK_ITLS_PROVIDER_H
#define ESPRACK_ITLS_PROVIDER_H

// ITLSProvider — framework-side primitive owning the device's mTLS
// material (CA bundle, optional client cert + key) and handing
// pre-configured TLS clients to consumer modules.
//
// Why a service and not a library function: every module that talks
// HTTPS to the mothership needs the SAME chain (same CA, same client
// cert) and the cert can ROTATE at runtime (Phase 4). Centralising
// avoids each module rebuilding mbedtls contexts independently and
// gives a single point to push "cert changed" events to.
//
// Skeleton in Phase 1.0 — no-op implementations until Phase 1.1
// fills in mbedtls plumbing. Once filled in, consumer modules just
// call `app->tls()->attachToClient(client)` and the WiFiClientSecure
// is ready for outbound HTTPS.

#include <Arduino.h>
#include <WiFiClientSecure.h>   // typedef'd in arduino-esp32, can't forward-declare

class ITLSProvider {
 public:
  virtual ~ITLSProvider() = default;

  // True once a CA bundle is loaded (server-side TLS works).
  // mTLS additionally requires a client cert (see hasClientCert).
  virtual bool hasCaChain() const = 0;

  // Push a CA bundle (PEM concat of trusted server roots) into the
  // active context. Subsequent attachToClient() calls will pin
  // server certs against this bundle. Called by CertManagerService
  // after enrollment when the server includes its trust bundle in
  // the response — replaces whatever bootstrap CA was loaded
  // previously. Returns true on PEM parse / accept, false on
  // malformed input (existing CA stays).
  virtual bool loadCaChain(const String& caBundlePem) = 0;

  // True when device has a valid client cert+key loaded — enables
  // mTLS on every outbound HTTPS handshake. Until then, only
  // server-side TLS (CA-pinned, no client auth) is possible.
  virtual bool hasClientCert() const = 0;

  // Apply the framework-owned CA bundle (and client cert+key when
  // present) to a freshly-constructed WiFiClientSecure so an HTTPS
  // request will get the right chain on handshake. Caller owns the
  // client lifetime — we just configure it.
  virtual void attachToClient(WiFiClientSecure& client) = 0;

  // Push a new client cert+key (PEM strings) into the active TLS
  // context. Called by CertManagerService after successful enroll
  // / rotate so all subsequently-built clients pick up the new
  // material. Already-handshaken connections are NOT renegotiated;
  // they keep using the old cert until their next reconnect.
  virtual void updateClientCert(const String& certPem, const String& keyPem) = 0;

  // Drop the client cert (after a logout / factory reset). CA
  // bundle stays so server-side TLS remains usable. Subsequent
  // attachToClient calls will configure server-side-only.
  virtual void clearClientCert() = 0;
};

#endif  // ESPRACK_ITLS_PROVIDER_H
