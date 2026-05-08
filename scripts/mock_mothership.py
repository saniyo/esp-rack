#!/usr/bin/env python3
"""
mock_mothership.py — single-file dev/test stand-in for the future Java
mothership server during Phase 1 of the ESP-Rack mothership roadmap
(see docs/plans/mothership-roadmap.md).

Implements just enough of /api/v1/enroll for a device running the
cert-manager module to:
  1. POST a CSR with `Authorization: Bearer <bootstrap_token>`
  2. Get back a signed cert + CA bundle + recovery_token
  3. Successfully parse the response and reach State::Ready

The ROOT CA is generated fresh on every server start and printed to
stdout — no persistent identity, no production safety. Devices that
enrolled against an earlier server start will fail mTLS to a new one
(re-enrollment required). That's intentional for dev: you can scratch
the slate by Ctrl-C / re-run.

Usage:
  pip install cryptography
  python mock_mothership.py [--host 0.0.0.0] [--port 8443] [--token CHANGE-ME]

Then on the device's PKI > Settings tab set:
  Enroll URL = https://<your-pc-lan-ip>:8443/api/v1/enroll

Then PKI > Enrollment, paste the bootstrap token (default
"dev-bootstrap-token") and click Enroll.

Production flow is much more involved (proper CA, token DB with TTLs,
audit logging, command queue, mTLS-only post-enroll endpoints) — this
script exists ONLY to unblock device-side Phase 1.5+ verification
while the real Java service is being built.
"""

import argparse
import datetime
import http.server
import json
import os
import secrets
import ssl
import sys
import tempfile
from typing import Tuple

# Windows console defaults to cp1252 which can't encode the em-dashes
# / arrows / other non-ASCII chars sprinkled across this file's
# print() calls. Force UTF-8 with replace-on-unencodable so the
# server doesn't silently 500 mid-handler when its log line contains
# a non-ASCII byte.
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, padding  # noqa: F401
    from cryptography.x509.oid import NameOID
except ImportError:
    print("ERROR: cryptography lib missing — run: pip install cryptography",
          file=sys.stderr)
    sys.exit(1)


# ----- Per-device action queue (Phase 2 check-in command channel) -----
# Operator stages actions via POST /api/v1/admin/queue/<deviceId>; the
# next /api/v1/checkin from that device drains the queue (FIFO) into
# its response. In-memory only — restart wipes pending actions.
COMMAND_QUEUE: dict = {}


# ----- root CA generation (in-memory, fresh each run) -----

def _ca_path() -> Tuple[str, str]:
    """Persistent CA storage paths — alongside the script. Once a CA
    is generated it survives mock-server restarts so devices that
    enrolled against it keep their CA bundle valid for mTLS handshake.
    Without this every server restart would force re-enrollment of
    every test device."""
    here = os.path.dirname(os.path.abspath(__file__))
    return (os.path.join(here, ".mock_ca.pem"),
            os.path.join(here, ".mock_ca.key"))


def make_root_ca() -> Tuple[x509.Certificate, ec.EllipticCurvePrivateKey]:
    """Self-signed ECDSA-P256 CA — 10 year validity. Persisted to
    .mock_ca.{pem,key} alongside the script so successive mock
    restarts present the same CA to enrolled devices.

    Delete those two files (or pass --reset-ca) to force a fresh CA
    on the next start (will require re-enrolling every test device)."""
    cert_path, key_path = _ca_path()

    if os.path.exists(cert_path) and os.path.exists(key_path):
        try:
            with open(cert_path, "rb") as f:
                cert = x509.load_pem_x509_certificate(f.read())
            with open(key_path, "rb") as f:
                key = serialization.load_pem_private_key(f.read(), password=None)
            print(f"[ca] loaded persisted CA from {cert_path}")
            return cert, key
        except Exception as e:
            print(f"[ca] persisted CA load failed ({e}); regenerating")

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, "esprack-mock-mothership-CA"),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=3650))
        .add_extension(x509.BasicConstraints(ca=True, path_length=0),
                       critical=True)
        .add_extension(x509.KeyUsage(digital_signature=False,
                                      content_commitment=False,
                                      key_encipherment=False,
                                      data_encipherment=False,
                                      key_agreement=False,
                                      key_cert_sign=True,
                                      crl_sign=True,
                                      encipher_only=False,
                                      decipher_only=False),
                       critical=True)
        .sign(key, hashes.SHA256())
    )

    # Persist for future runs.
    try:
        with open(cert_path, "wb") as f:
            f.write(cert.public_bytes(serialization.Encoding.PEM))
        with open(key_path, "wb") as f:
            f.write(key.private_bytes(
                encoding=serialization.Encoding.PEM,
                format=serialization.PrivateFormat.TraditionalOpenSSL,
                encryption_algorithm=serialization.NoEncryption(),
            ))
        print(f"[ca] persisted new CA to {cert_path}")
    except OSError as e:
        print(f"[ca] WARNING: failed to persist CA ({e}); next start "
              f"will regenerate and break enrolled devices")

    return cert, key


