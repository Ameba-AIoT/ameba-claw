"""
MCP tests for ameba_claw.

Part A (tests 1-7): Board as MCP SERVER — Python acts as MCP client.
Part B (tests 8-12): Board as MCP CLIENT — connects to realmcu-ask-ai-docs.

Part B requires REALMCU_MCP_TOKEN env var.
Auth steps:
  1. claude mcp add --transport http realmcu-ask-ai-docs https://ameba-aiot.mcp.kapa.ai
  2. In Claude Code run /mcp → authenticate (GitHub OAuth)
  3. Find token: find ~/.claude -name "*.json" | xargs grep -l "kapa" 2>/dev/null
     OR get from https://aiot.realmcu.com AI → "使用 MCP" panel
  4. export REALMCU_MCP_TOKEN=<token>
"""

import json
import os
import subprocess
import sys
import time
import urllib.request
import urllib.error

from config import BOARD_BASE_URL, HTTP_TIMEOUT

MCP_URL = f"{BOARD_BASE_URL}/mcp"
_seq = 0
passed = failed = 0

# Cap name the board registers after MCP client discovery
MCP_SERVER_NAME   = "realmcu-ask-ai-docs"
MCP_TOOL_NAME     = "search_realtek_knowledge_sources"
MCP_CAP_ID        = f"mcp_{MCP_SERVER_NAME}_{MCP_TOOL_NAME}"
MCP_ENDPOINT_HOST = "ameba-aiot.mcp.kapa.ai"
MCP_ENDPOINT_PATH = "/"


