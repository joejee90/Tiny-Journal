#!/usr/bin/env python3
"""Exercise the real deterministic guide generator and stale/version checks."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
root = Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    readme, version, header = (temp / name for name in ('README.md', 'JournalVersion.h', 'JournalGuideText.h'))
    body = '# Tiny Journal\nUK £, French é, Russian Ж.\n'
    readme.write_text(body, encoding='utf-8')
    version.write_text('#define TJ_VERSION "2026.11.14"\n')
    command = [sys.executable, str(root / 'tools/generate-guide.py'), '--readme', str(readme), '--version-header', str(version), '--output', str(header)]
    subprocess.run(command, check=True)
    original = header.read_bytes()
    text = header.read_text(encoding='utf-8')
    match = re.search(r'R"([A-Za-z0-9]+)\((.*)\)\1";', text, re.S)
    assert match and match[2] == 'Tiny Journal generated user guide | V. 2026.11.14\n' + body
    subprocess.run(command, check=True)
    assert header.read_bytes() == original
    subprocess.run(command + ['--check'], check=True)
    readme.write_text(body + 'New instructions\n', encoding='utf-8')
    assert subprocess.run(command + ['--check'], stderr=subprocess.DEVNULL).returncode != 0
    assert header.read_bytes() == original
    readme.write_text('x' * 65537)
    assert subprocess.run(command, stderr=subprocess.DEVNULL).returncode != 0
    version.write_text('#define TJ_VERSION "wrong"\n')
    assert subprocess.run(command, stderr=subprocess.DEVNULL).returncode != 0
print('Guide generator preserves UTF-8, is deterministic, detects stale content and enforces version/size bounds')
