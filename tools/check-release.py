#!/usr/bin/env python3
"""Heuristic privacy gate for tracked release files and explicit assets.

This catches known high-confidence leak shapes; it cannot identify arbitrary
personal prose, unknown passwords, encrypted data or every possible secret.
Use a clean release repository and a manual review as well. --history scans
reachable Git blobs/commit metadata; asset directories must never include .git.
"""
from __future__ import annotations
import argparse
import ipaddress
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tarfile
import zipfile

IPV4 = re.compile(rb"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])")
PRIVATE_NETS = tuple(ipaddress.IPv4Network(n) for n in ((0x0A000000, 8), (0xAC100000, 12), (0xC0A80000, 16)))
RULES = (
    ("local-home-path", re.compile(rb"/(?:Users|home)/[^\s\x00/]+(?:/|(?=[\s\x00]))")),
    ("Google OAuth/API credential", re.compile(rb"(?:1//|ya29\.|AIza|GOCSPX-)[A-Za-z0-9_./-]{20,}")),
    ("Google OAuth client identity", re.compile(rb"[0-9]{8,}-[a-z0-9-]{10,}\.apps\.googleusercontent\.com")),
    ("private PEM key payload", re.compile(rb"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----\r?\n[A-Za-z0-9+/=\r\n]{40,}-----END (?:RSA |EC |OPENSSH )?PRIVATE KEY-----")),
    ("machine-local email", re.compile(rb"[A-Za-z0-9_.+-]+@(?:[A-Za-z0-9_.-]+\.local|localhost|local)\b")),
    ("literal personal configuration", re.compile(rb'(?:wifiSsid|wifiPassword|gdriveRefreshToken|gdriveClientSecret)\s*=\s*"(?!YOUR_|EXAMPLE_|PLACEHOLDER)[^"\r\n]+"')),
)
PRIVATE_NAMES = {"wifi.cfg", "gdrive.cfg", "printer.cfg", "btkeyboard.cfg", "theme.cfg", "rtc.txt", "lastfile.txt", "usb_storage_mode.flag"}
PRIVATE_COMPONENTS = {"backups", "sd-journal", ".git"}
DEBUG_SUFFIXES = {".elf", ".map", ".bundle"}
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024


def content_findings(data: bytes) -> set[str]:
    found = {name for name, pattern in RULES if pattern.search(data)}
    for match in IPV4.finditer(data):
        try:
            address = ipaddress.ip_address(match.group().decode("ascii"))
        except ValueError:
            continue
        if any(address in network for network in PRIVATE_NETS):
            found.add("private network address")
    return found


def path_findings(name: str) -> set[str]:
    path = PurePosixPath(name.split("!")[-1].replace("\\", "/"))
    parts = {part.lower() for part in path.parts}
    found = set()
    if path.is_absolute() or ".." in path.parts:
        found.add("absolute/traversing asset path")
    if path.name.lower() in PRIVATE_NAMES or parts & PRIVATE_COMPONENTS:
        found.add("private SD/configuration/history artifact")
    if "journal" in parts and path.suffix.lower() in {".txt", ".wav", ".cfg", ".json", ".part", ".bak"} and path.name.lower() not in {"readme.txt", "config-readme.txt"}:
        found.add("exported journal data")
    if path.suffix.lower() in DEBUG_SUFFIXES or path.name.lower() == "build.options.json":
        found.add("debug/build/history artifact")
    if "original" in path.name.lower() and path.suffix.lower() == ".bin":
        found.add("possible private recovery image")
    return found


def safe_label(name: str) -> str:
    # Report locations without revealing a username, credential or IP even when
    # it appears in the filename itself.
    data = name.encode("utf-8", "replace")
    for _, pattern in RULES:
        data = pattern.sub(b"[redacted]", data)
    data = IPV4.sub(b"[address]", data)
    return data.decode("utf-8", "replace")


