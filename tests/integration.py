"""Exercise live USN updates and CLI behavior using a dedicated disposable fixture."""
import json
import os
import shutil
import subprocess
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXE = ROOT / "build/bin/Release/locate.exe"
PREFIX = "wltest_" + uuid.uuid4().hex[:12]
BASE = ROOT / "test-output" / PREFIX
checks = []

def run(*args, success=True):
    result = subprocess.run([str(EXE), *args], capture_output=True, timeout=30)
    if success and result.returncode not in (0, 1):
        raise AssertionError(result.stderr.decode("utf-8", errors="replace"))
    return result

def paths(pattern):
    return set(run(pattern).stdout.decode("utf-8").splitlines())

def eventual(label, expected, pattern=PREFIX + "*.txt"):
    start = time.perf_counter()
    while True:
        actual = paths(pattern)
        subtree = paths(str(BASE) + "\\*" + pattern)
        relative_query = '"' + BASE.name + '\\" file: ext:' + pattern.rsplit('.', 1)[1]
        relative = set(run("--query", relative_query).stdout.decode("utf-8").splitlines())
        if actual == subtree == relative == set(map(str, expected)):
            checks.append({"name": label, "latency_ms": (time.perf_counter() - start) * 1000, "rows": len(actual)})
            print(label, round(checks[-1]["latency_ms"], 1), "ms", flush=True)
            return
        if time.perf_counter() - start > 10:
            raise AssertionError((label, "missing", set(map(str, expected)) - actual, "extra", actual - set(map(str, expected)), "subtree", subtree, "relative", relative))
        time.sleep(0.1)

BASE.mkdir(parents=True)
try:
    child = BASE / "folder" / "nested"
    child.mkdir(parents=True)
    original = child / (PREFIX + "_中文😀.txt")
    original.write_bytes(b"a" * 9000)
    eventual("create Unicode file and parent directories", [original])
    links = []
    for i in range(30):
        link = child / (PREFIX + f"_hardlink_{i:02d}_long-enough-to-require-MFT-extension.txt")
        os.link(original, link)
        links.append(link)
    eventual("30 hardlinks spanning MFT extension records", [original, *links])
    renamed = BASE / "renamed"
    (BASE / "folder").rename(renamed)
    original = renamed / "nested" / original.name
    links = [renamed / "nested" / item.name for item in links]
    eventual("directory rename moves every descendant path", [original, *links])
    assert paths(str(child) + r"\*.txt") == set(), "old directory prefix survived rename"
    assert paths(str(renamed) + r"\*.txt") == set(map(str, [original, *links])), "renamed subtree lost hardlinks"
    links[0].unlink()
    links = links[1:]
    eventual("remove one hardlink preserves siblings", [original, *links])
    moved = BASE / (PREFIX + "_moved.txt")
    original.rename(moved)
    eventual("move base file preserves other hardlinks", [moved, *links])
    result = run("-c", PREFIX + "*.txt")
    assert result.stdout.strip() == b"30"
    result = run("-n", "3", "-0", PREFIX + "*.txt")
    assert result.stdout.count(b"\0") == 3 and b"\n" not in result.stdout
    assert run("--query", f"{PREFIX} ext:txt size:>8kb", "-c").stdout.strip() == b"30"
    moved.write_bytes(b"b" * 17000)
    deadline = time.monotonic() + 10
    while run("--query", f"{PREFIX} ext:txt size:>16kb", "-c").stdout.strip() != b"30":
        assert time.monotonic() < deadline, "metadata updates did not reach every hardlink"
        time.sleep(0.1)
    checks.append({"name": "size updates for all hardlinks and CLI count/NUL/limit", "passed": True})
    deep = BASE
    for i in range(5):
        deep /= "long_directory_" + str(i) + "_" + "深" * 35
    os.makedirs("\\\\?\\" + str(deep))
    long_file = deep / (PREFIX + "_long_path.dat")
    with open("\\\\?\\" + str(long_file), "wb") as file:
        file.write(b"long path")
    assert len(str(long_file)) > 300
    eventual("Unicode path longer than MAX_PATH", [long_file], PREFIX + "*.dat")
    bulk = BASE / "bulk"
    bulk.mkdir()
    for i in range(500):
        (bulk / f"{PREFIX}_burst_{i:04d}.dat").write_bytes(b"burst")
    expected_bulk = [long_file, *(bulk / f"{PREFIX}_burst_{i:04d}.dat" for i in range(500))]
    eventual("500-file USN create burst", expected_bulk, PREFIX + "*.dat")
    assert run("--regex", "[", success=False).returncode == 2
    assert run(PREFIX + "no-such-file").returncode == 1
    assert run("--limit", "-1", "foo", success=False).returncode == 2
    checks.append({"name": "error and no-match exit codes", "passed": True})
finally:
    # This fixture path is a direct child under the workspace test-output directory.
    assert BASE.parent.resolve() == (ROOT / "test-output").resolve() and BASE.name.startswith("wltest_")
    shutil.rmtree("\\\\?\\" + str(BASE))
eventual("delete fixture removes every indexed entry", [])
eventual("delete burst and long-path entries", [], PREFIX + "*.dat")
(ROOT / "test-output/integration.json").write_text(json.dumps(checks, indent=2), encoding="utf-8")
print("All live integration checks passed")
