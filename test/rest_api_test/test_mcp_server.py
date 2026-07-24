"""
MCP server connectivity test for ameba_claw.
Tests: initialize → tools/list → tools/call (rtk.device.state + rtk.router.trigger)
"""

import json
import sys
import urllib.request
import urllib.error

from config import BOARD_BASE_URL, HTTP_TIMEOUT

MCP_URL = f"{BOARD_BASE_URL}/mcp"
_seq = 0


def rpc(method, params=None):
    global _seq
    _seq += 1
    body = json.dumps({"jsonrpc": "2.0", "id": _seq, "method": method,
                       **({"params": params} if params else {})}).encode()
    req = urllib.request.Request(MCP_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return {"_http_error": e.code, "body": e.read().decode()}
    except Exception as e:
        return {"_error": str(e)}


def check(label, resp, *, key=None, value=None):
    ok = "error" not in resp and "_error" not in resp and "_http_error" not in resp
    if ok and key:
        ok = resp.get("result", {}).get(key) == value
    status = "PASS" if ok else "FAIL"
    print(f"  [{status}] {label}")
    if not ok:
        print(f"         got: {json.dumps(resp, indent=2)[:400]}")
    return ok


def main():
    passed = failed = 0

    # ── 1. initialize ──────────────────────────────────────────────────────────
    print("\n[1] initialize")
    r = rpc("initialize", {"protocolVersion": "2024-11-05",
                            "clientInfo": {"name": "test-client", "version": "0.1"}})
    if check("response is result (not error)", r):
        check("protocolVersion == 2024-11-05", r, key="protocolVersion", value="2024-11-05")
        sv = r.get("result", {}).get("serverInfo", {})
        print(f"         serverInfo: {sv}")
        passed += 1
    else:
        failed += 1
        print("  ABORT: cannot continue without initialize")
        sys.exit(1)

    # MCP-5: version negotiation — server must handle 2025-03-26 without RPC error
    r2 = rpc("initialize", {"protocolVersion": "2025-03-26",
                             "clientInfo": {"name": "test-client", "version": "0.1"}})
    ok_v = "result" in r2
    print(f"  [{'PASS' if ok_v else 'FAIL'}] initialize with 2025-03-26 → no RPC error")
    if ok_v: passed += 1
    else:
        print(f"         got: {json.dumps(r2, indent=2)[:300]}")
        failed += 1
    neg = r2.get("result", {}).get("protocolVersion", "") if ok_v else ""
    ok_nv = neg in ("2025-03-26", "2024-11-05")
    print(f"  [{'PASS' if ok_nv else 'FAIL'}] negotiated version valid: {neg!r}")
    if ok_nv: passed += 1
    else: failed += 1

    # notifications/initialized (fire-and-forget, expect 204)
    body = json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}).encode()
    req = urllib.request.Request(MCP_URL, data=body,
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=HTTP_TIMEOUT) as resp:
            assert resp.status == 204, f"expected 204, got {resp.status}"
        print("  [PASS] notifications/initialized → 204")
        passed += 1
    except Exception as e:
        print(f"  [FAIL] notifications/initialized: {e}")
        failed += 1

    # ── 2. tools/list ──────────────────────────────────────────────────────────
    print("\n[2] tools/list")
    r = rpc("tools/list")
    tools = r.get("result", {}).get("tools", [])
    tool_names = [t["name"] for t in tools]
    print(f"         tools: {tool_names}")

    if check("response has tools array", r) and isinstance(tools, list) and len(tools) > 0:
        passed += 1
    else:
        failed += 1

    for expected in ("rtk.device.state", "rtk.router.trigger"):
        if check(f"tool '{expected}' present", r) and expected in tool_names:
            passed += 1
        else:
            # re-evaluate truthfully
            ok = expected in tool_names
            print(f"  [{'PASS' if ok else 'FAIL'}] tool '{expected}' present")
            (passed if ok else failed).__class__  # dummy; increment below
            if ok:
                passed += 1
            else:
                failed += 1

    # ── 3. tools/call: rtk.device.state ────────────────────────────────────────
    print("\n[3] tools/call rtk.device.state")
    r = rpc("tools/call", {"name": "rtk.device.state",
                            "arguments": {"device_id": "test_device",
                                          "state_name": "connectivity",
                                          "value": "ok"}})
    ok = ("result" in r and
          not r.get("result", {}).get("isError", True) and
          r["result"].get("content"))
    print(f"  [{'PASS' if ok else 'FAIL'}] call accepted, isError=False")
    if ok:
        print(f"         content: {r['result']['content'][0].get('text','')}")
        passed += 1
    else:
        print(f"         got: {json.dumps(r, indent=2)[:400]}")
        failed += 1

    # ── 4. tools/call: rtk.router.trigger ──────────────────────────────────────
    print("\n[4] tools/call rtk.router.trigger")
    r = rpc("tools/call", {"name": "rtk.router.trigger",
                            "arguments": {"event_type": "mcp_test_ping",
                                          "payload_json": "{\"source\":\"mcp_test\"}"}})
    ok = ("result" in r and not r.get("result", {}).get("isError", True))
    print(f"  [{'PASS' if ok else 'FAIL'}] trigger accepted")
    if ok:
        print(f"         content: {r['result']['content'][0].get('text','')}")
        passed += 1
    else:
        print(f"         got: {json.dumps(r, indent=2)[:400]}")
        failed += 1

    # ── 5. error handling ──────────────────────────────────────────────────────
    print("\n[5] error handling")

    r = rpc("tools/call", {"name": "nonexistent.tool", "arguments": {}})
    ok = "error" in r and r["error"].get("code") == -32601
    print(f"  [{'PASS' if ok else 'FAIL'}] unknown tool → -32601")
    (passed if ok else failed)
    if ok: passed += 1
    else: failed += 1

    r = rpc("tools/call", {"name": "rtk.device.state",
                            "arguments": {"device_id": "x"}})  # missing state_name/value
    ok = "result" in r and r["result"]["content"][0]["text"].startswith("error:")
    print(f"  [{'PASS' if ok else 'FAIL'}] missing required args → error text")
    if ok: passed += 1
    else:
        print(f"         got: {json.dumps(r, indent=2)[:300]}")
        failed += 1
    # MCP-1: isError must be True for tool-level errors (MCP spec §5.9)
    ok_ie = "result" in r and r.get("result", {}).get("isError") is True
    print(f"  [{'PASS' if ok_ie else 'FAIL'}] missing required args → isError == True")
    if ok_ie: passed += 1
    else:
        print(f"         isError was: {r.get('result', {}).get('isError', 'MISSING')}")
        failed += 1

    r = rpc("ping")
    ok = "result" in r and r["error"] if "error" in r else "result" in r
    ok = "result" in r
    print(f"  [{'PASS' if ok else 'FAIL'}] ping → result")
    if ok: passed += 1
    else: failed += 1

    # ── summary ────────────────────────────────────────────────────────────────
    total = passed + failed
    print(f"\n{'='*40}")
    print(f"  {passed}/{total} passed")
    print(f"{'='*40}\n")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
