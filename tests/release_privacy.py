#!/usr/bin/env python3
"""Deterministic synthetic fixtures for the actual release privacy gate."""
from pathlib import Path
import importlib.util
import io
import subprocess
import sys
import tarfile
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("release_gate", ROOT / "tools/check-release.py")
assert spec and spec.loader
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)
private_ip = ".".join(map(str, (192, 168, 250, 42))).encode()
home = b"/" + b"Users" + b"/fixture-person/source.cpp"
refresh = b"1" + b"//" + b"SYNTHETIC_NOT_A_REAL_TOKEN_1234567890"
key = b"-----BEGIN " + b"PRIVATE KEY-----\n" + b"QUJD" * 20 + b"\n-----END PRIVATE KEY-----"
public_cert = b"-----BEGIN " + b"CERTIFICATE-----\n" + b"QUJD" * 20 + b"\n-----END CERTIFICATE-----"
for payload, category in [(private_ip, "private network address"), (home, "local-home-path"), (refresh, "Google OAuth/API credential"), (key, "private PEM key payload")]:
    assert category in gate.content_findings(payload)
assert not gate.content_findings(b"127.0.0.1 localhost 192.0.2.10 example.invalid YOUR_REFRESH_TOKEN_HERE")
assert not gate.content_findings(public_cert)
assert not gate.content_findings((ROOT / "GoogleRoots.h").read_bytes())
assert not gate.content_findings(b"-----BEGIN PRIVATE KEY-----\x00-----END PRIVATE KEY-----")
assert "private SD/configuration/history artifact" in gate.path_findings("release/journal/config/printer.cfg")
assert "possible private recovery image" in gate.path_findings("cardputer-original.bin")
assert "debug/build/history artifact" in gate.path_findings("release/build.options.json")
assert not gate.path_findings("release/journal/README.txt")
for filename in [home.decode(), private_ip.decode(), refresh.decode()]:
    safe = gate.safe_label(filename)
    assert filename not in safe
with tempfile.TemporaryDirectory(prefix="journal-release-privacy-") as directory:
    root = Path(directory)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    (root / "README.md").write_bytes(b"V. 2026.11.14\n" + public_cert)
    subprocess.run(["git", "-C", str(root), "add", "README.md"], check=True)
    script = str(ROOT / "tools/check-release.py")
    def run(*args):
        return subprocess.run([sys.executable, script, "--repo", str(root), *args], capture_output=True)
    assert run().returncode == 0
    binary = root / "firmware.bin"
    binary.write_bytes(b"image\x00" + home + b"\x00" + refresh + b"\x00" + private_ip)
    result = run("--asset", str(binary))
    assert result.returncode == 1
    for value in [home, refresh, private_ip]: assert value not in result.stdout + result.stderr
    binary.write_bytes(b"image\x00" + public_cert)
    assert run("--asset", str(binary)).returncode == 0
    # Compressed source packages must be examined, not just their raw bytes.
    archive = root / "source.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as z:
        z.writestr(".git/config", b"private history")
        z.writestr("journal/config/wifi.cfg", b"synthetic-network\nsynthetic-password\n")
    result = run("--asset", str(archive))
    assert result.returncode == 1 and b"private SD/configuration/history artifact" in result.stdout
    tar = root / "source.tar.gz"
    with tarfile.open(tar, "w:gz") as t:
        item = tarfile.TarInfo("README.md");item.size=len(public_cert);t.addfile(item, io.BytesIO(public_cert))
    assert run("--asset", str(tar)).returncode == 0
    # A cleaned working tree does not sanitize reachable history.
    (root / "README.md").write_bytes(private_ip)
    subprocess.run(["git", "-C", str(root), "add", "README.md"], check=True)
    commit = ["git", "-C", str(root), "-c", "user.name=Fixture", "-c", "user.email=fixture@example.invalid", "commit", "-qm"]
    subprocess.run(commit + ["synthetic private address"], check=True)
    (root / "README.md").write_bytes(b"V. 2026.11.14\n")
    subprocess.run(["git", "-C", str(root), "add", "README.md"], check=True)
    subprocess.run(commit + ["clean current tree"], check=True)
    assert run().returncode == 0
    result = run("--history")
    assert result.returncode == 1 and private_ip not in result.stdout + result.stderr
print("Release privacy gate passed synthetic secrets, redacted reporting, public-certificate/parser false positives, archives and reachable-history tests")
