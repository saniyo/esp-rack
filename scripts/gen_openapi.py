#!/usr/bin/env python3
"""gen_openapi.py - generate api/openapi.json (+ .yaml when PyYAML exists)
from a device's /api/v1/ui/manifest dump.

api-standardization phase C4: the WebManager registry IS the API
inventory - every typed feature (form/state faces), every action and
every bespoke page endpoint is present in the manifest, so the OpenAPI
document is GENERATED, never hand-maintained, and can never drift from
the firmware. The spec is built off-device (CI / dev PC); the device
serves no spec bytes.

Sources, in priority order:
  --manifest <file>     a saved manifest JSON (e.g. api/manifest-dump.json)
  --device <ip> [--user/--pwd | --token]   fetch live from a device

Optional api/schema-hints.json is deep-merged OVER the generated
document - use it to add response schemas the registry cannot know
(e.g. the flat shape of /system/health). Generated parts always win on
structure; hints win on leaves they define.

Typical use:
  python scripts/gen_openapi.py --device 10.222.225.152 --user admin --pwd admin
  python scripts/gen_openapi.py --manifest api/manifest-dump.json
  npx -y @stoplight/spectral-cli lint api/openapi.json   # CI gate
  npx -y @openapitools/openapi-generator-cli generate \
      -i api/openapi.json -g python -o clients/python    # server client
"""

import argparse
import copy
import json
import sys
import urllib.request
from pathlib import Path

API = "/api/v1"

PROBLEM_SCHEMA = {
    "type": "object",
    "description": "RFC 9457 problem details (application/problem+json). "
                   "Clients branch on the stable `title` slug.",
    "properties": {
        "status": {"type": "integer"},
        "title": {"type": "string", "description": "machine-readable slug, e.g. heap_pressure"},
        "detail": {"type": "string"},
        "retryAfter": {"type": "integer", "description": "seconds; present on transient 503s"},
    },
    "required": ["status", "title"],
}

STD_RESPONSES = {
    "Unauthorized": {
        "description": "Missing or invalid bearer token",
        "content": {"application/problem+json": {"schema": {"$ref": "#/components/schemas/Problem"}}},
    },
    "Forbidden": {
        "description": "Valid identity, insufficient role (admin endpoint, or a "
                       "mutation attempted with a reader token)",
        "content": {"application/problem+json": {"schema": {"$ref": "#/components/schemas/Problem"}}},
    },
    "HeapPressure": {
        "description": "Transient heap pressure - retry after `retryAfter` seconds",
        "content": {"application/problem+json": {"schema": {"$ref": "#/components/schemas/Problem"}}},
    },
}


