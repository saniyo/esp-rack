#!/usr/bin/env python3
"""ota_sign.py: signs ESP-Rack firmware images for the OTA loader.

The signature is ECDSA P-256 over the SHA-256 of the whole image. A signed
image is the image followed by a 128-byte trailer, so one .bin carries its
signature (little-endian):

  off  len  field
    0    8  magic "ESRKSIG1"
    8    4  imageLen: the image bytes before the trailer
   12    1  sigLen: the DER signature's length (8..72)
   13   72  the DER signature (ECDSA-Sig-Value), zero-padded
   85   39  zero
  124    4  CRC-32 (IEEE, zlib's; persist/Crc32.h) of bytes 0..123

A pull handoff carries the SHA-256 and the DER signature itself (the
`sign --json` sidecar has both).

Keys. The private key never goes into a repository:
  --key PATH / ESPRACK_OTA_KEY   a production key (must exist)
  ~/.esprack/ota_sign_key.pem    the production key, made by `keygen`
  ~/.esprack/ota_dev_key.pem     the dev key: made on first use while there
                                 is no production key. A loader built with it
                                 warns ("dev signing key") and installs
                                 unsigned or badly signed images anyway.
A key whose file is named ota_dev_key.pem is a dev key, any other one is a
production key: a loader built with it refuses images it can't verify.

Commands:
  keygen [--out PATH]          a new production key (never overwrites one)
  sign IMAGE [-o OUT] [--json PATH]
                               IMAGE + trailer (default IMAGE.signed.bin);
                               prints size, SHA-256, signature, key id
  verify FILE [--pub PATH]     checks a signed file: trailer, CRC, signature
                               (--pub: a .pem or the loader's ota_pubkey.h)
  export-pubkey [--out PATH]   the loader's header (default
                               loader/main/ota_pubkey.h), or a .pem
  info                         the key in use, its id, dev or production

Backend: the `cryptography` package when installed (PlatformIO's penv has
it), else a built-in pure-Python P-256 (slower, not constant-time: fine for
a build machine, prefer `cryptography` on a signing server).
"""

import argparse
import base64
import hashlib
import json
import os
import re
import secrets
import struct
import sys
import zlib

TRAILER_MAGIC = b"ESRKSIG1"
TRAILER_LEN = 128
SIG_MAX = 72
DEV_KEY_NAME = "ota_dev_key.pem"
PROD_KEY_NAME = "ota_sign_key.pem"
KEY_ENV = "ESPRACK_OTA_KEY"

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_HEADER = os.path.normpath(os.path.join(HERE, "..", "loader", "main", "ota_pubkey.h"))

# ESP image header: chip id (u16 at offset 12) -> name
CHIP_IDS = {0: "esp32", 2: "esp32s2", 5: "esp32c3", 9: "esp32s3", 12: "esp32c2",
            13: "esp32c6", 16: "esp32h2", 18: "esp32p4"}


class SignError(Exception):
    pass


# --- P-256 (pure Python) -----------------------------------------------------

P = 0xFFFFFFFF00000001000000000000000000000000FFFFFFFFFFFFFFFFFFFFFFFF
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551
G = (0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296,
     0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5)


def _add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    if p1[0] == p2[0]:
        if (p1[1] + p2[1]) % P == 0:
            return None
        lam = (3 * p1[0] * p1[0] + A) * pow(2 * p1[1], -1, P) % P
    else:
        lam = (p2[1] - p1[1]) * pow(p2[0] - p1[0], -1, P) % P
    x = (lam * lam - p1[0] - p2[0]) % P
    return (x, (lam * (p1[0] - x) - p1[1]) % P)


def _mul(k, pt):
    out = None
    while k:
        if k & 1:
            out = _add(out, pt)
        pt = _add(pt, pt)
        k >>= 1
    return out


def _on_curve(pt):
    x, y = pt
    return 0 <= x < P and 0 <= y < P and (y * y - (x * x * x + A * x + B)) % P == 0


