"""Separate service search time from process/IPC/output latency, on a warm index."""
import json
import re
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/bin/Release/locate.exe"
cases = {
    "prefix_limit_10": ["-n", "10", "python*.exe"],
    "basename_substring_count": ["-bic", "python"],
    "fullpath_substring_count": ["-ic", "python"],
    "regex_count": ["-ic", "-r", r"^python.*\.exe$"],
    "gui_all_name_first_page": ["--query", "*", "--sort", "name", "-n", "256"],
    "gui_all_path_first_page": ["--query", "*", "--sort", "path", "-n", "256"],
    "gui_all_size_first_page": ["--query", "*", "--sort", "size", "-n", "256"],
}
report = {}
deadline = time.monotonic() + 60
while True:
    status = subprocess.run([str(EXE), "-S"], capture_output=True, timeout=5)
    if status.returncode == 0 and status.stdout and all(b"\tready\t" in line for line in status.stdout.splitlines()):
        break
    assert time.monotonic() < deadline, "Index did not become ready"
    time.sleep(0.2)
for label, args in cases.items():
    results = []
    for _ in range(3):
        start = time.perf_counter_ns()
        result = subprocess.run([str(EXE), *args, "--timing"], capture_output=True, timeout=30)
        elapsed = (time.perf_counter_ns() - start) / 1e6
        assert result.returncode == 0, result.stderr
        match = re.search(rb"search=([0-9.]+) ms, matches=(\d+)", result.stderr)
        assert match, result.stderr
        results.append({"process_ms": elapsed, "service_ms": float(match[1]), "matches": int(match[2])})
    report[label] = results
    print(label, results, flush=True)
(ROOT / "test-output/performance.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