def rpc(method, params=None):
    global _seq
    _seq += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _seq, "method": method,
                       **({"params": params} if params else {})}).encode()
    req = urllib.request.Request(MCP_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        return {"_http_error": e.code, "body": e.read().decode()}
    except Exception as e:
        return {"_error": str(e)}


def check(label, ok, detail=""):
    global passed, failed
    print(f"  [{'PASS' if ok else 'FAIL'}] {label}")
    if not ok and detail:
        print(f"         {detail[:300]}")
    if ok:
        passed += 1
    else:
        failed += 1
    return ok


def board_get(path, params=None):
    url = BOARD_BASE_URL + path
    if params:
        url += "?" + "&".join(f"{k}={v}" for k, v in params.items())
    req = urllib.request.Request(url)
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        return e.code, {}
    except Exception as e:
        return -1, {"_error": str(e)}


def board_post(path, body, timeout=HTTP_TIMEOUT):
    req = urllib.request.Request(
        BOARD_BASE_URL + path,
        data=json.dumps(body).encode() if isinstance(body, dict) else body.encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read())
        except Exception:
            return e.code, {}
    except Exception as e:
        return -1, {"_error": str(e)}


def cap_invoke(cap_id, input_dict, timeout=40):
    """Invoke a capability. Returns (http_status, body_bytes).
    Cap output may be plain text (markdown), not JSON."""
    req = urllib.request.Request(
        BOARD_BASE_URL + "/api/cap/invoke",
        data=json.dumps({"cap": cap_id, "input": input_dict}).encode(),
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()
    except Exception as e:
        return -1, str(e).encode()


def board_put_file(path, content):
    url = BOARD_BASE_URL + "/api/files/content"
    req = urllib.request.Request(
        url + f"?path={path}",
        data=content.encode() if isinstance(content, str) else content,
        method="PUT",
        headers={"Content-Type": "text/plain"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except Exception:
        return -1


def board_mkdir(path):
    req = urllib.request.Request(
        BOARD_BASE_URL + f"/api/files/mkdir?path={path}",
        data=b"",
        method="POST",
        headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except Exception:
        return -1


# ══════════════════════════════════════════════════════════════════════════════
# PART A: Board as MCP SERVER (Python is the MCP client)
# ══════════════════════════════════════════════════════════════════════════════

# ── 1. Board online ───────────────────────────────────────────────────────────
print("\n[1] Board online check")
try:
    with urllib.request.urlopen(f"{BOARD_BASE_URL}/status", timeout=HTTP_TIMEOUT) as r:
        st = json.loads(r.read())
    check("GET /status", st.get("wifi", {}).get("connected"), str(st))
except Exception as e:
    check("GET /status", False, str(e))
    sys.exit(1)


# ── 2. MCP initialize ─────────────────────────────────────────────────────────
print("\n[2] MCP initialize")
r = rpc("initialize", {"protocolVersion": "2024-11-05",
                        "clientInfo": {"name": "test-client", "version": "0.1"}})
if not check("initialize response ok", "result" in r, json.dumps(r)[:200]):
    print("  ABORT: cannot reach /mcp")
    sys.exit(1)
check("protocolVersion == 2024-11-05",
      r["result"].get("protocolVersion") == "2024-11-05")
sv = r["result"].get("serverInfo", {})
print(f"         serverInfo: {sv}")

# MCP-5: version negotiation — server must handle 2025-03-26 without RPC error
r_v2 = rpc("initialize", {"protocolVersion": "2025-03-26",
                           "clientInfo": {"name": "test-client", "version": "0.1"}})
check("initialize with 2025-03-26 → no RPC error", "result" in r_v2, json.dumps(r_v2)[:200])
neg = r_v2.get("result", {}).get("protocolVersion", "")
check(f"negotiated version valid ({neg!r})",
      neg in ("2025-03-26", "2024-11-05"), f"got protocolVersion={neg!r}")

# notifications/initialized (fire-and-forget)
notif_body = json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}).encode()
notif_req = urllib.request.Request(MCP_URL, data=notif_body,
                                   headers={"Content-Type": "application/json"})
try:
    with urllib.request.urlopen(notif_req, timeout=HTTP_TIMEOUT) as r2:
        check("notifications/initialized → 204", r2.status == 204)
except Exception as e:
    check("notifications/initialized", False, str(e))


# ── 3. tools/list ─────────────────────────────────────────────────────────────
print("\n[3] tools/list")
r = rpc("tools/list")
tools = r.get("result", {}).get("tools", [])
tool_names = [t["name"] for t in tools]
print(f"         tools: {tool_names}")
check("tools array non-empty", bool(tools))
for expected in ("rtk.device.state", "rtk.router.trigger"):
    check(f"tool '{expected}' present", expected in tool_names)


# ── 4. tools/call rtk.device.state ───────────────────────────────────────────
print("\n[4] tools/call rtk.device.state")
r = rpc("tools/call", {"name": "rtk.device.state",
                        "arguments": {"device_id": "test_sensor",
                                      "state_name": "temperature",
                                      "value": "25.0"}})
ok = "result" in r and not r["result"].get("isError", True)
check("call accepted, isError=False", ok, json.dumps(r)[:300])
if ok:
    text = r["result"].get("content", [{}])[0].get("text", "")
    print(f"         content: {text}")
    check("response contains 'accepted'", "accepted" in text)


# ── 5. tools/call rtk.router.trigger ─────────────────────────────────────────
print("\n[5] tools/call rtk.router.trigger")
r = rpc("tools/call", {"name": "rtk.router.trigger",
                        "arguments": {"event_type": "mcp_client_test_ping",
                                      "payload_json": "{\"source\":\"test_mcp_client\"}"}})
ok = "result" in r and not r.get("result", {}).get("isError", True)
check("trigger accepted", ok, json.dumps(r)[:300])
if ok:
    text = r["result"].get("content", [{}])[0].get("text", "")
    print(f"         content: {text}")


# ── 6. mcp_server_status cap invoke (LLM-layer integration) ───────────────────
print("\n[6] mcp_server_status cap invoke")
cap_req = urllib.request.Request(
    f"{BOARD_BASE_URL}/api/cap/invoke",
    data=json.dumps({"cap": "mcp_server_status", "input": {}}).encode(),
    headers={"Content-Type": "application/json"})
try:
    with urllib.request.urlopen(cap_req, timeout=HTTP_TIMEOUT) as r2:
        cap_resp = json.loads(r2.read())
    print(f"         response: {json.dumps(cap_resp)[:200]}")
    resp_str = json.dumps(cap_resp)
    check("mcp_server field present", "mcp_server" in resp_str)
    check("status == running", '"status":"running"' in resp_str or '"running"' in resp_str)
    check('mcp_server protocol_version == "2025-03-26"',
          '"2025-03-26"' in resp_str and "protocol_version" in resp_str,
          f"full resp: {resp_str[:300]}")
except Exception as e:
    check("cap invoke", False, str(e))


# ── 7. Error handling ─────────────────────────────────────────────────────────
print("\n[7] Error handling")

r = rpc("tools/call", {"name": "no.such.tool", "arguments": {}})
check("unknown tool → error code -32601",
      "error" in r and r["error"].get("code") == -32601,
      json.dumps(r)[:200])

r = rpc("tools/call", {"name": "rtk.device.state", "arguments": {"device_id": "x"}})
ok = "result" in r and r["result"]["content"][0]["text"].startswith("error:")
check("missing required args → error text in content", ok, json.dumps(r)[:200])
# MCP-1: isError must be True for tool-level errors (MCP spec §5.9)
check("missing required args → isError == True",
      r.get("result", {}).get("isError") is True, json.dumps(r)[:200])

r = rpc("ping")
check("ping → result", "result" in r, json.dumps(r)[:200])


# ══════════════════════════════════════════════════════════════════════════════
# PART B: Board as MCP CLIENT (connects to realmcu-ask-ai-docs)
# ══════════════════════════════════════════════════════════════════════════════

token = os.environ.get("REALMCU_MCP_TOKEN", "").strip()
if not token:
    print("\n[SKIP] Part B skipped: REALMCU_MCP_TOKEN not set")
    print("       Auth: complete GitHub OAuth in Claude Code (/mcp), token in ~/.claude/.credentials.json")
    print("       Run:  REALMCU_MCP_TOKEN=<token> python test_mcp_client.py")
else:
    _abort = False

    # ── 8. Write servers.json to board ────────────────────────────────────────
    print("\n[8] Configure board MCP client (write /mcp/servers.json)")
    servers_cfg = json.dumps({
        "servers": [{
            "name":       MCP_SERVER_NAME,
            "host":       MCP_ENDPOINT_HOST,
            "path":       MCP_ENDPOINT_PATH,
            "api_key":    token,
            "use_bearer": True
        }]
    }, indent=2)
    board_mkdir("/mcp")
    sc = board_put_file("/mcp/servers.json", servers_cfg)
    if not check("PUT /mcp/servers.json", sc in (200, 204), f"HTTP {sc}"):
        _abort = True
    else:
        # Read back to confirm write persisted before restart
        sc_r, rb = board_get("/api/files/content", {"path": "/mcp/servers.json"})
        if not check("servers.json readback ok", sc_r == 200 and MCP_SERVER_NAME in str(rb),
                     f"HTTP {sc_r}: {str(rb)[:200]}"):
            _abort = True

    # ── 9. Restart board via AT command (hardware reset — triggers proper WiFi callback)
    # sys_reset() (REST API) may not reliably fire WiFi reconnect callbacks.
    # AT "reboot" uses DTR/RTS hardware reset, which always goes through the full
    # WiFi init cycle and fires on_wifi_connected → cap_mcp_client_init.
    if not _abort:
        print("\n[9] Restart board (AT reboot) and wait for MCP client init")
        at_cmd = os.path.expanduser("~/tools/at_cmd.py")
        at_cwd = os.path.expanduser("~/ameba_claw")
        result = subprocess.run(
            ["python", at_cmd, "reboot", "-t", "30",
             "-m", r"cap_mcp_client-I\] Initialized", "-p", "COM5"],
            capture_output=True, text=True, cwd=at_cwd, timeout=40)
        reboot_log = result.stdout + result.stderr
        print(f"         at_cmd exit={result.returncode}")
        # Check for 'Initialized' in log
        mcp_init_seen = "cap_mcp_client-I] Initialized" in reboot_log
        check("MCP client initialized in boot log", mcp_init_seen,
              reboot_log[-500:] if not mcp_init_seen else "")
        if not mcp_init_seen:
            _abort = True
        else:
            # Brief wait for port forwarding to catch up
            time.sleep(5)
            print("         waiting for board accessible via HTTP ...")
            deadline = time.time() + 20
            online = False
            while time.time() < deadline:
                time.sleep(2)
                try:
                    with urllib.request.urlopen(f"{BOARD_BASE_URL}/status", timeout=3) as r2:
                        if json.loads(r2.read()).get("wifi", {}).get("connected"):
                            online = True
                            break
                except Exception:
                    pass
            if not check("board accessible via HTTP", online):
                _abort = True

    # ── 10. Poll until MCP cap is callable ────────────────────────────────────
    # cap_mcp_server's tools/list is static (hardcoded); use cap/invoke to verify.
    # Cap output is plain-text markdown, NOT JSON — use cap_invoke(), not board_post().
    if not _abort:
        print("\n[10] Verify MCP cap callable after restart (up to 30s)")
        deadline2 = time.time() + 30
        _t0 = time.time()
        tool_ready = False
        while time.time() < deadline2:
            time.sleep(3)
            sc, body = cap_invoke(MCP_CAP_ID, {"query": "Ameba"})
            elapsed = int(time.time() - _t0)
            if sc == 200 and len(body) > 0:
                tool_ready = True
                print(f"         T+{elapsed:02d}s: cap callable, response {len(body)} bytes")
                break
            print(f"         T+{elapsed:02d}s: HTTP {sc} {body[:80]}")
        if not check(f"cap '{MCP_CAP_ID}' callable", tool_ready):
            _abort = True

    # ── 11. Invoke tool with documentation queries ────────────────────────────
    if not _abort:
        print("\n[11a] Query: OTA compression feature overview")
        sc, body = cap_invoke(MCP_CAP_ID,
                              {"query": "OTA compression feature overview Ameba"})
        if check("OTA query HTTP 200", sc == 200, f"HTTP {sc}"):
            check("OTA response non-empty", len(body) > 10)
            check("OTA: no error in response",
                  b'"error"' not in body[:80] and b'error:' not in body[:80].lower())
            print(f"         content ({len(body)}B): {body[:200]}")

        print("\n[11b] Query: RTL8721F GPIO pin configuration")
        sc2, body2 = cap_invoke(MCP_CAP_ID,
                                {"query": "RTL8721F GPIO pin configuration"})
        if check("GPIO query HTTP 200", sc2 == 200, f"HTTP {sc2}"):
            check("GPIO response non-empty", len(body2) > 10)
            check("GPIO: no error in response",
                  b'"error"' not in body2[:80] and b'error:' not in body2[:80].lower())
            print(f"         content ({len(body2)}B): {body2[:200]}")

    # ── 12. Cleanup: restore empty servers.json ───────────────────────────────
    print("\n[12] Cleanup: restore empty servers.json")
    sc2 = board_put_file("/mcp/servers.json", json.dumps({"servers": []}, indent=2))
    check("restored empty servers.json", sc2 in (200, 204), f"HTTP {sc2}")
    # ponytail: no restart after cleanup — stale tools in memory cleared on next boot


# ── Summary ───────────────────────────────────────────────────────────────────
total = passed + failed
print(f"\n{'='*44}")
print(f"  {passed}/{total} passed")
print(f"{'='*44}\n")
sys.exit(0 if failed == 0 else 1)