def fetch_manifest(device: str, user: str, pwd: str, token: str) -> dict:
    base = f"http://{device}"
    if not token:
        body = json.dumps({"username": user, "pwd": pwd}).encode()
        req = urllib.request.Request(f"{base}{API}/auth/token", data=body,
                                     headers={"Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=10) as r:
            token = json.load(r)["access_token"]
    req = urllib.request.Request(f"{base}{API}/ui/manifest",
                                 headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=20) as r:
        return json.load(r)


def sec(auth: str):
    """Map a manifest auth level to the operation security requirement."""
    return [] if auth == "none" else [{"bearerAuth": []}]


def err_refs(auth: str) -> dict:
    out = {"503": {"$ref": "#/components/responses/HeapPressure"}}
    if auth != "none":
        out["401"] = {"$ref": "#/components/responses/Unauthorized"}
        out["403"] = {"$ref": "#/components/responses/Forbidden"}
    return out


def op(summary: str, auth: str, tag: str, ok_desc: str = "OK",
       ok_content: str = "application/json", request_body: bool = False) -> dict:
    o = {
        "summary": summary,
        "tags": [tag],
        "security": sec(auth),
        "responses": {"200": {"description": ok_desc,
                              "content": {ok_content: {"schema": {"type": "object"}}}},
                      **err_refs(auth)},
    }
    if request_body:
        o["requestBody"] = {
            "required": True,
            "content": {"application/json": {"schema": {"type": "object"}}},
        }
    return o


def deep_merge(dst: dict, src: dict) -> dict:
    for k, v in src.items():
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            deep_merge(dst[k], v)
        else:
            dst[k] = copy.deepcopy(v)
    return dst


def build(manifest: dict) -> dict:
    device = manifest.get("device", {}) or {}
    paths: dict = {}

    # ---- framework-contract seeds (not registry entries) -------------
    paths[f"{API}/auth/token"] = {"post": {
        "summary": "Mint a human session JWT (24 h)",
        "tags": ["auth"],
        "security": [],
        "requestBody": {"required": True, "content": {"application/json": {"schema": {
            "type": "object",
            "properties": {"username": {"type": "string"}, "pwd": {"type": "string"}},
            "required": ["username", "pwd"]}}}},
        "responses": {
            "200": {"description": "Token minted", "content": {"application/json": {"schema": {
                "type": "object", "properties": {"access_token": {"type": "string"}}}}}},
            "401": {"$ref": "#/components/responses/Unauthorized"},
        },
    }}
    paths[f"{API}/auth/verify"] = {"get": op("Validate the presented bearer token",
                                             "authenticated", "auth", "Token valid")}
    paths[f"{API}/ui/manifest"] = {"get": op(
        "UI manifest - the machine-readable registry this spec is generated from "
        f"(schemaVersion {manifest.get('schemaVersion')})", "none", "ui")}

    # ---- registry walk ------------------------------------------------
    for f in manifest.get("features", []):
        kind = f.get("kind")
        fid = f.get("id", "?")
        auth = f.get("auth", "authenticated")
        title = f.get("title") or fid

        if kind == "feature":
            rest = f.get("rest") or {}
            form = rest.get("read")
            state = rest.get("state") or rest.get("update")
            if form:
                paths.setdefault(form, {})["get"] = op(
                    f"{title}: Form-DSL envelope (web-UI face)", auth, fid)
            if state:
                paths.setdefault(state, {})["get"] = op(
                    f"{title}: flat values (machine face)", auth, fid)
                post = op(f"{title}: set values (any subset; echoes the new state)",
                          auth, fid, request_body=True)
                post["description"] = ("Requires a write-capable role: reader tokens "
                                       "receive 403 forbidden.")
                post["responses"]["400"] = {"description": "Values rejected by the service validator"}
                paths[state]["post"] = post

        elif kind == "action":
            a_path = f"{API}/actions/{fid}"
            o = op(title, auth, "actions", "Action accepted/executed")
            o["description"] = ("Mutation - requires a write-capable role on top of the "
                                "endpoint auth level (reader tokens receive 403). Arguments, "
                                "when any, ride as query parameters.")
            o["responses"]["202"] = {"description": "Accepted - long-running action started"}
            paths.setdefault(a_path, {})["post"] = o

        # tabs may point at bespoke form pages not otherwise in `paths`
        for t in f.get("tabs", []) or []:
            rp = t.get("restPath")
            if rp and rp not in paths:
                paths[rp] = {"get": op(f"{title} / {t.get('title') or t.get('key')}: Form-DSL envelope",
                                       t.get("auth", auth), fid)}

        # bespoke page endpoints (fs, upload, peers, system forms...)
        for e in f.get("endpoints", []) or []:
            p = e.get("path")
            m = (e.get("method") or "GET").lower()
            if not p:
                continue
            o = op(f"{title}: {e.get('role') or p}", e.get("auth", auth), fid,
                   request_body=(m in ("post", "put")))
            if m in ("post", "put"):
                o["description"] = "Mutation - requires a write-capable role (reader tokens: 403)."
            paths.setdefault(p, {})[m] = o

    spec = {
        "openapi": "3.1.0",
        "info": {
            "title": f"{device.get('name', 'ESP-Rack')} device API",
            "version": str(device.get('version', '0.0.0')).lstrip('v'),
            "description": ("Generated by scripts/gen_openapi.py from the device's "
                            "/api/v1/ui/manifest registry dump - do not edit by hand. "
                            "Errors are RFC 9457 application/problem+json everywhere."),
        },
        "servers": [{"url": "http://{device_ip}",
                     "variables": {"device_ip": {"default": "192.168.4.1"}}}],
        "tags": [{"name": t} for t in sorted(
            {t for ops in paths.values() for o in ops.values()
             for t in o.get("tags", [])}, key=str.lower)],
        "paths": dict(sorted(paths.items())),
        "components": {
            "securitySchemes": {"bearerAuth": {
                "type": "http", "scheme": "bearer", "bearerFormat": "JWT",
                "description": ("Human session JWT from /auth/token, or a long-lived "
                                "machine token minted in System -> API Tokens "
                                "(roles: reader = GET only, operator = read/write, admin)."),
            }},
            "schemas": {"Problem": PROBLEM_SCHEMA},
            "responses": STD_RESPONSES,
        },
    }
    return spec


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--manifest", help="path to a saved manifest JSON dump")
    ap.add_argument("--device", help="device IP to fetch the manifest from")
    ap.add_argument("--user", default="admin")
    ap.add_argument("--pwd", default="admin")
    ap.add_argument("--token", help="bearer token (skips /auth/token)")
    ap.add_argument("--hints", default="api/schema-hints.json")
    ap.add_argument("--out", default="api/openapi.json")
    args = ap.parse_args()

    if args.manifest:
        manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    elif args.device:
        manifest = fetch_manifest(args.device, args.user, args.pwd, args.token)
    else:
        ap.error("provide --manifest or --device")

    if not manifest.get("features"):
        print("manifest has no features - wrong file or anonymous stub?", file=sys.stderr)
        return 2

    spec = build(manifest)

    hints = Path(args.hints)
    if hints.exists():
        deep_merge(spec, json.loads(hints.read_text(encoding="utf-8")))
        print(f"merged hints from {hints}")

    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(spec, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out} ({out.stat().st_size} B, {len(spec['paths'])} paths)")

    try:
        import yaml  # type: ignore
        ypath = out.with_suffix(".yaml")
        ypath.write_text(yaml.safe_dump(spec, sort_keys=False, allow_unicode=True),
                         encoding="utf-8")
        print(f"wrote {ypath}")
    except ImportError:
        print("PyYAML not installed - JSON only (spectral accepts JSON)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