class Scanner:
    def __init__(self) -> None:
        self.failures: list[tuple[str, str]] = []
        self.count = 0

    def fail(self, name: str, category: str) -> None:
        self.failures.append((safe_label(name), category))

    def scan(self, name: str, data: bytes, depth: int = 0) -> None:
        self.count += 1
        for category in path_findings(name) | content_findings(data):
            self.fail(name, category)
        # ZIP and tar source packages need their contents checked too; compressed
        # history/configuration cannot be found by scanning raw archive bytes.
        suffix = name.lower()
        if suffix.endswith((".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tar.xz")):
            if depth >= 2:
                self.fail(name, "nested archive needs manual review")
                return
            import io
            try:
                if suffix.endswith(".zip"):
                    with zipfile.ZipFile(io.BytesIO(data)) as archive:
                        members = archive.infolist()
                        if sum(m.file_size for m in members) > MAX_ARCHIVE_BYTES:
                            raise ValueError("archive size limit")
                        for member in members:
                            label = name + "!" + member.filename
                            if ((member.external_attr >> 16) & 0o170000) == 0o120000:
                                self.fail(label, "archive link needs manual review")
                            elif member.is_dir():
                                for category in path_findings(member.filename): self.fail(label, category)
                            else:
                                self.scan(label, archive.read(member), depth + 1)
                else:
                    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
                        members = archive.getmembers()
                        if sum(m.size for m in members) > MAX_ARCHIVE_BYTES:
                            raise ValueError("archive size limit")
                        for member in members:
                            label = name + "!" + member.name
                            if member.issym() or member.islnk():
                                self.fail(label, "archive link needs manual review")
                            elif member.isfile():
                                stream = archive.extractfile(member)
                                if stream is None: raise ValueError("unreadable member")
                                self.scan(label, stream.read(), depth + 1)
                            else:
                                for category in path_findings(member.name): self.fail(label, category)
            except (OSError, ValueError, RuntimeError, zipfile.BadZipFile, tarfile.TarError):
                self.fail(name, "unreadable/oversized archive")

    def asset(self, path: Path, label: str | None = None) -> None:
        label = label or path.name
        if path.is_symlink():
            self.fail(label, "symlink needs manual review")
        elif path.is_dir():
            for category in path_findings(label): self.fail(label, category)
            if path.name == ".git": return
            for child in sorted(path.iterdir()): self.asset(child, label + "/" + child.name)
        elif path.is_file():
            if path.stat().st_size > MAX_ARCHIVE_BYTES:
                self.fail(label, "oversized asset needs manual review")
            else:
                self.scan(label, path.read_bytes())
        else:
            self.fail(label, "missing/unreadable asset")


def git(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(repo), *args], stderr=subprocess.DEVNULL)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parent.parent, help="Git checkout whose tracked files are checked")
    parser.add_argument("--asset", type=Path, action="append", default=[], help="additional binary, directory or source archive; repeat as needed")
    parser.add_argument("--history", action="store_true", help="also check all reachable Git blobs and commit metadata")
    args = parser.parse_args(argv)
    scanner = Scanner()
    try:
        names = git(args.repo, "ls-files", "-z").decode().split("\0")
        for name in filter(None, names): scanner.asset(args.repo / name, name)
        if args.history:
            for line in git(args.repo, "rev-list", "--objects", "--all").decode().splitlines():
                fields = line.split(" ", 1)
                oid = fields[0]
                kind = git(args.repo, "cat-file", "-t", oid).strip()
                if kind == b"blob":
                    scanner.scan("history/" + (fields[1] if len(fields) == 2 else oid[:12]), git(args.repo, "cat-file", "blob", oid))
                elif kind == b"commit":
                    for category in content_findings(git(args.repo, "cat-file", "commit", oid)):
                        scanner.fail("commit/" + oid[:12], category)
        for asset in args.asset: scanner.asset(asset)
    except (OSError, subprocess.CalledProcessError, UnicodeError):
        print("FAIL: unable to read release inputs (no contents shown)", file=sys.stderr)
        return 2
    for name, category in sorted(set(scanner.failures)):
        print(f"FAIL {name}: {category}")
    if scanner.failures:
        print(f"Release privacy check failed: {len(set(scanner.failures))} findings across {scanner.count} scanned inputs.")
        return 1
    print(f"Release privacy patterns passed across {scanner.count} inputs; manual review remains necessary.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