def make_server_cert(ca_cert: x509.Certificate,
                     ca_key: ec.EllipticCurvePrivateKey,
                     hostname: str
                     ) -> Tuple[x509.Certificate, ec.EllipticCurvePrivateKey]:
    """ECDSA-P256 server cert signed by our mock CA. SAN includes the
    hostname the operator types into the device + 'localhost' + a
    127.0.0.1 IP for convenience."""
    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([
        x509.NameAttribute(NameOID.COMMON_NAME, hostname),
    ])
    now = datetime.datetime.now(datetime.timezone.utc)
    san = [x509.DNSName(hostname), x509.DNSName("localhost"),
           x509.DNSName("mothership.local")]
    # If hostname is an IP, also add IP SAN — covers operator typing
    # the LAN IP directly into the device's Enroll URL.
    try:
        import ipaddress
        san.append(x509.IPAddress(ipaddress.ip_address(hostname)))
    except ValueError:
        pass

    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=365))
        .add_extension(x509.SubjectAlternativeName(san), critical=False)
        .sign(ca_key, hashes.SHA256())
    )
    return cert, key


def sign_device_csr(csr_pem: bytes,
                    ca_cert: x509.Certificate,
                    ca_key: ec.EllipticCurvePrivateKey
                    ) -> x509.Certificate:
    """Take operator-pasted CSR PEM, validate signature, sign as a
    90-day device cert. Real mothership would persist {csr → cert}
    and apply per-device policy / fleet-grouping / etc."""
    csr = x509.load_pem_x509_csr(csr_pem)
    if not csr.is_signature_valid:
        raise ValueError("CSR self-signature invalid")

    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(csr.subject)
        .issuer_name(ca_cert.subject)
        .public_key(csr.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now)
        .not_valid_after(now + datetime.timedelta(days=90))
        .sign(ca_key, hashes.SHA256())
    )
    return cert


# ----- HTTP handler -----

