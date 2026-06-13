#!/usr/bin/env python3
"""
ameba_claw REST API Test Runner
Runs all HTTP REST API tests (no serial/AT dependencies).
Usage:
  python3 run_tests.py                  # run all, save test_report.md
  python3 run_tests.py -o report.md     # specify output path
  python3 run_tests.py -q               # quiet mode
"""
import unittest
import sys
import os
import time
import datetime
import argparse
import traceback

# Ensure this directory is on the path so config.py and test_*.py are importable
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

TEST_MODULES = [
    ("HTTP Server Core",      "test_http_server"),
    ("WebUI API",             "test_webui_api"),
    ("File System API",       "test_files"),
    ("File Mgmt Enhanced",    "test_files_enhanced"),
    ("File Content API",      "test_files_content"),
    ("Untested Endpoints",    "test_untested_endpoints"),
    ("Config Boundary Tests", "test_config_api"),
    ("Feishu Webhook",        "test_feishu_webhook"),
    ("Lua Script HTTP API",   "test_lua_api"),
    ("WeChat Smoke",          "test_wechat_smoke"),
    ("Board Manager Caps",    "test_board_mgr"),
]

TEST_PLAN_MAP = {
    "HTTP":     "claw_http_server",
    "WUI":      "cap_webui",
    "FILE":     "cap_files",
    "FILE_EXT": "cap_files (enhanced)",
    "UNT":      "untested endpoints",
    "CFG":      "claw_config",
    "FS":       "cap_im_feishu",
    "LUA":      "cap_lua",
    "WX":       "cap_im_wechat",
    "BRD":      "cap_board_mgr",
}


class DetailedResult(unittest.TestResult):
    def __init__(self):
        super().__init__()
        self.test_details = []
        self.start_times = {}

    def startTest(self, test):
        super().startTest(test)
        self.start_times[test] = time.time()

    def addSuccess(self, test):
        elapsed = time.time() - self.start_times.get(test, time.time())
        self.test_details.append(("PASS", test, elapsed, ""))

    def addError(self, test, err):
        super().addError(test, err)
        elapsed = time.time() - self.start_times.get(test, time.time())
        self.test_details.append(("ERROR", test, elapsed,
                                   "".join(traceback.format_exception(*err))))

    def addFailure(self, test, err):
        super().addFailure(test, err)
        elapsed = time.time() - self.start_times.get(test, time.time())
        self.test_details.append(("FAIL", test, elapsed,
                                   "".join(traceback.format_exception(*err))))

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        elapsed = time.time() - self.start_times.get(test, time.time())
        self.test_details.append(("SKIP", test, elapsed, reason))


def extract_test_id(method_name):
    parts = method_name.split("_")
    if len(parts) >= 3 and parts[0] == "test":
        prefix = parts[1]
        if len(parts) >= 3 and parts[2].isdigit():
            return f"{prefix}-{parts[2]}"
        if prefix in TEST_PLAN_MAP:
            return prefix
    return None


