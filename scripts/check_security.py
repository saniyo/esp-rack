#!/usr/bin/env python3
"""On-device security regression check for an ESPRack firmware (LIB-FIX-003/004/005/011).

Runs against a LIVE device over HTTP + WebSocket using only the Python
standard library. Every write it performs re-sends a value the device already
has, so settings are not changed.

    python scripts/check_security.py --host 192.168.4.1 --admin admin:admin
    python scripts/check_security.py --host 10.0.0.42 --admin admin:secret \\
        --guest guest:guest --reader-token <JWT of a "reader" machine token>

Mint a reader token beforehand in System -> API tokens (role "reader"), or
pass --mint-reader to let the script create one named "sec-check" (revoke it
afterwards in the same tab).

Checks (each PASS / FAIL / SKIP):
  auth-required    unauthenticated GET /api/v1/ntp/form -> 401
  ui-dynamic       unauthenticated GET /api/v1/ui/dynamic -> 401        (005)
  presence-ws      unauthenticated upgrade of /ws/presence refused      (005)
  mask-form        guest GET /api/v1/wifi/form: secret values masked    (004)
  mask-state       guest GET /api/v1/mqtt/state: no "pwd" key            (004)
  write-guest      guest POST /api/v1/ntp/state (admin-only settings) -> 403   (011)
  write-admin      admin POST /api/v1/ntp/state (same value) -> 200
  write-reader     reader POST /api/v1/ntp/state -> 403                  (C3)
  ws-sub-admin     guest WS sub to admin-only channel "telegram" gets no data (003)
  ws-write-reader  reader WS "p" write to "ntp" is ignored               (003)
"""
import argparse
import base64
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request

RESULTS = []


def record(name, ok, detail=""):
    status = "SKIP" if ok is None else ("PASS" if ok else "FAIL")
    RESULTS.append((name, status, detail))
    print(f"[{status}] {name}{(' - ' + detail) if detail else ''}")


class Device:
    def __init__(self, host, port=80, timeout=8):
        self.base = f"http://{host}:{port}"
        self.host, self.port, self.timeout = host, port, timeout

    def request(self, method, path, token=None, body=None):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header("Content-Type", "application/json")
        if token:
            req.add_header("Authorization", "Bearer " + token)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                raw = r.read()
                return r.status, (json.loads(raw) if raw[:1] in (b"{", b"[") else raw)
        except urllib.error.HTTPError as e:
            raw = e.read()
            try:
                return e.code, json.loads(raw)
            except Exception:
                return e.code, raw

    def sign_in(self, cred):
        user, _, pwd = cred.partition(":")
        st, body = self.request("POST", "/api/v1/auth/token", body={"username": user, "pwd": pwd})
        if st != 200 or not isinstance(body, dict) or "access_token" not in body:
            return None
        return body["access_token"]