class EnrollHandler(http.server.BaseHTTPRequestHandler):
    # Populated in main() before serve_forever
    BOOTSTRAP_TOKEN = None
    CA_CERT_PEM     = None
    CA_CERT_OBJ     = None
    CA_KEY_OBJ      = None

    def _send_json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:  # noqa: N802 (BaseHTTPRequestHandler API)
        try:
            if self.path == "/api/v1/enroll":
                self._handle_enroll()
            elif self.path == "/api/v1/checkin":
                self._handle_checkin()
            elif self.path.startswith("/api/v1/admin/queue/"):
                # /api/v1/admin/queue/<deviceId> — operator-side endpoint
                # for staging actions to be delivered on the device's next
                # check-in. Body is the action payload as-is. NOT secured
                # in this mock — production server has admin auth.
                self._handle_admin_queue()
            else:
                self._send_json(404, {"err": "not found"})
        except Exception as e:
            # Catch-all so a bug in the handler doesn't drop the TLS
            # connection mid-flight (schannel:close_notify error on the
            # client side); always send something HTTP-shaped back.
            import traceback
            sys.stderr.write(f"[mock] handler exception: {e}\n")
            traceback.print_exc(file=sys.stderr)
            try:
                self._send_json(500, {"err": str(e)})
            except Exception:
                pass

    def _handle_enroll(self) -> None:
        # Bearer token check — must match what device pasted in UI.
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer "):
            self._send_json(401, {"err": "missing bearer token"})
            return
        token = auth[len("Bearer "):]
        if token != self.BOOTSTRAP_TOKEN:
            self._send_json(401, {"err": "invalid bootstrap token"})
            return

        # Read body — small (CSR ~480 B + framing).
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 16384:
            self._send_json(400, {"err": "bad content-length"})
            return
        raw = self.rfile.read(length)
        try:
            req = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as e:
            self._send_json(400, {"err": f"json parse: {e}"})
            return

        device_id = req.get("deviceId", "")
        csr_pem   = req.get("csr_pem", "")
        if not csr_pem:
            self._send_json(400, {"err": "missing csr_pem"})
            return

        try:
            cert = sign_device_csr(csr_pem.encode("utf-8"),
                                    self.CA_CERT_OBJ, self.CA_KEY_OBJ)
        except Exception as e:
            self._send_json(400, {"err": f"sign failed: {e}"})
            return

        cert_pem = cert.public_bytes(serialization.Encoding.PEM).decode("utf-8")
        recovery_token = secrets.token_hex(16)

        print(f"[enroll] signed deviceId={device_id} "
              f"serial={cert.serial_number:x} "
              f"not_after={cert.not_valid_after_utc.isoformat()}")

        self._send_json(200, {
            "cert_pem":       cert_pem,
            "ca_bundle_pem":  self.CA_CERT_PEM,
            "recovery_token": recovery_token,
        })

    def _handle_checkin(self) -> None:
        # Phase 2.2 — mTLS check-in. Production server validates the
        # client cert against its allow-list, here we accept any
        # client cert presented (Python http.server doesn't easily
        # surface peer cert to handlers, so this mock is "anyone who
        # got past TLS handshake passes"). The deviceId in the
        # request body identifies which device's command queue to
        # drain.
        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 16384:
            self._send_json(400, {"err": "bad content-length"})
            return
        raw = self.rfile.read(length)
        try:
            req = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as e:
            self._send_json(400, {"err": f"json parse: {e}"})
            return

        device_id  = req.get("deviceId", "")
        uptime_sec = req.get("uptimeSec", 0)
        free_heap  = req.get("freeHeap", 0)
        fw         = req.get("fwVer", "?")
        hw         = req.get("hwVer", "?")

        # Drain the per-device action queue (FIFO).
        actions = COMMAND_QUEUE.pop(device_id, [])
        if actions:
            print(f"[checkin] {device_id} fw={fw} up={uptime_sec}s "
                  f"heap={free_heap} -> DELIVERING {len(actions)} actions: "
                  + ", ".join(a.get("type", "?") for a in actions))
        else:
            print(f"[checkin] {device_id} fw={fw} up={uptime_sec}s "
                  f"heap={free_heap} -> no pending actions")

        self._send_json(200, {
            "actions":         actions,
            "nextCheckInSec":  300,  # device ignores this for now
        })

    def _handle_admin_queue(self) -> None:
        # /api/v1/admin/queue/<deviceId>  — POST a single action.
        # Stages it for delivery on the device's next checkin.
        device_id = self.path[len("/api/v1/admin/queue/"):]
        if not device_id:
            self._send_json(400, {"err": "missing deviceId in path"})
            return

        length = int(self.headers.get("Content-Length", "0"))
        if length <= 0 or length > 16384:
            self._send_json(400, {"err": "bad content-length"})
            return
        raw = self.rfile.read(length)
        try:
            action = json.loads(raw.decode("utf-8"))
        except json.JSONDecodeError as e:
            self._send_json(400, {"err": f"json parse: {e}"})
            return

        if "type" not in action:
            self._send_json(400, {"err": "action missing 'type'"})
            return

        COMMAND_QUEUE.setdefault(device_id, []).append(action)
        print(f"[admin] queued action {action.get('type')} for {device_id}")
        self._send_json(202, {
            "ok":     True,
            "queued": len(COMMAND_QUEUE[device_id]),
        })

    def log_message(self, format: str, *args) -> None:  # noqa: A002
        # quieter than the default
        sys.stderr.write(f"[mock] {self.address_string()} - {format % args}\n")


