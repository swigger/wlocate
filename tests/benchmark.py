"""Measure process-to-exit latency; capture output so console rendering is excluded."""
import json
import statistics
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
NATIVE = ROOT / "build/bin/Release/locate.exe"
OLD = Path(r"D:\tools\bin\locate.old.exe")
cases = {
    "python_all": ["python*.exe"],
    "python_10": ["-n", "10", "python*.exe"],
    "no_matches": ["wlocate-no-such-file-a62f*.invalid"],
    "exe_10": ["-n", "10", "*.exe"],
}
report = {"cases": {}}
for name, args in cases.items():
    times = {"old": [], "native": []}
    outputs = {}
    for iteration in range(9):
        for label, exe in (("old", OLD), ("native", NATIVE)):
            if label == "old" and "10" in args:
                continue  # The legacy wrapper does not implement -n; comparing it would be invalid.
            start = time.perf_counter_ns()
            result = subprocess.run([str(exe), *args], capture_output=True, timeout=30)
            elapsed = (time.perf_counter_ns() - start) / 1e6
            if iteration:
                times[label].append(elapsed)
            outputs[label] = result.stdout.decode("utf-8", errors="replace").splitlines()
            if result.returncode not in (0, 1):
                raise RuntimeError((label, name, result.stderr.decode(errors="replace")))
    summary = {label: {"median_ms": statistics.median(data), "min_ms": min(data), "max_ms": max(data), "runs_ms": data, "rows": len(outputs[label])} for label, data in times.items() if data}
    if name == "python_all":
        summary["only_old"] = sorted(set(outputs["old"]) - set(outputs["native"]))
        summary["only_native"] = sorted(set(outputs["native"]) - set(outputs["old"]))
    report["cases"][name] = summary
    print(name, {label: (round(value["median_ms"], 2), value["rows"]) for label, value in summary.items() if isinstance(value, dict)})
(ROOT / "test-output").mkdir(exist_ok=True)
(ROOT / "test-output/benchmark.json").write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding="utf-8")
print("Python result differences:", report["cases"]["python_all"].get("only_old"), report["cases"]["python_all"].get("only_native"))
