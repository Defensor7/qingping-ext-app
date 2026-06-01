#!/usr/bin/env python3
"""Drive HA's onboarding + add the MQTT integration + mint a long-lived
access token. Used by dev/run.sh once the HA container is up.

Idempotent: if HA is already onboarded, just skip ahead and (re)issue a
fresh long-lived token. Writes the token to dev/ha-config/.qpext-token so
dev/switch-device.sh can pick it up.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

HA = os.environ.get("HA_URL", "http://localhost:8123")
USERNAME = os.environ.get("HA_USER", "admin")
PASSWORD = os.environ.get("HA_PASS", "admin")
CLIENT_ID = "http://qpext.local/"   # any URL; HA stores it as the client id
TOKEN_FILE = os.environ.get("QPEXT_TOKEN_FILE", "ha-config/.qpext-token")

MQTT_BROKER_HOST = os.environ.get("MQTT_HOST", "mqtt")
MQTT_BROKER_PORT = int(os.environ.get("MQTT_PORT", "1883"))


def _req(method: str, path: str, body=None, headers=None, expect_json=True):
    url = f"{HA}{path}"
    data = None
    hdrs = {"User-Agent": "qpext-bootstrap/1"}
    if headers:
        hdrs.update(headers)
    if body is not None:
        if isinstance(body, dict) and hdrs.get("Content-Type", "application/json") == "application/json":
            data = json.dumps(body).encode()
            hdrs.setdefault("Content-Type", "application/json")
        else:
            data = body.encode() if isinstance(body, str) else body
    req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            payload = r.read()
            if not expect_json or not payload:
                return r.status, payload
            return r.status, json.loads(payload)
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def wait_for_ha(timeout: int = 120) -> None:
    print(f"[bootstrap] waiting for {HA} (up to {timeout}s)…", flush=True)
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(f"{HA}/manifest.json", timeout=3) as r:
                if r.status == 200:
                    print("[bootstrap] HA is up", flush=True)
                    return
        except Exception:
            pass
        time.sleep(2)
    sys.exit(f"[bootstrap] timed out waiting for {HA}")


def onboard() -> str | None:
    """Run onboarding. Returns an auth code if a fresh user was created,
    None if onboarding was already finished by a previous run."""
    status, body = _req("POST", "/api/onboarding/users", {
        "name": "Debug Admin",
        "username": USERNAME,
        "password": PASSWORD,
        "language": "en",
        "client_id": CLIENT_ID,
    })
    if status == 200 and isinstance(body, dict) and "auth_code" in body:
        print("[bootstrap] created admin user", flush=True)
        return body["auth_code"]
    # Already onboarded? The endpoint 403s once onboarding is complete.
    if status in (403, 404):
        print("[bootstrap] onboarding already done — will reuse existing user", flush=True)
        return None
    sys.exit(f"[bootstrap] onboarding/users failed: {status} {body!r}")


def exchange_code(code: str) -> str:
    status, body = _req(
        "POST",
        "/auth/token",
        urllib.parse.urlencode({
            "grant_type": "authorization_code",
            "code": code,
            "client_id": CLIENT_ID,
        }),
        headers={"Content-Type": "application/x-www-form-urlencoded"},
    )
    if status != 200 or not isinstance(body, dict):
        sys.exit(f"[bootstrap] /auth/token (code) failed: {status} {body!r}")
    return body["access_token"]


def login_with_password() -> str:
    """Onboarding already done — log in via the auth flow to get a fresh
    short-lived access token that we can use to mint a long-lived one."""
    # 1. Start the login flow
    status, body = _req(
        "POST",
        "/auth/login_flow",
        {"client_id": CLIENT_ID, "handler": ["homeassistant", None], "redirect_uri": CLIENT_ID},
    )
    if status != 200 or not isinstance(body, dict):
        sys.exit(f"[bootstrap] /auth/login_flow failed: {status} {body!r}")
    flow_id = body["flow_id"]
    # 2. Submit credentials
    status, body = _req(
        "POST",
        f"/auth/login_flow/{flow_id}",
        {"client_id": CLIENT_ID, "username": USERNAME, "password": PASSWORD},
    )
    if status != 200 or not isinstance(body, dict) or "result" not in body:
        sys.exit(f"[bootstrap] login failed: {status} {body!r}")
    return exchange_code(body["result"])


def finish_onboarding(access: str) -> None:
    h = {"Authorization": f"Bearer {access}"}
    # core_config and analytics take an empty body; integration requires
    # {client_id, redirect_uri} and (when accepted) returns a fresh auth_code.
    bodies = {
        "core_config": {},
        "analytics": {},
        "integration": {"client_id": CLIENT_ID, "redirect_uri": CLIENT_ID},
    }
    for step, body in bodies.items():
        status, _ = _req("POST", f"/api/onboarding/{step}", body, headers=h)
        if status in (200, 201):
            print(f"[bootstrap] onboarding step '{step}' done", flush=True)
        elif status == 403:
            pass  # already done — fine
        else:
            print(f"[bootstrap] onboarding/{step} -> {status}", flush=True)


def add_mqtt_integration(access: str) -> None:
    """Drive the MQTT config_flow. The first step asks for the broker
    address+port and optional auth — for our debug broker we leave auth
    empty (anonymous)."""
    h = {"Authorization": f"Bearer {access}"}
    # Check whether an MQTT entry already exists.
    status, body = _req("GET", "/api/config/config_entries/entry", headers=h)
    if status == 200 and isinstance(body, list):
        if any(e.get("domain") == "mqtt" for e in body):
            print("[bootstrap] MQTT integration already configured", flush=True)
            return

    status, body = _req(
        "POST",
        "/api/config/config_entries/flow",
        {"handler": "mqtt", "show_advanced_options": True},
        headers=h,
    )
    if status != 200 or not isinstance(body, dict):
        sys.exit(f"[bootstrap] starting mqtt flow failed: {status} {body!r}")
    flow_id = body["flow_id"]
    print(f"[bootstrap] MQTT flow started: step={body.get('step_id')}", flush=True)

    # Submit broker step. Field names match the MQTT integration's
    # `broker` step schema (HA 2024+: broker, port, username, password).
    status, body = _req(
        "POST",
        f"/api/config/config_entries/flow/{flow_id}",
        {
            "broker": MQTT_BROKER_HOST,
            "port": MQTT_BROKER_PORT,
        },
        headers=h,
    )
    if status not in (200, 201):
        sys.exit(f"[bootstrap] MQTT broker step failed: {status} {body!r}")
    # MQTT flow may have a follow-up "options" step (discovery, etc.) — accept defaults.
    while isinstance(body, dict) and body.get("type") == "form":
        flow_id = body["flow_id"]
        status, body = _req(
            "POST",
            f"/api/config/config_entries/flow/{flow_id}",
            {},
            headers=h,
        )
        if status not in (200, 201):
            sys.exit(f"[bootstrap] MQTT follow-up step failed: {status} {body!r}")
    print("[bootstrap] MQTT integration configured", flush=True)


def mint_long_lived(access: str) -> str:
    """Create a long-lived access token via the WebSocket API.

    HA dropped the REST `/auth/long_lived_access_token` endpoint; the only
    way to mint a long-lived token now is the websocket command of the
    same name. We talk RFC-6455 with the stdlib helpers above so we don't
    need a websockets/aiohttp dependency.
    """
    parsed = urllib.parse.urlparse(HA)
    host = parsed.hostname or "localhost"
    port = parsed.port or (443 if parsed.scheme == "https" else 80)
    if parsed.scheme == "https":
        sys.exit("[bootstrap] HTTPS WS not supported by this bootstrap (debug stack is HTTP)")

    ws = _ws_connect(host, port, "/api/websocket")
    try:
        hello = json.loads(_ws_recv(ws))
        if hello.get("type") != "auth_required":
            sys.exit(f"[bootstrap] WS hello not auth_required: {hello}")
        _ws_send(ws, json.dumps({"type": "auth", "access_token": access}))
        ack = json.loads(_ws_recv(ws))
        if ack.get("type") != "auth_ok":
            sys.exit(f"[bootstrap] WS auth failed: {ack}")
        _ws_send(ws, json.dumps({
            "id": 1,
            "type": "auth/long_lived_access_token",
            "client_name": "qpext debug",
            "lifespan": 3650,
        }))
        result = json.loads(_ws_recv(ws))
        if not result.get("success"):
            sys.exit(f"[bootstrap] long_lived_access_token failed: {result}")
        return result["result"]
    finally:
        ws.close()


# --------------------------------------------------------------------------- #
# Minimal stdlib WebSocket client                                             #
# --------------------------------------------------------------------------- #
# HA's REST API doesn't expose long-lived access token creation any more;
# the only path is the websocket command `auth/long_lived_access_token`.
# Rather than pull in `websockets`/`aiohttp` as a dep, implement just enough
# of RFC 6455 to ship one frame and read one back.


class _WS:
    """Tiny WS-over-socket wrapper. Holds leftover bytes from the HTTP-Upgrade
    handshake (HA sends `auth_required` immediately, often in the same TCP
    segment as the 101 response)."""

    def __init__(self, sock: socket.socket, leftover: bytes) -> None:
        self.sock = sock
        self.buf = leftover

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def read(self, n: int) -> bytes:
        out = bytearray()
        if self.buf:
            take = min(n, len(self.buf))
            out += self.buf[:take]
            self.buf = self.buf[take:]
            n -= take
        while n:
            chunk = self.sock.recv(n)
            if not chunk:
                raise EOFError
            out += chunk
            n -= len(chunk)
        return bytes(out)

    def write(self, payload: bytes) -> None:
        self.sock.sendall(payload)


def _ws_connect(host: str, port: int, path: str) -> _WS:
    sock = socket.create_connection((host, port), timeout=15)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n"
        f"\r\n"
    )
    sock.sendall(req.encode())
    buf = b""
    while b"\r\n\r\n" not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            sock.close()
            sys.exit("[bootstrap] WS upgrade: EOF during handshake")
        buf += chunk
    if b" 101 " not in buf.split(b"\r\n", 1)[0]:
        sock.close()
        sys.exit(f"[bootstrap] WS upgrade failed:\n{buf[:200].decode(errors='replace')}")
    # Sanity-check the server's Accept token.
    expected = base64.b64encode(
        hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()).digest()
    ).decode()
    if f"Sec-WebSocket-Accept: {expected}".encode() not in buf:
        sock.close()
        sys.exit("[bootstrap] WS upgrade: bad Accept header")
    leftover = buf.split(b"\r\n\r\n", 1)[1]
    return _WS(sock, leftover)


def _ws_send(ws: _WS, payload: str) -> None:
    data = payload.encode()
    n = len(data)
    hdr = bytearray([0x81])
    if n < 126:
        hdr.append(0x80 | n)
    elif n < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack(">H", n)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack(">Q", n)
    mask = os.urandom(4)
    hdr += mask
    ws.write(bytes(hdr))
    ws.write(bytes(b ^ mask[i & 3] for i, b in enumerate(data)))


def _ws_recv(ws: _WS) -> str:
    out = bytearray()
    while True:
        h = ws.read(2)
        fin = h[0] & 0x80
        op = h[0] & 0x0F
        masked = h[1] & 0x80
        n = h[1] & 0x7F
        if n == 126:
            n = struct.unpack(">H", ws.read(2))[0]
        elif n == 127:
            n = struct.unpack(">Q", ws.read(8))[0]
        mask = ws.read(4) if masked else b""
        payload = ws.read(n) if n else b""
        if masked:
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        if op == 0x8:
            raise EOFError("server closed")
        if op in (0x1, 0x2, 0x0):
            out += payload
            if fin:
                return out.decode()
        # 0x9 (ping) / 0xA (pong) — ignored


def write_token_file(token: str) -> None:
    os.makedirs(os.path.dirname(TOKEN_FILE) or ".", exist_ok=True)
    with open(TOKEN_FILE, "w") as f:
        f.write(token + "\n")
    os.chmod(TOKEN_FILE, 0o600)
    print(f"[bootstrap] long-lived token saved → {TOKEN_FILE}", flush=True)


def list_demo_entities(access: str) -> dict[str, str]:
    """Return {domain: entity_id} picking one demo entity per domain we care
    about. Demo entities ship via the `demo:` line in configuration.yaml —
    written by dev/run.sh on first boot."""
    status, body = _req("GET", "/api/states", headers={"Authorization": f"Bearer {access}"})
    if status != 200 or not isinstance(body, list):
        return {}
    # Each entry: {"entity_id": "...", "state": "...", "attributes": {...}}
    picks: dict[str, str] = {}
    for e in body:
        eid = e.get("entity_id", "")
        dot = eid.find(".")
        if dot < 0:
            continue
        dom = eid[:dot]
        if dom in picks:
            continue
        # Skip entities the demo platform doesn't own — best-effort filter
        # against entities qpext itself created (sensor.airmonitor_...).
        if "qpext" in eid or "airmonitor" in eid:
            continue
        picks[dom] = eid
    return picks


# Widget type → HA domain. Mirrors ha_integration/const.py's
# DOMAIN_FOR_WIDGET_TYPE but written here to keep bootstrap stdlib-only.
SEED_WIDGET_TYPES = [
    ("sensor",       "sensor"),
    ("switch",       "switch"),
    ("light",        "light"),
    ("climate",      "climate"),
    ("media_player", "media_player"),
    ("cover",        "cover"),
    ("script",       "script"),
    ("scene",        "scene"),
]


def find_qpext_entry(access: str) -> str | None:
    """Return the qpext_airmonitor entry_id, if any."""
    h = {"Authorization": f"Bearer {access}"}
    status, body = _req("GET", "/api/config/config_entries/entry", headers=h)
    if status != 200 or not isinstance(body, list):
        return None
    for e in body:
        if e.get("domain") == "qpext_airmonitor":
            return e.get("entry_id")
    return None


def seed_widgets(access: str) -> None:
    """Auto-seed one widget per supported type on the qpext_airmonitor entry,
    using demo entities. Only seeds if the entry currently has zero widgets,
    so re-running bootstrap doesn't clobber user edits."""
    h = {"Authorization": f"Bearer {access}"}
    entry_id = find_qpext_entry(access)
    if not entry_id:
        print("[bootstrap] no qpext_airmonitor entry yet — skipping widget seed", flush=True)
        return
    # Check existing options to avoid clobbering.
    status, body = _req("GET", "/api/config/config_entries/entry", headers=h)
    cur_widgets = []
    if status == 200 and isinstance(body, list):
        for e in body:
            if e.get("entry_id") == entry_id:
                cur_widgets = (e.get("options") or {}).get("widgets") or []
                break
    if cur_widgets:
        print(f"[bootstrap] qpext entry already has {len(cur_widgets)} widgets — skipping seed",
              flush=True)
        return

    picks = list_demo_entities(access)
    widgets = []
    for wtype, dom in SEED_WIDGET_TYPES:
        eid = picks.get(dom)
        if eid:
            widgets.append({"type": wtype, "entity": eid})
    widgets.append({
        "type": "button",
        "label": "Restart HA",
        "service": "homeassistant.restart",
        "icon": "mdi:restart",
    })
    if not widgets:
        print("[bootstrap] no demo entities found — is `demo:` in configuration.yaml?",
              flush=True)
        return

    # Call our integration's set_options service to write entry.options
    # transactionally. The update listener picks it up and republishes to
    # MQTT, so the device immediately sees the new widget list.
    status, _ = _req(
        "POST",
        "/api/services/qpext_airmonitor/set_options",
        {"entry_id": entry_id, "widgets": widgets},
        headers=h,
    )
    if status in (200, 201):
        print(f"[bootstrap] seeded {len(widgets)} widgets on entry {entry_id[:8]}…",
              flush=True)
    else:
        print(f"[bootstrap] set_options failed: {status}", flush=True)


def existing_token_valid() -> bool:
    """If TOKEN_FILE has a still-working long-lived token, reuse it
    instead of minting another one (HA refuses duplicate client_names)."""
    if not os.path.exists(TOKEN_FILE):
        return False
    try:
        with open(TOKEN_FILE) as f:
            t = f.read().strip()
        if not t:
            return False
        status, body = _req("GET", "/api/", headers={"Authorization": f"Bearer {t}"})
        return status == 200
    except Exception:
        return False


def main() -> None:
    wait_for_ha()
    code = onboard()
    if code is not None:
        access = exchange_code(code)
        finish_onboarding(access)
    else:
        access = login_with_password()
    add_mqtt_integration(access)
    if existing_token_valid():
        print("[bootstrap] reusing existing long-lived token (still valid)", flush=True)
    else:
        token = mint_long_lived(access)
        write_token_file(token)
    seed_widgets(access)
    print()
    print(f"[bootstrap] done.")
    print(f"  HA URL:   {HA}")
    print(f"  Login:    {USERNAME} / {PASSWORD}")
    print(f"  LLT file: {TOKEN_FILE}")


if __name__ == "__main__":
    main()