# ----- main -----

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="0.0.0.0",
                    help="Bind interface (default: all)")
    ap.add_argument("--port", type=int, default=8443,
                    help="HTTPS port (default: 8443)")
    ap.add_argument("--token", default="dev-bootstrap-token",
                    help="Bootstrap token operator pastes in device UI "
                         "(default: dev-bootstrap-token)")
    ap.add_argument("--cn", default="mothership.local",
                    help="Server cert CN / primary SAN (default: "
                         "mothership.local — set to your LAN IP if "
                         "device can't resolve mDNS)")
    ap.add_argument("--reset-ca", action="store_true",
                    help="Delete the persisted CA before start. Every "
                         "previously enrolled device will need to "
                         "re-enroll because their CA bundle won't match.")
    args = ap.parse_args()

    if args.reset_ca:
        cp, kp = _ca_path()
        for p in (cp, kp):
            if os.path.exists(p):
                os.unlink(p)
                print(f"[ca] reset: removed {p}")

    print("=" * 60)
    print("ESPRack mock mothership — DEV ONLY, no persistence")
    print("=" * 60)

    ca_cert, ca_key = make_root_ca()
    ca_pem = ca_cert.public_bytes(serialization.Encoding.PEM).decode("utf-8")
    print("\n--- mock CA cert (this is what devices will trust after "
          "enrollment) ---")
    print(ca_pem)

    srv_cert, srv_key = make_server_cert(ca_cert, ca_key, args.cn)
    srv_cert_pem = srv_cert.public_bytes(serialization.Encoding.PEM)
    srv_key_pem  = srv_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.TraditionalOpenSSL,
        encryption_algorithm=serialization.NoEncryption(),
    )

    # Python's ssl wants files on disk for cert/key. Write to temp,
    # delete on exit.
    with tempfile.NamedTemporaryFile(suffix=".pem", delete=False) as fc, \
         tempfile.NamedTemporaryFile(suffix=".pem", delete=False) as fk:
        fc.write(srv_cert_pem); fc_path = fc.name
        fk.write(srv_key_pem);  fk_path = fk.name

    try:
        EnrollHandler.BOOTSTRAP_TOKEN = args.token
        EnrollHandler.CA_CERT_PEM     = ca_pem
        EnrollHandler.CA_CERT_OBJ     = ca_cert
        EnrollHandler.CA_KEY_OBJ      = ca_key

        server = http.server.HTTPServer((args.host, args.port), EnrollHandler)
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=fc_path, keyfile=fk_path)
        # Devices use setInsecure() for first-enroll TLS verification —
        # we don't strictly need TLS 1.2+ here, but be modern.
        ctx.minimum_version = ssl.TLSVersion.TLSv1_2
        server.socket = ctx.wrap_socket(server.socket, server_side=True)

        print(f"\nListening on https://{args.host}:{args.port}/")
        print(f"Endpoints:")
        print(f"  POST /api/v1/enroll                  bootstrap-token CSR signing")
        print(f"  POST /api/v1/checkin                 mTLS device check-in")
        print(f"  POST /api/v1/admin/queue/<deviceId>  stage action for next checkin")
        print(f"\nBootstrap token (paste into device PKI Enrollment tab):")
        print(f"  {args.token}")
        print(f"\nOperator action examples:")
        print(f"  # Schedule reboot on the next check-in:")
        print(f"  curl -k -X POST -H 'Content-Type: application/json' \\")
        print(f"    -d '{{\"type\":\"reboot\"}}' \\")
        print(f"    https://{args.cn}:{args.port}/api/v1/admin/queue/device-<MAC>")
        print(f"")
        print(f"  # Send a log line to device's serial console:")
        print(f"  curl -k -X POST -H 'Content-Type: application/json' \\")
        print(f"    -d '{{\"type\":\"log\",\"params\":{{\"level\":\"info\",\"msg\":\"hi from server\"}}}}' \\")
        print(f"    https://{args.cn}:{args.port}/api/v1/admin/queue/device-<MAC>")
        print(f"\nDevice config:")
        print(f"  Enroll URL  : https://<LAN-ip>:{args.port}/api/v1/enroll")
        print(f"  Checkin URL : https://<LAN-ip>:{args.port}/api/v1/checkin\n")

        try:
            server.serve_forever()
        except KeyboardInterrupt:
            print("\n[mock] Ctrl-C — shutting down")
            server.shutdown()
    finally:
        for p in (fc_path, fk_path):
            try:
                os.unlink(p)
            except OSError:
                pass

    return 0


if __name__ == "__main__":
    sys.exit(main())