# ---- minimal RFC 6455 client (text frames only) ----------------------------
class WsClient:
    def __init__(self, host, port, path, timeout=4):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        req = (f"GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
        self.sock.sendall(req.encode())
        head = b""
        while b"\r\n\r\n" not in head:
            chunk = self.sock.recv(1024)
            if not chunk:
                break
            head += chunk
        self.status_line = head.split(b"\r\n", 1)[0].decode(errors="replace")
        self.ok = " 101 " in self.status_line
        self.buf = head.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in head else b""

    def send(self, obj):
        payload = json.dumps(obj).encode()
        mask = os.urandom(4)
        hdr = bytearray([0x81])
        n = len(payload)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr.append(0x80 | 126)
            hdr += n.to_bytes(2, "big")
        else:
            hdr.append(0x80 | 127)
            hdr += n.to_bytes(8, "big")
        hdr += mask
        self.sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("closed")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv_until(self, seconds):
        """Collect text frames (parsed JSON) for `seconds`."""
        frames, end = [], time.time() + seconds
        while time.time() < end:
            self.sock.settimeout(max(0.05, end - time.time()))
            try:
                b0, b1 = self._read(2)
            except (socket.timeout, ConnectionError):
                break
            n = b1 & 0x7F
            if n == 126:
                n = int.from_bytes(self._read(2), "big")
            elif n == 127:
                n = int.from_bytes(self._read(8), "big")
            data = self._read(n)
            if b0 & 0x0F == 0x1:
                try:
                    frames.append(json.loads(data))
                except ValueError:
                    pass
            elif b0 & 0x0F == 0x8:
                break
        return frames

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=80)
    ap.add_argument("--admin", required=True, help="user:password of an admin")
    ap.add_argument("--guest", default="guest:guest", help="user:password of a NON-admin user")
    ap.add_argument("--reader-token", help="JWT of a reader machine token")
    ap.add_argument("--mint-reader", action="store_true", help="create a reader token 'sec-check' via the admin")
    ap.add_argument("--ws-path", default="/ws/app")
    a = ap.parse_args()

    dev = Device(a.host, a.port)
    admin = dev.sign_in(a.admin)
    if not admin:
        print("cannot sign in as admin - check --host/--admin")
        return 2
    guest = dev.sign_in(a.guest)
    reader = a.reader_token
    if not reader and a.mint_reader:
        st, body = dev.request("POST", "/api/v1/actions/tokens.create?name=sec-check&role=0", token=admin)
        msg = body.get("message", "") if isinstance(body, dict) else ""
        reader = msg.rsplit(": ", 1)[-1].strip() if "shown once" in msg else None

    st, _ = dev.request("GET", "/api/v1/ntp/form")
    record("auth-required", st == 401, f"status {st}")
    st, _ = dev.request("GET", "/api/v1/ui/dynamic")
    record("ui-dynamic", st in (401, 404), f"status {st} (404 = module not installed)")
    try:
        ws = WsClient(a.host, a.port, "/ws/presence")
        record("presence-ws", not ws.ok, ws.status_line)
        ws.close()
    except OSError as e:
        record("presence-ws", None, str(e))

    if guest:
        st, form = dev.request("GET", "/api/v1/wifi/form", token=guest)
        if st == 200 and isinstance(form, dict):
            leaks = []

            def walk(node):
                if isinstance(node, dict):
                    o = node.get("o")
                    if isinstance(o, str) and o.startswith("secret;"):
                        for k, v in node.items():
                            if k != "o" and v not in ("", "********"):
                                leaks.append(k)
                        return
                    for v in node.values():
                        walk(v)
                elif isinstance(node, list):
                    for v in node:
                        walk(v)
            walk(form)
            record("mask-form", not leaks, f"leaked: {leaks}" if leaks else "")
        else:
            record("mask-form", None, f"wifi form status {st}")
        st, state = dev.request("GET", "/api/v1/mqtt/state", token=guest)
        if st == 200 and isinstance(state, dict):
            record("mask-state", state.get("pwd", "") in ("", "********"), "")
        else:
            record("mask-state", None, f"mqtt state status {st} (module installed?)")
    else:
        record("mask-form", None, "no guest login")
        record("mask-state", None, "no guest login")

    st, ntp = dev.request("GET", "/api/v1/ntp/state", token=admin)
    server = ntp.get("server") if st == 200 and isinstance(ntp, dict) else None
    if server is None:
        for name in ("write-guest", "write-admin", "write-reader", "ws-write-reader"):
            record(name, None, "ntp module not installed")
    else:
        same = {"server": server}
        if guest:
            st, _ = dev.request("POST", "/api/v1/ntp/state", token=guest, body=same)
            record("write-guest", st == 403, f"status {st}")
        else:
            record("write-guest", None, "no guest login")
        st, _ = dev.request("POST", "/api/v1/ntp/state", token=admin, body=same)
        record("write-admin", st == 200, f"status {st}")
        if reader:
            st, _ = dev.request("POST", "/api/v1/ntp/state", token=reader, body=same)
            record("write-reader", st == 403, f"status {st}")
            try:
                ws = WsClient(a.host, a.port, f"{a.ws_path}?access_token={reader}")
                ws.recv_until(0.5)
                probe = server + ".sec-check"
                ws.send({"type": "p", "ch": "ntp", "origin_id": "sec-check", "p": {"server": probe}})
                ws.recv_until(1.0)
                ws.close()
                st, after = dev.request("GET", "/api/v1/ntp/state", token=admin)
                changed = isinstance(after, dict) and after.get("server") == probe
                if changed:  # restore before reporting
                    dev.request("POST", "/api/v1/ntp/state", token=admin, body=same)
                record("ws-write-reader", not changed, "write applied!" if changed else "")
            except OSError as e:
                record("ws-write-reader", None, str(e))
        else:
            record("write-reader", None, "no reader token (--reader-token / --mint-reader)")
            record("ws-write-reader", None, "no reader token")

    if guest:
        try:
            ws = WsClient(a.host, a.port, f"{a.ws_path}?access_token={guest}")
            ws.recv_until(0.5)
            ws.send({"type": "sub", "ch": "telegram"})
            frames = [f for f in ws.recv_until(1.5) if f.get("ch") == "telegram"]
            ws.close()
            record("ws-sub-admin", not frames, f"{len(frames)} frame(s) received" if frames else "")
        except OSError as e:
            record("ws-sub-admin", None, str(e))
    else:
        record("ws-sub-admin", None, "no guest login")

    failed = [r for r in RESULTS if r[1] == "FAIL"]
    print(f"\n{len(RESULTS) - len(failed)} ok/skip, {len(failed)} failed")
    if a.mint_reader and reader:
        print("NOTE: revoke the 'sec-check' token in System -> API tokens.")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
