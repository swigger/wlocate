"""Compare relative path fragments, keeping warm-index searches uncached."""
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
patterns = [r"foo\bar", r"Windows\Logs", "Windows\\Logs\\", r"Windows\System32\drivers\etc"]
report = {}
target = ROOT / "test-output" / (sys.argv[1] if len(sys.argv) > 1 else "relative-path-benchmark.json")
for pattern in patterns:
    case = {"runs": {}, "outputs": {}}
    for label, exe, args in (
        ("old", OLD, [pattern]),
        ("native_cli", NATIVE, [pattern, "--timing"]),
        ("native_gui_uncached", NATIVE, ["--query", pattern, "--sort", "name", "--timing"]),
    ):
        runs = []
        for iteration in range(4):
            if label == "native_gui_uncached":
                reset = subprocess.run([str(NATIVE), "--query", "wl-relative-benchmark-cache-reset-zyx"], capture_output=True, timeout=60)
                assert reset.returncode in (0, 1), reset.stderr
            start = time.perf_counter_ns()
            result = subprocess.run([str(exe), *args], capture_output=True, timeout=60)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            assert result.returncode in (0, 1), result.stderr
            rows = result.stdout.decode("utf-8").splitlines()
            item = {"process_ms": elapsed, "rows": len(rows)}
            timing = re.search(rb"search=([0-9.]+) ms, matches=(\d+)", result.stderr)
            if timing:
                item.update(service_ms=float(timing[1]), matches=int(timing[2]))
            if iteration:
                runs.append(item)
            case["outputs"][label] = rows
        summary = {"samples": runs, "median_process_ms": statistics.median(r["process_ms"] for r in runs)}
        if "service_ms" in runs[0]:
            summary["median_service_ms"] = statistics.median(r["service_ms"] for r in runs)
        case["runs"][label] = summary
        print(pattern, label, {k: v for k, v in summary.items() if k != "samples"}, "rows", len(rows), flush=True)
    old, native, gui = (set(case["outputs"][key]) for key in ("old", "native_cli", "native_gui_uncached"))
    case["only_old"] = sorted(old - native)
    case["only_native"] = sorted(native - old)
    case["gui_only"] = sorted(gui - native)
    case["cli_only"] = sorted(native - gui)
    report[pattern] = case
    target.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
print("Saved", target)
