"""Exact user-reported path glob, with uncached GUI and CLI timings."""
import json
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "build/bin/Release/locate.exe"
OLD = Path(r"D:\tools\bin\locate.old.exe")
PATTERN = r"C:\Windows\Logs\*.log"
report = {"pattern": PATTERN, "cases": {}}
outputs = {}
for label, exe, args in (
    ("old", OLD, [PATTERN]),
    ("native_cli", NATIVE, [PATTERN, "--timing"]),
    ("native_gui_uncached", NATIVE, ["--query", PATTERN, "--sort", "name", "-n", "256", "--timing"]),
):
    runs = []
    for i in range(6):
        if label == "native_gui_uncached":
            cleared = subprocess.run([str(NATIVE), "--query", "wlocate-cache-reset-no-match-xyzyx"], capture_output=True, timeout=60)
            assert cleared.returncode in (0, 1), cleared.stderr
        start = time.perf_counter_ns()
        result = subprocess.run([str(exe), *args], capture_output=True, timeout=60)
        elapsed = (time.perf_counter_ns() - start) / 1e6
        assert result.returncode in (0, 1), result.stderr
        outputs[label] = result.stdout.decode("utf-8", errors="strict").splitlines()
        match = re.search(rb"search=([0-9.]+) ms, matches=(\d+)", result.stderr)
        item = {"process_ms": elapsed, "rows": len(outputs[label])}
        if match:
            item.update(service_ms=float(match[1]), matches=int(match[2]))
        if i:
            runs.append(item)
        print(label, i, item, flush=True)
    report["cases"][label] = {"runs": runs, "median_process_ms": statistics.median(r["process_ms"] for r in runs)}
    if "service_ms" in runs[0]:
        report["cases"][label]["median_service_ms"] = statistics.median(r["service_ms"] for r in runs)
report["only_old"] = sorted(set(outputs["old"]) - set(outputs["native_cli"]))
report["only_native"] = sorted(set(outputs["native_cli"]) - set(outputs["old"]))
report["gui_matches_cli"] = set(outputs["native_gui_uncached"]) == set(outputs["native_cli"])
assert report["gui_matches_cli"], "GUI-equivalent and CLI result sets differ"
report["outputs"] = outputs
target = ROOT / "test-output" / (sys.argv[1] if len(sys.argv) > 1 else "path-benchmark.json")
target.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
print("Saved", target)