def _pure_sign(d, digest):
    e = int.from_bytes(digest, "big")
    while True:
        k = secrets.randbelow(N - 1) + 1
        r = _mul(k, G)[0] % N
        if r == 0:
            continue
        s = pow(k, -1, N) * (e + r * d) % N
        if s:
            return r, s


def _pure_verify(pub, digest, r, s):
    if not (0 < r < N and 0 < s < N):
        return False
    e = int.from_bytes(digest, "big")
    w = pow(s, -1, N)
    pt = _add(_mul(e * w % N, G), _mul(r * w % N, pub))
    return pt is not None and pt[0] % N == r


# --- DER ---------------------------------------------------------------------

OID_EC_PUBLIC_KEY = bytes.fromhex("2a8648ce3d0201")  # 1.2.840.10045.2.1
OID_P256 = bytes.fromhex("2a8648ce3d030107")         # 1.2.840.10045.3.1.7


def _der_len(n):
    if n < 0x80:
        return bytes([n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([0x80 | len(b)]) + b


def _tlv(tag, value):
    return bytes([tag]) + _der_len(len(value)) + value


def _der_int(v):
    b = v.to_bytes((v.bit_length() + 8) // 8 or 1, "big")  # a leading 0 keeps it positive
    return _tlv(0x02, b)


def _read_tlv(buf, pos):
    if pos + 2 > len(buf):
        raise SignError("DER: truncated")
    tag, n = buf[pos], buf[pos + 1]
    pos += 2
    if n & 0x80:
        cnt = n & 0x7F
        if cnt == 0 or cnt > 4 or pos + cnt > len(buf):
            raise SignError("DER: bad length")
        n = int.from_bytes(buf[pos:pos + cnt], "big")
        pos += cnt
    if pos + n > len(buf):
        raise SignError("DER: truncated")
    return tag, buf[pos:pos + n], pos + n


def _seq_items(value):
    items, pos = [], 0
    while pos < len(value):
        tag, v, pos = _read_tlv(value, pos)
        items.append((tag, v))
    return items


def sig_to_der(r, s):
    return _tlv(0x30, _der_int(r) + _der_int(s))


def sig_from_der(der):
    tag, value, end = _read_tlv(der, 0)
    items = _seq_items(value) if tag == 0x30 else []
    if end != len(der) or len(items) != 2 or items[0][0] != 0x02 or items[1][0] != 0x02:
        raise SignError("signature: not a DER ECDSA-Sig-Value")
    return int.from_bytes(items[0][1], "big"), int.from_bytes(items[1][1], "big")


def _pem(label, der):
    b = base64.b64encode(der).decode()
    lines = [b[i:i + 64] for i in range(0, len(b), 64)]
    return ("-----BEGIN %s-----\n%s\n-----END %s-----\n" % (label, "\n".join(lines), label)).encode()


def _unpem(data):
    m = re.search(rb"-----BEGIN ([A-Z ]+)-----(.*?)-----END \1-----", data, re.S)
    if not m:
        raise SignError("not a PEM file")
    if b"ENCRYPTED" in m.group(1) or b"Proc-Type" in m.group(2):
        raise SignError("encrypted keys need the cryptography package")
    return m.group(1).decode(), base64.b64decode(b"".join(m.group(2).split()))


def _point_bytes(pub):
    return b"\x04" + pub[0].to_bytes(32, "big") + pub[1].to_bytes(32, "big")


def _point_from_bytes(b):
    if len(b) != 65 or b[0] != 4:
        raise SignError("public key: not an uncompressed P-256 point")
    pub = (int.from_bytes(b[1:33], "big"), int.from_bytes(b[33:], "big"))
    if not _on_curve(pub):
        raise SignError("public key: not on P-256")
    return pub


def _sec1_private_der(d):
    pub = _mul(d, G)
    return _tlv(0x30, _der_int(1) + _tlv(0x04, d.to_bytes(32, "big"))
                + _tlv(0xA0, _tlv(0x06, OID_P256))
                + _tlv(0xA1, _tlv(0x03, b"\x00" + _point_bytes(pub))))


def _spki_der(pub):
    alg = _tlv(0x30, _tlv(0x06, OID_EC_PUBLIC_KEY) + _tlv(0x06, OID_P256))
    return _tlv(0x30, alg + _tlv(0x03, b"\x00" + _point_bytes(pub)))


def _parse_sec1(der):
    tag, value, _ = _read_tlv(der, 0)
    items = _seq_items(value)
    if tag != 0x30 or len(items) < 2 or items[1][0] != 0x04:
        raise SignError("EC PRIVATE KEY: bad structure")
    for t, v in items[2:]:
        if t == 0xA0 and _read_tlv(v, 0)[1] != OID_P256:
            raise SignError("the key is not a P-256 key")
    return int.from_bytes(items[1][1], "big")


def _parse_pkcs8(der):
    tag, value, _ = _read_tlv(der, 0)
    items = _seq_items(value)
    if tag != 0x30 or len(items) < 3 or items[1][0] != 0x30 or items[2][0] != 0x04:
        raise SignError("PRIVATE KEY: bad structure")
    alg = _seq_items(items[1][1])
    if len(alg) < 2 or alg[0][1] != OID_EC_PUBLIC_KEY or alg[1][1] != OID_P256:
        raise SignError("the key is not a P-256 key")
    return _parse_sec1(items[2][1])


def _parse_spki(der):
    tag, value, _ = _read_tlv(der, 0)
    items = _seq_items(value)
    if tag != 0x30 or len(items) != 2 or items[0][0] != 0x30 or items[1][0] != 0x03:
        raise SignError("PUBLIC KEY: bad structure")
    alg = _seq_items(items[0][1])
    if len(alg) < 2 or alg[0][1] != OID_EC_PUBLIC_KEY or alg[1][1] != OID_P256:
        raise SignError("the key is not a P-256 key")
    return _point_from_bytes(items[1][1][1:])


# --- backends ----------------------------------------------------------------

class PureBackend:
    name = "pure-python"

    def generate(self):
        return secrets.randbelow(N - 1) + 1

    def load_private(self, pem):
        label, der = _unpem(pem)
        if label == "EC PRIVATE KEY":
            d = _parse_sec1(der)
        elif label == "PRIVATE KEY":
            d = _parse_pkcs8(der)
        else:
            raise SignError("not an EC private key (%s)" % label)
        if not 0 < d < N:
            raise SignError("the private key is out of range")
        return d

    def private_pem(self, d):
        return _pem("EC PRIVATE KEY", _sec1_private_der(d))

    def public(self, d):
        return _mul(d, G)

    def sign(self, d, data):
        r, s = _pure_sign(d, hashlib.sha256(data).digest())
        return sig_to_der(r, s)

    def verify(self, pub, data, der):
        r, s = sig_from_der(der)
        return _pure_verify(pub, hashlib.sha256(data).digest(), r, s)


class CryptographyBackend:
    name = "cryptography"

    def __init__(self):
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
        self._ec, self._hashes, self._ser, self._bad = ec, hashes, serialization, InvalidSignature

    def generate(self):
        return self._ec.generate_private_key(self._ec.SECP256R1()).private_numbers().private_value

    def load_private(self, pem):
        pw = os.environ.get("ESPRACK_OTA_KEY_PASS")
        try:
            key = self._ser.load_pem_private_key(pem, password=pw.encode() if pw else None)
        except (TypeError, ValueError) as e:
            raise SignError("can't read the key: %s (encrypted: set ESPRACK_OTA_KEY_PASS)" % e)
        if not isinstance(key, self._ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
            raise SignError("the key is not a P-256 key")
        return key.private_numbers().private_value

    def _key(self, d):
        return self._ec.derive_private_key(d, self._ec.SECP256R1())

    def private_pem(self, d):
        s = self._ser
        return self._key(d).private_bytes(s.Encoding.PEM, s.PrivateFormat.TraditionalOpenSSL,
                                          s.NoEncryption())

    def public(self, d):
        n = self._key(d).public_key().public_numbers()
        return (n.x, n.y)

    def sign(self, d, data):
        return self._key(d).sign(data, self._ec.ECDSA(self._hashes.SHA256()))

    def verify(self, pub, data, der):
        key = self._ec.EllipticCurvePublicNumbers(pub[0], pub[1], self._ec.SECP256R1()).public_key()
        try:
            key.verify(der, data, self._ec.ECDSA(self._hashes.SHA256()))
            return True
        except self._bad:
            return False


def backend(name="auto"):
    if name == "pure":
        return PureBackend()
    try:
        return CryptographyBackend()
    except ImportError:
        if name == "cryptography":
            raise SignError("the cryptography package is not installed")
    return PureBackend()


# --- keys --------------------------------------------------------------------

def key_dir():
    return os.path.join(os.environ.get("USERPROFILE") or os.path.expanduser("~"), ".esprack")


def key_id(pub):
    return hashlib.sha256(_point_bytes(pub)).hexdigest()[:16]


def is_dev_key(path):
    return os.path.basename(path).lower() == DEV_KEY_NAME


def _write_key(be, path, d):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)  # never overwrites
    with os.fdopen(fd, "wb") as f:
        f.write(be.private_pem(d))


def resolve_key(explicit=None, create_dev=True, log=print):
    """(path, dev) of the signing key; makes the dev key on first use."""
    path = explicit or os.environ.get(KEY_ENV)
    if path:
        if not os.path.isfile(path):
            src = "--key" if explicit else KEY_ENV
            raise SignError("%s points to %s, which does not exist (make it with `keygen`)" % (src, path))
        return path, is_dev_key(path)
    prod = os.path.join(key_dir(), PROD_KEY_NAME)
    if os.path.isfile(prod):
        return prod, False
    dev = os.path.join(key_dir(), DEV_KEY_NAME)
    if not os.path.isfile(dev) and create_dev:
        be = backend()
        _write_key(be, dev, be.generate())
        log("ota_sign: WARNING no production key: made a dev signing key at %s" % dev)
    return dev, True


def load_key(be, path):
    with open(path, "rb") as f:
        d = be.load_private(f.read())
    return d, be.public(d)


def load_public(be, path):
    """A public key from a PEM (public or private) or a generated ota_pubkey.h."""
    with open(path, "rb") as f:
        data = f.read()
    if path.lower().endswith(".h"):
        m = re.search(rb"(?:esprack_ota_pubkey|kOtaPubKey)\[65\]\s*=\s*\{([^}]*)\}", data)
        if not m:
            raise SignError("%s: no esprack_ota_pubkey[65] array" % path)
        return _point_from_bytes(bytes(int(x, 16) for x in re.findall(rb"0x([0-9a-fA-F]{2})", m.group(1))))
    label, der = _unpem(data)
    if label == "PUBLIC KEY":
        return _parse_spki(der)
    return be.public(be.load_private(data))


# --- the trailer -------------------------------------------------------------

def make_trailer(image_len, der):
    if not 8 <= len(der) <= SIG_MAX:
        raise SignError("signature length %d out of range" % len(der))
    body = TRAILER_MAGIC + struct.pack("<IB", image_len, len(der)) + der.ljust(SIG_MAX, b"\0")
    body = body.ljust(TRAILER_LEN - 4, b"\0")
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def split_signed(data):
    """(image, der) when data ends with a valid trailer, else (data, None)."""
    if len(data) < TRAILER_LEN:
        return data, None
    t = data[-TRAILER_LEN:]
    if t[:8] != TRAILER_MAGIC:
        return data, None
    if struct.unpack("<I", t[-4:])[0] != zlib.crc32(t[:-4]) & 0xFFFFFFFF:
        raise SignError("signature trailer: CRC mismatch")
    image_len, sig_len = struct.unpack("<IB", t[8:13])
    if image_len != len(data) - TRAILER_LEN:
        raise SignError("signature trailer: imageLen %d, the file has %d image bytes"
                        % (image_len, len(data) - TRAILER_LEN))
    if not 1 <= sig_len <= SIG_MAX:
        raise SignError("signature trailer: sigLen %d out of range" % sig_len)
    return data[:image_len], t[13:13 + sig_len]


def image_chip(image):
    if len(image) < 24 or image[0] != 0xE9:
        return None
    cid = struct.unpack("<H", image[12:14])[0]
    return CHIP_IDS.get(cid, "chip id %d" % cid)


# --- commands ----------------------------------------------------------------

def cmd_keygen(args, be):
    path = args.out or os.environ.get(KEY_ENV) or os.path.join(key_dir(), PROD_KEY_NAME)
    if os.path.exists(path):
        raise SignError("%s exists: not overwritten (delete it yourself if you mean it)" % path)
    d = be.generate()
    _write_key(be, path, d)
    print("key     %s (%s)" % (path, "dev" if is_dev_key(path) else "production"))
    print("key id  %s" % key_id(be.public(d)))
    if not is_dev_key(path):
        print("Next: `ota_sign.py export-pubkey` and rebuild the loader: it then refuses images\n"
              "not signed with this key. Keep an offline backup: without the key a device\n"
              "running that loader takes a new image only over USB.")
    return 0


def cmd_sign(args, be):
    path, dev = resolve_key(args.key)
    d, pub = load_key(be, path)
    with open(args.image, "rb") as f:
        data = f.read()
    image, old = split_signed(data)
    if old is not None:
        print("ota_sign: %s was signed already: signed again" % args.image)
    chip = image_chip(image)
    if chip is None and not args.force:
        raise SignError("%s is not an ESP app image (no 0xE9 header); --force signs it anyway" % args.image)
    der = be.sign(d, image)
    out = args.out or re.sub(r"(\.bin)?$", ".signed.bin", args.image, count=1)
    with open(out, "wb") as f:
        f.write(image + make_trailer(len(image), der))
    info = {
        "file": os.path.basename(out),
        "image_size": len(image),
        "chip": chip,
        "sha256": hashlib.sha256(image).hexdigest(),
        "sig_der": der.hex(),
        "key_id": key_id(pub),
        "dev_key": dev,
    }
    if args.json:
        with open(args.json, "w") as f:
            json.dump(info, f, indent=2)
            f.write("\n")
    if not args.quiet:
        print("signed  %s" % out)
        print("image   %d bytes, %s" % (len(image), chip or "not an ESP image"))
        print("sha256  %s" % info["sha256"])
        print("sig     %s (%d bytes DER)" % (info["sig_der"], len(der)))
        print("key     %s %s" % (info["key_id"], "DEV KEY (the loader warns, nothing is enforced)" if dev else "production"))
    return 0


def cmd_verify(args, be):
    if args.pub:
        pub, src = load_public(be, args.pub), args.pub
    else:
        path, _ = resolve_key(args.key, create_dev=False)
        if not os.path.isfile(path):
            raise SignError("no key to verify with (--pub PATH, or make one: sign / keygen)")
        pub, src = load_key(be, path)[1], path
    with open(args.file, "rb") as f:
        data = f.read()
    image, der = split_signed(data)
    chip = image_chip(image)
    print("image   %d bytes, %s" % (len(image), chip or "not an ESP image"))
    print("sha256  %s" % hashlib.sha256(image).hexdigest())
    if der is None:
        print("UNSIGNED: no signature trailer")
        return 2
    ok = be.verify(pub, image, der)
    print("%s  key %s (%s)" % ("GOOD SIGNATURE" if ok else "BAD SIGNATURE", key_id(pub), src))
    return 0 if ok else 1


def _header(pub, dev):
    pt = _point_bytes(pub)
    rows = ",\n".join("  " + ", ".join("0x%02x" % b for b in pt[i:i + 13]) for i in range(0, 65, 13))
    pem = _pem("PUBLIC KEY", _spki_der(pub)).decode().splitlines()
    pem_c = "\n".join('  "%s\\n"' % line for line in pem)
    if dev:
        note = ("// DEV KEY: no production key was configured. The loader logs \"dev signing\n"
                "// key\" and installs unsigned or badly signed images anyway. `ota_sign.py\n"
                "// keygen`, then export again, makes signatures mandatory.\n")
    else:
        note = "// Production key: the loader installs only images signed with it.\n"
    return ("// Generated by esp-rack/tools/ota_sign.py export-pubkey: do not edit.\n"
            "// The OTA loader checks image signatures (ECDSA P-256 over SHA-256) with it.\n"
            + note +
            "#pragma once\n\n#include <stdint.h>\n\n"
            "#define ESPRACK_OTA_KEY_DEV %d  // 1: a dev key (warn only), 0: signatures mandatory\n"
            "#define ESPRACK_OTA_KEY_ID \"%s\"  // SHA-256 of the point, first 8 bytes\n\n"
            "// The public point, uncompressed SEC1: 0x04 || X || Y.\n"
            "static const uint8_t esprack_ota_pubkey[65] = {\n%s\n};\n\n"
            "// The same key as PEM (SubjectPublicKeyInfo); for mbedtls_pk_parse_public_key\n"
            "// pass sizeof (it counts the terminating NUL).\n"
            "static const char esprack_ota_pubkey_pem[] =\n%s;\n"
            % (1 if dev else 0, key_id(pub), rows, pem_c))


def cmd_export(args, be):
    path, dev = resolve_key(args.key)
    pub = load_key(be, path)[1]
    out = args.out or DEFAULT_HEADER
    content = (_header(pub, dev) if not out.lower().endswith(".pem")
               else _pem("PUBLIC KEY", _spki_der(pub)).decode())
    old = None
    if os.path.isfile(out):
        with open(out, "r", newline="") as f:
            old = f.read()
    if old != content:  # unchanged: not touched (no rebuild)
        os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
        with open(out, "w", newline="\n") as f:
            f.write(content)
    print("%s %s: key %s (%s)" % ("wrote" if old != content else "unchanged", out, key_id(pub),
                                  "DEV KEY" if dev else "production"))
    return 0


def cmd_info(args, be):
    path, dev = resolve_key(args.key, create_dev=False)
    print("backend %s" % be.name)
    if not os.path.isfile(path):
        print("key     none yet (the dev key %s is made on first sign/export)" % path)
        return 0
    print("key     %s (%s)" % (path, "DEV KEY" if dev else "production"))
    print("key id  %s" % key_id(load_key(be, path)[1]))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description="Signs ESP-Rack firmware images for the OTA loader.")
    ap.add_argument("--backend", choices=("auto", "cryptography", "pure"), default="auto")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("keygen", help="a new production key (never overwrites one)")
    p.add_argument("--out", help="the key file (default: %s, else ~/.esprack/%s)" % (KEY_ENV, PROD_KEY_NAME))
    p = sub.add_parser("sign", help="append the signature trailer")
    p.add_argument("image")
    p.add_argument("-o", "--out", help="default: IMAGE with .signed.bin")
    p.add_argument("--json", help="also write size, sha256 and the signature as JSON")
    p.add_argument("--key", help="the private key (default: see the module doc)")
    p.add_argument("--force", action="store_true", help="sign a file that is not an ESP image")
    p.add_argument("-q", "--quiet", action="store_true")
    p = sub.add_parser("verify", help="check a signed image")
    p.add_argument("file")
    p.add_argument("--pub", help="a .pem (public or private) or the loader's ota_pubkey.h")
    p.add_argument("--key", help="the private key whose public half checks it")
    p = sub.add_parser("export-pubkey", help="the loader's public key header (or a .pem)")
    p.add_argument("--out", help="default: %s" % DEFAULT_HEADER)
    p.add_argument("--key")
    p = sub.add_parser("info", help="the key in use")
    p.add_argument("--key")
    args = ap.parse_args(argv)
    try:
        be = backend(args.backend)
        return {"keygen": cmd_keygen, "sign": cmd_sign, "verify": cmd_verify,
                "export-pubkey": cmd_export, "info": cmd_info}[args.cmd](args, be)
    except (SignError, OSError) as e:
        print("ota_sign: error: %s" % e, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