def run_all(output_file=None, verbose=True):
    all_results = []
    suite_results = {}
    total_start = time.time()

    print("=" * 70)
    print("  ameba_claw REST API Test Suite")
    print(f"  Started: {datetime.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print("=" * 70)

    try:
        import requests
        r = requests.get("http://127.0.0.1/status", timeout=5)
        board = r.json()
        print(f"\n[Board] REACHABLE — WiFi: {board['wifi']['ssid']} "
              f"({board['wifi']['ip']})  heap: {board['heap']['free_bytes']:,}B")
    except Exception as e:
        print(f"\n[Board] NOT REACHABLE: {e}")
        print("  Make sure the board is powered and the proxy is running.")

    print()

    for suite_name, module_name in TEST_MODULES:
        print(f"\n{'─' * 70}")
        print(f"  {suite_name}")
        print(f"{'─' * 70}")

        result = DetailedResult()
        try:
            module = __import__(module_name)
            loader = unittest.TestLoader()
            suite = loader.loadTestsFromModule(module)
            suite.run(result)
        except Exception as e:
            print(f"  ERROR loading {module_name}: {e}")
            traceback.print_exc()
            continue

        suite_results[suite_name] = result
        all_results.extend(result.test_details)

        symbols = {"PASS": "✓", "FAIL": "✗", "ERROR": "!", "SKIP": "~"}
        for status, test, elapsed, msg in result.test_details:
            method = getattr(test, "_testMethodName", None) or str(test)
            tid = extract_test_id(method) or method
            sym = symbols.get(status, "?")
            print(f"  [{sym}] {tid}: {method} ({elapsed:.2f}s)")
            if status in ("FAIL", "ERROR") and verbose:
                for line in msg.strip().split("\n")[-5:]:
                    print(f"       {line}")
            elif status == "SKIP":
                print(f"       ↳ {msg[:100]}")

    total_elapsed = time.time() - total_start
    passed  = sum(1 for s, *_ in all_results if s == "PASS")
    failed  = sum(1 for s, *_ in all_results if s == "FAIL")
    errors  = sum(1 for s, *_ in all_results if s == "ERROR")
    skipped = sum(1 for s, *_ in all_results if s == "SKIP")
    total   = len(all_results)

    print("\n" + "=" * 70)
    print("  SUMMARY")
    print("=" * 70)
    print(f"  Total:   {total}  |  Passed: {passed}  |  Failed: {failed}"
          f"  |  Errors: {errors}  |  Skipped: {skipped}")
    print(f"  Time:    {total_elapsed:.1f}s")
    print("=" * 70)

    if failed > 0 or errors > 0:
        print("\n  FAILURES / ERRORS:")
        for status, test, elapsed, msg in all_results:
            if status in ("FAIL", "ERROR"):
                print(f"\n  [{status}] {test.__class__.__name__}.{test._testMethodName}")
                for line in msg.strip().split("\n"):
                    print(f"    {line}")

    report = _generate_report(suite_results, all_results, total_elapsed)
    dest = output_file or os.path.join(os.path.dirname(__file__), "test_report.md")
    with open(dest, "w", encoding="utf-8") as f:
        f.write("\n".join(report))
    print(f"\n  Report → {dest}")

    return failed + errors == 0


def _generate_report(suite_results, all_results, total_elapsed):
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    passed  = sum(1 for s, *_ in all_results if s == "PASS")
    failed  = sum(1 for s, *_ in all_results if s == "FAIL")
    errors  = sum(1 for s, *_ in all_results if s == "ERROR")
    skipped = sum(1 for s, *_ in all_results if s == "SKIP")
    total   = len(all_results)
    runnable = total - skipped

    lines = [
        "# ameba_claw REST API 测试报告",
        "",
        f"> 执行时间：{now}  SoC：RTL8721F",
        "",
        "## 总览",
        "",
        "| 指标 | 数值 |",
        "|------|------|",
        f"| 总用例数 | {total} |",
        f"| 通过 | {passed} |",
        f"| 失败 | {failed} |",
        f"| 错误 | {errors} |",
        f"| 跳过 | {skipped} |",
        f"| 总耗时 | {total_elapsed:.1f}s |",
        (f"| 通过率 | {passed/runnable*100:.1f}% |" if runnable > 0
         else "| 通过率 | N/A |"),
        "",
        "## 各模块详情",
        "",
    ]

    for suite_name, result in suite_results.items():
        s_fail = sum(1 for s, *_ in result.test_details if s in ("FAIL", "ERROR"))
        icon = "✅" if s_fail == 0 else "❌"
        lines += [
            f"### {icon} {suite_name}",
            "",
            "| 用例 | 状态 | 耗时 | 备注 |",
            "|------|------|------|------|",
        ]
        for status, test, elapsed, msg in result.test_details:
            method = getattr(test, "_testMethodName", None) or str(test)
            tid = extract_test_id(method) or "—"
            status_text = {
                "PASS": "✅", "FAIL": "❌", "ERROR": "⚠️", "SKIP": "⏭️",
            }.get(status, status)
            note = ""
            if status in ("FAIL", "ERROR"):
                note = (msg.strip().split("\n")[-1] or "")[:80].replace("|", "\\|")
            elif status == "SKIP":
                note = msg[:80].replace("|", "\\|")
            lines.append(f"| `{tid}` {method} | {status_text} | {elapsed:.2f}s | {note} |")
        lines.append("")

    failures = [(s, t, e, m) for s, t, e, m in all_results if s in ("FAIL", "ERROR")]
    if failures:
        lines += ["## 失败详情", ""]
        for status, test, elapsed, msg in failures:
            lines += [
                f"### {test.__class__.__name__}.{test._testMethodName}",
                "```", msg.strip(), "```", "",
            ]

    lines += [
        "## 测试环境",
        "",
        "| 项目 | 值 |",
        "|------|---|",
        "| SoC | RTL8721F |",
        "| Proxy | 127.0.0.1 |",
        "| Framework | Python unittest |",
        f"| 生成时间 | {now} |",
    ]
    return lines


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="ameba_claw REST API test runner")
    parser.add_argument("--output", "-o", help="Report output path")
    parser.add_argument("--quiet", "-q", action="store_true")
    args = parser.parse_args()
    sys.exit(0 if run_all(output_file=args.output, verbose=not args.quiet) else 1)
