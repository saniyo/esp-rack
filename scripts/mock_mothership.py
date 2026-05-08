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

try:
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec, padding  # noqa: F401
    from cryptography.x509.oid import NameOID
except ImportError:
    print("ERROR: cryptography lib missing — run: pip install cryptography",
          file=sys.stderr)
    sys.exit(1)


# ----- root CA generation (in-memory, fresh each run) -----

def make_root_ca() -> Tuple[x509.Certificate, ec.EllipticCurvePrivateKey]:
    """Self-signed ECDSA-P256 CA — 10 year validity. Lives only in
    process memory; killed when this script stops."""
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
        if self.path != "/api/v1/enroll":
            self._send_json(404, {"err": "not found"})
            return

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
    args = ap.parse_args()

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

        print(f"\nListening on https://{args.host}:{args.port}/api/v1/enroll")
        print(f"Bootstrap token (paste into device PKI Enrollment tab):")
        print(f"  {args.token}")
        print(f"\nDevice's `Enroll URL` should be:")
        print(f"  https://<your-LAN-ip>:{args.port}/api/v1/enroll")
        print(f"  (or https://{args.cn}:{args.port}/api/v1/enroll if "
              f"the device can resolve mDNS)\n")

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
