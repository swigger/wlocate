"""Live local protocol, concurrency, deadlines and bulk-output regression tests."""
import concurrent.futures
import json
import struct
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/bin/Release/locate.exe"
MAGIC = 0x31434C57
checks = []


def run(*args):
    return subprocess.run([str(EXE), *args], capture_output=True, timeout=30)


def exact(pipe, size):
    data = b""
    while len(data) < size:
        chunk = pipe.read(size - len(data))
        assert chunk, "truncated reply"
        data += chunk
    return data


def malformed(**changes):
    fields = dict(magic=MAGIC, size=0, operation=0, flags=0, limit=10,
                  offset=0, sort=0, descending=0, timeout=1000, reserved=0)
    fields.update(changes)
    with open(r"\\.\pipe\WLocate.Search.v1", "r+b", buffering=0) as pipe:
        pipe.write(struct.pack("<IIIIQQIIII", *fields.values()))
        reply = struct.unpack("<IIQQQQ", exact(pipe, 40))
        message = exact(pipe, struct.unpack("<I", exact(pipe, 4))[0]).decode()
        assert reply[0] == MAGIC and reply[1] == 1 and message
        pipe.write(struct.pack("<I", MAGIC))
    return message


for fields in ({"magic": 0}, {"size": 65537}, {"flags": 0xFFFFFFFF},
               {"sort": 99}, {"reserved": 1}, {"operation": 99}):
    checks.append({"malformed": fields, "error": malformed(**fields)})

baseline = run("python*.exe")
assert baseline.returncode == 0 and baseline.stdout
with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
    jobs = list(pool.map(lambda _: run("python*.exe"), range(60)))
assert all(job.returncode == 0 and set(job.stdout.splitlines()) == set(baseline.stdout.splitlines()) for job in jobs)
checks.append({"concurrent_queries": 60, "workers": 20, "passed": True})

large = run("-i", "-n", "20000", "-0", "*.txt")
assert large.returncode == 0 and large.stdout.count(b"\0") == 20000
assert all(path for path in large.stdout[:-1].decode("utf-8").split("\0"))
checks.append({"bulk_rows": 20000, "bytes": len(large.stdout), "passed": True})

start = time.monotonic()
limited = run("--timeout", "100", "--sort", "path", "-n", "10", "*")
elapsed = time.monotonic() - start
assert limited.returncode == 2 and elapsed < 3 and (b"timed out" in limited.stderr or b"cancelled" in limited.stderr)
assert run("python*.exe").returncode == 0
checks.append({"deadline_ms": elapsed * 1000, "service_recovers": True})

permission = run("--reindex")
assert permission.returncode == 2 and b"elevated" in permission.stderr
checks.append({"unelevated_rebuild_denied": True})

(ROOT / "test-output/protocol.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
print("Protocol, 60 concurrent queries, 20,000-row output, deadline and permission checks passed")
